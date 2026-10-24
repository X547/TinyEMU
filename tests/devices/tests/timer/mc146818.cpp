/*
 * RTC tests
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
#include "intc.h"
#include "mmio.h"
#include "test.h"

/* The MC146818 behind the index and data ports the DSDT gives it. Its line
   is the AT's IRQ 8 unless the table names one. */

#define RTC_SECONDS 0x00
#define RTC_MINUTES 0x02
#define RTC_HOURS   0x04
#define RTC_WEEKDAY 0x06
#define RTC_DAY     0x07
#define RTC_MONTH   0x08
#define RTC_YEAR    0x09
#define RTC_A       0x0a
#define RTC_B       0x0b
#define RTC_C       0x0c
#define RTC_D       0x0d

#define B_PIE 0x40
#define B_AIE 0x20
#define B_UIE 0x10
#define B_BINARY 0x04
#define B_24H 0x02

namespace {

struct Rtc {
    uint16_t port;
    uint32_t irq;
};

Rtc sRtc;

uint8_t rd(uint8_t reg)
{
    outb(sRtc.port, reg);
    return inb(sRtc.port + 1);
}

void wr(uint8_t reg, uint8_t v)
{
    outb(sRtc.port, reg);
    outb(sRtc.port + 1, v);
}

Rtc need_rtc()
{
    const HwDevice *d = t_require_device("PNP0B00");
    const HwRegion *r = hw_region(d, HW_IO);
    if (r == nullptr)
        t_skip("no RTC ports");
    sRtc = {(uint16_t)r->base, d->nirqs ? d->irqs[0].line : 8};
    /* BCD, 24 hour, no interrupts, the 1024 Hz rate, as it starts */
    t_on_cleanup([] {
        wr(RTC_B, B_24H);
        wr(RTC_A, 0x26);
        rd(RTC_C);
    });
    return sRtc;
}

int from_bcd(uint8_t v)
{
    return (v >> 4) * 10 + (v & 0xf);
}

bool is_bcd(uint8_t v)
{
    return (v & 0xf) <= 9 && (v >> 4) <= 9;
}

/* seconds since midnight, read twice so that a carry between the fields
   is not mistaken for a time */
int seconds_of_day(bool binary)
{
    for (;;) {
        uint8_t s = rd(RTC_SECONDS), m = rd(RTC_MINUTES), h = rd(RTC_HOURS);
        if (rd(RTC_SECONDS) != s)
            continue;
        if (binary)
            return h * 3600 + m * 60 + s;
        return from_bcd(h) * 3600 + from_bcd(m) * 60 + from_bcd(s);
    }
}

volatile int sCount;

}


FRESH_TEST(rtc, reset_state)
{
    Rtc r = need_rtc();
    (void)r;
    SNAP("a", "%#x", rd(RTC_A) & 0x7f);
    SNAP("b", "%#x", rd(RTC_B));
    SNAP("c", "%#x", rd(RTC_C));
    SNAP("d", "%#x", rd(RTC_D));
    /* what the machine put in the NVRAM: the equipment byte and the
       memory sizes */
    char buf[160];
    size_t n = 0;
    for (int reg = 0x0e; reg < 0x40; reg++) {
        if (reg == 0x32 || reg == 0x37) /* the century */
            continue;
        n += snprintf(buf + n, sizeof(buf) - n, "%02x", rd(reg));
    }
    SNAP("nvram_0e_3f", "%s", buf);
    SNAP("table_irq", "%d", registry_find("PNP0B00")->nirqs);
}

TEST(rtc, bcd_time)
{
    need_rtc();
    static const uint8_t regs[] = {RTC_SECONDS, RTC_MINUTES, RTC_HOURS,
                                   RTC_WEEKDAY, RTC_DAY, RTC_MONTH, RTC_YEAR};
    for (uint8_t reg : regs)
        EXPECT_TRUE(is_bcd(rd(reg)));
    EXPECT_LE(from_bcd(rd(RTC_SECONDS)), 59);
    EXPECT_LE(from_bcd(rd(RTC_MINUTES)), 59);
    EXPECT_LE(from_bcd(rd(RTC_HOURS)), 23);
    EXPECT_GE(from_bcd(rd(RTC_MONTH)), 1);
    EXPECT_LE(from_bcd(rd(RTC_MONTH)), 12);
    EXPECT_GE(from_bcd(rd(RTC_DAY)), 1);
    EXPECT_LE(from_bcd(rd(RTC_DAY)), 31);
    EXPECT_GE(from_bcd(rd(0x32)), 20);
}

TEST(rtc, binary_mode)
{
    need_rtc();
    int bcd = seconds_of_day(false);
    wr(RTC_B, B_24H | B_BINARY);
    int bin = seconds_of_day(true);
    /* the same clock, read the moment after */
    EXPECT_GE(bin, bcd);
    EXPECT_LE(bin - bcd, 2);
}

TEST(rtc, twelve_hour_mode)
{
    need_rtc();
    uint8_t h24 = rd(RTC_HOURS);
    wr(RTC_B, 0);
    uint8_t h12 = rd(RTC_HOURS);
    int hour = from_bcd(h24);
    int shown = from_bcd(h12 & 0x7f);
    bool pm = h12 & 0x80;
    EXPECT_EQ(pm, hour >= 12);
    SNAP("noon_and_midnight_as", "%s", hour % 12 == 0 ?
         (shown == 12 ? "12" : "0") : "n/a");
    if (hour % 12 != 0)
        EXPECT_EQ(shown, hour % 12);
}

