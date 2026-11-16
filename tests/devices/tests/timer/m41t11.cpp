/*
 * M41T11 serial RTC tests
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
#include "fdt.h"
#include "mmio.h"
#include "registry.h"
#include "test.h"

/* ST's M41T11 behind a DesignWare I2C controller, as its datasheet and
   the Linux binding rtc/trivial-rtc.yaml describe it: at the address in
   its node, which the bus chose as none is configured, an address pointer, then seconds, minutes, hours with the century bits,
   day, date, month and year in BCD, a control register and 56 bytes of
   RAM. A read takes the clock as it was when the read began; a write to it
   takes effect from the start of a second. The controller runs the commands
   as they are queued, so it is polled. */

#define IC_CON          0x00
#define IC_TAR          0x04
#define IC_DATA_CMD     0x10
#define IC_RAW_INTR_STAT 0x34
#define IC_CLR_INTR     0x40
#define IC_ENABLE       0x6c
#define IC_STATUS       0x70

#define IC_CON_MASTER        0x01
#define IC_CON_SPEED_FAST    0x04
#define IC_CON_RESTART_EN    0x20
#define IC_CON_SLAVE_DISABLE 0x40

#define IC_CMD_READ    0x100
#define IC_CMD_STOP    0x200
#define IC_CMD_RESTART 0x400

#define IC_INTR_TX_ABRT 0x40
#define IC_STATUS_TFNF  0x02
#define IC_STATUS_RFNE  0x08

#define REG_SECONDS 0x00
#define REG_HOURS   0x02
#define REG_CONTROL 0x07
#define REG_RAM     0x08
#define RAM_SIZE    56
#define CLOCK_REGS  7

#define SECONDS_ST 0x80
#define HOURS_CEB  0x80
#define HOURS_CB   0x40

#define US_PER_SEC 1000000ull

