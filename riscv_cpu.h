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
#include "cutils.h"
#include "iomem.h"

#define MIP_USIP (1 << 0)
#define MIP_SSIP (1 << 1)
#define MIP_HSIP (1 << 2)
#define MIP_MSIP (1 << 3)
#define MIP_UTIP (1 << 4)
#define MIP_STIP (1 << 5)
#define MIP_HTIP (1 << 6)
#define MIP_MTIP (1 << 7)
#define MIP_UEIP (1 << 8)
#define MIP_SEIP (1 << 9)
#define MIP_HEIP (1 << 10)
#define MIP_MEIP (1 << 11)

/* One implementation per supported XLEN; riscv_cpu.cpp is compiled once for
   each and each build keeps its implementation class internal. */
class RISCVCPU {
public:
    virtual ~RISCVCPU() = default;

    virtual void Interp(int n_cycles) = 0;
    virtual uint64_t Cycles() = 0;
    virtual void SetMip(uint32_t mask) = 0;
    virtual void ResetMip(uint32_t mask) = 0;
    virtual uint32_t Mip() = 0;
    virtual bool PowerDown() = 0;
    virtual uint32_t Misa() = 0;
    virtual void FlushTlbWriteRangeRam(uint8_t *ram_ptr, size_t ram_size) = 0;
};

int riscv_cpu_get_max_xlen(void);

/* Return nullptr if max_xlen is not supported by this build. */
RISCVCPU *riscv_cpu_create(PhysMemoryMap *mem_map, int max_xlen);

RISCVCPU *riscv_cpu_create32(PhysMemoryMap *mem_map);
RISCVCPU *riscv_cpu_create64(PhysMemoryMap *mem_map);
RISCVCPU *riscv_cpu_create128(PhysMemoryMap *mem_map);

#endif /* RISCV_CPU_H */
