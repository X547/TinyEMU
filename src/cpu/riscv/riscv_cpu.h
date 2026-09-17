/*
 * RISCV CPU emulator
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
#ifndef RISCV_CPU_H
#define RISCV_CPU_H

#include <stdlib.h>
#include "bits.h"
#include "cutils.h"
#include "iomem.h"

#define MIP_USIP bit_at(0)
#define MIP_SSIP bit_at(1)
#define MIP_HSIP bit_at(2)
#define MIP_MSIP bit_at(3)
#define MIP_UTIP bit_at(4)
#define MIP_STIP bit_at(5)
#define MIP_HTIP bit_at(6)
#define MIP_MTIP bit_at(7)
#define MIP_UEIP bit_at(8)
#define MIP_SEIP bit_at(9)
#define MIP_HEIP bit_at(10)
#define MIP_MEIP bit_at(11)

/* Supplies the machine's real time counter. The 'time' CSR and the timer
   device the firmware programs must read the same counter: firmware computes
   a deadline as "now + delta" from one and writes it to the other, so two
   clocks with different origins put every deadline arbitrarily far away. */
class RtcTimeSource {
public:
    virtual ~RtcTimeSource() = default;

    virtual uint64_t RtcTime() = 0;
};


/* What a hart implements for interrupt control, as the machine's interrupt
   controller requires: the base privileged architecture, the Smaia and Ssaia
   CSRs, or those together with IMSIC interrupt files. */
enum RISCVInterruptArch {
    RISCV_INTR_BASE,
    RISCV_INTR_AIA,
    RISCV_INTR_AIA_IMSIC,
};

/* Interrupt identities each IMSIC interrupt file implements. */
#define RISCV_IMSIC_NUM_IDS 255

/* One implementation per supported XLEN; riscv_cpu.cpp is compiled once for
   each and each build keeps its implementation class internal.

   The hart belongs to the processor thread. SetIrqLine() and
   ImsicSetPending() may be called from any thread; the hart takes them in
   before it next looks at its interrupts. */
class RISCVCPU {
public:
    virtual ~RISCVCPU() = default;

    virtual void Interp(int n_cycles) = 0;
    virtual uint64_t Cycles() = 0;
    /* The level of the interrupt lines in 'mask' that a device drives: MSIP,
       MTIP, and MEIP and SEIP unless an IMSIC drives those. */
    virtual void SetIrqLine(uint32_t mask, bool level) = 0;
    /* Brings the Sstc supervisor timer interrupt up to date with the real time
       counter and returns when the next one falls due, or UINT64_MAX when the
       comparator is not driving it. */
    virtual uint64_t UpdateSTimer() = 0;
    /* Whether the hart waits for an interrupt, after taking in the ones
       posted to it. */
    virtual bool PowerDown() = 0;
    virtual uint32_t Misa() = 0;
    virtual void FlushTlbWriteRangeRam(uint8_t *ram_ptr, size_t ram_size) = 0;
    virtual void SetRtcTimeSource(RtcTimeSource *source) = 0;
    virtual void SetDeviceLock(DeviceLock *lock) = 0;
    virtual void SetInterruptArch(RISCVInterruptArch arch) = 0;
    /* A write of 'id' to the seteipnum register of the hart's machine or
       supervisor level IMSIC interrupt file. */
    virtual void ImsicSetPending(bool supervisor, uint32_t id) = 0;
};

int riscv_cpu_get_max_xlen(void);

/* Return nullptr if max_xlen is not supported by this build. */
RISCVCPU *riscv_cpu_create(PhysMemoryMap *mem_map, int max_xlen,
                           uint32_t hart_id);

RISCVCPU *riscv_cpu_create32(PhysMemoryMap *mem_map, uint32_t hart_id);
RISCVCPU *riscv_cpu_create64(PhysMemoryMap *mem_map, uint32_t hart_id);
RISCVCPU *riscv_cpu_create128(PhysMemoryMap *mem_map, uint32_t hart_id);

#endif /* RISCV_CPU_H */
