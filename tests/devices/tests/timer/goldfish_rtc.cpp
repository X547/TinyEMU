/*
 * Goldfish RTC tests
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
#include "mmio.h"
#include "test.h"

/* The Android emulator's goldfish RTC, as its documentation
   (GOLDFISH-VIRTUAL-HARDWARE.TXT) and the Linux binding
   rtc/google,goldfish-rtc.yaml describe it: a nanosecond count since the
   epoch whose low half latches the high one when read, and one alarm that
   disarms itself when it fires. */

#define TIME_LOW        0x00
#define TIME_HIGH       0x04
#define ALARM_LOW       0x08
#define ALARM_HIGH      0x0c
#define IRQ_ENABLED     0x10
#define CLEAR_ALARM     0x14
#define ALARM_STATUS    0x18
#define CLEAR_INTERRUPT 0x1c

#define NS_PER_US 1000ull
#define NS_PER_SEC 1000000000ull

/* 2020-01-01 */
#define Y2020_NS (1577836800ull * NS_PER_SEC)

namespace {

const HwDevice *sDev;
uint64_t sBase;
/* the time when the test began, put back after it */
uint64_t sSavedNs, sSavedUs;
volatile int sIrqCount;
uint64_t sIrqTime;

uint32_t rd(uint32_t reg)
{
    return mmio_read32(sBase + reg);
}

void wr(uint32_t reg, uint32_t v)
{
    mmio_write32(sBase + reg, v);
}

uint64_t now()
{
    uint32_t lo = rd(TIME_LOW);
    return (uint64_t)rd(TIME_HIGH) << 32 | lo;
}

void set_time(uint64_t ns)
{
    wr(TIME_HIGH, (uint32_t)(ns >> 32));
    wr(TIME_LOW, (uint32_t)ns);
}

void set_alarm(uint64_t ns)
{
    wr(ALARM_HIGH, (uint32_t)(ns >> 32));
    wr(ALARM_LOW, (uint32_t)ns);
}

void quiet()
{
    wr(IRQ_ENABLED, 0);
    wr(CLEAR_ALARM, 1);
    wr(CLEAR_INTERRUPT, 1);
}

void need_rtc()
{
    sDev = t_require_device("google,goldfish-rtc");
    const HwRegion *r = hw_region(sDev, HW_MMIO);
    ASSERT_TRUE(r != nullptr);
    sBase = r->base;
    quiet();
    sSavedNs = now();
    sSavedUs = t_now_us();
    t_on_cleanup([] {
        irq_unroute(sDev, 0);
        quiet();
        set_time(sSavedNs + (t_now_us() - sSavedUs) * NS_PER_US);
    });
}

/* Records when it came and takes the interrupt back. */
void rtc_irq(void *)
{
    sIrqTime = now();
    sIrqCount++;
    wr(CLEAR_INTERRUPT, 1);
}

void route_irq()
{
    sIrqCount = 0;
    if (!irq_route(sDev, 0, rtc_irq, nullptr))
        t_skip("the RTC's interrupt reaches no controller the suite drives");
}

}


/* One page of registers and one interrupt, nothing armed. */
TEST(goldfish_rtc, node)
{
    need_rtc();
    const HwRegion *r = hw_region(sDev, HW_MMIO);
    SNAP("size", "%#llx", T_FMT_VAL(r->size));
    SNAP("irqs", "%d", sDev->nirqs);
    SNAP("irq_enabled", "%u", rd(IRQ_ENABLED));
    SNAP("alarm_status", "%u", rd(ALARM_STATUS));
    SNAP("unassigned", "%#x", rd(0x20));
}

/* The host's wall clock, so a date after 2020, counting nanoseconds at the
   rate the suite's own clock does. */
TEST(goldfish_rtc, counts)
{
    need_rtc();
    EXPECT_GT(now(), Y2020_NS);
    for (int tries = 0; tries < 5; tries++) {
        uint64_t h0 = t_now_us(), r0 = now(), h1 = t_now_us();
        t_delay_us(20000);
        uint64_t h2 = t_now_us(), r1 = now(), h3 = t_now_us();
        /* host clocks differ by more than a few microseconds per run */
        uint64_t lo = (h2 - h1) * NS_PER_US, hi = (h3 - h0) * NS_PER_US;
        uint64_t d = r1 - r0;
        if (d + 2000000 >= lo && d <= hi + 2000000)
            return;
    }
    t_fail(__FILE__, __LINE__, "the time does not follow the suite's clock");
}

/* A write of the high half waits for the low one; then both are the time,
   which goes on counting from there. */
TEST(goldfish_rtc, set_time)
{
    need_rtc();
    uint64_t before = now();
    wr(TIME_HIGH, 0x12345);
    EXPECT_LT(now() - before, 10 * NS_PER_SEC);
    wr(TIME_LOW, 0x10000000);
    uint64_t t = now();
    EXPECT_GE(t, 0x1234510000000ull);
    EXPECT_LT(t, 0x1234510000000ull + 10 * NS_PER_SEC);
    t_delay_us(2000);
    EXPECT_GE(now(), 0x1234510000000ull + 1000 * NS_PER_US);
}

