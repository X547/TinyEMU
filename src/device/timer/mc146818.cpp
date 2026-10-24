/*
 * MC146818 RTC
 *
 * Copyright (c) 2011-2017 Fabrice Bellard
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
#include <stdio.h>

#include "bits.h"
#include "host_time.h"
#include "mc146818.h"

//#define DEBUG_CMOS

#define RTC_SECONDS             0
#define RTC_SECONDS_ALARM       1
#define RTC_MINUTES             2
#define RTC_MINUTES_ALARM       3
#define RTC_HOURS               4
#define RTC_HOURS_ALARM         5
#define RTC_ALARM_DONT_CARE    0xC0

#define RTC_DAY_OF_WEEK         6
#define RTC_DAY_OF_MONTH        7
#define RTC_MONTH               8
#define RTC_YEAR                9

#define RTC_REG_A               10
#define RTC_REG_B               11
#define RTC_REG_C               12
#define RTC_REG_D               13

/* not the part's, but where a PC keeps the century */
#define RTC_CENTURY             0x32
#define RTC_CENTURY_PS2         0x37

#define REG_A_UIP 0x80
/* the divider field: 010 runs, 11x holds the divider in reset */
#define REG_A_DV_POS 4
#define REG_A_DV_RUN 2

#define REG_B_SET 0x80
#define REG_B_PIE 0x40
#define REG_B_AIE 0x20
#define REG_B_UIE 0x10
#define REG_B_DM  0x04  /* binary rather than BCD */
#define REG_B_24H 0x02

#define REG_C_IRQF 0x80
#define REG_C_PF   0x40
#define REG_C_AF   0x20
#define REG_C_UF   0x10

#define CMOS_FREQ 32768

/* UIP is set this long before the update of the time registers, and stays
   so for the update cycle, at a 32.768 kHz time base */
#define UIP_BEFORE_US 244
#define UPDATE_CYCLE_US 1984

#define US_PER_SEC 1000000LL

struct CMOSState {
    uint8_t cmos_index = 0;
    uint8_t cmos_data[128] {};
    IRQSignal *irq = nullptr;
    bool use_local_time = false;
    /* the time shown less the host's, from what the guest wrote */
    int64_t time_offset_us = 0;
    /* what the day of week counter is ahead of the date's weekday */
    int dow_adjust = 0;
    /* the last second the update and alarm flags were looked at for */
    int64_t flags_second = 0;
    /* the periodic irq, in CMOS_FREQ ticks; no period while it is off */
    uint32_t irq_timeout = 0;
    uint32_t irq_period = 0;
    /* the periodic flag: when C was last read, in CMOS_FREQ ticks */
    uint32_t pf_read = 0;

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t data, int size_log2);

    DeviceIOAdapter<CMOSState, &CMOSState::Read, &CMOSState::Write> fIo {*this};
};

//#pragma mark - calendar

