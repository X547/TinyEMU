/*
 * Host keyboard and pointer
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
#pragma once

#include <stdint.h>

/* Implemented by a device that takes key presses. */
class KeyboardTarget {
public:
    virtual ~KeyboardTarget() = default;

    /* 'key_code' is a Linux evdev key code. */
    virtual void SendKeyEvent(bool is_down, uint16_t key_code) = 0;
};


/* Implemented by a pointing device: a mouse or a tablet. */
class PointerTarget {
public:
    virtual ~PointerTarget() = default;

    /* When MouseIsAbsolute(), dx and dy are a position in 0..32767 rather
       than a displacement. Bit 0 of 'buttons' is the left button, bit 1 the
       right one and bit 2 the middle one. */
    virtual void SendMouseEvent(int dx, int dy, int dz,
                                unsigned int buttons) = 0;
    virtual bool MouseIsAbsolute() = 0;
};


class HostKeyboard {
public:
    virtual ~HostKeyboard() = default;

    /* nullptr discards the keys */
    virtual void SetTarget(KeyboardTarget *target) = 0;
};


class HostPointer {
public:
    virtual ~HostPointer() = default;

    /* nullptr discards the motion */
    virtual void SetTarget(PointerTarget *target) = 0;
};
