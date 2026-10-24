/*
 * x86 local APIC
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
#include "lapic.h"

#include "heap.h"
#include "mmio.h"
#include "vectors.h"

#if defined(__i386__) || defined(__x86_64__)

namespace {

Lapic *sLapic;
bool sLooked;
volatile int sSpurious;

void spurious(void *)
{
    /* no EOI for the spurious vector */
    sSpurious = sSpurious + 1;
}

}


uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return (uint64_t)hi << 32 | lo;
}


void wrmsr(uint32_t msr, uint64_t value)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)value),
                     "d"((uint32_t)(value >> 32)));
}


Lapic *Lapic::Get()
{
    if (sLooked)
        return sLapic;
    sLooked = true;
    uint64_t base = acpi_madt_lapic_address();
    if (base == 0)
        return nullptr;
    void *mem = boot_alloc(sizeof(Lapic), alignof(Lapic));
    sLapic = new (mem) Lapic(base);
    intc_register(sLapic);
    return sLapic;
}


Lapic::Lapic(uint64_t base):
    fBase(base)
{
}


uint32_t Lapic::Read(uint32_t reg) const
{
    return mmio_read32(fBase + reg);
}


void Lapic::Write(uint32_t reg, uint32_t value) const
{
    mmio_write32(fBase + reg, value);
}


void Lapic::Enable() const
{
    arch_set_irq_handler(VEC_SPURIOUS, spurious, nullptr);
    Write(LAPIC_SVR, LAPIC_SVR_ENABLE | VEC_SPURIOUS);
}


void Lapic::SendIpi(uint32_t dest, uint32_t low) const
{
    Write(LAPIC_ICR_HIGH, dest << 24);
    Write(LAPIC_ICR_LOW, low);
}


bool Lapic::InService(int vector) const
{
    return (Read(LAPIC_ISR(vector / 32)) >> (vector % 32)) & 1;
}


bool Lapic::Requested(int vector) const
{
    return (Read(LAPIC_IRR(vector / 32)) >> (vector % 32)) & 1;
}


int Lapic::SpuriousCount() const
{
    return sSpurious;
}


void Lapic::Reset()
{
    /* back on, if a test turned it off at its base register */
    uint64_t msr = rdmsr(MSR_APIC_BASE);
    if (!(msr & (1u << 11)))
        wrmsr(MSR_APIC_BASE, msr | (1u << 11));
    Write(LAPIC_TIMER_INITIAL, 0);
    Write(LAPIC_TIMER_DIVIDE, 0);
    Enable();
    Write(LAPIC_TPR, 0);
    Write(LAPIC_LDR, 0);
    Write(LAPIC_DFR, 0xffffffff);
    Write(LAPIC_ICR_HIGH, 0);
    Write(LAPIC_LVT_TIMER, LAPIC_LVT_MASKED);
    Write(LAPIC_LVT_THERMAL, LAPIC_LVT_MASKED);
    Write(LAPIC_LVT_PERF, LAPIC_LVT_MASKED);
    Write(LAPIC_LVT_LINT1, LAPIC_LVT_MASKED);
    Write(LAPIC_LVT_ERROR, LAPIC_LVT_MASKED);
    Write(LAPIC_LVT_CMCI, LAPIC_LVT_MASKED);
    /* the 8259s' INTR comes in on LINT0, as firmware leaves it */
    Write(LAPIC_LVT_LINT0, LAPIC_MODE_EXTINT);

    /* Nothing may stay in service, and nothing requested may reach the
       next test: only an INIT clears the IRR, so what is there is taken
       now and ended. */
    for (int i = 0; i < 256; i++) {
        bool any = false;
        for (int w = 0; w < 8; w++)
            any |= Read(LAPIC_ISR(w)) != 0;
        if (!any)
            break;
        Eoi();
    }
    bool requested = false;
    for (int v = 32; v < 256; v++) {
        if (Requested(v)) {
            arch_set_irq_handler(v, [](void *self) {
                ((Lapic *)self)->Eoi();
            }, (void *)this);
            requested = true;
        }
    }
    if (requested) {
        arch_irq_enable();
        for (int i = 0; i < 1000; i++)
            arch_cpu_relax();
        arch_irq_disable();
        for (int v = 32; v < 256; v++)
            arch_set_irq_handler(v, nullptr, nullptr);
        Enable();
    }
    sSpurious = 0;
}

#else

Lapic *Lapic::Get()
{
    return nullptr;
}

#endif
