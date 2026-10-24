/*
 * Local APIC tests
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

#include "format.h"
#include "lapic.h"
#include "test.h"
#include "vectors.h"

/* The boot processor's own APIC: its registers, IPIs it sends itself, and
   its timer. */

namespace {

Lapic *need_lapic()
{
    Lapic *l = Lapic::Get();
    if (l == nullptr)
        t_skip("no local APIC");
    l->Enable();
    return l;
}

volatile int sCount;
volatile int sOrder[8];
volatile uint32_t sSeen;

void count_and_eoi(void *)
{
    sCount = sCount + 1;
    Lapic::Get()->Eoi();
}

uint64_t sWrmsrValue;

void write_apic_base(void *)
{
    wrmsr(MSR_APIC_BASE, sWrmsrValue);
}

}


FRESH_TEST(lapic, reset_state)
{
    Lapic *l = Lapic::Get();
    if (l == nullptr)
        t_skip("no local APIC");
    static const struct {
        const char *name;
        uint32_t reg;
    } regs[] = {
        {"id", LAPIC_ID}, {"version", LAPIC_VERSION}, {"tpr", LAPIC_TPR},
        {"ppr", LAPIC_PPR}, {"ldr", LAPIC_LDR}, {"dfr", LAPIC_DFR},
        {"svr", LAPIC_SVR}, {"esr", LAPIC_ESR}, {"icr_low", LAPIC_ICR_LOW},
        {"icr_high", LAPIC_ICR_HIGH}, {"lvt_cmci", LAPIC_LVT_CMCI},
        {"lvt_timer", LAPIC_LVT_TIMER}, {"lvt_thermal", LAPIC_LVT_THERMAL},
        {"lvt_perf", LAPIC_LVT_PERF}, {"lvt_lint0", LAPIC_LVT_LINT0},
        {"lvt_lint1", LAPIC_LVT_LINT1}, {"lvt_error", LAPIC_LVT_ERROR},
        {"timer_initial", LAPIC_TIMER_INITIAL},
        {"timer_current", LAPIC_TIMER_CURRENT},
        {"timer_divide", LAPIC_TIMER_DIVIDE},
    };
    for (const auto &r : regs)
        SNAP(r.name, "%#x", l->Read(r.reg));
    SNAP("apic_base_msr", "%#llx", T_FMT_VAL(rdmsr(MSR_APIC_BASE)));
    SNAP("madt_address", "%#llx", T_FMT_VAL(l->Base()));
}

TEST(lapic, read_only)
{
    Lapic *l = need_lapic();
    uint32_t id = l->Read(LAPIC_ID), ver = l->Read(LAPIC_VERSION);
    l->Write(LAPIC_ID, 0x0f000000);
    l->Write(LAPIC_VERSION, 0);
    l->Write(LAPIC_PPR, 0xff);
    EXPECT_EQ(l->Read(LAPIC_ID), id);
    EXPECT_EQ(l->Read(LAPIC_VERSION), ver);
    EXPECT_EQ(l->Read(LAPIC_PPR), 0u);
    l->Write(LAPIC_ESR, 0);
    SNAP("esr", "%#x", l->Read(LAPIC_ESR));
    SNAP("apr", "%#x", l->Read(LAPIC_APR));
    /* a reserved register */
    SNAP("reserved_40", "%#x", l->Read(0x40));
}

TEST(lapic, register_masks)
{
    Lapic *l = need_lapic();
    char buf[200];
    size_t n = 0;
    static const uint32_t regs[] = {
        LAPIC_TPR, LAPIC_LDR, LAPIC_DFR, LAPIC_ICR_HIGH, LAPIC_LVT_CMCI,
        LAPIC_LVT_TIMER, LAPIC_LVT_THERMAL, LAPIC_LVT_PERF, LAPIC_LVT_LINT0,
        LAPIC_LVT_LINT1, LAPIC_LVT_ERROR, LAPIC_TIMER_DIVIDE,
    };
    for (uint32_t reg : regs) {
        l->Write(reg, 0xffffffff);
        n += snprintf(buf + n, sizeof(buf) - n, "%s%x", n ? " " : "",
                      l->Read(reg));
    }
    SNAP("all_ones", "%s", buf);
}

