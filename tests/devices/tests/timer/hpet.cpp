/*
 * HPET tests
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

#include "acpi.h"
#include "intc.h"
#include "ioapic.h"
#include "mmio.h"
#include "test.h"

/* The HPET where its ACPI table says, by the IA-PC HPET specification. */

#define HPET_CAP        0x000
#define HPET_CONFIG     0x010
#define HPET_STATUS     0x020
#define HPET_COUNTER    0x0f0
#define HPET_TCONF(n)   (0x100 + 0x20 * (n))
#define HPET_TCMP(n)    (0x108 + 0x20 * (n))

#define CONF_ENABLE     0x1
#define CONF_LEGACY     0x2
#define TN_LEVEL        (1u << 1)
#define TN_INT_ENABLE   (1u << 2)
#define TN_PERIODIC     (1u << 3)
#define TN_SET_VALUE    (1u << 6)
#define TN_32BIT        (1u << 8)
#define TN_ROUTE(r)     ((uint32_t)(r) << 9)

namespace {

uint64_t sBase;

uint32_t rd(uint32_t off)
{
    return mmio_read32(sBase + off);
}

void wr(uint32_t off, uint32_t v)
{
    mmio_write32(sBase + off, v);
}

uint64_t counter()
{
    uint32_t lo = rd(HPET_COUNTER);
    uint32_t hi = rd(HPET_COUNTER + 4);
    return (uint64_t)hi << 32 | lo;
}

void set_counter(uint64_t v)
{
    wr(HPET_COUNTER, (uint32_t)v);
    wr(HPET_COUNTER + 4, v >> 32);
}

void set_comparator(int n, uint64_t v)
{
    wr(HPET_TCMP(n), (uint32_t)v);
    wr(HPET_TCMP(n) + 4, v >> 32);
}

uint64_t comparator(int n)
{
    return rd(HPET_TCMP(n)) | (uint64_t)rd(HPET_TCMP(n) + 4) << 32;
}

int timers()
{
    return ((rd(HPET_CAP) >> 8) & 0x1f) + 1;
}

/* ticks per microsecond, from the period in femtoseconds */
uint64_t ticks_per_us()
{
    uint32_t fs = rd(HPET_CAP + 4);
    return fs ? 1000000000ull / fs : 0;
}

void need_hpet()
{
    const uint8_t *t = acpi_table("HPET");
    if (t == nullptr)
        t_skip("no HPET table");
    sBase = le64(t + 44);
    t_on_cleanup([] {
        wr(HPET_CONFIG, 0);
        for (int n = 0; n < timers(); n++) {
            wr(HPET_TCONF(n), 0);
            set_comparator(n, ~0ull);
        }
        wr(HPET_STATUS, 0xffffffff);
        set_counter(0);
    });
}

/* The first IOAPIC input timer 'n' may be routed to that has an IOAPIC
   behind it, or -1. */
int route_for(int n)
{
    uint32_t mask = rd(HPET_TCONF(n) + 4);
    for (int pin = 0; pin < 32; pin++) {
        if ((mask >> pin) & 1 && IoApic::ForGsi(pin))
            return pin;
    }
    return -1;
}

volatile int sCount;

}


FRESH_TEST(hpet, reset_state)
{
    need_hpet();
    SNAP("cap", "%#x %#x", rd(HPET_CAP), rd(HPET_CAP + 4));
    SNAP("config", "%#x", rd(HPET_CONFIG));
    SNAP("status", "%#x", rd(HPET_STATUS));
    SNAP("counter", "%#llx", T_FMT_VAL(counter()));
    for (int n = 0; n < timers(); n++) {
        SNAP(n == 0 ? "t0" : n == 1 ? "t1" : "t2", "%#x %#x %#llx",
             rd(HPET_TCONF(n)), rd(HPET_TCONF(n) + 4),
             T_FMT_VAL(comparator(n)));
    }
}

TEST(hpet, table)
{
    need_hpet();
    const uint8_t *t = acpi_table("HPET");
    /* the event timer block ID repeats the capabilities' low half */
    EXPECT_EQ(le32(t + 36), rd(HPET_CAP));
    SNAP("number", "%u", t[52]);
    SNAP("min_tick", "%u", le16(t + 53));
    SNAP("protection", "%#x", t[55]);
    const HwDevice *d = registry_find("PNP0103");
    EXPECT_TRUE(d != nullptr && d->nregions > 0 &&
                d->regions[0].base == sBase);
}

