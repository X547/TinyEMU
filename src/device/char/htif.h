/*
 * RISC-V HTIF
 *
 * Copyright (c) 2016-2017 Fabrice Bellard
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

#include "iomem.h"

class HostConsole;
class VirtMachine;


/* the tohost and fromhost registers */
#define HTIF_REG_SIZE 16


/* The Berkeley host-target interface: the guest writes a command to tohost
   and reads any answer from fromhost. Device 0 powers off, device 1 is a
   console that only writes, and device 2 gives the calendar time. */
class HTIF final: public DeviceIO {
private:
    VirtMachine &fMachine;
    HostConsole *fConsole;
    uint64_t fToHost = 0;
    uint64_t fFromHost = 0;

    void HandleCommand();

public:
    /* 'console' may be nullptr, which drops what is written to it. */
    HTIF(VirtMachine &machine, HostConsole *console);

    /* DeviceIO */
    uint64_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint64_t val, int size_log2) override;
};