/* Days since 1970-01-01 of a date of the proleptic Gregorian calendar. */
static int64_t days_from_civil(int64_t y, int m, int d)
{
    y -= m <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static void civil_from_days(int64_t z, int *y, int *m, int *d)
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

static int64_t floor_div(int64_t a, int64_t b)
{
    return a / b - (a % b != 0 && (a < 0) != (b < 0));
}

/* The host's clock, as the calendar the RTC shows counts it. */
static int64_t host_clock_us(CMOSState *s)
{
    int64_t us = host_real_time_us();

    if (s->use_local_time)
        us += (int64_t)host_utc_offset_minutes() * 60 * US_PER_SEC;
    return us;
}

static int64_t cmos_time_us(CMOSState *s)
{
    return host_clock_us(s) + s->time_offset_us;
}

//#pragma mark - registers

static int to_bcd(CMOSState *s, unsigned int a)
{
    if (s->cmos_data[RTC_REG_B] & REG_B_DM) {
        return a;
    } else {
        return set_bits(a % 10, 4, 4, a / 10);
    }
}

static int from_bcd(CMOSState *s, unsigned int a)
{
    if (s->cmos_data[RTC_REG_B] & REG_B_DM)
        return a;
    return get_bits(a, 4, 4) * 10 + get_bits(a, 0, 4);
}

/* An hour, 0 to 23, as the hours register shows it: in 12 hour mode 12
   for noon and midnight, with bit 7 set from noon on. */
static int encode_hour(CMOSState *s, int hour)
{
    if (s->cmos_data[RTC_REG_B] & REG_B_24H)
        return to_bcd(s, hour);
    int h12 = hour % 12 == 0 ? 12 : hour % 12;
    return to_bcd(s, h12) | (hour >= 12 ? 0x80 : 0);
}

static int decode_hour(CMOSState *s, int reg)
{
    if (s->cmos_data[RTC_REG_B] & REG_B_24H)
        return from_bcd(s, reg);
    return from_bcd(s, reg & 0x7f) % 12 + (reg & 0x80 ? 12 : 0);
}

/* Updates are held while SET is on, or the divider is in reset. */
static bool cmos_frozen(CMOSState *s)
{
    return (s->cmos_data[RTC_REG_B] & REG_B_SET) ||
        get_bits(s->cmos_data[RTC_REG_A], REG_A_DV_POS + 1, 2) == 3;
}

/* The time registers from the time shown now. */
static void cmos_update_time(CMOSState *s)
{
    int64_t us = cmos_time_us(s);
    int64_t secs = floor_div(us, US_PER_SEC);
    int64_t days = floor_div(secs, 86400);
    int sod = secs - days * 86400;
    int year, month, day;

    civil_from_days(days, &year, &month, &day);
    s->cmos_data[RTC_SECONDS] = to_bcd(s, sod % 60);
    s->cmos_data[RTC_MINUTES] = to_bcd(s, sod / 60 % 60);
    s->cmos_data[RTC_HOURS] = encode_hour(s, sod / 3600);
    /* 1 is Sunday; 1970-01-01 was a Thursday */
    s->cmos_data[RTC_DAY_OF_WEEK] =
        to_bcd(s, ((days + 4) % 7 + 7 + s->dow_adjust) % 7 + 1);
    s->cmos_data[RTC_DAY_OF_MONTH] = to_bcd(s, day);
    s->cmos_data[RTC_MONTH] = to_bcd(s, month);
    s->cmos_data[RTC_YEAR] = to_bcd(s, year % 100);
}

/* What the time registers hold becomes the time shown, from the start of
   the second, or half way into it after the divider was in reset. */
static void cmos_commit_time(CMOSState *s, int64_t into_us)
{
    int year = from_bcd(s, s->cmos_data[RTC_CENTURY]) * 100 +
        from_bcd(s, s->cmos_data[RTC_YEAR]);
    int month = from_bcd(s, s->cmos_data[RTC_MONTH]);
    int day = from_bcd(s, s->cmos_data[RTC_DAY_OF_MONTH]);
    int64_t days = days_from_civil(year, month, day);
    int64_t secs = days * 86400 +
        decode_hour(s, s->cmos_data[RTC_HOURS]) * 3600 +
        from_bcd(s, s->cmos_data[RTC_MINUTES]) * 60 +
        from_bcd(s, s->cmos_data[RTC_SECONDS]);
    int dow = from_bcd(s, s->cmos_data[RTC_DAY_OF_WEEK]) - 1;

    s->time_offset_us = secs * US_PER_SEC + into_us - host_clock_us(s);
    s->dow_adjust = ((dow - (days + 4) % 7) % 7 + 7) % 7;
    s->flags_second = secs;
}

/* Whether the alarm registers match the time of day, where a register
   with its top two bits set matches anything. */
static bool cmos_alarm_matches(CMOSState *s, int64_t secs)
{
    int sod = secs - floor_div(secs, 86400) * 86400;
    static const int regs[3][2] = {
        {RTC_SECONDS_ALARM, RTC_SECONDS},
        {RTC_MINUTES_ALARM, RTC_MINUTES},
        {RTC_HOURS_ALARM, RTC_HOURS},
    };
    int now[3] = {
        to_bcd(s, sod % 60), to_bcd(s, sod / 60 % 60),
        encode_hour(s, sod / 3600),
    };

    for (int i = 0; i < 3; i++) {
        uint8_t alarm = s->cmos_data[regs[i][0]];
        if ((alarm & RTC_ALARM_DONT_CARE) != RTC_ALARM_DONT_CARE &&
            alarm != now[i])
            return false;
    }
    return true;
}

//#pragma mark - interrupts

static uint32_t cmos_get_timer()
{
    uint64_t us = host_monotonic_us();

    return (uint32_t)(us / 1000000) * CMOS_FREQ +
        ((us % 1000000) * CMOS_FREQ / 1000000);
}

/* The period of the rate register A selects, in CMOS_FREQ ticks, or 0. */
static uint32_t cmos_period(CMOSState *s)
{
    int period_code = get_bits(s->cmos_data[RTC_REG_A], 0, 4);

    /* off, or rate 0, which stops it on the part */
    if (period_code == 0)
        return 0;
    if (period_code <= 2)
        period_code += 7;
    return 1 << (period_code - 1);
}

/* The periodic interrupt is timed only while it is enabled; the flag alone
   is worked out when register C is read. */
static void cmos_update_timer(CMOSState *s)
{
    uint32_t period = cmos_period(s);

    if ((s->cmos_data[RTC_REG_B] & REG_B_PIE) && period != 0) {
        s->irq_period = period;
        s->irq_timeout = (cmos_get_timer() + period) & ~(period - 1);
    } else {
        s->irq_period = 0;
    }
}

/* The update ended, and the alarm, flags for the seconds that passed since
   they were last looked at. */
static void cmos_update_flags(CMOSState *s)
{
    int64_t secs = floor_div(cmos_time_us(s), US_PER_SEC);

    if (cmos_frozen(s)) {
        s->flags_second = secs;
        return;
    }
    if (secs <= s->flags_second)
        return;
    s->cmos_data[RTC_REG_C] |= REG_C_UF;
    /* a day's worth covers every time of day there is */
    int64_t from = secs - s->flags_second > 86400 ? secs - 86399
                                                  : s->flags_second + 1;
    for (int64_t t = from; t <= secs; t++) {
        if (cmos_alarm_matches(s, t)) {
            s->cmos_data[RTC_REG_C] |= REG_C_AF;
            break;
        }
    }
    s->flags_second = secs;
}

/* IRQF, and the line, follow the flags whose interrupts are enabled. */
static void cmos_update_irq_line(CMOSState *s, bool deliver)
{
    uint8_t b = s->cmos_data[RTC_REG_B];
    uint8_t c = s->cmos_data[RTC_REG_C];
    bool on = ((c & REG_C_PF) && (b & REG_B_PIE)) ||
        ((c & REG_C_AF) && (b & REG_B_AIE)) ||
        ((c & REG_C_UF) && (b & REG_B_UIE));

    if (on && !(c & REG_C_IRQF)) {
        s->cmos_data[RTC_REG_C] |= REG_C_IRQF;
        if (deliver)
            s->irq->Set(1);
    }
}

/* The microseconds until the next interrupt that is enabled, rounded up,
   or -1. */
static int64_t cmos_delay_us(CMOSState *s)
{
    int64_t delay = -1;

    if (s->irq_period != 0) {
        int32_t ticks = s->irq_timeout - cmos_get_timer();
        delay = ticks <= 0 ? 0 :
            ((int64_t)ticks * 1000000 + CMOS_FREQ - 1) / CMOS_FREQ;
    }
    if ((s->cmos_data[RTC_REG_B] & (REG_B_UIE | REG_B_AIE)) &&
        !cmos_frozen(s)) {
        int64_t us = cmos_time_us(s);
        int64_t next = (floor_div(us, US_PER_SEC) + 1) * US_PER_SEC - us;
        if (delay < 0 || next < delay)
            delay = next;
    }
    return delay;
}

/* Raises what is due and returns the microseconds until the next, or -1.
   Periods that ran out while nobody looked are one interrupt. Without
   'deliver' the flags are set but the line is left alone. */
static int64_t cmos_update_irq(CMOSState *s, bool deliver)
{
    if (s->irq_period != 0) {
        uint32_t now = cmos_get_timer();
        if ((int32_t)(now - s->irq_timeout) >= 0) {
            s->cmos_data[RTC_REG_C] |= REG_C_PF;
            s->irq_timeout += ((now - s->irq_timeout) / s->irq_period + 1) *
                s->irq_period;
        }
    }
    cmos_update_flags(s);
    cmos_update_irq_line(s, deliver);
    return cmos_delay_us(s);
}

//#pragma mark - the device

MC146818::MC146818(PhysMemoryMap *port_map, int addr, IRQSignal *irq,
                   bool use_local_time)
{
    std::unique_ptr<CMOSState> &s = fState;

    s = std::make_unique<CMOSState>();
    s->use_local_time = use_local_time;
    s->cmos_data[RTC_REG_A] = 0x26;
    s->cmos_data[RTC_REG_B] = 0x02;
    s->cmos_data[RTC_REG_C] = 0x00;
    s->cmos_data[RTC_REG_D] = 0x80;

    /* the century is not the part's to keep, but firmware sets it */
    int64_t days = floor_div(host_clock_us(s.get()), 86400 * US_PER_SEC);
    int year, month, day;
    civil_from_days(days, &year, &month, &day);
    s->cmos_data[RTC_CENTURY] = to_bcd(s.get(), year / 100);
    s->cmos_data[RTC_CENTURY_PS2] = s->cmos_data[RTC_CENTURY];
    s->flags_second = floor_div(cmos_time_us(s.get()), US_PER_SEC);
    s->pf_read = cmos_get_timer();

    s->irq = irq;
    port_map->RegisterDevice(addr, 2, &s->fIo, DEVIO_SIZE8);
}

MC146818::~MC146818() = default;

void MC146818::SetNvram(int index, uint8_t val)
{
    fState->cmos_data[index] = val;
}

static bool is_time_register(int index)
{
    return index <= RTC_YEAR && index != RTC_SECONDS_ALARM &&
        index != RTC_MINUTES_ALARM && index != RTC_HOURS_ALARM;
}

void CMOSState::Write(uint32_t offset, uint32_t data, int size_log2)
{
    CMOSState *s = this;

    if (offset == 0) {
        s->cmos_index = get_bits(data, 0, 7);
        return;
    }
#ifdef DEBUG_CMOS
    printf("cmos_write: reg=0x%02x val=0x%02x\n", s->cmos_index, data);
#endif
    bool was_frozen = cmos_frozen(s);
    if (is_time_register(s->cmos_index) || s->cmos_index == RTC_CENTURY) {
        /* written into what is shown now, which becomes the time unless
           updates are held */
        if (!was_frozen)
            cmos_update_time(s);
        s->cmos_data[s->cmos_index] = data;
        if (!was_frozen)
            cmos_commit_time(s, 0);
        return;
    }
    switch(s->cmos_index) {
    case RTC_REG_A: {
        bool reset = get_bits(s->cmos_data[RTC_REG_A], REG_A_DV_POS + 1, 2)
            == 3;
        if (!was_frozen)
            cmos_update_time(s);
        s->cmos_data[RTC_REG_A] = (data & ~REG_A_UIP) |
            (s->cmos_data[RTC_REG_A] & REG_A_UIP);
        bool running = get_bits(data, REG_A_DV_POS, 3) == REG_A_DV_RUN;
        /* out of reset, the first update comes half a second later */
        if (reset && running && !cmos_frozen(s))
            cmos_commit_time(s, US_PER_SEC / 2);
        cmos_update_timer(s);
        break;
    }
    case RTC_REG_B:
        if (!was_frozen)
            cmos_update_time(s);
        /* SET stops the updates and clears UIE */
        if (data & REG_B_SET)
            data &= ~REG_B_UIE;
        s->cmos_data[RTC_REG_B] = data;
        if (was_frozen && !cmos_frozen(s))
            cmos_commit_time(s, 0);
        cmos_update_timer(s);
        cmos_update_irq_line(s, true);
        break;
    case RTC_REG_C:
    case RTC_REG_D:
        /* read only */
        break;
    default:
        s->cmos_data[s->cmos_index] = data;
        break;
    }
}

uint32_t CMOSState::Read(uint32_t offset, int size_log2)
{
    CMOSState *s = this;
    int ret;

    /* the index register cannot be read */
    if (offset == 0)
        return 0xff;

    if (is_time_register(s->cmos_index)) {
        if (!cmos_frozen(s))
            cmos_update_time(s);
        ret = s->cmos_data[s->cmos_index];
    } else if (s->cmos_index == RTC_REG_A) {
        /* UIP from shortly before the seconds change to the end of the
           update cycle */
        int64_t into = cmos_time_us(s) % US_PER_SEC;
        if (into < 0)
            into += US_PER_SEC;
        bool uip = !cmos_frozen(s) &&
            (into >= US_PER_SEC - UIP_BEFORE_US || into < UPDATE_CYCLE_US);
        ret = (s->cmos_data[RTC_REG_A] & ~REG_A_UIP) | (uip ? REG_A_UIP : 0);
    } else if (s->cmos_index == RTC_REG_C) {
        /* The periodic flag is set at its rate whether or not it may
           interrupt: by the interrupt's timer while it may, so that a
           period is flagged once, and otherwise by the periods since C
           was last read. */
        uint32_t period = cmos_period(s);
        uint32_t now = cmos_get_timer();
        if (s->irq_period != 0) {
            if ((int32_t)(now - s->irq_timeout) >= 0) {
                s->cmos_data[RTC_REG_C] |= REG_C_PF;
                s->irq_timeout += ((now - s->irq_timeout) / s->irq_period +
                                   1) * s->irq_period;
            }
        } else if (period != 0 && now / period != s->pf_read / period) {
            s->cmos_data[RTC_REG_C] |= REG_C_PF;
        }
        cmos_update_flags(s);
        cmos_update_irq_line(s, false);
        ret = s->cmos_data[RTC_REG_C];
        s->cmos_data[RTC_REG_C] = 0x00;
        s->pf_read = now;
        s->irq->Set(0);
    } else {
        ret = s->cmos_data[s->cmos_index];
    }
#ifdef DEBUG_CMOS
    printf("cmos_read: reg=0x%02x val=0x%02x\n", s->cmos_index, ret);
#endif
    return ret;
}

int64_t MC146818::DelayUs()
{
    return cmos_delay_us(fState.get());
}

int64_t MC146818::RunTimers(bool deliver)
{
    return cmos_update_irq(fState.get(), deliver);
}
