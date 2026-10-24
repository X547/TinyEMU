/*
 * The PC's 8259 pair
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
#include "i8259.h"

#include "heap.h"
#include "mmio.h"
#include "vectors.h"

#if defined(__i386__) || defined(__x86_64__)

namespace {

I8259 *sPic;
bool sLooked;

}


I8259 *I8259::Get()
{
    if (sLooked)
        return sPic;
    sLooked = true;
    uint16_t master = 0, slave = 0;
    const HwDevice *d = registry_find("PNP0000");
    if (d) {
        const HwRegion *a = hw_region(d, HW_IO, 0), *b = hw_region(d, HW_IO, 1);
        if (a && b) {
            master = a->base;
            slave = b->base;
        }
    } else if (acpi_madt_pcat_compat()) {
        master = 0x20;
        slave = 0xa0;
    }
    if (master == 0)
        return nullptr;
    void *mem = boot_alloc(sizeof(I8259), alignof(I8259));
    sPic = new (mem) I8259(master, slave);
    intc_register(sPic);
    return sPic;
}


I8259::I8259(uint16_t master, uint16_t slave):
    fPort {master, slave}
{
}


void I8259::Init(uint8_t base0, uint8_t base1, bool auto_eoi) const
{
    for (int pic = 0; pic < 2; pic++) {
        outb(CommandPort(pic), 0x11);         /* ICW1: edge, cascade, ICW4 */
        outb(DataPort(pic), pic ? base1 : base0); /* ICW2: vector base */
        outb(DataPort(pic), pic ? 2 : 1 << 2); /* ICW3: the cascade */
        outb(DataPort(pic), auto_eoi ? 0x03 : 0x01); /* ICW4: 8086 mode */
    }
}


uint8_t I8259::Irr(int pic) const
{
    outb(CommandPort(pic), 0x0a); /* OCW3: read IRR */
    return inb(CommandPort(pic));
}


uint8_t I8259::Isr(int pic) const
{
    outb(CommandPort(pic), 0x0b); /* OCW3: read ISR */
    uint8_t v = inb(CommandPort(pic));
    outb(CommandPort(pic), 0x0a);
    return v;
}


uint8_t I8259::Imr(int pic) const
{
    return inb(DataPort(pic));
}


void I8259::SetImr(int pic, uint8_t mask) const
{
    outb(DataPort(pic), mask);
}


uint8_t I8259::Elcr(int pic) const
{
    return inb(ElcrPort(pic));
}


void I8259::SetElcr(int pic, uint8_t value) const
{
    outb(ElcrPort(pic), value);
}


void I8259::Eoi(uint32_t line) const
{
    if (line >= 8)
        outb(CommandPort(1), 0x20);
    outb(CommandPort(0), 0x20);
}


void I8259::SetMasked(uint32_t line, bool masked) const
{
    int pic = line >= 8;
    uint8_t bit = 1 << (line & 7);
    uint8_t imr = Imr(pic);
    SetImr(pic, masked ? imr | bit : imr & ~bit);
}


void I8259::Dispatch(void *p)
{
    Slot *s = (Slot *)p;
    if (s->fn)
        s->fn(s->ctx);
    s->pic->Eoi(s->line);
}


bool I8259::Route(uint32_t line, IrqHandler fn, void *ctx, bool level)
{
    if (line >= 16 || line == 2)
        return false;
    /* from reset the vector base is 0, where the exceptions are */
    if (!fReady)
        Reset();
    fRoutes[line] = {this, line, fn, ctx};
    int pic = line >= 8;
    uint8_t bit = 1 << (line & 7);
    uint8_t elcr = Elcr(pic);
    SetElcr(pic, level ? elcr | bit : elcr & ~bit);
    arch_set_irq_handler((pic ? VEC_PIC_SLAVE : VEC_PIC_MASTER) + (line & 7),
                         Dispatch, &fRoutes[line]);
    if (pic)
        SetMasked(2, false);
    SetMasked(line, false);
    return true;
}


void I8259::Unroute(uint32_t line)
{
    if (line >= 16)
        return;
    SetMasked(line, true);
    fRoutes[line] = {};
}


void I8259::Reset()
{
    Init(VEC_PIC_MASTER, VEC_PIC_SLAVE);
    SetImr(0, 0xff);
    SetImr(1, 0xff);
    SetElcr(0, 0);
    SetElcr(1, 0);
    for (auto &r : fRoutes)
        r = {};
    fReady = true;
}

#else

I8259 *I8259::Get()
{
    return nullptr;
}

#endif
