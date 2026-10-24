/*
 * 8254 PIT
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
#include "i8254.h"

#define PIT_FREQ 1193182

/* the access field of a control word */
#define ACCESS_LATCH 0
#define ACCESS_LSB   1
#define ACCESS_MSB   2
#define ACCESS_WORD  3

/* port B */
#define PORT_B_GATE2     bit_at(0)
#define PORT_B_ENABLES   0x0c      /* the SERR# and IOCHK# enables */
/* the refresh request, bit 4, toggles every 15.085 us */
#define REFRESH_NS 15085

//#define DEBUG_PIT


/* One counter, by the 8254 data sheet. Time is counted in PIT clocks from
   'count_load_time' while it runs; while the gate holds it, or before a
   trigger, 'held' is how far it got. */
typedef struct {
    uint32_t count;         /* 1 to 65536, or to 10000 in BCD */
    uint8_t access;
    uint8_t mode;           /* 0 to 5 */
    uint8_t bcd;
    uint8_t gate;
    bool loaded;            /* a count was written since the control word */
    bool running;
    bool triggered;         /* modes 1 and 5: the gate rose since the count */
    bool null_count;        /* a control word, and no count loaded since */
    bool write_msb;         /* a word's LSB was written, the MSB is next */
    bool read_msb;          /* a word's LSB was read, the MSB is next */
    uint16_t write_lsb;
    bool count_latched;
    bool latch_msb;         /* the latched LSB was read */
    uint16_t latched_count;
    bool status_latched;
    uint8_t latched_status;
    int64_t count_load_time;
    int64_t held;
    int64_t last_irq_time;
} PITChannel;

struct PITState {
    PITChannel pit_channels[3] {};
    uint8_t speaker_data_on = 0;
    uint8_t port_b_enables = 0;
    IRQSignal *irq = nullptr;

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t val, int size_log2);
    uint32_t SpeakerRead(uint32_t offset, int size_log2);
    void SpeakerWrite(uint32_t offset, uint32_t val, int size_log2);

    DeviceIOAdapter<PITState, &PITState::Read, &PITState::Write> fIo {*this};
    DeviceIOAdapter<PITState, &PITState::SpeakerRead,
                    &PITState::SpeakerWrite> fSpeakerIo {*this};
};

/* unit = PIT frequency  */
static int64_t pit_get_time()
{
    uint64_t us = host_monotonic_us();

    return (us / 1000000) * PIT_FREQ + (us % 1000000) * PIT_FREQ / 1000000;
}

static uint32_t from_bcd(uint32_t v)
{
    return get_bits(v, 12, 4) * 1000 + get_bits(v, 8, 4) * 100 +
        get_bits(v, 4, 4) * 10 + get_bits(v, 0, 4);
}

static uint32_t to_bcd(uint32_t v)
{
    return (v / 1000 % 10) << 12 | (v / 100 % 10) << 8 | (v / 10 % 10) << 4 |
        v % 10;
}

/* A count as written, where 0 is the largest there is. Modes 1 and 5 wait
   for the gate to rise; the others count while it is high. */
static void pit_load_count(PITChannel *s, uint32_t val, int64_t now)
{
    if (s->bcd)
        val = from_bcd(val);
    if (val == 0)
        val = s->bcd ? 10000 : 0x10000;
    s->count = val;
    s->loaded = true;
    s->null_count = false;
    s->triggered = false;
    s->count_load_time = now;
    s->held = 0;
    s->last_irq_time = 0;
    s->running = s->mode != 1 && s->mode != 5 && s->gate;
}

/* Clocks counted since the count was loaded or triggered. */
static int64_t pit_elapsed(const PITChannel *s, int64_t now)
{
    return s->running ? now - s->count_load_time : s->held;
}

/* What the counting element holds, in binary. */
static uint32_t pit_get_count(const PITChannel *s, int64_t now)
{
    int64_t d = pit_elapsed(s, now);
    int64_t wrap = s->bcd ? 10000 : 0x10000;

    if (!s->loaded)
        return 0;
    switch(s->mode) {
    case 1:
    case 5:
        if (!s->triggered)
            return s->count % wrap;
        /* fall through */
    case 0:
    case 4:
        /* on past the terminal count, wrapping */
        return ((s->count - d) % wrap + wrap) % wrap;
    case 2:
        return s->count - d % s->count;
    default: {
        /* mode 3 counts down by two, twice a period */
        uint32_t half = (s->count + 1) / 2;
        uint32_t into = d % s->count;
        if (into >= half)
            into -= half;
        return (s->count - 2 * into) & ~1u;
    }
    }
}

