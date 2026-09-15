/*
 * SDL display driver
 *
 * Copyright (c) 2017 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <condition_variable>
#include <mutex>
#include <vector>

#include <SDL/SDL.h>

#include "cutils.h"
#include "device_lock.h"
#include "platform_backends.h"
#include "run_control.h"
#include "sdl_keymap.h"

#define KEYCODE_MAX 127

/* How long after a user resize a frame buffer of another size is taken as
   the guest still catching up rather than choosing a size of its own. */
#define RESIZE_GRACE_MS 1000
/* A user resize is applied once no resize event came for this long. Setting
   the video mode while the size still changes makes the window system
   report the sizes set, after newer ones, as further resizes. */
#define RESIZE_SETTLE_MS 150

/* SDL_USEREVENT codes */
enum {
    USER_EVENT_UPDATE,
    USER_EVENT_QUIT,
    USER_EVENT_RESIZE,
};


/* On SDL's timer thread. */
static Uint32 resize_timer_hook(Uint32 interval, void *param)
{
    SDL_Event ev;

    (void)param;
    ev.type = SDL_USEREVENT;
    ev.user.code = USER_EVENT_RESIZE;
    ev.user.data1 = nullptr;
    ev.user.data2 = nullptr;
    SDL_PushEvent(&ev);
    return 0; /* once */
}


struct ScreenRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    ScreenRect() = default;
    ScreenRect(int x, int y, int w, int h):
        x0(x), y0(y), x1(x + w), y1(y + h) {}

    bool Empty() const {return x0 >= x1 || y0 >= y1;}

    ScreenRect operator&(const ScreenRect &r) const
    {
        ScreenRect res;
        res.x0 = max_int(x0, r.x0);
        res.y0 = max_int(y0, r.y0);
        res.x1 = min_int(x1, r.x1);
        res.y1 = min_int(y1, r.y1);
        return res;
    }

    ScreenRect &operator|=(const ScreenRect &r)
    {
        if (r.Empty())
            return *this;
        if (Empty()) {
            *this = r;
        } else {
            x0 = min_int(x0, r.x0);
            y0 = min_int(y0, r.y0);
            x1 = max_int(x1, r.x1);
            y1 = max_int(y1, r.y1);
        }
        return *this;
    }

    SDL_Rect ToSDL() const
    {
        SDL_Rect r;
        r.x = x0;
        r.y = y0;
        r.w = x1 - x0;
        r.h = y1 - y0;
        return r;
    }
};


class SDLDisplay final: public HostDisplay {
private:
    DeviceLock &fDeviceLock;
    RunControl &fRunControl;

    /* Everything below the mutex is shared with the other threads; the rest
       belongs to the thread in Run(). */
    std::mutex fMutex;
    std::condition_variable fCond;
    ScreenSource *fSource = nullptr;
    int fSourceWidth = 0;
    int fSourceHeight = 0;
    bool fResizable = false;
    KeyboardTarget *fKeyboard = nullptr;
    PointerTarget *fPointer = nullptr;
    /* the source's pixels, read in SetFramebuffer() and Update() */
    const uint8_t *fData = nullptr;
    int fDataStride = 0;
    /* the copy the window is drawn from, fFbWidth pixels per row */
    std::vector<uint32_t> fFb;
    int fFbWidth = 0;
    int fFbHeight = 0;
    /* changes whenever fFb is reallocated */
    uint32_t fFbGeneration = 0;
    ScreenRect fDirty;
    std::vector<uint32_t> fCursor;
    int fCursorWidth = 0;
    int fCursorHeight = 0;
    int fCursorX = 0;
    int fCursorY = 0;
    bool fCursorChanged = false;
    /* an update event is queued and not yet handled */
    bool fUpdatePosted = false;
    bool fRunning = false;
    bool fQuitRequested = false;

