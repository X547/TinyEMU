/*
 * RISC-V CLINT
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


/* The size of a SiFive CLINT. It must be exactly this: firmware splits the
   region into its ACLINT MSWI and MTIMER halves by size, so declaring a
   larger one moves mtimecmp somewhere this CLINT does not decode and the
   timer silently never fires. */
#define CLINT_SIZE 0x00010000


/* Implemented by the machine, which keeps the counter the harts' 'time' CSR
   reads as well. */
class ClintHost {
public:
    virtual ~ClintHost() = default;

    /* mtime, and a write to it */
    virtual uint64_t ClintTime() = 0;
    virtual void SetClintTime(uint64_t time) = 0;
    /* Drives the machine software or timer interrupt of 'hart'. */
    virtual void SetClintIrq(int hart, bool timer, bool level) = 0;
    /* An mtimecmp now asks for the timers to be run by 'deadline'. */
    virtual void ClintTimerChanged(uint64_t deadline) = 0;
};


/* A SiFive CLINT: one msip word and one mtimecmp per hart, and mtime. It
   maps as the two halves an ACLINT splits it into: the MSWI, of 32 bit
   registers, and the MTIMER, of 64 bit ones. */
class Clint final {
private:
    ClintHost &fHost;
    int fHartCount;
    std::unique_ptr<uint64_t[]> fTimecmp;
    /* the MSIP and MTIP levels driven into each hart */
    std::unique_ptr<bool[]> fSoftIrq;
    std::unique_ptr<bool[]> fTimerIrq;

    void SetIrq(int hart, bool timer, bool level);

    uint32_t MswiRead(uint32_t offset, int size_log2);
    void MswiWrite(uint32_t offset, uint32_t val, int size_log2);
    uint64_t MtimerRead(uint32_t offset, int size_log2);
    void MtimerWrite(uint32_t offset, uint64_t val, int size_log2);
    DeviceIOAdapter<Clint, &Clint::MswiRead, &Clint::MswiWrite>
        fMswiIo {*this};
    DeviceIOAdapter<Clint, &Clint::MtimerRead, &Clint::MtimerWrite>
        fMtimerIo {*this};

public:
    Clint(ClintHost &host, int hart_count);

    /* Registers the CLINT_SIZE bytes of registers at 'base'. */
    void Map(PhysMemoryMap *map, uint64_t base);

    /* Raises the timer interrupt of 'hart' if it is due at 'now', and
       returns the counter it is due at otherwise, or UINT64_MAX. */
    uint64_t RunTimer(int hart, uint64_t now);
};
