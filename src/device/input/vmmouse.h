/*
 * VMware absolute pointer protocol
 *
 * Copyright (c) 2003 Fabrice Bellard
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
#include <memory>

#include "ps2.h"

/* A guest driver that speaks the VMware port protocol reads absolute
   positions from here instead of decoding the relative packets of the PS/2
   pointer the positions arrive alongside. Until it does, and whenever it
   turns the protocol off again, the events go to that pointer. */
typedef struct VMMouseState VMMouseState;

struct VMMouseDeleter {
    void operator()(VMMouseState *s) const;
};
typedef std::unique_ptr<VMMouseState, VMMouseDeleter> VMMousePtr;

VMMousePtr vmmouse_init(PS2Mouse *ps2_mouse);
bool vmmouse_is_absolute(VMMouseState *s);
void vmmouse_send_mouse_event(VMMouseState *s, int x, int y, int dz,
                              int buttons);
void vmmouse_handler(VMMouseState *s, uint32_t *regs);