    SDL_Surface *fScreen = nullptr;
    SDL_Surface *fFbSurface = nullptr;
    SDL_Cursor *fCursorHidden = nullptr;
    int fScreenWidth = 0;
    int fScreenHeight = 0;
    uint32_t fShownGeneration = 0;
    /* where the cursor was last drawn, in window pixels */
    ScreenRect fCursorDrawn;
    bool fRedrawAll = false;
    /* the user resized the window and the guest has not followed yet */
    bool fResizePending = false;
    Uint32 fResizeTicks = 0;
    /* the last user resize, not applied until it settles */
    bool fResizeRequested = false;
    int fResizeWidth = 0;
    int fResizeHeight = 0;
    Uint32 fResizeEventTicks = 0;
    bool fResizeTimerArmed = false;
    uint8_t fKeyPressed[KEYCODE_MAX + 1] {};

    void PostUpdate();
    bool PostEvent(int code);
    void CopyFromSource(int x, int y, int w, int h);
    void OpenWindow(int width, int height);
    void SetVideoMode(int width, int height);
    void HideCursor();
    void PaintArea(const ScreenRect &r, const ScreenRect &cursor);
    bool WindowFollowsFramebuffer();
    void Draw();
    void HandleResizeEvent(int width, int height);
    void ArmResizeTimer(Uint32 delay);
    void HandleResizeTimer();
    void HandleResize(int width, int height);
    void ResetKeys();
    void HandleKeyEvent(const SDL_KeyboardEvent *ev);
    void SendMouseEvent(int x1, int y1, int dz, int state, bool is_absolute);
    void HandleMouseMotionEvent(const SDL_Event *ev);
    void HandleMouseButtonEvent(const SDL_Event *ev);

public:
    SDLDisplay(DeviceLock &lock, RunControl &run_control):
        fDeviceLock(lock), fRunControl(run_control) {}
    ~SDLDisplay() override;

    /* HostScreen */
    void SetSource(ScreenSource *source, int width, int height) override;
    void SetFramebuffer(uint8_t *data, int width, int height,
                        int stride) override;
    void Update(int x, int y, int w, int h) override;
    void SetCursor(const uint32_t *pixels, int width, int height) override;
    void MoveCursor(int x, int y) override;

    /* HostKeyboard, HostPointer */
    void SetTarget(KeyboardTarget *target) override;
    void SetTarget(PointerTarget *target) override;

    /* HostDisplay */
    void Run() override;
    void Quit() override;
};


SDLDisplay::~SDLDisplay()
{
    if (fScreen != nullptr) {
        if (fFbSurface != nullptr)
            SDL_FreeSurface(fFbSurface);
        if (fCursorHidden != nullptr)
            SDL_FreeCursor(fCursorHidden);
        SDL_Quit();
    }
}


/* With fMutex held, and only while Run() has SDL up. Fails when the queue
   is full. */
bool SDLDisplay::PostEvent(int code)
{
    SDL_Event ev;

    ev.type = SDL_USEREVENT;
    ev.user.code = code;
    ev.user.data1 = nullptr;
    ev.user.data2 = nullptr;
    return SDL_PushEvent(&ev) == 0;
}


/* With fMutex held. */
void SDLDisplay::PostUpdate()
{
    if (fRunning && !fUpdatePosted)
        fUpdatePosted = PostEvent(USER_EVENT_UPDATE);
}


void SDLDisplay::SetTarget(KeyboardTarget *target)
{
    std::lock_guard<std::mutex> locker(fMutex);
    fKeyboard = target;
}


void SDLDisplay::SetTarget(PointerTarget *target)
{
    std::lock_guard<std::mutex> locker(fMutex);
    fPointer = target;
}


void SDLDisplay::SetSource(ScreenSource *source, int width, int height)
{
    std::lock_guard<std::mutex> locker(fMutex);
    fSource = source;
    fSourceWidth = width;
    fSourceHeight = height;
    fResizable = source != nullptr && source->Resizable();
}


/* With fMutex held: copies a rectangle, already clipped, into fFb. */
void SDLDisplay::CopyFromSource(int x, int y, int w, int h)
{
    for (int i = 0; i < h; i++) {
        memcpy(&fFb[(y + i) * fFbWidth + x],
               fData + (y + i) * fDataStride + x * 4, w * 4);
    }
}