namespace {

uint64_t sBase;
/* the RTC's address, from its node */
uint32_t sAddr;
/* the time when the test began, put back after it */
int64_t sSavedSecs;
uint64_t sSavedUs;

struct Clock {
    int year;   /* 2000-2199, from the century bit */
    int month, date, day;
    int hours, minutes, seconds;
    bool stopped, ceb;
};

uint32_t rd(uint32_t reg)
{
    return mmio_read32(sBase + reg);
}

void wr(uint32_t reg, uint32_t v)
{
    mmio_write32(sBase + reg, v);
}

void push(uint32_t cmd)
{
    t_wait_until([] { return (rd(IC_STATUS) & IC_STATUS_TFNF) != 0; });
    wr(IC_DATA_CMD, cmd);
}

/* Writes the address pointer and 'wn' bytes, then reads 'rn' after a
   repeated START; false if the target did not answer. */
bool xfer(uint8_t ptr, const uint8_t *w, int wn, uint8_t *r, int rn)
{
    rd(IC_CLR_INTR);
    push(ptr | (wn == 0 && rn == 0 ? IC_CMD_STOP : 0));
    for (int i = 0; i < wn; i++)
        push(w[i] | (i == wn - 1 && rn == 0 ? IC_CMD_STOP : 0));
    for (int i = 0; i < rn; i++) {
        push(IC_CMD_READ | (i == 0 ? IC_CMD_RESTART : 0) |
             (i == rn - 1 ? IC_CMD_STOP : 0));
        if (!t_wait_until([] {
                return (rd(IC_STATUS) & IC_STATUS_RFNE) != 0 ||
                       (rd(IC_RAW_INTR_STAT) & IC_INTR_TX_ABRT) != 0;
            }))
            return false;
        if (rd(IC_RAW_INTR_STAT) & IC_INTR_TX_ABRT)
            break;
        r[i] = (uint8_t)rd(IC_DATA_CMD);
    }
    return (rd(IC_RAW_INTR_STAT) & IC_INTR_TX_ABRT) == 0;
}

bool rtc_write(uint8_t ptr, const uint8_t *data, int n)
{
    return xfer(ptr, data, n, nullptr, 0);
}

bool rtc_read(uint8_t ptr, uint8_t *data, int n)
{
    return xfer(ptr, nullptr, 0, data, n);
}

int bcd(int v)
{
    return v / 10 << 4 | v % 10;
}

int unbcd(int v)
{
    return (v >> 4) * 10 + (v & 0xf);
}

int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
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

Clock decode(const uint8_t *r)
{
    Clock c;
    c.stopped = (r[0] & SECONDS_ST) != 0;
    c.seconds = unbcd(r[0] & 0x7f);
    c.minutes = unbcd(r[1] & 0x7f);
    c.hours = unbcd(r[2] & 0x3f);
    c.ceb = (r[2] & HOURS_CEB) != 0;
    c.day = r[3] & 0x07;
    c.date = unbcd(r[4] & 0x3f);
    c.month = unbcd(r[5] & 0x1f);
    c.year = 2000 + (r[2] & HOURS_CB ? 100 : 0) + unbcd(r[6]);
    return c;
}

bool read_clock(Clock *c)
{
    uint8_t r[CLOCK_REGS];
    if (!rtc_read(REG_SECONDS, r, CLOCK_REGS))
        return false;
    *c = decode(r);
    return true;
}

int64_t to_secs(const Clock &c)
{
    return days_from_civil(c.year, c.month, c.date) * 86400 +
        c.hours * 3600 + c.minutes * 60 + c.seconds;
}

/* The clock at 'secs' since the epoch, with the day of the week Linux
   gives it, 1 for Sunday. */
bool set_clock(int64_t secs, bool stopped = false, bool ceb = true)
{
    int64_t days = secs / 86400;
    int sod = (int)(secs % 86400);
    int y, m, d;
    civil_from_days(days, &y, &m, &d);
    uint8_t r[CLOCK_REGS] = {
        (uint8_t)(bcd(sod % 60) | (stopped ? SECONDS_ST : 0)),
        (uint8_t)bcd(sod / 60 % 60),
        (uint8_t)(bcd(sod / 3600) | (ceb ? HOURS_CEB : 0) |
                  ((y - 2000) / 100 % 2 ? HOURS_CB : 0)),
        (uint8_t)((days + 4) % 7 + 1),
        (uint8_t)bcd(d),
        (uint8_t)bcd(m),
        (uint8_t)bcd(y % 100),
    };
    return rtc_write(REG_SECONDS, r, CLOCK_REGS);
}

int64_t secs_of(int y, int m, int d, int hh, int mm, int ss)
{
    return days_from_civil(y, m, d) * 86400 + hh * 3600 + mm * 60 + ss;
}

void need_rtc()
{
    const HwDevice *rtc = t_require_device("st,m41t11");
    const HwDevice *ctl = nullptr;
    for (int i = 0; i < registry_count(); i++) {
        if (registry_at(i)->fdt != nullptr &&
            registry_at(i)->fdt == rtc->fdt->parent)
            ctl = registry_at(i);
    }
    ASSERT_TRUE(ctl != nullptr && hw_is(ctl, "snps,designware-i2c"));
    const HwRegion *r = hw_region(ctl, HW_MMIO);
    ASSERT_TRUE(r != nullptr);
    sBase = r->base;
    ASSERT_TRUE(fdt_prop_u32(rtc->fdt, "reg", &sAddr));

    wr(IC_ENABLE, 0);
    wr(IC_CON, IC_CON_MASTER | IC_CON_SPEED_FAST | IC_CON_RESTART_EN |
               IC_CON_SLAVE_DISABLE);
    wr(IC_TAR, sAddr);
    wr(IC_ENABLE, 1);

    Clock c;
    ASSERT_TRUE(read_clock(&c));
    sSavedSecs = to_secs(c);
    sSavedUs = t_now_us();
    t_on_cleanup([] {
        set_clock(sSavedSecs + (int64_t)((t_now_us() - sSavedUs) /
                                         US_PER_SEC));
        wr(IC_ENABLE, 0);
    });
}

/* Polls until the seconds move off 'from'; false after two seconds. */
bool wait_tick(int from, Clock *c)
{
    uint64_t end = t_now_us() + 2 * US_PER_SEC;
    while (t_now_us() < end) {
        if (!read_clock(c))
            return false;
        if (c->seconds != from)
            return true;
        t_delay_us(10000);
    }
    return false;
}

}


/* The first free address on the controller's bus, as no address is
   configured, and nothing at the next one. */
TEST(m41t11, node)
{
    need_rtc();
    const HwDevice *rtc = t_require_device("st,m41t11");
    uint32_t reg = 0;
    EXPECT_TRUE(fdt_prop_u32(rtc->fdt, "reg", &reg));
    SNAP("reg", "%#x", reg);
    SNAP("regions", "%d", rtc->nregions);

    wr(IC_ENABLE, 0);
    wr(IC_TAR, sAddr + 1);
    wr(IC_ENABLE, 1);
    uint8_t b;
    EXPECT_FALSE(rtc_read(REG_SECONDS, &b, 1));
    wr(IC_ENABLE, 0);
    wr(IC_TAR, sAddr);
    wr(IC_ENABLE, 1);
    EXPECT_TRUE(rtc_read(REG_SECONDS, &b, 1));
}

/* Running, on the host's clock: a date after 2020 in valid BCD. */
TEST(m41t11, host_time)
{
    need_rtc();
    uint8_t r[CLOCK_REGS];
    ASSERT_TRUE(rtc_read(REG_SECONDS, r, CLOCK_REGS));
    Clock c = decode(r);
    EXPECT_FALSE(c.stopped);
    EXPECT_GE(c.year, 2020);
    EXPECT_GE(c.month, 1);
    EXPECT_LE(c.month, 12);
    EXPECT_GE(c.date, 1);
    EXPECT_LE(c.date, 31);
    EXPECT_LE(c.hours, 23);
    for (int i = 0; i < CLOCK_REGS; i++) {
        EXPECT_LE(r[i] & 0x0f, 9);
    }
}