TEST(lapic, svr_disable_masks_lvt)
{
    Lapic *l = need_lapic();
    l->Write(LAPIC_SVR, 0xffffffff);
    SNAP("svr_all_ones", "%#x", l->Read(LAPIC_SVR));
    l->Write(LAPIC_LVT_TIMER, VEC_LAPIC_TIMER);
    l->Write(LAPIC_SVR, VEC_SPURIOUS);
    /* software disabled: every LVT masked, and stays so */
    EXPECT_TRUE(l->Read(LAPIC_LVT_TIMER) & LAPIC_LVT_MASKED);
    EXPECT_TRUE(l->Read(LAPIC_LVT_LINT0) & LAPIC_LVT_MASKED);
    l->Write(LAPIC_LVT_TIMER, VEC_LAPIC_TIMER);
    EXPECT_TRUE(l->Read(LAPIC_LVT_TIMER) & LAPIC_LVT_MASKED);
}

TEST(lapic, self_ipi)
{
    Lapic *l = need_lapic();
    sCount = 0;
    sSeen = 0;
    arch_set_irq_handler(VEC_TEST, [](void *) {
        sSeen = Lapic::Get()->InService(VEC_TEST);
        sCount = sCount + 1;
        Lapic::Get()->Eoi();
    }, nullptr);
    l->SendIpi(0, LAPIC_ICR_SELF | VEC_TEST);
    EXPECT_TRUE(l->Requested(VEC_TEST));
    SNAP("icr_low", "%#x", l->Read(LAPIC_ICR_LOW));
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
    EXPECT_EQ(sSeen, 1u);
    EXPECT_FALSE(l->InService(VEC_TEST));
    EXPECT_FALSE(l->Requested(VEC_TEST));
}

TEST(lapic, physical_destination)
{
    Lapic *l = need_lapic();
    sCount = 0;
    arch_set_irq_handler(VEC_TEST, count_and_eoi, nullptr);
    l->SendIpi(l->Id(), VEC_TEST);
    EXPECT_TRUE(l->Requested(VEC_TEST));
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
    /* an ID nobody has */
    l->SendIpi(0x7e, VEC_TEST);
    EXPECT_FALSE(l->Requested(VEC_TEST));
    /* broadcast */
    l->SendIpi(0xff, VEC_TEST);
    EXPECT_TRUE(l->Requested(VEC_TEST));
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 1; }));
    arch_irq_disable();
}

TEST(lapic, logical_flat)
{
    Lapic *l = need_lapic();
    l->Write(LAPIC_DFR, 0xffffffff);
    l->Write(LAPIC_LDR, 0x02000000);
    l->SendIpi(0x01, LAPIC_ICR_LOGICAL | VEC_TEST);
    EXPECT_FALSE(l->Requested(VEC_TEST));
    l->SendIpi(0x03, LAPIC_ICR_LOGICAL | VEC_TEST);
    EXPECT_TRUE(l->Requested(VEC_TEST));
    sCount = 0;
    arch_set_irq_handler(VEC_TEST, count_and_eoi, nullptr);
    arch_irq_enable();
    t_wait_until([] { return sCount > 0; });
    arch_irq_disable();
}

TEST(lapic, logical_cluster)
{
    Lapic *l = need_lapic();
    l->Write(LAPIC_DFR, 0x0fffffff);
    l->Write(LAPIC_LDR, 0x32000000); /* cluster 3, member bit 1 */
    l->SendIpi(0x22, LAPIC_ICR_LOGICAL | VEC_TEST);
    EXPECT_FALSE(l->Requested(VEC_TEST));
    l->SendIpi(0x31, LAPIC_ICR_LOGICAL | VEC_TEST);
    EXPECT_FALSE(l->Requested(VEC_TEST));
    l->SendIpi(0x32, LAPIC_ICR_LOGICAL | VEC_TEST);
    EXPECT_TRUE(l->Requested(VEC_TEST));
    sCount = 0;
    arch_set_irq_handler(VEC_TEST, count_and_eoi, nullptr);
    arch_irq_enable();
    t_wait_until([] { return sCount > 0; });
    arch_irq_disable();
    /* the broadcast cluster */
    l->SendIpi(0xf2, LAPIC_ICR_LOGICAL | VEC_TEST);
    EXPECT_TRUE(l->Requested(VEC_TEST));
    arch_irq_enable();
    t_wait_until([] { return sCount > 1; });
    arch_irq_disable();
}

