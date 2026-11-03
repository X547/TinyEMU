/*
 * Local APIC tests in x2APIC mode
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
#if defined(__i386__) || defined(__x86_64__)

#include "console.h"
#include "i8259.h"
#include "ioapic.h"
#include "lapic.h"
#include "mmio.h"
#include "smp.h"
#include "test.h"
#include "testctl.h"
#include "vectors.h"

/* The registers as MSRs, and what changes with them: the 32 bit IDs and
   destinations, the LDR the ID gives, and the faults. The runner's reset
   puts the APIC back in xAPIC mode. */

namespace {

/* the boot processor's APIC ID, read in xAPIC mode */
uint32_t sXApicId;

Lapic *need_x2apic()
{
    Lapic *l = Lapic::Get();
    if (l == nullptr)
        t_skip("no local APIC");
    if (!cpu_has_x2apic())
        t_skip("no x2APIC mode");
    sXApicId = l->Id();
    Lapic::EnterX2Apic();
    l->Enable();
    return l;
}

volatile int sCount;

void count_and_eoi(void *)
{
    sCount = sCount + 1;
    Lapic::Get()->Eoi();
}

uint32_t sMsr;
uint64_t sValue;

void read_msr(void *)
{
    sValue = rdmsr(sMsr);
}

void write_msr(void *)
{
    wrmsr(sMsr, sValue);
}

bool read_traps(uint32_t msr)
{
    sMsr = msr;
    return t_trap(read_msr);
}

bool write_traps(uint32_t msr, uint64_t value)
{
    sMsr = msr;
    sValue = value;
    return t_trap(write_msr);
}

uint32_t ldr_of(uint32_t id)
{
    return (id >> 4) << 16 | 1u << (id & 15);
}

}


TEST(x2apic, registers)
{
    Lapic *l = need_x2apic();
    uint32_t id = l->Read(LAPIC_ID);
    EXPECT_EQ(id, sXApicId);
    EXPECT_EQ(l->Read(LAPIC_LDR), ldr_of(id));
    SNAP("version", "%#x", l->Read(LAPIC_VERSION));
    /* the page is gone */
    SNAP("page_version", "%#x", mmio_read32(l->Base() + LAPIC_VERSION));
    mmio_write32(l->Base() + LAPIC_TPR, 0x20);
    EXPECT_EQ(l->Read(LAPIC_TPR), 0u);
    /* the ICR is one register, with no delivery status */
    uint64_t icr = (uint64_t)0x12345678 << 32 | VEC_TEST;
    wrmsr(X2APIC_MSR(LAPIC_ICR_LOW), icr);
    EXPECT_EQ(rdmsr(X2APIC_MSR(LAPIC_ICR_LOW)), icr);
    EXPECT_FALSE(l->Requested(VEC_TEST));
}

