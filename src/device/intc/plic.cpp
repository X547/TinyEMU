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
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "cutils.h"
#include "fdt.h"
#include "plic.h"

/* register layout */
#define PLIC_PENDING_BASE  0x001000
#define PLIC_ENABLE_BASE   0x002000
#define PLIC_ENABLE_SIZE   0x80
#define PLIC_CONTEXT_BASE  0x200000
#define PLIC_CONTEXT_SIZE  0x1000
#define PLIC_MAX_PRIORITY  7


PLIC::PLIC(HartIrqTarget *harts, int hart_count):
    fHarts(harts),
    fHartCount(hart_count),
    fEnable(new uint32_t[2 * hart_count] {}),
    fThreshold(new uint8_t[2 * hart_count] {})
{
}


PLIC::~PLIC()
{
    delete[] fEnable;
    delete[] fThreshold;
}


/* A line that is still high once its interrupt has been completed is pending
   again. */
uint32_t PLIC::Pending() const
{
    return fLevel & ~fServed;
}


/* The source a claim on 'ctx' would return, or 0. */
uint32_t PLIC::BestIrq(int ctx) const
{
    uint32_t mask = Pending() & fEnable[ctx];
    uint32_t best = 0, best_priority = fThreshold[ctx];

    while (mask != 0) {
        uint32_t irq = ctz32(mask);
        mask &= mask - 1;
        /* ties go to the lowest source number */
        if (fPriority[irq] > best_priority) {
            best = irq;
            best_priority = fPriority[irq];
        }
    }
    return best;
}


void PLIC::Update()
{
    for (int hart = 0; hart < fHartCount; hart++) {
        fHarts->SetExternalIrq(hart, false, BestIrq(2 * hart) != 0);
        fHarts->SetExternalIrq(hart, true, BestIrq(2 * hart + 1) != 0);
    }
}


uint32_t PLIC::DeviceRead(uint32_t offset, int size_log2)
{
    uint32_t val = 0;
    uint32_t context_count = 2 * fHartCount;

    assert(size_log2 == 2);
    if (offset < PLIC_PENDING_BASE) {
        uint32_t irq = offset / 4;
        if (irq < PLIC_NUM_SOURCES)
            val = fPriority[irq];
    } else if (offset < PLIC_ENABLE_BASE) {
        if (offset == PLIC_PENDING_BASE)
            val = Pending();
    } else if (offset < PLIC_CONTEXT_BASE) {
        uint32_t ctx = (offset - PLIC_ENABLE_BASE) / PLIC_ENABLE_SIZE;
        uint32_t reg = (offset - PLIC_ENABLE_BASE) % PLIC_ENABLE_SIZE;
        if (ctx < context_count && reg == 0)
            val = fEnable[ctx];
    } else {
        uint32_t ctx = (offset - PLIC_CONTEXT_BASE) / PLIC_CONTEXT_SIZE;
        uint32_t reg = (offset - PLIC_CONTEXT_BASE) % PLIC_CONTEXT_SIZE;
        if (ctx >= context_count)
            return 0;
        if (reg == 0) {
            val = fThreshold[ctx];
        } else if (reg == 4) {
            /* claim */
            val = BestIrq(ctx);
            if (val != 0) {
                fServed |= 1u << val;
                Update();
            }
        }
    }
    return val;
}


void PLIC::DeviceWrite(uint32_t offset, uint32_t val, int size_log2)
{
    uint32_t context_count = 2 * fHartCount;

    assert(size_log2 == 2);
    if (offset < PLIC_PENDING_BASE) {
        uint32_t irq = offset / 4;
        if (irq == 0 || irq >= PLIC_NUM_SOURCES)
            return;
        fPriority[irq] = val & PLIC_MAX_PRIORITY;
    } else if (offset < PLIC_ENABLE_BASE) {
        /* the pending bits are read-only */
        return;
    } else if (offset < PLIC_CONTEXT_BASE) {
        uint32_t ctx = (offset - PLIC_ENABLE_BASE) / PLIC_ENABLE_SIZE;
        uint32_t reg = (offset - PLIC_ENABLE_BASE) % PLIC_ENABLE_SIZE;
        if (ctx >= context_count || reg != 0)
            return;
        fEnable[ctx] = val & ~1u; /* source 0 does not exist */
    } else {
        uint32_t ctx = (offset - PLIC_CONTEXT_BASE) / PLIC_CONTEXT_SIZE;
        uint32_t reg = (offset - PLIC_CONTEXT_BASE) % PLIC_CONTEXT_SIZE;
        if (ctx >= context_count)
            return;
        if (reg == 0) {
            fThreshold[ctx] = val & PLIC_MAX_PRIORITY;
        } else if (reg == 4) {
            /* complete; like QEMU, whether the source is still enabled for
               this context does not matter */
            if (val == 0 || val >= PLIC_NUM_SOURCES)
                return;
            fServed &= ~(1u << val);
        } else {
            return;
        }
    }
    Update();
}


void PLIC::SetIRQ(int irq_num, int level)
{
    uint32_t mask = 1u << irq_num;
    if (level) {
        fLevel |= mask;
    } else {
        fLevel &= ~mask;
    }
    Update();
}


uint32_t PLIC::BuildFDT(FDTBuilder &fdt, uint64_t base,
                        const uint32_t *intc_phandle)
{
    uint32_t *tab = new uint32_t[4 * fHartCount];
    uint32_t phandle;

    fdt.BeginNodeNum("plic", base);
    fdt.PropU32("#interrupt-cells", 1);
    /* Needed so that an "interrupt-map" naming this controller as the parent
       has an unambiguous parent specifier length. */
    fdt.PropU32("#address-cells", 0);
    fdt.PropEmpty("interrupt-controller");
    fdt.PropStr("compatible", "riscv,plic0");
    fdt.PropU32("riscv,ndev", PLIC_NUM_SOURCES - 1);
    fdt.PropU64Range("reg", base, PLIC_SIZE);

    /* the context numbering DeviceRead() and DeviceWrite() decode */
    for (int hart = 0; hart < fHartCount; hart++) {
        tab[4 * hart] = intc_phandle[hart];
        tab[4 * hart + 1] = 11; /* M ext irq */
        tab[4 * hart + 2] = intc_phandle[hart];
        tab[4 * hart + 3] = 9; /* S ext irq */
    }
    fdt.PropTabU32("interrupts-extended", tab, 4 * fHartCount);
    delete[] tab;

    phandle = fdt.AllocPhandle();
    fdt.PropU32("phandle", phandle);

    fdt.EndNode(); /* plic */
    return phandle;
}