void SDLDisplay::SetFramebuffer(uint8_t *data, int width, int height,
                                int stride)
{
    std::lock_guard<std::mutex> locker(fMutex);

    if (width <= 0 || height <= 0) {
        data = nullptr;
        width = height = 0;
    }
    bool resized = width != fFbWidth || height != fFbHeight;
    if (!resized && data == fData && stride == fDataStride)
        return;

    if (resized) {
        fFb.assign((size_t)width * height, 0);
        fFbWidth = width;
        fFbHeight = height;
        fFbGeneration++;
    }
    fData = data;
    fDataStride = stride;
    if (data != nullptr) {
        CopyFromSource(0, 0, width, height);
    } else if (!resized) {
        std::fill(fFb.begin(), fFb.end(), 0);
    }
    fDirty |= ScreenRect(0, 0, width, height);
    PostUpdate();
}


void SDLDisplay::Update(int x, int y, int w, int h)
{
    std::lock_guard<std::mutex> locker(fMutex);

    ScreenRect r = ScreenRect(x, y, w, h) & ScreenRect(0, 0, fFbWidth, fFbHeight);
    if (r.Empty() || fData == nullptr)
        return;
    CopyFromSource(r.x0, r.y0, r.x1 - r.x0, r.y1 - r.y0);
    fDirty |= r;
    PostUpdate();
}


void SDLDisplay::SetCursor(const uint32_t *pixels, int width, int height)
{
    std::lock_guard<std::mutex> locker(fMutex);

    if (pixels == nullptr || width <= 0 || height <= 0) {
        fCursor.clear();
        fCursorWidth = fCursorHeight = 0;
    } else {
        fCursor.assign(pixels, pixels + width * height);
        fCursorWidth = width;
        fCursorHeight = height;
    }
    fCursorChanged = true;
    PostUpdate();
}


void SDLDisplay::MoveCursor(int x, int y)
{
    std::lock_guard<std::mutex> locker(fMutex);

    if (x == fCursorX && y == fCursorY)
        return;
    fCursorX = x;
    fCursorY = y;
    fCursorChanged = true;
    PostUpdate();
}


void SDLDisplay::Quit()
{
    std::lock_guard<std::mutex> locker(fMutex);
    fQuitRequested = true;
    if (fRunning)
        PostEvent(USER_EVENT_QUIT);
    fCond.notify_all();
}


/* With fMutex held: draws the frame buffer, black past its edges, and the
   cursor over both, into one rectangle of the window. */
void SDLDisplay::PaintArea(const ScreenRect &area, const ScreenRect &cursor)
{
    ScreenRect r = area & ScreenRect(0, 0, fScreenWidth, fScreenHeight);
    if (r.Empty())
        return;

    ScreenRect fb = r & ScreenRect(0, 0, fFbWidth, fFbHeight);
    if (!fb.Empty() && fFbSurface != nullptr) {
        SDL_Rect sr = fb.ToSDL();
        SDL_Rect dr = sr;
        SDL_BlitSurface(fFbSurface, &sr, fScreen, &dr);
    }
    /* the strips right of and below the frame buffer */
    if (r.x1 > fFbWidth) {
        SDL_Rect sr = ScreenRect(max_int(r.x0, fFbWidth), r.y0,
                           r.x1 - max_int(r.x0, fFbWidth),
                           r.y1 - r.y0).ToSDL();
        SDL_FillRect(fScreen, &sr, 0);
    }
    if (r.y1 > fFbHeight) {
        int x1 = min_int(r.x1, fFbWidth);
        if (x1 > r.x0) {
            SDL_Rect sr = ScreenRect(r.x0, max_int(r.y0, fFbHeight), x1 - r.x0,
                               r.y1 - max_int(r.y0, fFbHeight)).ToSDL();
            SDL_FillRect(fScreen, &sr, 0);
        }
    }

    ScreenRect c = r & cursor;
    if (c.Empty())
        return;
    int w = c.x1 - c.x0;
    int h = c.y1 - c.y0;
    std::vector<uint32_t> pixels((size_t)w * h);
    for (int y = 0; y < h; y++) {
        int sy = c.y0 + y;
        for (int x = 0; x < w; x++) {
            int sx = c.x0 + x;
            uint32_t dst = 0;
            if (sx < fFbWidth && sy < fFbHeight)
                dst = fFb[sy * fFbWidth + sx];
            uint32_t src = fCursor[(sy - fCursorY) * fCursorWidth +
                                   (sx - fCursorX)];
            uint32_t a = src >> 24;
            uint32_t out = 0;
            for (int shift = 0; shift < 24; shift += 8) {
                uint32_t s = (src >> shift) & 0xff;
                uint32_t d = (dst >> shift) & 0xff;
                out |= ((s * a + d * (255 - a) + 127) / 255) << shift;
            }
            pixels[y * w + x] = out;
        }
    }
    SDL_Surface *surface = SDL_CreateRGBSurfaceFrom(pixels.data(), w, h, 32,
                                                    w * 4, 0x00ff0000,
                                                    0x0000ff00, 0x000000ff,
                                                    0x00000000);
    if (surface != nullptr) {
        SDL_Rect dr = c.ToSDL();
        SDL_BlitSurface(surface, nullptr, fScreen, &dr);
        SDL_FreeSurface(surface);
    }
}