TEST(x2apic, faults)
{
    Lapic *l = need_x2apic();
    /* no register, or write only */
    static const uint32_t no_read[] = {
        0x800, 0x801, 0x804, 0x807, X2APIC_MSR(LAPIC_APR),
        X2APIC_MSR(LAPIC_EOI), 0x80c, X2APIC_MSR(LAPIC_DFR), 0x829, 0x82e,
        X2APIC_MSR(LAPIC_ICR_HIGH), 0x83a, 0x83d, X2APIC_MSR(LAPIC_SELF_IPI),
        0x840, 0x8ff,
    };
    for (uint32_t msr : no_read) {
        if (!read_traps(msr))
            t_fail(__FILE__, __LINE__, "reading %#x did not trap", msr);
    }
    /* read only */
    static const uint32_t read_only[] = {
        X2APIC_MSR(LAPIC_ID), X2APIC_MSR(LAPIC_VERSION),
        X2APIC_MSR(LAPIC_PPR), X2APIC_MSR(LAPIC_LDR),
        X2APIC_MSR(LAPIC_ISR(0)), X2APIC_MSR(LAPIC_TMR(7)),
        X2APIC_MSR(LAPIC_IRR(3)), X2APIC_MSR(LAPIC_TIMER_CURRENT),
    };
    for (uint32_t msr : read_only) {
        if (!write_traps(msr, 0))
            t_fail(__FILE__, __LINE__, "writing %#x did not trap", msr);
    }
    /* reserved bits */
    EXPECT_TRUE(write_traps(X2APIC_MSR(LAPIC_EOI), 1));
    EXPECT_TRUE(write_traps(X2APIC_MSR(LAPIC_ESR), 1));
    EXPECT_TRUE(write_traps(X2APIC_MSR(LAPIC_SELF_IPI), 0x100));
    EXPECT_TRUE(write_traps(X2APIC_MSR(LAPIC_TPR), 1ull << 32));
    EXPECT_TRUE(write_traps(X2APIC_MSR(LAPIC_ICR_LOW), 1u << 13 | VEC_TEST));
    EXPECT_TRUE(write_traps(X2APIC_MSR(LAPIC_ICR_LOW), 1u << 20 | VEC_TEST));
    EXPECT_FALSE(l->Requested(VEC_TEST));
    /* those that are not checked are dropped */
    EXPECT_FALSE(write_traps(X2APIC_MSR(LAPIC_TPR), 0xffffffff));
    EXPECT_EQ(l->Read(LAPIC_TPR), 0xffu);
    l->Write(LAPIC_TPR, 0);
    /* the ESR is not where these go */
    l->Write(LAPIC_ESR, 0);
    EXPECT_EQ(l->Read(LAPIC_ESR), 0u);
}

TEST(x2apic, base_transitions)
{
    Lapic *l = Lapic::Get();
    if (l == nullptr)
        t_skip("no local APIC");
    if (!cpu_has_x2apic())
        t_skip("no x2APIC mode");
    uint64_t base = rdmsr(MSR_APIC_BASE) & ~(uint64_t)APIC_BASE_X2APIC;
    uint64_t off = base & ~(uint64_t)APIC_BASE_ENABLE;
    /* x2APIC mode while disabled */
    EXPECT_TRUE(write_traps(MSR_APIC_BASE, off | APIC_BASE_X2APIC));
    /* disabled straight to x2APIC mode */
    wrmsr(MSR_APIC_BASE, off);
    EXPECT_TRUE(write_traps(MSR_APIC_BASE, base | APIC_BASE_X2APIC));
    EXPECT_EQ(rdmsr(MSR_APIC_BASE), off);
    /* the x2APIC registers only in x2APIC mode */
    wrmsr(MSR_APIC_BASE, base);
    EXPECT_TRUE(read_traps(X2APIC_MSR(LAPIC_VERSION)));
    EXPECT_TRUE(write_traps(X2APIC_MSR(LAPIC_TPR), 0));
    /* a reserved bit */
    EXPECT_TRUE(write_traps(MSR_APIC_BASE, base | 1u << 9));
    EXPECT_FALSE(write_traps(MSR_APIC_BASE, base | APIC_BASE_X2APIC));
    EXPECT_EQ(rdmsr(MSR_APIC_BASE), base | APIC_BASE_X2APIC);
    EXPECT_FALSE(read_traps(X2APIC_MSR(LAPIC_VERSION)));
    /* and back only by way of disabled */
    EXPECT_TRUE(write_traps(MSR_APIC_BASE, base));
    EXPECT_FALSE(write_traps(MSR_APIC_BASE, off));
    EXPECT_FALSE(write_traps(MSR_APIC_BASE, base));
    EXPECT_EQ(rdmsr(MSR_APIC_BASE), base);
    EXPECT_EQ(l->Read(LAPIC_VERSION) & 0xff, 0x14u);
}