/* A time written is read back, and carries over a year's end with the
   day of the week following on. */
TEST(m41t11, rollover)
{
    need_rtc();
    ASSERT_TRUE(set_clock(secs_of(2021, 12, 31, 23, 59, 59)));
    Clock c;
    ASSERT_TRUE(read_clock(&c));
    EXPECT_EQ(to_secs(c), secs_of(2021, 12, 31, 23, 59, 59));
    EXPECT_EQ(c.day, 6);    /* a Friday */
    ASSERT_TRUE(wait_tick(59, &c));
    EXPECT_EQ(c.year, 2022);
    EXPECT_EQ(c.month, 1);
    EXPECT_EQ(c.date, 1);
    EXPECT_EQ(c.hours, 0);
    EXPECT_EQ(c.minutes, 0);
    EXPECT_EQ(c.seconds, 0);
    EXPECT_EQ(c.day, 7);
}

/* With CEB set the century bit toggles as the year goes from 99 to 00. */
TEST(m41t11, century)
{
    need_rtc();
    ASSERT_TRUE(set_clock(secs_of(2099, 12, 31, 23, 59, 59)));
    uint8_t r[CLOCK_REGS];
    ASSERT_TRUE(rtc_read(REG_SECONDS, r, CLOCK_REGS));
    SNAP("hours_before", "%#x", r[REG_HOURS]);
    Clock c;
    ASSERT_TRUE(wait_tick(59, &c));
    ASSERT_TRUE(rtc_read(REG_SECONDS, r, CLOCK_REGS));
    SNAP("hours_after", "%#x", r[REG_HOURS]);
    SNAP("year_after", "%#x", r[6]);
    EXPECT_EQ(c.year, 2100);
}

/* ST stops the clock where it was set; clearing it starts it from there. */
TEST(m41t11, stop)
{
    need_rtc();
    int64_t t = secs_of(2030, 6, 15, 12, 0, 0);
    ASSERT_TRUE(set_clock(t, true));
    t_delay_us(1200000);
    Clock c;
    ASSERT_TRUE(read_clock(&c));
    EXPECT_TRUE(c.stopped);
    EXPECT_EQ(to_secs(c), t);

    uint8_t sec = 0;    /* ST clear, at 00 seconds */
    ASSERT_TRUE(rtc_write(REG_SECONDS, &sec, 1));
    ASSERT_TRUE(read_clock(&c));
    EXPECT_FALSE(c.stopped);
    EXPECT_EQ(to_secs(c), t);
    ASSERT_TRUE(wait_tick(0, &c));
    EXPECT_EQ(to_secs(c), t + 1);
}

/* Writing some clock registers leaves the others as they were. */
TEST(m41t11, partial_write)
{
    need_rtc();
    ASSERT_TRUE(set_clock(secs_of(2025, 3, 10, 8, 30, 0)));
    uint8_t date[2] = {0x21, 0x04};     /* the 21st of April */
    ASSERT_TRUE(rtc_write(0x04, date, 2));
    Clock c;
    ASSERT_TRUE(read_clock(&c));
    EXPECT_EQ(c.year, 2025);
    EXPECT_EQ(c.month, 4);
    EXPECT_EQ(c.date, 21);
    EXPECT_EQ(c.hours, 8);
    EXPECT_EQ(c.minutes, 30);
    EXPECT_LE(c.seconds, 1);
}

/* The control register and the RAM keep what is written; the pointer
   wraps from the last byte to the seconds. */
TEST(m41t11, ram)
{
    need_rtc();
    uint8_t ctl = 0;
    ASSERT_TRUE(rtc_read(REG_CONTROL, &ctl, 1));
    SNAP("control", "%#x", ctl);
    uint8_t v = 0xa5;
    ASSERT_TRUE(rtc_write(REG_CONTROL, &v, 1));
    ASSERT_TRUE(rtc_read(REG_CONTROL, &v, 1));
    EXPECT_EQ(v, 0xa5);
    ASSERT_TRUE(rtc_write(REG_CONTROL, &ctl, 1));

    uint8_t w[RAM_SIZE], r[RAM_SIZE];
    for (int i = 0; i < RAM_SIZE; i++)
        w[i] = (uint8_t)(i * 7 + 3);
    ASSERT_TRUE(rtc_write(REG_RAM, w, RAM_SIZE));
    ASSERT_TRUE(rtc_read(REG_RAM, r, RAM_SIZE));
    for (int i = 0; i < RAM_SIZE; i++)
        EXPECT_EQ(r[i], w[i]);

    ASSERT_TRUE(set_clock(secs_of(2024, 2, 29, 1, 2, 3), true));
    uint8_t wrap[2];
    ASSERT_TRUE(rtc_read(REG_RAM + RAM_SIZE - 1, wrap, 2));
    EXPECT_EQ(wrap[0], w[RAM_SIZE - 1]);
    EXPECT_EQ(wrap[1], SECONDS_ST | 0x03);
}