TEST(rtc, time_follows_host)
{
    need_rtc();
    uint8_t before = rd(RTC_SECONDS);
    /* writes to the time do not stick: it is the host's clock */
    wr(RTC_SECONDS, before == 0x30 ? 0x10 : 0x30);
    wr(RTC_B, B_24H | 0x80); /* SET */
    uint8_t after = rd(RTC_SECONDS);
    SNAP("write_stuck", "%d", after == (before == 0x30 ? 0x10 : 0x30));
    int start = seconds_of_day(false);
    t_delay_us(1100000);
    int later = seconds_of_day(false);
    EXPECT_TRUE(later == start + 1 || later == start + 2 ||
                later < start); /* or midnight */
}

TEST(rtc, read_only_registers)
{
    need_rtc();
    uint8_t d = rd(RTC_D);
    wr(RTC_C, 0xff);
    wr(RTC_D, 0x00);
    EXPECT_EQ(rd(RTC_C), 0);
    EXPECT_EQ(rd(RTC_D), d);
    /* UIP is the clock's to set */
    wr(RTC_A, 0xa6);
    SNAP("a_after_uip_write", "%#x", rd(RTC_A) & 0x7f);
}

TEST(rtc, update_in_progress)
{
    need_rtc();
    /* set for 244 us of every second */
    bool seen = false;
    uint64_t end = t_now_us() + 1100000;
    while (!seen && t_now_us() < end)
        seen = rd(RTC_A) & 0x80;
    EXPECT_TRUE(seen);
}

TEST(rtc, nvram)
{
    need_rtc();
    for (int reg = 0x40; reg < 0x80; reg++)
        wr(reg, reg ^ 0xa5);
    bool ok = true;
    for (int reg = 0x40; reg < 0x80; reg++)
        ok &= rd(reg) == (reg ^ 0xa5);
    EXPECT_TRUE(ok);
    /* bit 7 of the index is the NMI mask, not part of the register */
    outb(sRtc.port, 0x80 | 0x41);
    EXPECT_EQ(inb(sRtc.port + 1), 0x41 ^ 0xa5);
    SNAP("index_read", "%#x", inb(sRtc.port));
}

TEST(rtc, periodic_flags)
{
    need_rtc();
    rd(RTC_C);
    wr(RTC_A, 0x26); /* 1024 Hz */
    t_delay_us(5000);
    /* not enabled: no flag */
    EXPECT_EQ(rd(RTC_C), 0);
    wr(RTC_B, B_24H | B_PIE);
    EXPECT_TRUE(t_wait_until([] { return (rd(RTC_C) & 0x40) != 0; }, 20000));
    /* reading C cleared it */
    SNAP("c_right_after", "%#x", rd(RTC_C) & 0x80);
}

TEST(rtc, rates)
{
    need_rtc();
    char buf[96];
    size_t n = 0;
    /* Periods in 50 ms, counted by polling C, which keeps up with 1 kHz
       but not with the fastest rates: those are only checked to be
       faster. */
    auto periods_at = [](uint8_t rate) {
        wr(RTC_B, B_24H);
        wr(RTC_A, 0x20 | rate);
        wr(RTC_B, B_24H | B_PIE);
        rd(RTC_C);
        int periods = 0;
        uint64_t end = t_now_us() + 50000;
        while (t_now_us() < end)
            periods += (rd(RTC_C) & 0x40) != 0;
        return periods;
    };
    static const uint8_t rates[] = {0, 1, 2, 6, 7, 8, 9};
    for (uint8_t rate : rates) {
        int periods = periods_at(rate);
        const char *hz = periods == 0 ? "off" : periods > 38 ? "1024+" :
            periods > 19 ? "512" : periods > 9 ? "256" : "128";
        n += snprintf(buf + n, sizeof(buf) - n, "%s%u:%s", n ? " " : "",
                      rate, hz);
    }
    SNAP("hz", "%s", buf);
    EXPECT_GT(periods_at(3), 38);
}

TEST(rtc, periodic_interrupt)
{
    Rtc r = need_rtc();
    sCount = 0;
    ASSERT_TRUE(irq_route_isa(r.irq, [](void *) {
        sCount = sCount + 1;
        rd(RTC_C); /* which lowers the line */
    }, nullptr));
    rd(RTC_C);
    wr(RTC_A, 0x26);
    wr(RTC_B, B_24H | B_PIE);
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount >= 5; }, 200000));
    arch_irq_disable();
}

TEST(rtc, alarm_and_update)
{
    need_rtc();
    rd(RTC_C);
    wr(RTC_B, B_24H | B_AIE | B_UIE);
    /* the update interrupt comes once a second; the alarm, with every
       field set to don't care, then as well */
    wr(0x01, 0xc0);
    wr(0x03, 0xc0);
    wr(0x05, 0xc0);
    uint8_t flags = 0;
    uint64_t end = t_now_us() + 1100000;
    while (t_now_us() < end)
        flags |= rd(RTC_C);
    SNAP("flags", "%#x", flags);
}

#endif