/* With fMutex held, when the frame buffer changed size. While the user
   resizes, the guest answers sizes the window has already left behind, so
   the window keeps the user's size; otherwise it takes the guest's. */
bool SDLDisplay::WindowFollowsFramebuffer()
{
    if (fFbWidth == fScreenWidth && fFbHeight == fScreenHeight) {
        fResizePending = false;
        return false;
    }
    if (fResizePending && SDL_GetTicks() - fResizeTicks < RESIZE_GRACE_MS)
        return false;
    fResizePending = false;
    return true;
}


/* Redraws what changed since the last time. */
void SDLDisplay::Draw()
{
    std::lock_guard<std::mutex> locker(fMutex);

    fUpdatePosted = false;
    if (fFbGeneration != fShownGeneration) {
        fShownGeneration = fFbGeneration;
        if (fFbSurface != nullptr) {
            SDL_FreeSurface(fFbSurface);
            fFbSurface = nullptr;
        }
        if (fFbWidth > 0) {
            fFbSurface = SDL_CreateRGBSurfaceFrom(fFb.data(), fFbWidth,
                                                  fFbHeight, 32, fFbWidth * 4,
                                                  0x00ff0000,
                                                  0x0000ff00,
                                                  0x000000ff,
                                                  0x00000000);
            if (!fFbSurface) {
                fprintf(stderr, "Could not create SDL framebuffer surface\n");
                exit(1);
            }
            if (WindowFollowsFramebuffer())
                SetVideoMode(fFbWidth, fFbHeight);
        }
        fRedrawAll = true;
    }

    ScreenRect dirty = fDirty;
    fDirty = ScreenRect();
    if (fRedrawAll) {
        fRedrawAll = false;
        dirty = ScreenRect(0, 0, fScreenWidth, fScreenHeight);
        fCursorChanged = true;
    }

    ScreenRect cursor;
    if (!fCursor.empty()) {
        cursor = ScreenRect(fCursorX, fCursorY, fCursorWidth, fCursorHeight) &
            ScreenRect(0, 0, fScreenWidth, fScreenHeight);
    }

    SDL_Rect rects[3];
    int count = 0;
    ScreenRect areas[3] = {dirty};
    int area_count = 1;
    if (fCursorChanged) {
        fCursorChanged = false;
        areas[area_count++] = fCursorDrawn;
        areas[area_count++] = cursor;
        fCursorDrawn = cursor;
    }
    for (int i = 0; i < area_count; i++) {
        ScreenRect r = areas[i] & ScreenRect(0, 0, fScreenWidth, fScreenHeight);
        if (r.Empty())
            continue;
        PaintArea(r, cursor);
        rects[count++] = r.ToSDL();
    }
    if (count > 0)
        SDL_UpdateRects(fScreen, count, rects);
}


/* release all pressed keys */
void SDLDisplay::ResetKeys()
{
    int i;

    for(i = 1; i <= KEYCODE_MAX; i++) {
        if (fKeyPressed[i]) {
            if (fKeyboard != nullptr)
                fKeyboard->SendKeyEvent(false, i);
            fKeyPressed[i] = false;
        }
    }
}


