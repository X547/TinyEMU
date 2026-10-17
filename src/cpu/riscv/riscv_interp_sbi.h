/*
 * RISC-V SBI for interpreted harts
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

#include "riscv_cpu.h"

class DeviceLock;
class RISCVCpuTarget;


/* The harts an SBI implementation serves. */
class RISCVSbiHarts {
public:
    virtual ~RISCVSbiHarts() = default;

    virtual int HartCount() = 0;
    virtual RISCVCPU *Hart(int hart) = 0;
};


/* The SBI a guest that starts in S-mode calls, in place of firmware: the
   base, timer, IPI, remote fence and hart state management extensions and
   the legacy calls. The system reset and the console are the target's. */
class RISCVSbi final: public RISCVSbiHandler {
private:
    RISCVSbiHarts &fHarts;
    RISCVCpuTarget &fTarget;
    DeviceLock &fDeviceLock;

    int64_t Base(uint32_t fid, uint64_t a[8], uint64_t *value);
    int64_t Hsm(RISCVCPU *cpu, uint32_t fid, uint64_t a[8], uint64_t *value);
    int64_t Reset(RISCVCPU *cpu, uint64_t type, uint64_t reason);
    void Legacy(RISCVCPU *cpu, int xlen, uint32_t eid, uint64_t a[8]);
    /* a call of the target's */
    bool TargetCall(RISCVCPU *cpu, int xlen, uint64_t a[8]);

    /* The harts a mask names, as a bitmap of every hart; false if one of
       them does not exist. */
    bool HartSet(int xlen, uint64_t mask, uint64_t base, uint64_t *set);
    bool LegacyHartSet(RISCVCPU *cpu, uint64_t mask_addr, uint64_t *set);
    void SendIpi(uint64_t set);
    void RemoteSfence(RISCVCPU *cpu, uint64_t set);

public:
    RISCVSbi(RISCVSbiHarts &harts, RISCVCpuTarget &target,
             DeviceLock &device_lock);

    /* RISCVSbiHandler */
    void Ecall(RISCVCPU *cpu, int xlen, uint64_t a[8]) override;
    void MachineTrap(RISCVCPU *cpu, uint64_t cause, uint64_t pc,
                     uint64_t tval) override;
};