static int pit_get_out(const PITChannel *s, int64_t now)
{
    int64_t d = pit_elapsed(s, now);

    if (!s->loaded)
        return s->mode != 0;
    switch(s->mode) {
    default:
    case 0:
        /* low from the control word to the terminal count */
        return d >= s->count;
    case 1:
        /* a one-shot pulse, low from the trigger to the terminal count */
        return !s->triggered || d >= s->count;
    case 2:
        /* low for one clock before each reload */
        return !s->running || d % s->count != s->count - 1;
    case 3:
        /* a square wave, high for the first half */
        return !s->running || d % s->count < (s->count + 1) / 2;
    case 4:
        /* a strobe, low for one clock at the terminal count */
        return d != s->count;
    case 5:
        return !s->triggered || d != s->count;
    }
}

static uint8_t pit_status(const PITChannel *s, int64_t now)
{
    return pit_get_out(s, now) << 7 | s->null_count << 6 | s->access << 4 |
        s->mode << 1 | s->bcd;
}

static void pit_latch_count(PITChannel *s, int64_t now)
{
    /* a latch holds until it is read */
    if (s->count_latched)
        return;
    uint32_t v = pit_get_count(s, now);
    s->latched_count = s->bcd ? to_bcd(v) : v;
    s->count_latched = true;
    s->latch_msb = false;
}

/* The gate: modes 0 and 4 count only while it is high; modes 2 and 3 hold
   their count with it low, output high, and start over as it rises; modes
   1 and 5 start, or start again, as it rises. */
static void pit_set_gate(PITChannel *s, int level, int64_t now)
{
    level = !!level;
    if (level == s->gate)
        return;
    s->gate = level;
    if (!s->loaded)
        return;
    switch(s->mode) {
    case 0:
    case 4:
        if (level) {
            s->count_load_time = now - s->held;
            s->running = true;
        } else {
            s->held = now - s->count_load_time;
            s->running = false;
        }
        break;
    default:
        if (level) {
            s->count_load_time = now;
            s->last_irq_time = 0;
            s->running = true;
            s->triggered = true;
        } else if (s->running && (s->mode == 2 || s->mode == 3)) {
            s->held = now - s->count_load_time;
            s->running = false;
        }
        break;
    }
}

I8254::I8254(PhysMemoryMap *port_map, int addr0, int addr1, IRQSignal *irq)
{
    std::unique_ptr<PITState> &s = fState;
    int64_t now = pit_get_time();

    s = std::make_unique<PITState>();
    s->irq = irq;
    /* Counting from reset, as firmware leaves them: square waves, the gates
       of channels 0 and 1 tied high, channel 2's on port B. */
    for(int i = 0; i < 3; i++) {
        PITChannel *pc = &s->pit_channels[i];
        pc->mode = 3;
        pc->access = ACCESS_WORD;
        pc->gate = i != 2;
        pit_load_count(pc, 0, now);
    }

    port_map->RegisterDevice(addr0, 4, &s->fIo, DEVIO_SIZE8);
    port_map->RegisterDevice(addr1, 1, &s->fSpeakerIo, DEVIO_SIZE8);
}

I8254::~I8254() = default;

/* Read-back: bit 5 clear latches the counts, bit 4 clear the status, of
   the channels bits 1 to 3 select. */
static void pit_read_back(PITState *pit, uint32_t val, int64_t now)
{
    for (int i = 0; i < 3; i++) {
        PITChannel *s = &pit->pit_channels[i];
        if (!get_bit(val, 1 + i))
            continue;
        if (!get_bit(val, 4) && !s->status_latched) {
            s->latched_status = pit_status(s, now);
            s->status_latched = true;
        }
        if (!get_bit(val, 5))
            pit_latch_count(s, now);
    }
}

void PITState::Write(uint32_t offset, uint32_t val, int size_log2)
{
    int64_t now = pit_get_time();
    int addr = offset & 3;
    PITChannel *s;

#ifdef DEBUG_PIT
    printf("pit_write: off=%d val=0x%02x\n", addr, val);
#endif
    if (addr == 3) {
        int channel = get_bits(val, 6, 2);
        if (channel == 3) {
            pit_read_back(this, val, now);
            return;
        }
        s = &pit_channels[channel];
        int access = get_bits(val, 4, 2);
        if (access == ACCESS_LATCH) {
            pit_latch_count(s, now);
            return;
        }
        /* A control word starts the counter over, waiting for a count.
           Modes 6 and 7 are 2 and 3. */
        s->access = access;
        s->mode = get_bits(val, 1, 3);
        if (s->mode >= 6)
            s->mode -= 4;
        s->bcd = val & 1;
        s->loaded = false;
        s->running = false;
        s->null_count = true;
        s->write_msb = false;
        s->read_msb = false;
        s->count_latched = false;
        s->status_latched = false;
        return;
    }

    s = &pit_channels[addr];
    switch(s->access) {
    case ACCESS_LSB:
        pit_load_count(s, val, now);
        break;
    case ACCESS_MSB:
        pit_load_count(s, val << 8, now);
        break;
    default:
        if (!s->write_msb) {
            s->write_lsb = val;
            s->write_msb = true;
            /* in mode 0 the first byte stops the count */
            if (s->mode == 0 && s->running) {
                s->held = now - s->count_load_time;
                s->running = false;
            }
        } else {
            s->write_msb = false;
            pit_load_count(s, concat_bits<uint32_t>(val, s->write_lsb, 8),
                           now);
        }
        break;
    }
}