TEST(hpet, counter_runs_when_enabled)
{
    need_hpet();
    wr(HPET_CONFIG, 0);
    set_counter(0x1000);
    t_delay_us(1000);
    EXPECT_EQ(counter(), 0x1000ull);
    wr(HPET_CONFIG, CONF_ENABLE);
    uint64_t a = counter();
    t_delay_us(10000);
    uint64_t b = counter();
    uint64_t expect = 10000 * ticks_per_us();
    EXPECT_GT(b - a, expect * 8 / 10);
    EXPECT_LT(b - a, expect * 3);
    /* halting keeps the count */
    wr(HPET_CONFIG, 0);
    uint64_t c = counter();
    t_delay_us(1000);
    EXPECT_EQ(counter(), c);
}

TEST(hpet, counter_halves)
{
    need_hpet();
    wr(HPET_CONFIG, 0);
    set_counter(0x00000001fffffff0ull);
    EXPECT_EQ(counter(), 0x00000001fffffff0ull);
    wr(HPET_CONFIG, CONF_ENABLE);
    /* both halves of one read agree, across the carry */
    bool consistent = true;
    for (int i = 0; i < 1000; i++) {
        uint64_t v = counter();
        consistent &= v >= 0x00000001fffffff0ull;
    }
    EXPECT_TRUE(consistent);
}

TEST(hpet, register_masks)
{
    need_hpet();
    wr(HPET_CONFIG, 0xffffffff);
    SNAP("config", "%#x", rd(HPET_CONFIG));
    wr(HPET_CONFIG, 0);
    wr(HPET_TCONF(0), 0xffffffff);
    SNAP("t0_config", "%#x", rd(HPET_TCONF(0)));
    wr(HPET_CAP, 0);
    SNAP("cap_written", "%#x", rd(HPET_CAP));
    SNAP("past_last_timer", "%#x", rd(HPET_TCONF(timers())));
    SNAP("reserved_30", "%#x", rd(0x30));
    /* the FSB route register, for timers that can send messages */
    wr(HPET_TCONF(0) + 0x10, 0xffffffff);
    SNAP("t0_fsb_route", "%#x", rd(HPET_TCONF(0) + 0x10));
}

TEST(hpet, one_shot)
{
    need_hpet();
    int n = 0, pin = route_for(n);
    if (pin < 0)
        t_skip("timer 0 reaches no IOAPIC input");
    sCount = 0;
    ASSERT_TRUE(IoApic::ForGsi(pin)->Route(pin, [](void *) {
        sCount = sCount + 1;
    }, nullptr, false));
    wr(HPET_TCONF(n), TN_INT_ENABLE | TN_ROUTE(pin));
    wr(HPET_CONFIG, CONF_ENABLE);
    set_comparator(n, counter() + 1000 * ticks_per_us());
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }, 200000));
    t_delay_us(5000);
    arch_irq_disable();
    EXPECT_EQ(sCount, 1);
}

TEST(hpet, periodic)
{
    need_hpet();
    int n = 0, pin = route_for(n);
    if (pin < 0)
        t_skip("timer 0 reaches no IOAPIC input");
    sCount = 0;
    ASSERT_TRUE(IoApic::ForGsi(pin)->Route(pin, [](void *) {
        sCount = sCount + 1;
    }, nullptr, false));
    wr(HPET_CONFIG, 0);
    set_counter(0);
    uint64_t period = 1000 * ticks_per_us();
    wr(HPET_TCONF(n), TN_INT_ENABLE | TN_PERIODIC | TN_SET_VALUE |
                      TN_ROUTE(pin));
    set_comparator(n, period);
    /* without SET_VALUE a write sets the period only */
    set_comparator(n, period);
    wr(HPET_CONFIG, CONF_ENABLE);
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount >= 4; }, 500000));
    arch_irq_disable();
    /* the comparator moved on by periods */
    EXPECT_GT(comparator(n), period);
    EXPECT_EQ(comparator(n) % period, 0ull);
}

TEST(hpet, level_status)
{
    need_hpet();
    int n = 1, pin = route_for(n);
    if (pin < 0)
        t_skip("timer 1 reaches no IOAPIC input");
    sCount = 0;
    ASSERT_TRUE(IoApic::ForGsi(pin)->Route(pin, [](void *) {
        sCount = sCount + 1;
        wr(HPET_STATUS, 1 << 1); /* which lowers the line */
    }, nullptr, true));
    wr(HPET_TCONF(n), TN_LEVEL | TN_INT_ENABLE | TN_ROUTE(pin));
    wr(HPET_CONFIG, CONF_ENABLE);
    set_comparator(n, counter() + 500 * ticks_per_us());
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }, 200000));
    t_delay_us(2000);
    arch_irq_disable();
    EXPECT_EQ(sCount, 1);
    EXPECT_EQ(rd(HPET_STATUS) & 2, 0u);
}