TEST(x2apic, self_ipi)
{
    Lapic *l = need_x2apic();
    sCount = 0;
    arch_set_irq_handler(VEC_TEST, count_and_eoi, nullptr);
    l->Write(LAPIC_SELF_IPI, VEC_TEST);
    EXPECT_TRUE(l->Requested(VEC_TEST));
    EXPECT_FALSE(l->Read(LAPIC_TMR(VEC_TEST / 32)) >> (VEC_TEST % 32) & 1);
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
    EXPECT_FALSE(l->InService(VEC_TEST));
    /* an illegal vector is a send error */
    l->Write(LAPIC_ESR, 0);
    l->Write(LAPIC_SELF_IPI, 5);
    l->Write(LAPIC_ESR, 0);
    EXPECT_EQ(l->Read(LAPIC_ESR), 0x20u);
    EXPECT_EQ(l->Read(LAPIC_IRR(0)), 0u);
}

TEST(x2apic, physical_destination)
{
    Lapic *l = need_x2apic();
    sCount = 0;
    arch_set_irq_handler(VEC_TEST, count_and_eoi, nullptr);
    l->SendIpi(l->Id(), VEC_TEST);
    EXPECT_TRUE(l->Requested(VEC_TEST));
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
    /* 0xff is an ID like any other, and the ID's upper bits count */
    l->SendIpi(0xff, VEC_TEST);
    l->SendIpi(l->Id() | 0x100, VEC_TEST);
    EXPECT_FALSE(l->Requested(VEC_TEST));
    /* broadcast */
    l->SendIpi(0xffffffff, VEC_TEST);
    EXPECT_TRUE(l->Requested(VEC_TEST));
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 1; }));
    arch_irq_disable();
}

TEST(x2apic, logical_cluster)
{
    Lapic *l = need_x2apic();
    uint32_t ldr = l->Read(LAPIC_LDR);
    sCount = 0;
    arch_set_irq_handler(VEC_TEST, count_and_eoi, nullptr);
    /* another cluster, then another member of this one */
    l->SendIpi(ldr + 0x10000, LAPIC_ICR_LOGICAL | VEC_TEST);
    l->SendIpi((ldr & 0xffff0000) | (~ldr & 0xffff),
               LAPIC_ICR_LOGICAL | VEC_TEST);
    EXPECT_FALSE(l->Requested(VEC_TEST));
    /* this cluster with every member */
    l->SendIpi(ldr | 0xffff, LAPIC_ICR_LOGICAL | VEC_TEST);
    EXPECT_TRUE(l->Requested(VEC_TEST));
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
    /* broadcast */
    l->SendIpi(0xffffffff, LAPIC_ICR_LOGICAL | VEC_TEST);
    EXPECT_TRUE(l->Requested(VEC_TEST));
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 1; }));
    arch_irq_disable();
}

TEST(x2apic, timer)
{
    Lapic *l = need_x2apic();
    sCount = 0;
    arch_set_irq_handler(VEC_LAPIC_TIMER, count_and_eoi, nullptr);
    l->Write(LAPIC_TIMER_DIVIDE, 0xb);
    l->Write(LAPIC_LVT_TIMER, VEC_LAPIC_TIMER);
    l->Write(LAPIC_TIMER_INITIAL, 1000000); /* 1 ms */
    EXPECT_LE(l->Read(LAPIC_TIMER_CURRENT), 1000000u);
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }, 200000));
    arch_irq_disable();
    EXPECT_EQ(l->Read(LAPIC_TIMER_CURRENT), 0u);
}

#ifdef __x86_64__
TEST(x2apic, cr8)
{
    Lapic *l = need_x2apic();
    __asm__ volatile("mov %0, %%cr8" : : "r"(5ul));
    EXPECT_EQ(l->Read(LAPIC_TPR), 0x50u);
    l->Write(LAPIC_TPR, 0x30);
    unsigned long cr8;
    __asm__ volatile("mov %%cr8, %0" : "=r"(cr8));
    EXPECT_EQ(cr8, 3ul);
    l->Write(LAPIC_TPR, 0);
}
#endif

