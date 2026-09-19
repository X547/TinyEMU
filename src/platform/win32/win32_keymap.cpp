/*
 * Windows scan codes to Linux evdev key codes
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
#include "win32_keymap.h"

#include <windows.h>

#include "WaylandKeycodes.h"


int win32_key_to_evdev(unsigned vk, unsigned scancode, bool extended)
{
    /* Pause arrives with Num Lock's scan code, and Print Screen and Ctrl +
       Pause with codes of their own, so these go by the virtual key. */
    switch (vk) {
    case VK_PAUSE:
    case VK_CANCEL:
        return KEY_PAUSE;
    case VK_NUMLOCK:
        return KEY_NUMLOCK;
    case VK_SNAPSHOT:
        return KEY_SYSRQ;
    }

    if (!extended) {
        /* Up to the keypad's full stop, set 1 scan codes and evdev codes are
           the same numbers. */
        if (scancode >= 0x01 && scancode <= 0x53)
            return scancode;
        switch (scancode) {
        case 0x54: return KEY_SYSRQ; /* Alt + Print Screen */
        case 0x56: return KEY_102ND;
        case 0x57: return KEY_F11;
        case 0x58: return KEY_F12;
        case 0x59: return KEY_KPEQUAL;
        case 0x70: return KEY_KATAKANAHIRAGANA;
        case 0x73: return KEY_RO;
        case 0x79: return KEY_HENKAN;
        case 0x7b: return KEY_MUHENKAN;
        case 0x7d: return KEY_YEN;
        case 0x7e: return KEY_KPCOMMA;
        }
        return 0;
    }

    switch (scancode) {
    case 0x1c: return KEY_KPENTER;
    case 0x1d: return KEY_RIGHTCTRL;
    case 0x20: return KEY_MUTE;
    case 0x2e: return KEY_VOLUMEDOWN;
    case 0x30: return KEY_VOLUMEUP;
    case 0x35: return KEY_KPSLASH;
    case 0x37: return KEY_SYSRQ;
    case 0x38: return KEY_RIGHTALT;
    case 0x47: return KEY_HOME;
    case 0x48: return KEY_UP;
    case 0x49: return KEY_PAGEUP;
    case 0x4b: return KEY_LEFT;
    case 0x4d: return KEY_RIGHT;
    case 0x4f: return KEY_END;
    case 0x50: return KEY_DOWN;
    case 0x51: return KEY_PAGEDOWN;
    case 0x52: return KEY_INSERT;
    case 0x53: return KEY_DELETE;
    case 0x5b: return KEY_LEFTMETA;
    case 0x5c: return KEY_RIGHTMETA;
    case 0x5d: return KEY_COMPOSE;
    case 0x5e: return KEY_POWER;
    }
    /* including the shift an extended key is wrapped in with Num Lock on */
    return 0;
}