uint32_t PITState::Read(uint32_t offset, int size_log2)
{
    int64_t now = pit_get_time();
    int addr = offset & 3;
    PITChannel *s;
    uint32_t ret;

    /* the control word register cannot be read */
    if (addr == 3)
        return 0xff;

    s = &pit_channels[addr];
    if (s->status_latched) {
        s->status_latched = false;
        ret = s->latched_status;
    } else {
        bool latched = s->count_latched;
        uint32_t count;
        if (latched)
            count = s->latched_count;
        else if (s->bcd)
            count = to_bcd(pit_get_count(s, now));
        else
            count = pit_get_count(s, now);
        bool *msb = latched ? &s->latch_msb : &s->read_msb;
        bool high;
        switch(s->access) {
        case ACCESS_LSB:
            high = false;
            break;
        case ACCESS_MSB:
            high = true;
            break;
        default:
            high = *msb;
            *msb = !*msb;
            break;
        }
        ret = get_bits(count, high ? 8 : 0, 8);
        /* a latch is spent once its bytes are read */
        if (latched && (s->access != ACCESS_WORD || high))
            s->count_latched = false;
    }
#ifdef DEBUG_PIT
    printf("pit_read: off=%d val=0x%02x\n", addr, ret);
#endif
    return ret;
}

void PITState::SpeakerWrite(uint32_t offset, uint32_t val, int size_log2)
{
    speaker_data_on = get_bit(val, 1);
    port_b_enables = val & PORT_B_ENABLES;
    pit_set_gate(&pit_channels[2], val & PORT_B_GATE2, pit_get_time());
}

uint32_t PITState::SpeakerRead(uint32_t offset, int size_log2)
{
    PITChannel *s = &pit_channels[2];
    uint64_t refresh = host_monotonic_us() * 1000 / REFRESH_NS;

    return s->gate | speaker_data_on << 1 | port_b_enables |
        (refresh & 1) << 4 | pit_get_out(s, pit_get_time()) << 5;
}

/* Channel 0's ticks until its next interrupt, which comes as its output
   rises, or INT64_MAX when none will. */
static int64_t pit_next_ticks(PITChannel *s, int64_t now)
{
    if (!s->running)
        return INT64_MAX;
    int64_t d = now - s->count_load_time;

    switch(s->mode) {
    default:
    case 0:
    case 1:
    case 4:
    case 5:
        return s->last_irq_time == 0 ? s->count - d : INT64_MAX;
    case 2:
    case 3:
        return s->last_irq_time + s->count - d;
    }
}

/* The microseconds until channel 0's next interrupt, rounded up, or -1. */
static int64_t pit_delay_us(PITState *pit)
{
    int64_t ticks = pit_next_ticks(&pit->pit_channels[0], pit_get_time());

    if (ticks == INT64_MAX)
        return -1;
    if (ticks <= 0)
        return 0;
    return (ticks * 1000000 + PIT_FREQ - 1) / PIT_FREQ;
}

/* Raises the IRQ if it is due and returns the delay in us until the next
   one, or -1. Without 'deliver' the line is someone else's: the counting
   goes on, the interrupts are dropped. */
static int64_t pit_update_irq(PITState *pit, bool deliver)
{
    PITChannel *s = &pit->pit_channels[0];

    if (s->running) {
        int64_t d = pit_get_time() - s->count_load_time;
        bool fire = false;
        switch(s->mode) {
        default:
        case 0:
        case 1:
        case 4:
        case 5:
            if (s->last_irq_time == 0 && s->count <= d) {
                fire = true;
                s->last_irq_time = d;
            }
            break;
        case 2:
        case 3:
            if (s->last_irq_time + s->count <= d) {
                fire = true;
                s->last_irq_time += s->count;
            }
            break;
        }
        if (fire && deliver) {
            pit->irq->Set(1);
            pit->irq->Set(0);
        }
    }
    return pit_delay_us(pit);
}

int64_t I8254::DelayUs()
{
    return pit_delay_us(fState.get());
}

int64_t I8254::RunTimers(bool deliver)
{
    return pit_update_irq(fState.get(), deliver);
}