/* Reading the low half fixes the high half, until the next such read. */
TEST(goldfish_rtc, latch)
{
    need_rtc();
    const uint32_t before_carry = 0xffffffffu - (uint32_t)(10000 * NS_PER_US);
    uint32_t lo = 0;
    for (int tries = 0; tries < 5; tries++) {
        set_time(0x700000000ull - 10000 * NS_PER_US);
        lo = rd(TIME_LOW);
        if (lo >= before_carry)
            break;
    }
    ASSERT_GE(lo, before_carry);
    t_delay_us(20000);
    EXPECT_EQ(rd(TIME_HIGH), 6u);
    EXPECT_EQ(rd(TIME_HIGH), 6u);
    lo = rd(TIME_LOW);
    EXPECT_LT(lo, before_carry);
    EXPECT_EQ(rd(TIME_HIGH), 7u);
}

#if __riscv_xlen == 64
/* A 64 bit read is the low half then the high one, so a whole time. */
TEST(goldfish_rtc, read64)
{
    need_rtc();
    set_time(0x900000000ull - 50 * NS_PER_US);
    uint64_t prev = 0;
    for (int i = 0; i < 200; i++) {
        uint64_t t = *(volatile uint64_t *)(uintptr_t)sBase;
        EXPECT_GE(t, prev);
        prev = t;
    }
    EXPECT_GE(prev >> 32, 8u);
}
#endif

/* The alarm reads back. It is armed by the low half, and disarmed by
   CLEAR_ALARM. */
TEST(goldfish_rtc, alarm_registers)
{
    need_rtc();
    uint64_t at = now() + 3600 * NS_PER_SEC;
    wr(ALARM_HIGH, (uint32_t)(at >> 32));
    EXPECT_EQ(rd(ALARM_STATUS), 0u);
    wr(ALARM_LOW, (uint32_t)at);
    EXPECT_EQ(rd(ALARM_STATUS), 1u);
    EXPECT_EQ((uint64_t)rd(ALARM_HIGH) << 32 | rd(ALARM_LOW), at);
    wr(CLEAR_ALARM, 1);
    EXPECT_EQ(rd(ALARM_STATUS), 0u);
    wr(IRQ_ENABLED, 3);
    SNAP("irq_enabled", "%u", rd(IRQ_ENABLED));
}

/* An alarm already due fires at once. Its interrupt stays pending while
   disabled, comes when enabled, and goes with CLEAR_INTERRUPT. */
TEST(goldfish_rtc, pending_while_disabled)
{
    need_rtc();
    route_irq();
    arch_irq_enable();
    set_alarm(now() - NS_PER_SEC);
    EXPECT_EQ(rd(ALARM_STATUS), 0u);
    t_delay_us(1000);
    EXPECT_EQ(sIrqCount, 0);
    wr(IRQ_ENABLED, 1);
    EXPECT_TRUE(t_wait_until([] { return sIrqCount >= 1; }));
    t_delay_us(1000);
    EXPECT_EQ(sIrqCount, 1);
    arch_irq_disable();

    /* cleared while disabled, nothing is left to come */
    wr(IRQ_ENABLED, 0);
    set_alarm(0);
    wr(CLEAR_INTERRUPT, 1);
    arch_irq_enable();
    wr(IRQ_ENABLED, 1);
    t_delay_us(1000);
    arch_irq_disable();
    EXPECT_EQ(sIrqCount, 1);
}

/* An alarm ahead fires once, not before its time. */
TEST(goldfish_rtc, alarm_fires)
{
    need_rtc();
    route_irq();
    wr(IRQ_ENABLED, 1);
    arch_irq_enable();
    uint64_t at = now() + 20000 * NS_PER_US;
    set_alarm(at);
    EXPECT_EQ(rd(ALARM_STATUS), 1u);
    EXPECT_TRUE(t_wait_until([] { return sIrqCount >= 1; }, 2000000));
    arch_irq_disable();
    EXPECT_GE(sIrqTime, at);
    EXPECT_EQ(rd(ALARM_STATUS), 0u);
    arch_irq_enable();
    t_delay_us(30000);
    arch_irq_disable();
    EXPECT_EQ(sIrqCount, 1);
}

/* The alarm is against the time shown: setting the time past it fires it,
   setting it back keeps it waiting. */
TEST(goldfish_rtc, alarm_follows_time)
{
    need_rtc();
    route_irq();
    wr(IRQ_ENABLED, 1);
    arch_irq_enable();
    set_time(1000 * NS_PER_SEC);
    set_alarm(2000 * NS_PER_SEC);
    t_delay_us(5000);
    EXPECT_EQ(sIrqCount, 0);
    set_time(500 * NS_PER_SEC);
    t_delay_us(5000);
    EXPECT_EQ(sIrqCount, 0);
    EXPECT_EQ(rd(ALARM_STATUS), 1u);
    set_time(3000 * NS_PER_SEC);
    EXPECT_TRUE(t_wait_until([] { return sIrqCount >= 1; }));
    arch_irq_disable();
    EXPECT_EQ(rd(ALARM_STATUS), 0u);
}
