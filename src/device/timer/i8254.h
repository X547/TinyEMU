/*
 * 8254 PIT
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

struct PITState;


/* A PC's 8254, counting host time, and the port B bits wired to its channel
   2: the speaker gate and the channel's output. Channel 0 drives 'irq'. */
class I8254 final {
private:
    std::unique_ptr<PITState> fState;

public:
    /* The counters at four ports from 'addr0', port B at 'addr1'. */
    I8254(PhysMemoryMap *port_map, int addr0, int addr1, IRQSignal *irq);
    ~I8254();

    /* The microseconds until channel 0's next interrupt, or -1. */
    int64_t DelayUs();

    /* Raises channel 0's interrupt if it is due and returns DelayUs().
       Without 'deliver' the counting goes on but the line is left alone. */
    int64_t RunTimers(bool deliver);
};