TEST(hpet, status_without_interrupt)
{
    need_hpet();
    /* level triggered, interrupt off: the status bit is still set */
    wr(HPET_TCONF(2), TN_LEVEL);
    wr(HPET_CONFIG, CONF_ENABLE);
    set_comparator(2, counter() + 200 * ticks_per_us());
    EXPECT_TRUE(t_wait_until([] { return (rd(HPET_STATUS) & 4) != 0; },
                             100000));
    wr(HPET_STATUS, 4);
    EXPECT_EQ(rd(HPET_STATUS) & 4, 0u);
    /* switching to edge clears it */
    set_comparator(2, counter() + 200 * ticks_per_us());
    t_wait_until([] { return (rd(HPET_STATUS) & 4) != 0; }, 100000);
    wr(HPET_TCONF(2), 0);
    EXPECT_EQ(rd(HPET_STATUS) & 4, 0u);
}

TEST(hpet, unrouted)
{
    need_hpet();
    /* an input the capability mask leaves out goes nowhere */
    uint32_t mask = rd(HPET_TCONF(0) + 4);
    int pin = -1;
    for (int p = 0; p < 32 && pin < 0; p++) {
        if (!((mask >> p) & 1) && IoApic::ForGsi(p))
            pin = p;
    }
    if (pin < 0)
        t_skip("every IOAPIC input is allowed");
    sCount = 0;
    ASSERT_TRUE(IoApic::ForGsi(pin)->Route(pin, [](void *) {
        sCount = sCount + 1;
    }, nullptr, false));
    wr(HPET_TCONF(0), TN_INT_ENABLE | TN_ROUTE(pin));
    SNAP("route_field", "%#x", (rd(HPET_TCONF(0)) >> 9) & 0x1f);
    wr(HPET_CONFIG, CONF_ENABLE);
    set_comparator(0, counter() + 200 * ticks_per_us());
    arch_irq_enable();
    t_delay_us(5000);
    arch_irq_disable();
    EXPECT_EQ(sCount, 0);
}

TEST(hpet, legacy_replacement)
{
    need_hpet();
    sCount = 0;
    /* timer 0 takes over the PIT's line, timer 1 the RTC's */
    ASSERT_TRUE(irq_route_isa(0, [](void *) {
        sCount = sCount + 1;
    }, nullptr));
    ASSERT_TRUE(irq_route_isa(8, [](void *) {
        sCount = sCount + 100;
    }, nullptr));
    wr(HPET_TCONF(0), TN_INT_ENABLE);
    wr(HPET_TCONF(1), TN_INT_ENABLE);
    wr(HPET_CONFIG, CONF_ENABLE | CONF_LEGACY);
    set_comparator(0, counter() + 500 * ticks_per_us());
    set_comparator(1, counter() + 1000 * ticks_per_us());
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount >= 101; }, 200000));
    arch_irq_disable();
    EXPECT_EQ(sCount, 101);
}

TEST(hpet, narrow_timer)
{
    need_hpet();
    wr(HPET_TCONF(0), TN_32BIT);
    set_comparator(0, 0x123456789abcull);
    /* a 32 bit timer: the high half stays clear */
    SNAP("comparator", "%#llx", T_FMT_VAL(comparator(0)));
    wr(HPET_TCONF(0), 0);
    set_comparator(0, 0x123456789abcull);
    EXPECT_EQ(comparator(0), 0x123456789abcull);
}

TEST(hpet, narrow_wraps)
{
    need_hpet();
    int pin = route_for(0);
    if (pin < 0)
        t_skip("timer 0 reaches no IOAPIC input");
    sCount = 0;
    ASSERT_TRUE(IoApic::ForGsi(pin)->Route(pin, [](void *) {
        sCount = sCount + 1;
    }, nullptr, false));
    /* a 32 bit timer fires when the low half of the counter wraps past it */
    wr(HPET_CONFIG, 0);
    set_counter(0x00000005ffffff00ull);
    wr(HPET_TCONF(0), TN_INT_ENABLE | TN_32BIT | TN_ROUTE(pin));
    set_comparator(0, 0x100);
    wr(HPET_CONFIG, CONF_ENABLE);
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }, 200000));
    arch_irq_disable();
}

#endif