TEST(lapic, shorthands)
{
    Lapic *l = need_lapic();
    sCount = 0;
    arch_set_irq_handler(VEC_TEST, count_and_eoi, nullptr);
    /* all but self: not this one */
    l->SendIpi(0, LAPIC_ICR_OTHERS | VEC_TEST);
    EXPECT_FALSE(l->Requested(VEC_TEST));
    l->SendIpi(0, LAPIC_ICR_ALL | VEC_TEST);
    EXPECT_TRUE(l->Requested(VEC_TEST));
    arch_irq_enable();
    t_wait_until([] { return sCount > 0; });
    arch_irq_disable();
}

TEST(lapic, illegal_vector)
{
    Lapic *l = need_lapic();
    l->SendIpi(0, LAPIC_ICR_SELF | 5);
    EXPECT_EQ(l->Read(LAPIC_IRR(0)), 0u);
    SNAP("esr", "%#x", l->Read(LAPIC_ESR));
}

TEST(lapic, unmodelled_modes)
{
    Lapic *l = need_lapic();
    /* SMI, a reserved mode and NMI: accepted, and nothing happens */
    static const uint32_t modes[] = {2u << 8, 3u << 8, 4u << 8};
    for (uint32_t mode : modes)
        l->SendIpi(0, LAPIC_ICR_SELF | mode | VEC_TEST);
    EXPECT_FALSE(l->Requested(VEC_TEST));
}

TEST(lapic, priority_order)
{
    Lapic *l = need_lapic();
    sCount = 0;
    static const int vectors[] = {VEC_TEST, VEC_TEST + 0x10};
    for (int v : vectors) {
        arch_set_irq_handler(v, [](void *p) {
            sOrder[sCount] = (int)(uintptr_t)p;
            sCount = sCount + 1;
            Lapic::Get()->Eoi();
        }, (void *)(uintptr_t)v);
    }
    l->SendIpi(0, LAPIC_ICR_SELF | VEC_TEST);
    l->SendIpi(0, LAPIC_ICR_SELF | (VEC_TEST + 0x10));
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount >= 2; }));
    arch_irq_disable();
    EXPECT_EQ(sOrder[0], VEC_TEST + 0x10);
    EXPECT_EQ(sOrder[1], VEC_TEST);
}

TEST(lapic, tpr_blocks)
{
    Lapic *l = need_lapic();
    sCount = 0;
    arch_set_irq_handler(VEC_TEST, count_and_eoi, nullptr);
    l->Write(LAPIC_TPR, VEC_TEST);
    EXPECT_EQ(l->Read(LAPIC_PPR), (uint32_t)VEC_TEST);
    l->SendIpi(0, LAPIC_ICR_SELF | VEC_TEST);
    arch_irq_enable();
    t_delay_us(500);
    EXPECT_EQ(sCount, 0);
    /* the class above the TPR's gets through */
    l->Write(LAPIC_TPR, VEC_TEST - 0x10);
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
}

TEST(lapic, ppr_follows_isr)
{
    Lapic *l = need_lapic();
    sSeen = 0;
    arch_set_irq_handler(VEC_TEST + 0x10, [](void *) {
        sSeen = Lapic::Get()->Read(LAPIC_PPR);
        Lapic::Get()->Eoi();
    }, nullptr);
    l->Write(LAPIC_TPR, 0x10);
    l->SendIpi(0, LAPIC_ICR_SELF | (VEC_TEST + 0x10));
    arch_irq_enable();
    t_wait_until([] { return sSeen != 0; });
    arch_irq_disable();
    /* the class in service, above the TPR's */
    EXPECT_EQ(sSeen, (uint32_t)((VEC_TEST + 0x10) & 0xf0));
}