void SDLDisplay::HandleKeyEvent(const SDL_KeyboardEvent *ev)
{
    int keycode, keypress;

    keycode = sdl_get_keycode(ev);
    if (keycode) {
        keypress = (ev->type == SDL_KEYDOWN);
        if (keycode <= KEYCODE_MAX)
            fKeyPressed[keycode] = keypress;
        if (fKeyboard != nullptr)
            fKeyboard->SendKeyEvent(keypress, keycode);
    } else if (ev->type == SDL_KEYUP) {
        /* workaround to reset the keyboard state (used when changing
           desktop with ctrl-alt-x on Linux) */
        ResetKeys();
    }
}


void SDLDisplay::SendMouseEvent(int x1, int y1, int dz, int state,
                                bool is_absolute)
{
    int buttons, x, y;

    buttons = 0;
    if (state & SDL_BUTTON(SDL_BUTTON_LEFT))
        buttons |= (1 << 0);
    if (state & SDL_BUTTON(SDL_BUTTON_RIGHT))
        buttons |= (1 << 1);
    if (state & SDL_BUTTON(SDL_BUTTON_MIDDLE))
        buttons |= (1 << 2);
    if (is_absolute) {
        /* scaled to the frame buffer, which may be smaller than the window
           until the guest follows a resize */
        int width, height;
        {
            std::lock_guard<std::mutex> locker(fMutex);
            width = fFbWidth > 0 ? fFbWidth : fScreenWidth;
            height = fFbHeight > 0 ? fFbHeight : fScreenHeight;
        }
        x = (min_int(max_int(x1, 0), width - 1) * 32768) / width;
        y = (min_int(max_int(y1, 0), height - 1) * 32768) / height;
    } else {
        x = x1;
        y = y1;
    }
    fPointer->SendMouseEvent(x, y, dz, buttons);
}


void SDLDisplay::HandleMouseMotionEvent(const SDL_Event *ev)
{
    if (fPointer == nullptr)
        return;
    bool is_absolute = fPointer->MouseIsAbsolute();
    int x, y;
    if (is_absolute) {
        x = ev->motion.x;
        y = ev->motion.y;
    } else {
        x = ev->motion.xrel;
        y = ev->motion.yrel;
    }
    SendMouseEvent(x, y, 0, ev->motion.state, is_absolute);
}


void SDLDisplay::HandleMouseButtonEvent(const SDL_Event *ev)
{
    if (fPointer == nullptr)
        return;
    bool is_absolute = fPointer->MouseIsAbsolute();
    int state, dz;

    dz = 0;
    if (ev->type == SDL_MOUSEBUTTONDOWN) {
        if (ev->button.button == SDL_BUTTON_WHEELUP) {
            dz = 1;
        } else if (ev->button.button == SDL_BUTTON_WHEELDOWN) {
            dz = -1;
        }
    }

    state = SDL_GetMouseState(NULL, NULL);
    /* just in case */
    if (ev->type == SDL_MOUSEBUTTONDOWN)
        state |= SDL_BUTTON(ev->button.button);
    else
        state &= ~SDL_BUTTON(ev->button.button);

    if (is_absolute) {
        SendMouseEvent(ev->button.x, ev->button.y, dz, state, is_absolute);
    } else {
        SendMouseEvent(0, 0, dz, state, is_absolute);
    }
}


void SDLDisplay::HideCursor()
{
    uint8_t data = 0;
    fCursorHidden = SDL_CreateCursor(&data, &data, 8, 1, 0, 0);
    SDL_ShowCursor(1);
    SDL_SetCursor(fCursorHidden);
}


/* Also with fMutex held, from Draw(). */
void SDLDisplay::SetVideoMode(int width, int height)
{
    int flags = SDL_HWSURFACE | SDL_ASYNCBLIT | SDL_HWACCEL;

    if (fResizable)
        flags |= SDL_RESIZABLE;
    fScreen = SDL_SetVideoMode(width, height, 0, flags);
    if (!fScreen || !fScreen->pixels) {
        fprintf(stderr, "Could not open SDL display\n");
        exit(1);
    }
    fScreenWidth = width;
    fScreenHeight = height;
    fCursorDrawn = ScreenRect();
    fRedrawAll = true;
}


void SDLDisplay::OpenWindow(int width, int height)
{
    if (SDL_Init (SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_NOPARACHUTE)) {
        fprintf(stderr, "Could not initialize SDL - exiting\n");
        exit(1);
    }

    SetVideoMode(width, height);
    SDL_WM_SetCaption("TinyEMU", "TinyEMU");

    HideCursor();
}


