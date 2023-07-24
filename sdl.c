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
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <fcntl.h>
#include <errno.h>

#include <SDL/SDL.h>

#include "cutils.h"
#include "virtio.h"
#include "machine.h"

#ifdef __HAIKU__
#include "WaylandKeycodes.h"
#endif

#define KEYCODE_MAX 127

static SDL_Surface *screen;
static SDL_Surface *fb_surface;
static int screen_width, screen_height, fb_width, fb_height, fb_stride;
static SDL_Cursor *sdl_cursor_hidden;
static uint8_t key_pressed[KEYCODE_MAX + 1];

static void sdl_update_fb_surface(FBDevice *fb_dev)
{
    if (!fb_surface)
        goto force_alloc;
    if (fb_width != fb_dev->width ||
        fb_height != fb_dev->height ||
        fb_stride != fb_dev->stride) {
    force_alloc:
        if (fb_surface != NULL)
            SDL_FreeSurface(fb_surface);
        fb_width = fb_dev->width;
        fb_height = fb_dev->height;
        fb_stride = fb_dev->stride;
        fb_surface = SDL_CreateRGBSurfaceFrom(fb_dev->fb_data,
                                              fb_dev->width, fb_dev->height,
                                              32, fb_dev->stride,
                                              0x00ff0000,
                                              0x0000ff00,
                                              0x000000ff,
                                              0x00000000);
        if (!fb_surface) {
            fprintf(stderr, "Could not create SDL framebuffer surface\n");
            exit(1);
        }
    }
}

static void sdl_update(FBDevice *fb_dev, void *opaque,
                       int x, int y, int w, int h)
{
    SDL_Rect r;
    //    printf("sdl_update: %d %d %d %d\n", x, y, w, h);
    r.x = x;
    r.y = y;
    r.w = w;
    r.h = h;
    SDL_BlitSurface(fb_surface, &r, screen, &r);
    SDL_UpdateRect(screen, r.x, r.y, r.w, r.h);
}

#if defined(__HAIKU__)

