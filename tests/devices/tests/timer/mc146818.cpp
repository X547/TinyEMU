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

int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}

void civil_from_days(int64_t z, int *y, int *m, int *d)
{
    z += 719468;
    int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    int64_t doe = z - era * 146097;
    int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    int64_t mp = (5 * doy + 2) / 153;
    *d = doy - (153 * mp + 2) / 5 + 1;
    *m = mp < 10 ? mp + 3 : mp - 9;
    *y = yoe + era * 400 + (*m <= 2);
}

uint8_t to_bcd(int v)
{
    return (v / 10) << 4 | v % 10;
}

/* Sets the clock, BCD and 24 hour, under SET. */
void set_clock(int year, int month, int day, int h, int m, int s, int dow)
{
    wr(RTC_B, B_24H | 0x80);
    wr(0x32, to_bcd(year / 100));
    wr(RTC_YEAR, to_bcd(year % 100));
    wr(RTC_MONTH, to_bcd(month));
    wr(RTC_DAY, to_bcd(day));
    wr(RTC_WEEKDAY, dow);
    wr(RTC_HOURS, to_bcd(h));
    wr(RTC_MINUTES, to_bcd(m));
    wr(RTC_SECONDS, to_bcd(s));
    wr(RTC_B, B_24H);
}

struct SavedClock {
    int64_t secs;
    int dow;
    uint64_t at_us;
};

SavedClock sSaved;

/* Puts the clock back where it would have got to. */
void restore_clock()
{
    int64_t secs = sSaved.secs + (t_now_us() - sSaved.at_us) / 1000000;
    int64_t days = secs / 86400, sod = secs % 86400;
    int64_t saved_days = sSaved.secs / 86400;
    int y, mo, d;
    civil_from_days(days, &y, &mo, &d);
    set_clock(y, mo, d, sod / 3600, sod / 60 % 60, sod % 60,
              (sSaved.dow - 1 + days - saved_days) % 7 + 1);
}

/* For a test that sets the clock: what it reads now, BCD and 24 hour, to
   put back afterwards. */
void save_clock()
{
    for (;;) {
        uint8_t s = rd(RTC_SECONDS);
        int64_t days = days_from_civil(from_bcd(rd(0x32)) * 100 +
                                       from_bcd(rd(RTC_YEAR)),
                                       from_bcd(rd(RTC_MONTH)),
                                       from_bcd(rd(RTC_DAY)));
        int sod = from_bcd(rd(RTC_HOURS)) * 3600 +
            from_bcd(rd(RTC_MINUTES)) * 60 + from_bcd(s);
        sSaved = {days * 86400 + sod, from_bcd(rd(RTC_WEEKDAY)), t_now_us()};
        if (rd(RTC_SECONDS) == s)
            break;
    }
    t_on_cleanup(restore_clock);
}

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
    /* 1 to 7, Sunday being 1 */
    EXPECT_GE(rd(RTC_WEEKDAY), 1);
    EXPECT_LE(rd(RTC_WEEKDAY), 7);
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

/* 12 hour mode shows 12 for midnight and noon, with bit 7 from noon on,
   by the MC146818 data sheet. */
TEST(rtc, twelve_hour_mode)
{
    need_rtc();
    save_clock();
    static const struct {
        int hour;
        uint8_t shown;
    } cases[] = {{0, 0x12}, {1, 0x01}, {11, 0x11}, {12, 0x92}, {13, 0x81},
                 {23, 0x91}};
    for (const auto &c : cases) {
        set_clock(2030, 6, 15, c.hour, 30, 0, 7);
        wr(RTC_B, 0);
        EXPECT_EQ(rd(RTC_HOURS), c.shown);
        /* and written so, in the same mode: the part never converts what
           its registers hold */
        wr(RTC_B, 0x80);
        wr(RTC_HOURS, c.shown);
        wr(RTC_B, 0);
        EXPECT_EQ(rd(RTC_HOURS), c.shown);
    }
}

/* The time is the guest's to set: held while SET is on, and counting on
   from what was written once it is off. The day of the week is a counter
   of its own. */
TEST(rtc, set_time)
{
    need_rtc();
    save_clock();
    wr(RTC_B, B_24H | 0x80);
    set_clock(2032, 2, 28, 23, 59, 59, 3); /* a Saturday, 7, said to be 3 */
    wr(RTC_B, B_24H | 0x80);
    uint8_t held = rd(RTC_SECONDS);
    t_delay_us(1100000);
    EXPECT_EQ(rd(RTC_SECONDS), held);
    EXPECT_EQ(rd(RTC_HOURS), 0x23u);
    wr(RTC_B, B_24H);
    t_delay_us(1100000);
    /* a leap year */
    EXPECT_EQ(rd(RTC_DAY), 0x29u);
    EXPECT_EQ(rd(RTC_MONTH), 0x02u);
    EXPECT_EQ(rd(RTC_HOURS), 0x00u);
    EXPECT_LE(rd(RTC_SECONDS), 0x01u);
    EXPECT_EQ(rd(RTC_WEEKDAY), 4u);
    EXPECT_EQ(rd(RTC_YEAR), 0x32u);
    /* the end of a year, and the weekday counter wrapping */
    set_clock(2099, 12, 31, 23, 59, 59, 7);
    t_delay_us(1100000);
    EXPECT_EQ(rd(RTC_YEAR), 0x00u);
    EXPECT_EQ(rd(RTC_MONTH), 0x01u);
    EXPECT_EQ(rd(RTC_DAY), 0x01u);
    EXPECT_EQ(rd(RTC_WEEKDAY), 1u);
    /* a field written while it runs takes at once */
    wr(RTC_MINUTES, 0x42);
    EXPECT_EQ(rd(RTC_MINUTES), 0x42u);
}

