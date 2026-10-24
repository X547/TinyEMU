/*
 * x86 IOAPIC
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
#include "ioapic.h"

#include "heap.h"
#include "lapic.h"
#include "mmio.h"
#include "vectors.h"

#if defined(__i386__) || defined(__x86_64__)

namespace {

IoApic *sIoApics[4];
int sCount = -1;

}


int IoApic::Count()
{
    if (sCount < 0) {
        sCount = min(acpi_madt_ioapic_count(), (int)ARRAY_SIZE(sIoApics));
        for (int i = 0; i < sCount; i++) {
            void *mem = boot_alloc(sizeof(IoApic), alignof(IoApic));
            sIoApics[i] = new (mem) IoApic(acpi_madt_ioapic(i));
            intc_register(sIoApics[i]);
        }
    }
    return sCount;
}


IoApic *IoApic::At(int index)
{
    return index < Count() ? sIoApics[index] : nullptr;
}


IoApic *IoApic::ForGsi(uint32_t gsi)
{
    for (int i = 0; i < Count(); i++) {
        IoApic *io = sIoApics[i];
        if (gsi >= io->fGsiBase && gsi < io->fGsiBase + io->Pins())
            return io;
    }
    return nullptr;
}


IoApic::IoApic(const MadtIoApic &m):
    fBase(m.address), fGsiBase(m.gsi_base), fId(m.id)
{
}


uint32_t IoApic::Read(uint32_t reg) const
{
    mmio_write32(fBase + IOREGSEL, reg);
    return mmio_read32(fBase + IOWIN);
}


void IoApic::Write(uint32_t reg, uint32_t value) const
{
    mmio_write32(fBase + IOREGSEL, reg);
    mmio_write32(fBase + IOWIN, value);
}


uint32_t IoApic::Pins() const
{
    return ((Read(IOAPIC_REG_VER) >> 16) & 0xff) + 1;
}


uint64_t IoApic::Rte(uint32_t pin) const
{
    return Read(IOAPIC_REG_RTE(pin)) |
           (uint64_t)Read(IOAPIC_REG_RTE(pin) + 1) << 32;
}


void IoApic::SetRte(uint32_t pin, uint64_t value) const
{
    /* masked while the halves disagree */
    Write(IOAPIC_REG_RTE(pin), (uint32_t)RTE_MASKED);
    Write(IOAPIC_REG_RTE(pin) + 1, value >> 32);
    Write(IOAPIC_REG_RTE(pin), (uint32_t)value);
}


void IoApic::Dispatch(void *p)
{
    Slot *s = (Slot *)p;
    if (s->fn)
        s->fn(s->ctx);
    Lapic::Get()->Eoi();
}


bool IoApic::Route(uint32_t line, IrqHandler fn, void *ctx, bool level)
{
    Lapic *lapic = Lapic::Get();
    uint32_t pin = line - fGsiBase;
    if (lapic == nullptr || line < fGsiBase || pin >= Pins() ||
        VEC_IOAPIC + line >= VEC_MSI || pin >= ARRAY_SIZE(fRoutes))
        return false;
    lapic->Enable();
    fRoutes[pin] = {fn, ctx};
    arch_set_irq_handler(VEC_IOAPIC + line, Dispatch, &fRoutes[pin]);
    SetRte(pin, (VEC_IOAPIC + line) | (level ? RTE_LEVEL : 0) |
                RTE_DEST(lapic->Id()));
    return true;
}


void IoApic::Unroute(uint32_t line)
{
    uint32_t pin = line - fGsiBase;
    if (line < fGsiBase || pin >= Pins())
        return;
    SetRte(pin, RTE_MASKED);
    if (pin < ARRAY_SIZE(fRoutes))
        fRoutes[pin] = {};
}


void IoApic::Reset()
{
    /* edge triggered drops a remote IRR that no EOI will now clear */
    for (uint32_t pin = 0; pin < Pins(); pin++)
        SetRte(pin, RTE_MASKED);
    Write(IOAPIC_REG_ID, fId << 24);
    for (auto &r : fRoutes)
        r = {};
}

#else

int IoApic::Count()
{
    return 0;
}

IoApic *IoApic::At(int)
{
    return nullptr;
}

IoApic *IoApic::ForGsi(uint32_t)
{
    return nullptr;
}

#endif
