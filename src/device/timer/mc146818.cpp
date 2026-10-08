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

#define REG_A_UIP 0x80

#define REG_B_SET 0x80
#define REG_B_PIE 0x40
#define REG_B_AIE 0x20
#define REG_B_UIE 0x10

struct CMOSState {
    uint8_t cmos_index = 0;
    uint8_t cmos_data[128] {};
    IRQSignal *irq = nullptr;
    bool use_local_time = false;
    /* the periodic irq, in CMOS_FREQ ticks; no period while it is off */
    uint32_t irq_timeout = 0;
    uint32_t irq_period = 0;

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t data, int size_log2);

    DeviceIOAdapter<CMOSState, &CMOSState::Read, &CMOSState::Write> fIo {*this};
};

static int to_bcd(CMOSState *s, unsigned int a)
{
    if (get_bit(s->cmos_data[RTC_REG_B], 2)) {
        return a;
    } else {
        return set_bits(a % 10, 4, 4, a / 10);
    }
}

static void cmos_update_time(CMOSState *s, bool set_century)
{
    HostDateTime dt;
    int val;

    host_date_time(&dt, s->use_local_time);

    s->cmos_data[RTC_SECONDS] = to_bcd(s, dt.second);
    s->cmos_data[RTC_MINUTES] = to_bcd(s, dt.minute);
    if (get_bit(s->cmos_data[RTC_REG_B], 1)) {
        s->cmos_data[RTC_HOURS] = to_bcd(s, dt.hour);
    } else {
        s->cmos_data[RTC_HOURS] = to_bcd(s, dt.hour % 12);
        if (dt.hour >= 12)
            s->cmos_data[RTC_HOURS] |= 0x80;
    }
    s->cmos_data[RTC_DAY_OF_WEEK] = to_bcd(s, dt.weekday);
    s->cmos_data[RTC_DAY_OF_MONTH] = to_bcd(s, dt.day);
    s->cmos_data[RTC_MONTH] = to_bcd(s, dt.month);
    s->cmos_data[RTC_YEAR] = to_bcd(s, dt.year % 100);

    if (set_century) {
        /* not set by the hardware, but easier to do it here */
        val = to_bcd(s, dt.year / 100);
        s->cmos_data[0x32] = val;
        s->cmos_data[0x37] = val;
    }

    /* update in progress flag: 8/32768 seconds after change */
    if (dt.usec < 244) {
        s->cmos_data[RTC_REG_A] |= REG_A_UIP;
    } else {
        s->cmos_data[RTC_REG_A] &= ~REG_A_UIP;
    }
}

MC146818::MC146818(PhysMemoryMap *port_map, int addr, IRQSignal *irq,
                   bool use_local_time)
{
    std::unique_ptr<CMOSState> &s = fState;

    s = std::make_unique<CMOSState>();
    s->use_local_time = use_local_time;
    
    s->cmos_index = 0;

    s->cmos_data[RTC_REG_A] = 0x26;
    s->cmos_data[RTC_REG_B] = 0x02;
    s->cmos_data[RTC_REG_C] = 0x00;
    s->cmos_data[RTC_REG_D] = 0x80;

    cmos_update_time(s.get(), true);
    
    s->irq = irq;
    
    port_map->RegisterDevice(addr, 2, &s->fIo, DEVIO_SIZE8);
}

MC146818::~MC146818() = default;

void MC146818::SetNvram(int index, uint8_t val)
{
    fState->cmos_data[index] = val;
}

#define CMOS_FREQ 32768

static uint32_t cmos_get_timer(CMOSState *s)
{
    uint64_t us = host_monotonic_us();

    return (uint32_t)(us / 1000000) * CMOS_FREQ +
        ((us % 1000000) * CMOS_FREQ / 1000000);
}

static void cmos_update_timer(CMOSState *s)
{
    int period_code;

    period_code = get_bits(s->cmos_data[RTC_REG_A], 0, 4);
    if ((s->cmos_data[RTC_REG_B] & REG_B_PIE) &&
        period_code != 0) {
        if (period_code <= 2)
            period_code += 7;
        s->irq_period = 1 << (period_code - 1);
        s->irq_timeout = (cmos_get_timer(s) + s->irq_period) &
            ~(s->irq_period - 1);
    } else {
        /* off, or rate 0, which stops it on the part */
        s->irq_period = 0;
    }
}

/* The microseconds until the next periodic interrupt, rounded up, or -1. */
static int64_t cmos_delay_us(CMOSState *s)
{
    if (s->irq_period == 0)
        return -1;
    int32_t ticks = s->irq_timeout - cmos_get_timer(s);
    if (ticks <= 0)
        return 0;
    return ((int64_t)ticks * 1000000 + CMOS_FREQ - 1) / CMOS_FREQ;
}

/* Raises the periodic interrupt if it is due and returns the microseconds
   until the next one, or -1. Periods that ran out while nobody looked are
   one interrupt. Without 'deliver' the flags are set but the line is left
   alone. */
static int64_t cmos_update_irq(CMOSState *s, bool deliver)
{
    if (s->irq_period != 0) {
        uint32_t now = cmos_get_timer(s);
        if ((int32_t)(now - s->irq_timeout) >= 0) {
            s->cmos_data[RTC_REG_C] |= 0xc0;
            if (deliver)
                s->irq->Set(1);
            s->irq_timeout += ((now - s->irq_timeout) / s->irq_period + 1) *
                s->irq_period;
        }
    }
    return cmos_delay_us(s);
}

void CMOSState::Write(uint32_t offset, uint32_t data, int size_log2)
{
    CMOSState *s = this;

    if (offset == 0) {
        s->cmos_index = get_bits(data, 0, 7);
    } else {
#ifdef DEBUG_CMOS
        printf("cmos_write: reg=0x%02x val=0x%02x\n", s->cmos_index, data);
#endif
        switch(s->cmos_index) {
        case RTC_REG_A:
            s->cmos_data[RTC_REG_A] = (data & ~REG_A_UIP) |
                (s->cmos_data[RTC_REG_A] & REG_A_UIP);
            cmos_update_timer(s);
            break;
        case RTC_REG_B:
            s->cmos_data[s->cmos_index] = data;
            cmos_update_timer(s);
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
}

uint32_t CMOSState::Read(uint32_t offset, int size_log2)
{
    CMOSState *s = this;
    int ret;

    if (offset == 0) {
        return 0xff;
    } else {
        switch(s->cmos_index) {
        case RTC_SECONDS:
        case RTC_MINUTES:
        case RTC_HOURS:
        case RTC_DAY_OF_WEEK:
        case RTC_DAY_OF_MONTH:
        case RTC_MONTH:
        case RTC_YEAR:
        case RTC_REG_A:
            cmos_update_time(s, false);
            ret = s->cmos_data[s->cmos_index];
            break;
        case RTC_REG_C:
            ret = s->cmos_data[s->cmos_index];
            s->cmos_data[RTC_REG_C] = 0x00;
            s->irq->Set(0);
            break;
        default:
            ret = s->cmos_data[s->cmos_index];
        }
#ifdef DEBUG_CMOS
        printf("cmos_read: reg=0x%02x val=0x%02x\n", s->cmos_index, ret);
#endif
        return ret;
    }
}

int64_t MC146818::DelayUs()
{
    return cmos_delay_us(fState.get());
}

int64_t MC146818::RunTimers(bool deliver)
{
    return cmos_update_irq(fState.get(), deliver);
}