/* The divider held in reset stops the clock; out of reset, the first
   update comes half a second later. */
TEST(rtc, divider_reset)
{
    need_rtc();
    save_clock();
    set_clock(2030, 1, 1, 12, 0, 0, 3);
    wr(RTC_A, 0x66);
    t_delay_us(1100000);
    EXPECT_EQ(rd(RTC_SECONDS), 0u);
    wr(RTC_A, 0x26);
    uint64_t start = t_now_us();
    EXPECT_TRUE(t_wait_until([] { return rd(RTC_SECONDS) != 0; }, 1000000));
    uint64_t took = t_now_us() - start;
    EXPECT_GT(took, 400000u);
    EXPECT_LT(took, 700000u);
}

TEST(rtc, read_only_registers)
{
    need_rtc();
    /* the periodic flag out of the way */
    wr(RTC_A, 0x20);
    rd(RTC_C);
    uint8_t d = rd(RTC_D);
    wr(RTC_C, 0xff);
    wr(RTC_D, 0x00);
    EXPECT_EQ(rd(RTC_C), 0);
    EXPECT_EQ(rd(RTC_D), d);
    /* UIP is the clock's to set */
    wr(RTC_A, 0xa6);
    SNAP("a_after_uip_write", "%#x", rd(RTC_A) & 0x7f);
}

/* UIP is set 244 us before the time registers change and through the
   update cycle, so a time read while it is clear holds: no read with UIP
   clear before and after it sees the seconds change. */
TEST(rtc, update_in_progress)
{
    need_rtc();
    bool seen = false, torn = false;
    uint64_t end = t_now_us() + 1200000;
    while (t_now_us() < end) {
        if (rd(RTC_A) & 0x80) {
            seen = true;
            continue;
        }
        uint8_t s1 = rd(RTC_SECONDS);
        uint8_t s2 = rd(RTC_SECONDS);
        if (!(rd(RTC_A) & 0x80))
            torn |= s1 != s2;
    }
    EXPECT_TRUE(seen);
    EXPECT_FALSE(torn);
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
    /* PF is set at the rate whether or not it may interrupt; IRQF only
       when it may */
    uint8_t c = rd(RTC_C);
    EXPECT_EQ(c & 0xc0u, 0x40u);
    wr(RTC_B, B_24H | B_PIE);
    EXPECT_TRUE(t_wait_until([] { return (rd(RTC_C) & 0xc0) == 0xc0; },
                             20000));
    /* reading C cleared it */
    SNAP("c_right_after", "%#x", rd(RTC_C) & 0x80);
    /* rate 0 stops it */
    wr(RTC_B, B_24H);
    wr(RTC_A, 0x20);
    rd(RTC_C);
    t_delay_us(5000);
    EXPECT_EQ(rd(RTC_C) & 0x40u, 0u);
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

/* The update ended flag every second, the alarm flag when the alarm
   matches, a register with its top bits set matching anything; IRQF and
   the line only for those enabled. */
TEST(rtc, alarm_and_update)
{
    Rtc r = need_rtc();
    save_clock();
    auto flags_for = [](uint64_t us) {
        uint8_t flags = 0;
        uint64_t end = t_now_us() + us;
        while (t_now_us() < end)
            flags |= rd(RTC_C);
        return flags;
    };
    /* the periodic flag out of the way */
    wr(RTC_A, 0x20);
    rd(RTC_C);
    wr(0x01, 0xc0);
    wr(0x03, 0xc0);
    wr(0x05, 0xc0);
    /* not enabled: the flags, and no IRQF */
    EXPECT_EQ(flags_for(1100000), 0x30u);
    wr(RTC_B, B_24H | B_AIE | B_UIE);
    EXPECT_EQ(flags_for(1100000), 0xb0u);
    /* an alarm a second ahead matches, one far ahead does not */
    wr(RTC_B, B_24H);
    set_clock(2030, 1, 1, 10, 0, 0, 3);
    wr(0x01, 0x01);
    wr(0x03, 0x00);
    wr(0x05, 0x10);
    rd(RTC_C);
    EXPECT_EQ(flags_for(1500000) & 0x20u, 0x20u);
    wr(0x01, 0x30);
    rd(RTC_C);
    EXPECT_EQ(flags_for(1100000) & 0x20u, 0u);
    /* SET holds the updates, and clears UIE */
    wr(RTC_B, B_24H | 0x80 | B_UIE);
    EXPECT_EQ(rd(RTC_B) & B_UIE, 0);
    rd(RTC_C);
    EXPECT_EQ(flags_for(1100000) & 0x10u, 0u);
    wr(RTC_B, B_24H);
    /* the update interrupt, on its line */
    sCount = 0;
    ASSERT_TRUE(irq_route_isa(r.irq, [](void *) {
        sCount = sCount + 1;
        rd(RTC_C);
    }, nullptr));
    rd(RTC_C);
    wr(RTC_B, B_24H | B_UIE);
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount >= 2; }, 2500000));
    arch_irq_disable();
}

#endif
