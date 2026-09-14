/*
 * SDL key codes: Haiku
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
#include "sdl_keymap.h"
#include "WaylandKeycodes.h"


int sdl_get_keycode(const SDL_KeyboardEvent *ev)
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