TEST(lapic, nesting)
{
    Lapic *l = need_lapic();
    sCount = 0;
    arch_set_irq_handler(VEC_TEST, [](void *) {
        sOrder[sCount] = 1;
        sCount = sCount + 1;
        /* a higher class preempts while this one is in service */
        Lapic::Get()->SendIpi(0, LAPIC_ICR_SELF | (VEC_TEST + 0x10));
        arch_irq_enable();
        for (int i = 0; i < 100000 && sCount < 2; i++)
            arch_cpu_relax();
        arch_irq_disable();
        sOrder[sCount] = 3;
        sCount = sCount + 1;
        Lapic::Get()->Eoi();
    }, nullptr);
    arch_set_irq_handler(VEC_TEST + 0x10, [](void *) {
        sOrder[sCount] = 2;
        sCount = sCount + 1;
        Lapic::Get()->Eoi();
    }, nullptr);
    l->SendIpi(0, LAPIC_ICR_SELF | VEC_TEST);
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount >= 3; }));
    arch_irq_disable();
    EXPECT_EQ(sOrder[0], 1);
    EXPECT_EQ(sOrder[1], 2);
    EXPECT_EQ(sOrder[2], 3);
}

TEST(lapic, eoi_highest_in_service)
{
    Lapic *l = need_lapic();
    sCount = 0;
    arch_set_irq_handler(VEC_TEST, [](void *) {
        sCount = sCount + 1;
    }, nullptr);
    arch_set_irq_handler(VEC_TEST + 0x10, [](void *) {
        sCount = sCount + 1;
    }, nullptr);
    l->SendIpi(0, LAPIC_ICR_SELF | VEC_TEST);
    arch_irq_enable();
    t_wait_until([] { return sCount > 0; });
    l->SendIpi(0, LAPIC_ICR_SELF | (VEC_TEST + 0x10));
    t_wait_until([] { return sCount > 1; });
    arch_irq_disable();
    EXPECT_TRUE(l->InService(VEC_TEST));
    EXPECT_TRUE(l->InService(VEC_TEST + 0x10));
    l->Eoi();
    EXPECT_TRUE(l->InService(VEC_TEST));
    EXPECT_FALSE(l->InService(VEC_TEST + 0x10));
    l->Eoi();
    EXPECT_FALSE(l->InService(VEC_TEST));
    /* an EOI with nothing in service does nothing */
    l->Eoi();
}

TEST(lapic, lowest_priority)
{
    Lapic *l = need_lapic();
    sCount = 0;
    arch_set_irq_handler(VEC_TEST, count_and_eoi, nullptr);
    l->SendIpi(0xff, LAPIC_MODE_LOWEST | VEC_TEST);
    arch_irq_enable();
    t_delay_us(500);
    arch_irq_disable();
    /* the first of equal priorities, or another processor */
    SNAP("taken_here", "%d", sCount);
}

TEST(lapic, timer_one_shot)
{
    Lapic *l = need_lapic();
    sCount = 0;
    arch_set_irq_handler(VEC_LAPIC_TIMER, count_and_eoi, nullptr);
    l->Write(LAPIC_TIMER_DIVIDE, 0xb); /* by 1 */
    l->Write(LAPIC_LVT_TIMER, VEC_LAPIC_TIMER);
    l->Write(LAPIC_TIMER_INITIAL, 2000000); /* 2 ms at 1 GHz */
    uint32_t c1 = l->Read(LAPIC_TIMER_CURRENT);
    uint32_t c2 = l->Read(LAPIC_TIMER_CURRENT);
    EXPECT_LE(c2, c1);
    EXPECT_LE(c1, 2000000u);
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }, 200000));
    t_delay_us(5000);
    arch_irq_disable();
    EXPECT_EQ(sCount, 1);
    EXPECT_EQ(l->Read(LAPIC_TIMER_CURRENT), 0u);
    SNAP("initial_after", "%#x", l->Read(LAPIC_TIMER_INITIAL));
}

TEST(lapic, timer_periodic)
{
    Lapic *l = need_lapic();
    sCount = 0;
    arch_set_irq_handler(VEC_LAPIC_TIMER, count_and_eoi, nullptr);
    l->Write(LAPIC_TIMER_DIVIDE, 0xb);
    l->Write(LAPIC_LVT_TIMER, VEC_LAPIC_TIMER | LAPIC_LVT_PERIODIC);
    l->Write(LAPIC_TIMER_INITIAL, 1000000); /* 1 ms */
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount >= 3; }, 500000));
    arch_irq_disable();
    /* reloaded every period: never 0 while it runs */
    uint32_t current = l->Read(LAPIC_TIMER_CURRENT);
    EXPECT_GT(current, 0u);
    EXPECT_LE(current, 1000000u);
    l->Write(LAPIC_TIMER_INITIAL, 0);
    EXPECT_EQ(l->Read(LAPIC_TIMER_CURRENT), 0u);
}

