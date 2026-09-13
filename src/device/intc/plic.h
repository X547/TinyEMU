/*
 * RISC-V Platform-Level Interrupt Controller
 *
 * Copyright (c) 2016-2018 Fabrice Bellard
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

#include "iomem.h"
#include "hart_irq.h"

class FDTBuilder;

#define PLIC_SIZE 0x00400000

/* Input lines; line 0 does not exist. */
#define PLIC_NUM_SOURCES 32


/* Context 2n takes machine mode external interrupts for hart n and context
   2n + 1 supervisor mode ones, in the order the device tree lists them. The
   sources are level triggered. */
class PLIC final: public DeviceIO, public IRQTarget {
private:
    HartIrqTarget *fHarts;
    int fHartCount;
    /* Bit n of each mask is source n. */
    uint32_t fLevel = 0; /* input line levels */
    uint32_t fServed = 0; /* claimed and not yet completed */
    uint8_t fPriority[PLIC_NUM_SOURCES] {};
    uint32_t *fEnable;
    uint8_t *fThreshold;

    uint32_t Pending() const;
    uint32_t BestIrq(int ctx) const;
    void Update();

public:
    PLIC(HartIrqTarget *harts, int hart_count);
    ~PLIC() override;

    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;

    /* IRQTarget */
    void SetIRQ(int irq_num, int level) override;

    /* Emit the controller's node; returns its phandle. 'intc_phandle' holds
       the interrupt controller of each hart. */
    uint32_t BuildFDT(FDTBuilder &fdt, uint64_t base,
                      const uint32_t *intc_phandle);
};
