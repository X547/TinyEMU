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

#include <condition_variable>
#include <mutex>

#include <SDL/SDL.h>

#include "cutils.h"
#include "device_lock.h"
#include "platform_backends.h"
#include "run_control.h"
#include "sdl_keymap.h"

#define KEYCODE_MAX 127

/* SDL_USEREVENT codes */
enum {
    USER_EVENT_UPDATE,
    USER_EVENT_QUIT,
};


/* The frame buffer geometry and the area changed since the last blit, handed
   from the refreshing thread to the window's. */
struct PendingUpdate {
    uint8_t *data = nullptr;
    int width = 0;
    int height = 0;
    int stride = 0;
    /* empty when x0 >= x1 */
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
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
    KeyboardTarget *fKeyboard = nullptr;
    PointerTarget *fPointer = nullptr;
    PendingUpdate fPending;
    /* an update event is queued and not yet handled */
    bool fUpdatePosted = false;
    bool fRunning = false;
    bool fQuitRequested = false;

    SDL_Surface *fScreen = nullptr;
    SDL_Surface *fFbSurface = nullptr;
    SDL_Cursor *fCursorHidden = nullptr;
    int fScreenWidth = 0;
    int fScreenHeight = 0;
    int fFbWidth = 0;
    int fFbHeight = 0;
    int fFbStride = 0;
    uint8_t fKeyPressed[KEYCODE_MAX + 1] {};

    bool PostEvent(int code);
    void OpenWindow(int width, int height);
    void HideCursor();
    void ApplyUpdate();
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
}


void SDLDisplay::SetFramebuffer(uint8_t *data, int width, int height,
                                int stride)
{
    std::lock_guard<std::mutex> locker(fMutex);
    fPending.data = data;
    fPending.width = width;
    fPending.height = height;
    fPending.stride = stride;
}


void SDLDisplay::Update(int x, int y, int w, int h)
{
    std::lock_guard<std::mutex> locker(fMutex);
    PendingUpdate &p = fPending;

    if (p.x0 >= p.x1) {
        p.x0 = x;
        p.y0 = y;
        p.x1 = x + w;
        p.y1 = y + h;
    } else {
        p.x0 = min_int(p.x0, x);
        p.y0 = min_int(p.y0, y);
        p.x1 = max_int(p.x1, x + w);
        p.y1 = max_int(p.y1, y + h);
    }
    if (fRunning && !fUpdatePosted)
        fUpdatePosted = PostEvent(USER_EVENT_UPDATE);
}


void SDLDisplay::Quit()
{
    std::lock_guard<std::mutex> locker(fMutex);
    fQuitRequested = true;
    if (fRunning)
        PostEvent(USER_EVENT_QUIT);
    fCond.notify_all();
}


/* Blits what changed since the last time. */
void SDLDisplay::ApplyUpdate()
{
    PendingUpdate p;

    {
        std::lock_guard<std::mutex> locker(fMutex);
        p = fPending;
        fPending.x0 = fPending.x1 = 0;
        fUpdatePosted = false;
    }
    if (p.data == nullptr)
        return;

    if (fFbSurface == nullptr || fFbWidth != p.width ||
        fFbHeight != p.height || fFbStride != p.stride ||
        fFbSurface->pixels != p.data) {
        if (fFbSurface != nullptr)
            SDL_FreeSurface(fFbSurface);
        fFbWidth = p.width;
        fFbHeight = p.height;
        fFbStride = p.stride;
        fFbSurface = SDL_CreateRGBSurfaceFrom(p.data, p.width, p.height, 32,
                                              p.stride,
                                              0x00ff0000,
                                              0x0000ff00,
                                              0x000000ff,
                                              0x00000000);
        if (!fFbSurface) {
            fprintf(stderr, "Could not create SDL framebuffer surface\n");
            exit(1);
        }
    }

    if (p.x0 < p.x1) {
        SDL_Rect r;
        //    printf("sdl_update: %d %d %d %d\n", x, y, w, h);
        r.x = p.x0;
        r.y = p.y0;
        r.w = p.x1 - p.x0;
        r.h = p.y1 - p.y0;
        SDL_BlitSurface(fFbSurface, &r, fScreen, &r);
        SDL_UpdateRect(fScreen, r.x, r.y, r.w, r.h);
    }
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
        x = (x1 * 32768) / fScreenWidth;
        y = (y1 * 32768) / fScreenHeight;
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


void SDLDisplay::OpenWindow(int width, int height)
{
    int flags;

    fScreenWidth = width;
    fScreenHeight = height;

    if (SDL_Init (SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE)) {
        fprintf(stderr, "Could not initialize SDL - exiting\n");
        exit(1);
    }

    flags = SDL_HWSURFACE | SDL_ASYNCBLIT | SDL_HWACCEL;
    fScreen = SDL_SetVideoMode(width, height, 0, flags);
    if (!fScreen || !fScreen->pixels) {
        fprintf(stderr, "Could not open SDL display\n");
        exit(1);
    }

    SDL_WM_SetCaption("TinyEMU", "TinyEMU");

    HideCursor();
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
    }

    OpenWindow(width, height);

    {
        std::lock_guard<std::mutex> locker(fMutex);
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
                ApplyUpdate();
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