TEST(lapic, timer_masked)
{
    Lapic *l = need_lapic();
    sCount = 0;
    arch_set_irq_handler(VEC_LAPIC_TIMER, count_and_eoi, nullptr);
    l->Write(LAPIC_TIMER_DIVIDE, 0xb);
    l->Write(LAPIC_LVT_TIMER, VEC_LAPIC_TIMER | LAPIC_LVT_MASKED);
    l->Write(LAPIC_TIMER_INITIAL, 1000000);
    arch_irq_enable();
    t_delay_us(5000);
    arch_irq_disable();
    EXPECT_EQ(sCount, 0);
    /* it counted all the same */
    EXPECT_EQ(l->Read(LAPIC_TIMER_CURRENT), 0u);
}

TEST(lapic, timer_divide)
{
    Lapic *l = need_lapic();
    char buf[128];
    size_t n = 0;
    static const uint32_t divides[] = {0x0, 0x1, 0x2, 0x3, 0x8, 0x9, 0xa, 0xb};
    for (uint32_t d : divides) {
        l->Write(LAPIC_TIMER_DIVIDE, d);
        l->Write(LAPIC_TIMER_INITIAL, 0xffffffff);
        uint64_t start = arch_ticks();
        uint32_t c1 = l->Read(LAPIC_TIMER_CURRENT);
        while (arch_ticks() - start < arch_tick_hz() / 1000)
            arch_cpu_relax();
        uint32_t c2 = l->Read(LAPIC_TIMER_CURRENT);
        /* counts per millisecond, to the nearest divisor of 1 GHz: the
           bands meet halfway between powers of two */
        uint32_t per_ms = c1 - c2;
        n += snprintf(buf + n, sizeof(buf) - n, "%s%x:%s", n ? " " : "", d,
                      per_ms > 750000 ? "1G" : per_ms > 375000 ? "1/2" :
                      per_ms > 187500 ? "1/4" : per_ms > 93750 ? "1/8" :
                      per_ms > 46875 ? "1/16" : per_ms > 23437 ? "1/32" :
                      per_ms > 11718 ? "1/64" : "1/128");
    }
    l->Write(LAPIC_TIMER_INITIAL, 0);
    SNAP("rates", "%s", buf);
}

TEST(lapic, apic_base_msr)
{
    Lapic *l = need_lapic();
    uint64_t base = rdmsr(MSR_APIC_BASE);
    SNAP("value", "%#llx", T_FMT_VAL(base));
    /* moving the page, or x2APIC mode, is refused */
    sWrmsrValue = (base & ~0xfffull) + 0x100000;
    TrapInfo info;
    bool moved = t_trap(write_apic_base, nullptr, &info);
    SNAP("move", "%s", moved ? "trap" : "ok");
    sWrmsrValue = base | (1u << 10);
    SNAP("x2apic", "%s", t_trap(write_apic_base) ? "trap" : "ok");
    /* hardware disabled: the 8259s' INTR passes straight through */
    sWrmsrValue = base & ~(1ull << 11);
    EXPECT_FALSE(t_trap(write_apic_base));
    EXPECT_FALSE(rdmsr(MSR_APIC_BASE) & (1u << 11));
    wrmsr(MSR_APIC_BASE, base);
    l->Enable();
}

#ifdef __x86_64__
TEST(lapic, cr8)
{
    Lapic *l = need_lapic();
    __asm__ volatile("mov %0, %%cr8" : : "r"(5ul));
    EXPECT_EQ(l->Read(LAPIC_TPR), 0x50u);
    l->Write(LAPIC_TPR, 0x30);
    unsigned long cr8;
    __asm__ volatile("mov %%cr8, %0" : "=r"(cr8));
    EXPECT_EQ(cr8, 3ul);
    l->Write(LAPIC_TPR, 0);
}
#endif

#endif