static int sdl_get_keycode(const SDL_KeyboardEvent *ev)
{
    int haikuKey = ev->keysym.scancode;
    int wlKey;
		switch (haikuKey) {
			case 0x01: wlKey = KEY_ESC; break;
			case 0x02: wlKey = KEY_F1; break;
			case 0x03: wlKey = KEY_F2; break;
			case 0x04: wlKey = KEY_F3; break;
			case 0x05: wlKey = KEY_F4; break;
			case 0x06: wlKey = KEY_F5; break;
			case 0x07: wlKey = KEY_F6; break;
			case 0x08: wlKey = KEY_F7; break;
			case 0x09: wlKey = KEY_F8; break;
			case 0x0a: wlKey = KEY_F9; break;
			case 0x0b: wlKey = KEY_F10; break;
			case 0x0c: wlKey = KEY_F11; break;
			case 0x0d: wlKey = KEY_F12; break;
			case 0x0e: wlKey = KEY_SYSRQ; break;
			case 0x0f: wlKey = KEY_SCROLLLOCK; break;
			case 0x10: wlKey = KEY_PAUSE; break;
			case 0x11: wlKey = KEY_GRAVE; break;
			case 0x12: wlKey = KEY_1; break;
			case 0x13: wlKey = KEY_2; break;
			case 0x14: wlKey = KEY_3; break;
			case 0x15: wlKey = KEY_4; break;
			case 0x16: wlKey = KEY_5; break;
			case 0x17: wlKey = KEY_6; break;
			case 0x18: wlKey = KEY_7; break;
			case 0x19: wlKey = KEY_8; break;
			case 0x1a: wlKey = KEY_9; break;
			case 0x1b: wlKey = KEY_0; break;
			case 0x1c: wlKey = KEY_MINUS; break;
			case 0x1d: wlKey = KEY_EQUAL; break;
			case 0x1e: wlKey = KEY_BACKSPACE; break;
			case 0x1f: wlKey = KEY_INSERT; break;
			case 0x20: wlKey = KEY_HOME; break;
			case 0x21: wlKey = KEY_PAGEUP; break;
			case 0x22: wlKey = KEY_NUMLOCK; break;
			case 0x23: wlKey = KEY_KPSLASH; break;
			case 0x24: wlKey = KEY_KPASTERISK; break;
			case 0x25: wlKey = KEY_KPMINUS; break;
			case 0x26: wlKey = KEY_TAB; break;
			case 0x27: wlKey = KEY_Q; break;
			case 0x28: wlKey = KEY_W; break;
			case 0x29: wlKey = KEY_E; break;
			case 0x2a: wlKey = KEY_R; break;
			case 0x2b: wlKey = KEY_T; break;
			case 0x2c: wlKey = KEY_Y; break;
			case 0x2d: wlKey = KEY_U; break;
			case 0x2e: wlKey = KEY_I; break;
			case 0x2f: wlKey = KEY_O; break;
			case 0x30: wlKey = KEY_P; break;
			case 0x31: wlKey = KEY_LEFTBRACE; break;
			case 0x32: wlKey = KEY_RIGHTBRACE; break;
			case 0x33: wlKey = KEY_BACKSLASH; break;
			case 0x34: wlKey = KEY_DELETE; break;
			case 0x35: wlKey = KEY_END; break;
			case 0x36: wlKey = KEY_PAGEDOWN; break;
			case 0x37: wlKey = KEY_KP7; break;
			case 0x38: wlKey = KEY_KP8; break;
			case 0x39: wlKey = KEY_KP9; break;
			case 0x3a: wlKey = KEY_KPPLUS; break;
			case 0x3b: wlKey = KEY_CAPSLOCK; break;
			case 0x3c: wlKey = KEY_A; break;
			case 0x3d: wlKey = KEY_S; break;
			case 0x3e: wlKey = KEY_D; break;
			case 0x3f: wlKey = KEY_F; break;
			case 0x40: wlKey = KEY_G; break;
			case 0x41: wlKey = KEY_H; break;
			case 0x42: wlKey = KEY_J; break;
			case 0x43: wlKey = KEY_K; break;
			case 0x44: wlKey = KEY_L; break;
			case 0x45: wlKey = KEY_SEMICOLON; break;
			case 0x46: wlKey = KEY_APOSTROPHE; break;
			case 0x47: wlKey = KEY_ENTER; break;
			case 0x48: wlKey = KEY_KP4; break;
			case 0x49: wlKey = KEY_KP5; break;
			case 0x4a: wlKey = KEY_KP6; break;
			case 0x4b: wlKey = KEY_LEFTSHIFT; break;
			case 0x4c: wlKey = KEY_Z; break;
			case 0x4d: wlKey = KEY_X; break;
			case 0x4e: wlKey = KEY_C; break;
			case 0x4f: wlKey = KEY_V; break;
			case 0x50: wlKey = KEY_B; break;
			case 0x51: wlKey = KEY_N; break;
			case 0x52: wlKey = KEY_M; break;
			case 0x53: wlKey = KEY_COMMA; break;
			case 0x54: wlKey = KEY_DOT; break;
			case 0x55: wlKey = KEY_SLASH; break;
			case 0x56: wlKey = KEY_RIGHTSHIFT; break;
			case 0x57: wlKey = KEY_UP; break;
			case 0x58: wlKey = KEY_KP1; break;
			case 0x59: wlKey = KEY_KP2; break;
			case 0x5a: wlKey = KEY_KP3; break;
			case 0x5b: wlKey = KEY_KPENTER; break;
			case 0x5c: wlKey = KEY_LEFTCTRL; break;
			case 0x5d: wlKey = KEY_LEFTALT; break;
			case 0x5e: wlKey = KEY_SPACE; break;
			case 0x5f: wlKey = KEY_RIGHTALT; break;
			case 0x60: wlKey = KEY_RIGHTCTRL; break;
			case 0x61: wlKey = KEY_LEFT; break;
			case 0x62: wlKey = KEY_DOWN; break;
			case 0x63: wlKey = KEY_RIGHT; break;
			case 0x64: wlKey = KEY_KP0; break;
			case 0x65: wlKey = KEY_KPDOT; break;
			case 0x66: wlKey = KEY_LEFTMETA; break;
			case 0x67: wlKey = KEY_RIGHTMETA; break;
			case 0x68: wlKey = KEY_COMPOSE; break;
			case 0x69: wlKey = KEY_102ND; break;
			case 0x6a: wlKey = KEY_YEN; break;
			case 0x6b: wlKey = KEY_RO; break;

			default:
				wlKey = 0;
		}
    return wlKey;
}

#else