/* An IOAPIC message names its destination in 8 bits. */
TEST(x2apic, from_ioapic)
{
    Lapic *l = need_x2apic();
    IoApic *io = IoApic::At(0);
    if (io == nullptr)
        t_skip("no IOAPIC");
    t_require_tc_irqs(1);
    IntController *c;
    uint32_t gsi;
    if (!intc_resolve(test_control_device(), 0, &c, &gsi) || c != io)
        t_skip("test-control line 0 is not on the IOAPIC");
    uint32_t pin = gsi - io->GsiBase();
    sCount = 0;
    arch_set_irq_handler(VEC_TEST, [](void *) {
        sCount = sCount + 1;
        tc_irq_lower(0);
        Lapic::Get()->Eoi();
    }, nullptr);
    /* to this processor, and to all */
    const uint32_t dests[] = {l->Id(), 0xff};
    for (int i = 0; i < 2; i++) {
        io->SetRte(pin, VEC_TEST | RTE_DEST(dests[i]));
        arch_irq_enable();
        tc_irq_raise(0);
        if (!t_wait_until([i] { return sCount > i; })) {
            t_fail(__FILE__, __LINE__, "nothing for destination %#x",
                   dests[i]);
            tc_irq_lower(0);
            sCount = i + 1;
        }
        arch_irq_disable();
    }
    io->SetRte(pin, RTE_MASKED);
}

/* The 8259s' INTR through LINT0, which masking LVT0 holds back. */
TEST(x2apic, extint)
{
    Lapic *l = need_x2apic();
    I8259 *pic = I8259::Get();
    if (pic == nullptr)
        t_skip("no 8259s");
    const HwDevice *tc = test_control_device();
    int index = -1;
    for (int i = 0; tc && i < tc_irq_count() && i < tc->nirqs; i++) {
        if (tc->irqs[i].line < 8 && tc->irqs[i].line != 2) {
            index = i;
            break;
        }
    }
    if (index < 0)
        t_skip("needs a test-control line on the first 8259");
    static int sIndex;
    sIndex = index;
    sCount = 0;
    pic->Reset();
    ASSERT_TRUE(pic->Route(tc->irqs[index].line, [](void *) {
        sCount = sCount + 1;
        tc_irq_lower(sIndex);
    }, nullptr, false));
    l->Write(LAPIC_LVT_LINT0, LAPIC_MODE_EXTINT | LAPIC_LVT_MASKED);
    arch_irq_enable();
    tc_irq_raise(index);
    t_delay_us(2000);
    arch_irq_disable();
    EXPECT_EQ(sCount, 0);
    l->Write(LAPIC_LVT_LINT0, LAPIC_MODE_EXTINT);
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
    pic->Unroute(tc->irqs[index].line);
}

namespace {

volatile int sApCount;
volatile int sApReady;

/* On the other processor: x2APIC mode until an IPI came or a while went
   by, then back to xAPIC mode for the tests after. */
void ap_wait_ipi(void *)
{
    Lapic *l = Lapic::Get();
    Lapic::EnterX2Apic();
    l->Enable();
    l->Write(LAPIC_TPR, 0);
    __atomic_store_n(&sApReady, 1, __ATOMIC_RELEASE);
    arch_irq_enable();
    uint64_t end = t_now_us() + 200000;
    while (sApCount == 0 && t_now_us() < end)
        arch_cpu_relax();
    arch_irq_disable();
    Lapic::LeaveX2Apic();
    l->Enable();
}

}

TEST(x2apic, ipi_to_other)
{
    Lapic *l = need_x2apic();
    int other = t_require_other_cpu();
    sApCount = 0;
    sApReady = 0;
    arch_set_irq_handler(VEC_IPI, [](void *) {
        sApCount = sApCount + 1;
        Lapic::Get()->Eoi();
    }, nullptr);
    if (!smp_post(other, ap_wait_ipi, nullptr))
        t_skip("the other processor is busy");
    t_wait_until([] {
        return __atomic_load_n(&sApReady, __ATOMIC_ACQUIRE) != 0;
    });
    /* by its LDR, which its ID gives */
    l->SendIpi(ldr_of(arch_cpu_hw_id(other)), LAPIC_ICR_LOGICAL | VEC_IPI);
    EXPECT_TRUE(smp_wait(other));
    EXPECT_EQ(sApCount, 1);
    EXPECT_FALSE(l->Requested(VEC_IPI));
}

#endif
