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
#pragma once

#include "intc.h"

/* register offsets, by the Intel SDM */
#define LAPIC_ID            0x020
#define LAPIC_VERSION       0x030
#define LAPIC_TPR           0x080
#define LAPIC_APR           0x090
#define LAPIC_PPR           0x0a0
#define LAPIC_EOI           0x0b0
#define LAPIC_LDR           0x0d0
#define LAPIC_DFR           0x0e0
#define LAPIC_SVR           0x0f0
#define LAPIC_ISR(i)        (0x100 + 0x10 * (i))
#define LAPIC_TMR(i)        (0x180 + 0x10 * (i))
#define LAPIC_IRR(i)        (0x200 + 0x10 * (i))
#define LAPIC_ESR           0x280
#define LAPIC_LVT_CMCI      0x2f0
#define LAPIC_ICR_LOW       0x300
#define LAPIC_ICR_HIGH      0x310
#define LAPIC_LVT_TIMER     0x320
#define LAPIC_LVT_THERMAL   0x330
#define LAPIC_LVT_PERF      0x340
#define LAPIC_LVT_LINT0     0x350
#define LAPIC_LVT_LINT1     0x360
#define LAPIC_LVT_ERROR     0x370
#define LAPIC_TIMER_INITIAL 0x380
#define LAPIC_TIMER_CURRENT 0x390
#define LAPIC_TIMER_DIVIDE  0x3e0

#define LAPIC_SVR_ENABLE    (1u << 8)
#define LAPIC_LVT_MASKED    (1u << 16)
#define LAPIC_LVT_PERIODIC  (1u << 17)
#define LAPIC_LVT_LEVEL     (1u << 15)
#define LAPIC_MODE_FIXED    (0u << 8)
#define LAPIC_MODE_LOWEST   (1u << 8)
#define LAPIC_MODE_INIT     (5u << 8)
#define LAPIC_MODE_STARTUP  (6u << 8)
#define LAPIC_MODE_EXTINT   (7u << 8)
#define LAPIC_ICR_LOGICAL   (1u << 11)
#define LAPIC_ICR_ASSERT    (1u << 14)
#define LAPIC_ICR_SELF      (1u << 18)
#define LAPIC_ICR_ALL       (2u << 18)
#define LAPIC_ICR_OTHERS    (3u << 18)

#define MSR_APIC_BASE 0x1b

uint64_t rdmsr(uint32_t msr);
void wrmsr(uint32_t msr, uint64_t value);

/* The boot processor's xAPIC, where the MADT says it is. */
class Lapic final : public IntController {
public:
    static Lapic *Get();

    uint64_t Base() const {return fBase;}
    uint32_t Read(uint32_t reg) const;
    void Write(uint32_t reg, uint32_t value) const;
    uint32_t Id() const {return Read(LAPIC_ID) >> 24;}
    /* software enabled, with the spurious vector handled */
    void Enable() const;
    void Eoi() const {Write(LAPIC_EOI, 0);}
    /* an IPI by the ICR */
    void SendIpi(uint32_t dest, uint32_t low) const;
    bool InService(int vector) const;
    bool Requested(int vector) const;
    int SpuriousCount() const;

    /* IntController: no inputs of its own, only the reset between tests */
    const char *Kind() const override {return "lapic";}
    bool Route(uint32_t, IrqHandler, void *, bool) override {return false;}
    void Unroute(uint32_t) override {}
    void Reset() override;

private:
    Lapic(uint64_t base);

    uint64_t fBase;
};