/* we assume Xorg is used with a PC keyboard. Return 0 if no keycode found. */
static int sdl_get_keycode(const SDL_KeyboardEvent *ev)
{
    int keycode;
    keycode = ev->keysym.scancode;
    if (keycode < 9) {
        keycode = 0;
    } else if (keycode < 127 + 8) {
        keycode -= 8;
    } else {
        keycode = 0;
    }
    return keycode;
}

#endif

/* release all pressed keys */
static void sdl_reset_keys(VirtMachine *m)
{
    int i;

    for(i = 1; i <= KEYCODE_MAX; i++) {
        if (key_pressed[i]) {
            vm_send_key_event(m, FALSE, i);
            key_pressed[i] = FALSE;
        }
    }
}

static void sdl_handle_key_event(const SDL_KeyboardEvent *ev, VirtMachine *m)
{
    int keycode, keypress;

    keycode = sdl_get_keycode(ev);
    if (keycode) {
        keypress = (ev->type == SDL_KEYDOWN);
        if (keycode <= KEYCODE_MAX)
            key_pressed[keycode] = keypress;
        vm_send_key_event(m, keypress, keycode);
    } else if (ev->type == SDL_KEYUP) {
        /* workaround to reset the keyboard state (used when changing
           desktop with ctrl-alt-x on Linux) */
        sdl_reset_keys(m);
    }
}

static void sdl_send_mouse_event(VirtMachine *m, int x1, int y1,
                                 int dz, int state, BOOL is_absolute)
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
        x = (x1 * 32768) / screen_width;
        y = (y1 * 32768) / screen_height;
    } else {
        x = x1;
        y = y1;
    }
    vm_send_mouse_event(m, x, y, dz, buttons);
}

static void sdl_handle_mouse_motion_event(const SDL_Event *ev, VirtMachine *m)
{
    BOOL is_absolute = vm_mouse_is_absolute(m);
    int x, y;
    if (is_absolute) {
        x = ev->motion.x;
        y = ev->motion.y;
    } else {
        x = ev->motion.xrel;
        y = ev->motion.yrel;
    }
    sdl_send_mouse_event(m, x, y, 0, ev->motion.state, is_absolute);
}

static void sdl_handle_mouse_button_event(const SDL_Event *ev, VirtMachine *m)
{
    BOOL is_absolute = vm_mouse_is_absolute(m);
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
        sdl_send_mouse_event(m, ev->button.x, ev->button.y,
                             dz, state, is_absolute);
    } else {
        sdl_send_mouse_event(m, 0, 0, dz, state, is_absolute);
    }
}

void sdl_refresh(VirtMachine *m)
{
    SDL_Event ev_s, *ev = &ev_s;

    if (!m->fb_dev)
        return;

    sdl_update_fb_surface(m->fb_dev);

    m->fb_dev->refresh(m->fb_dev, sdl_update, NULL);

    while (SDL_PollEvent(ev)) {
        switch (ev->type) {
        case SDL_KEYDOWN:
        case SDL_KEYUP:
            sdl_handle_key_event(&ev->key, m);
            break;
        case SDL_MOUSEMOTION:
            sdl_handle_mouse_motion_event(ev, m);
            break;
        case SDL_MOUSEBUTTONDOWN:
        case SDL_MOUSEBUTTONUP:
            sdl_handle_mouse_button_event(ev, m);
            break;
        case SDL_QUIT:
            exit(0);
        }
    }
}

static void sdl_hide_cursor(void)
{
    uint8_t data = 0;
    sdl_cursor_hidden = SDL_CreateCursor(&data, &data, 8, 1, 0, 0);
    SDL_ShowCursor(1);
    SDL_SetCursor(sdl_cursor_hidden);
}

void sdl_init(int width, int height)
{
    int flags;

    screen_width = width;
    screen_height = height;

    if (SDL_Init (SDL_INIT_VIDEO | SDL_INIT_NOPARACHUTE)) {
        fprintf(stderr, "Could not initialize SDL - exiting\n");
        exit(1);
    }

    flags = SDL_HWSURFACE | SDL_ASYNCBLIT | SDL_HWACCEL;
    screen = SDL_SetVideoMode(width, height, 0, flags);
    if (!screen || !screen->pixels) {
        fprintf(stderr, "Could not open SDL display\n");
        exit(1);
    }

    SDL_WM_SetCaption("TinyEMU", "TinyEMU");

    sdl_hide_cursor();
}

