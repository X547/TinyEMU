/*
 * Interrupt controllers
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
#include "intc.h"

#include "aplic.h"
#include "i8259.h"
#include "ioapic.h"
#include "plic.h"

namespace {

IntController *sControllers[16];
int sControllerCount;

}


void intc_register(IntController *ctrl)
{
    for (int i = 0; i < sControllerCount; i++) {
        if (sControllers[i] == ctrl)
            return;
    }
    if (sControllerCount < (int)ARRAY_SIZE(sControllers))
        sControllers[sControllerCount++] = ctrl;
}


void intc_reset_all()
{
    for (int i = 0; i < sControllerCount; i++)
        sControllers[i]->Reset();
}


bool intc_resolve(const HwDevice *dev, int index, IntController **ctrl,
                  uint32_t *line)
{
    if (index >= dev->nirqs)
        return false;
    const HwIrq &irq = dev->irqs[index];
    if (dev->fdt) {
        const FdtNode *parent = fdt_find_phandle(irq.parent);
        if (parent == nullptr)
            return false;
        IntController *c = Plic::Get(parent);
        if (c == nullptr)
            c = Aplic::Get(parent);
        if (c == nullptr)
            return false;
        *ctrl = c;
        *line = irq.cells[0];
        return true;
    }
    if (dev->acpi) {
        /* an IOAPIC when there is one, as an OS would choose, else the
           8259s */
        uint32_t gsi = irq.isa ? acpi_isa_irq_gsi(irq.line, nullptr)
                               : irq.line;
        if (IoApic *io = IoApic::ForGsi(gsi)) {
            *ctrl = io;
            *line = gsi;
            return true;
        }
        I8259 *pic = I8259::Get();
        if (pic && irq.line < 16) {
            *ctrl = pic;
            *line = irq.line;
            return true;
        }
    }
    return false;
}


bool intc_level(const HwDevice *dev, int index)
{
    if (index >= dev->nirqs)
        return true;
    const HwIrq &irq = dev->irqs[index];
    if (dev->fdt) {
        /* the trigger cell, when the controller has one: 1 and 2 are the
           edges, 4 and 8 the levels */
        return irq.ncells < 2 || (irq.cells[1] & 0xc) != 0;
    }
    return irq.level;
}


bool irq_route(const HwDevice *dev, int index, IrqHandler fn, void *ctx)
{
    IntController *c;
    uint32_t line;
    return intc_resolve(dev, index, &c, &line) &&
           c->Route(line, fn, ctx, intc_level(dev, index));
}


void irq_unroute(const HwDevice *dev, int index)
{
    IntController *c;
    uint32_t line;
    if (intc_resolve(dev, index, &c, &line))
        c->Unroute(line);
}


uint32_t hart_intc_phandle(uint32_t hart)
{
    const FdtNode *cpus = fdt_root() ? fdt_find_path("/cpus") : nullptr;
    for (const FdtNode *cpu = cpus ? cpus->child : nullptr; cpu;
         cpu = cpu->sibling) {
        uint32_t reg;
        if (!fdt_prop_u32(cpu, "reg", &reg) || reg != hart)
            continue;
        for (const FdtNode *c = cpu->child; c; c = c->sibling) {
            if (fdt_is_compatible(c, "riscv,cpu-intc"))
                return c->phandle;
        }
    }
    return 0;
}


int hart_of_intc(uint32_t phandle)
{
    const FdtNode *intc = fdt_find_phandle(phandle);
    uint32_t reg;
    if (intc == nullptr || intc->parent == nullptr ||
        !fdt_prop_u32(intc->parent, "reg", &reg))
        return -1;
    return (int)reg;
}
