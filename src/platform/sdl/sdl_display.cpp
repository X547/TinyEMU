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

#include <SDL/SDL.h>

#include "platform_backends.h"
#include "sdl_keymap.h"
#include "wait_set.h"

#define KEYCODE_MAX 127


class SDLDisplay final: public HostDisplay, public PollSource {
private:
    EventLoop &fLoop;
    ScreenSource *fSource = nullptr;
    KeyboardTarget *fKeyboard = nullptr;
    PointerTarget *fPointer = nullptr;

    SDL_Surface *fScreen = nullptr;
    SDL_Surface *fFbSurface = nullptr;
    SDL_Cursor *fCursorHidden = nullptr;
    int fScreenWidth = 0;
    int fScreenHeight = 0;
    int fFbWidth = 0;
    int fFbHeight = 0;
    int fFbStride = 0;
    uint8_t fKeyPressed[KEYCODE_MAX + 1] {};

    void OpenWindow(int width, int height);
    void HideCursor();
    void ResetKeys();
    void HandleKeyEvent(const SDL_KeyboardEvent *ev);
    void SendMouseEvent(int x1, int y1, int dz, int state, bool is_absolute);
    void HandleMouseMotionEvent(const SDL_Event *ev);
    void HandleMouseButtonEvent(const SDL_Event *ev);

public:
    SDLDisplay(EventLoop &loop): fLoop(loop) {fLoop.Add(this);}
    ~SDLDisplay() override;

    /* HostScreen */
    void SetSource(ScreenSource *source, int width, int height) override;
    void SetFramebuffer(uint8_t *data, int width, int height,
                        int stride) override;
    void Update(int x, int y, int w, int h) override;

    /* HostKeyboard, HostPointer */
    void SetTarget(KeyboardTarget *target) override {fKeyboard = target;}
    void SetTarget(PointerTarget *target) override {fPointer = target;}

    /* PollSource */
    void Prepare(WaitSet &ws) override {(void)ws;}
    void Dispatch(WaitSet &ws) override;
};


SDLDisplay::~SDLDisplay()
{
    fLoop.Remove(this);
    if (fScreen != nullptr) {
        if (fFbSurface != nullptr)
            SDL_FreeSurface(fFbSurface);
        if (fCursorHidden != nullptr)
            SDL_FreeCursor(fCursorHidden);
        SDL_Quit();
    }
}


void SDLDisplay::SetFramebuffer(uint8_t *data, int width, int height,
                                int stride)
{
    if (fFbSurface != nullptr && fFbWidth == width && fFbHeight == height &&
        fFbStride == stride && fFbSurface->pixels == data) {
        return;
    }
    if (fFbSurface != nullptr)
        SDL_FreeSurface(fFbSurface);
    fFbWidth = width;
    fFbHeight = height;
    fFbStride = stride;
    fFbSurface = SDL_CreateRGBSurfaceFrom(data, width, height, 32, stride,
                                          0x00ff0000,
                                          0x0000ff00,
                                          0x000000ff,
                                          0x00000000);
    if (!fFbSurface) {
        fprintf(stderr, "Could not create SDL framebuffer surface\n");
        exit(1);
    }
}


void SDLDisplay::Update(int x, int y, int w, int h)
{
    SDL_Rect r;
    //    printf("sdl_update: %d %d %d %d\n", x, y, w, h);
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    SDL_BlitSurface(fFbSurface, &r, fScreen, &r);
    SDL_UpdateRect(fScreen, r.x, r.y, r.w, r.h);
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


void SDLDisplay::Dispatch(WaitSet &ws)
{
    SDL_Event ev_s, *ev = &ev_s;

    (void)ws;
    if (fSource == nullptr)
        return;

    fSource->Refresh(this);

    while (SDL_PollEvent(ev)) {
        switch (ev->type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            HandleKeyEvent(&ev->key);
            break;
        case SDL_MOUSEMOTION:
            HandleMouseMotionEvent(ev);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            HandleMouseButtonEvent(ev);
            break;
        case SDL_QUIT:
            fLoop.RequestQuit(0);
            break;
        }
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


void SDLDisplay::SetSource(ScreenSource *source, int width, int height)
{
    fSource = source;
    if (source != nullptr && fScreen == nullptr)
        OpenWindow(width, height);
}


std::unique_ptr<HostDisplay> host_display_create(EventLoop &loop)
{
    return std::make_unique<SDLDisplay>(loop);
}