/* The user is resizing the window; the size is applied once it settles. */
void SDLDisplay::HandleResizeEvent(int width, int height)
{
    fResizeRequested = true;
    fResizeWidth = width;
    fResizeHeight = height;
    fResizeEventTicks = SDL_GetTicks();
    if (!fResizeTimerArmed)
        ArmResizeTimer(RESIZE_SETTLE_MS);
}


void SDLDisplay::ArmResizeTimer(Uint32 delay)
{
    fResizeTimerArmed = SDL_AddTimer(delay, resize_timer_hook,
                                     nullptr) != nullptr;
    if (!fResizeTimerArmed) {
        fResizeRequested = false;
        HandleResize(fResizeWidth, fResizeHeight);
    }
}


void SDLDisplay::HandleResizeTimer()
{
    fResizeTimerArmed = false;
    if (!fResizeRequested)
        return;
    Uint32 elapsed = SDL_GetTicks() - fResizeEventTicks;
    if (elapsed < RESIZE_SETTLE_MS) {
        ArmResizeTimer(RESIZE_SETTLE_MS - elapsed);
        return;
    }
    fResizeRequested = false;
    HandleResize(fResizeWidth, fResizeHeight);
}


/* Take the size the user gave the window and ask the source to follow. */
void SDLDisplay::HandleResize(int width, int height)
{
    width = max_int(width, 1);
    height = max_int(height, 1);
    {
        std::lock_guard<std::mutex> locker(fMutex);
        if (width == fScreenWidth && height == fScreenHeight)
            return;
        SetVideoMode(width, height);
        fResizePending = true;
        fResizeTicks = SDL_GetTicks();
    }
    Draw();

    DeviceLocker device_locker(fDeviceLock);
    ScreenSource *source;
    {
        std::lock_guard<std::mutex> locker(fMutex);
        source = fSource;
    }
    if (source != nullptr)
        source->ScreenResized(width, height);
}


void SDLDisplay::Run()
{
    SDL_Event ev_s, *ev = &ev_s;
    int width, height;

    {
        std::unique_lock<std::mutex> locker(fMutex);
        if (fSource == nullptr) {
            /* nothing to show, so no window */
            fCond.wait(locker, [this]() {return fQuitRequested;});
            return;
        }
        if (fQuitRequested)
            return;
        width = fSourceWidth;
        height = fSourceHeight;
        OpenWindow(width, height);
        fRunning = true;
        /* whatever arrived before SDL could take events */
        fUpdatePosted = PostEvent(USER_EVENT_UPDATE);
    }

    for (;;) {
        bool got_event = SDL_WaitEvent(ev);
        {
            /* also seen here in case the event did not fit in the queue */
            std::lock_guard<std::mutex> locker(fMutex);
            if (fQuitRequested) {
                fRunning = false;
                return;
            }
        }
        if (!got_event)
            continue;
        switch (ev->type) {
        case SDL_USEREVENT:
            if (ev->user.code == USER_EVENT_UPDATE)
                Draw();
            else if (ev->user.code == USER_EVENT_RESIZE)
                HandleResizeTimer();
            break;
        case SDL_VIDEORESIZE:
            HandleResizeEvent(ev->resize.w, ev->resize.h);
            break;
        case SDL_VIDEOEXPOSE:
            fRedrawAll = true;
            Draw();
            break;
        case SDL_KEYDOWN:
        case SDL_KEYUP: {
            DeviceLocker locker(fDeviceLock);
            HandleKeyEvent(&ev->key);
            break;
        }
        case SDL_MOUSEMOTION: {
            DeviceLocker locker(fDeviceLock);
            HandleMouseMotionEvent(ev);
            break;
        }
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP: {
            DeviceLocker locker(fDeviceLock);
            HandleMouseButtonEvent(ev);
            break;
        }
        case SDL_QUIT:
            /* the machine's shutdown quits this loop */
            fRunControl.RequestShutdown(0);
            break;
        }
    }
}


std::unique_ptr<HostDisplay> host_display_create(DeviceLock &lock,
                                                 RunControl &run_control)
{
    return std::make_unique<SDLDisplay>(lock, run_control);
}
