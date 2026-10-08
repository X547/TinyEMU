/*
 * MC146818 RTC
 *
 * Copyright (c) 2011-2017 Fabrice Bellard
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

#include "iomem.h"

struct CMOSState;


/* A PC's RTC at an index and a data port: the host's calendar time, the
   periodic interrupt on 'irq' and 128 bytes of CMOS RAM. */
class MC146818 final {
private:
    std::unique_ptr<CMOSState> fState;

public:
    MC146818(PhysMemoryMap *port_map, int addr, IRQSignal *irq,
             bool use_local_time);
    ~MC146818();

    /* Sets a byte of CMOS RAM, as firmware would have left it. */
    void SetNvram(int index, uint8_t val);

    /* The microseconds until the next periodic interrupt, or -1. */
    int64_t DelayUs();

    /* Raises the periodic interrupt if it is due and returns DelayUs().
       Without 'deliver' the flags are set but the line is left alone. */
    int64_t RunTimers(bool deliver);
};
