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
#include "htif.h"

#include <assert.h>
#include <inttypes.h>
#include <stdio.h>

#include "bits.h"
#include "host_console.h"
#include "host_time.h"
#include "machine.h"

HTIF::HTIF(VirtMachine &machine, HostConsole *console):
    fMachine(machine),
    fConsole(console)
{
}

uint32_t HTIF::DeviceRead(uint32_t offset, int size_log2)
{
    uint32_t val;

    assert(size_log2 == 2);
    switch(offset) {
    case 0:
        val = fToHost;
        break;
    case 4:
        val = fToHost >> 32;
        break;
    case 8:
        val = fFromHost;
        break;
    case 12:
        val = fFromHost >> 32;
        break;
    default:
        val = 0;
        break;
    }
    return val;
}

void HTIF::HandleCommand()
{
    uint32_t device, cmd;

    /* the guest keeps running until the current step ends and may repeat
       the power off request meanwhile */
    if (fMachine.ShutdownRequested())
        return;

    device = get_bits(fToHost, 56, 8);
    cmd = get_bits(fToHost, 48, 8);
    if (device == 0 && cmd == 0 && get_bit(fToHost, 0)) {
        /* Power off, using the spike/riscv-tests convention: the guest writes
           (code << 1) | 1, so a plain 1 is a success exit. The exit status is
           what makes a guest usable as an automated test: it reports pass/fail
           without the harness having to grep the console log. */
        uint64_t code = get_bits(fToHost, 1, 47);
        if (code == 0) {
            printf("\nPower off.\n");
        } else {
            printf("\nPower off, exit code %" PRIu64 ".\n", code);
        }
        /* Only the low 8 bits survive wait(2), and 0 there would turn a failure
           into a pass, so codes that do not fit are reported as 255. */
        fMachine.RequestShutdown(code < 256 ? (int)code : 255);
    } else if (device == 1 && cmd == 1) {
        uint8_t buf[1];
        buf[0] = get_bits(fToHost, 0, 8);
        if (fConsole != nullptr)
            fConsole->WriteData(buf, 1);
        fToHost = 0;
        fFromHost = set_bits(concat_bits<uint64_t>(device, 0, 56),
                             48, 8, cmd);
    } else if (device == 1 && cmd == 0) {
        /* request keyboard interrupt */
        fToHost = 0;
    } else if (device == 2 && cmd == 0) {
        /* calendar time, in microseconds since the Unix epoch */
        fFromHost = host_real_time_us();
    } else {
        printf("HTIF: unsupported tohost=0x%016" PRIx64 "\n", fToHost);
    }
}

void HTIF::DeviceWrite(uint32_t offset, uint32_t val, int size_log2)
{
    assert(size_log2 == 2);
    switch(offset) {
    case 0:
        fToHost = set_bits(fToHost, 0, 32, val);
        break;
    case 4:
        fToHost = set_bits(fToHost, 32, 32, val);
        HandleCommand();
        break;
    case 8:
        fFromHost = set_bits(fFromHost, 0, 32, val);
        break;
    case 12:
        fFromHost = set_bits(fFromHost, 32, 32, val);
        break;
    default:
        break;
    }
}
