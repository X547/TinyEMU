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

#define RW_STATE_LSB 0
#define RW_STATE_MSB 1
#define RW_STATE_WORD0 2
#define RW_STATE_WORD1 3
#define RW_STATE_LATCHED_WORD0 4
#define RW_STATE_LATCHED_WORD1 5

//#define DEBUG_PIT


typedef struct {
    uint32_t count;
    uint32_t latched_count;
    uint8_t rw_state;
    uint8_t mode;
    uint8_t bcd;
    uint8_t gate;
    int64_t count_load_time;
    int64_t last_irq_time;
} PITChannel;

struct PITState {
    PITChannel pit_channels[3] {};
    uint8_t speaker_data_on = 0;
    IRQSignal *irq = nullptr;

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t val, int size_log2);
    uint32_t SpeakerRead(uint32_t offset, int size_log2);
    void SpeakerWrite(uint32_t offset, uint32_t val, int size_log2);

    DeviceIOAdapter<PITState, &PITState::Read, &PITState::Write> fIo {*this};
    DeviceIOAdapter<PITState, &PITState::SpeakerRead,
                    &PITState::SpeakerWrite> fSpeakerIo {*this};
};

static void pit_load_count(PITChannel *pc, int val);

I8254::I8254(PhysMemoryMap *port_map, int addr0, int addr1, IRQSignal *irq)
{
    std::unique_ptr<PITState> &s = fState;
    PITChannel *pc;
    int i;

    s = std::make_unique<PITState>();

    s->irq = irq;
    
    for(i = 0; i < 3; i++) {
        pc = &s->pit_channels[i];
        pc->mode = 3;
        pc->gate = (i != 2) >> 0;
        pit_load_count(pc, 0);
    }
    s->speaker_data_on = 0;

    port_map->RegisterDevice(addr0, 4, &s->fIo, DEVIO_SIZE8);
    port_map->RegisterDevice(addr1, 1, &s->fSpeakerIo, DEVIO_SIZE8);
}

I8254::~I8254() = default;

/* unit = PIT frequency  */
static int64_t pit_get_time()
{
    uint64_t us = host_monotonic_us();

    return (us / 1000000) * PIT_FREQ + (us % 1000000) * PIT_FREQ / 1000000;
}

static uint32_t pit_get_count(PITChannel *pc)
{
    uint32_t counter;
    uint64_t d;
    
    d = pit_get_time() - pc->count_load_time;
    switch(pc->mode) {
    case 0:
    case 1:
    case 4:
    case 5:
        counter = get_bits(pc->count - d, 0, 16);
        break;
    default:
        counter = pc->count - (d % pc->count);
        break;
    }
    return counter;
}

/* get pit output bit */
static int pit_get_out(PITChannel *pc)
{
    int out;
    int64_t d;
    
    d = pit_get_time() - pc->count_load_time;
    switch(pc->mode) {
    default:
    case 0:
        out = (d >= pc->count) >> 0;
        break;
    case 1:
        out = (d < pc->count) >> 0;
        break;
    case 2:
        /* mode used by Linux */
        if ((d % pc->count) == 0 && d != 0)
            out = 1;
        else
            out = 0;
        break;
    case 3:
        out = ((d % pc->count) < (pc->count >> 1)) >> 0;
        break;
    case 4:
    case 5:
        out = (d == pc->count) >> 0;
        break;
    }
    return out;
}

static void pit_load_count(PITChannel *s, int val)
{
    if (val == 0)
        val = 0x10000;
    s->count_load_time = pit_get_time();
    s->last_irq_time = 0;
    s->count = val;
}

void PITState::Write(uint32_t offset, uint32_t val, int size_log2)
{
    PITState *pit = this;
    int channel, access, addr;
    PITChannel *s;

    addr = offset & 3;
#ifdef DEBUG_PIT
    printf("pit_write: off=%d val=0x%02x\n", addr, val);
#endif
    if (addr == 3) {
        channel = val >> 6;
        if (channel == 3)
            return;
        s = &pit->pit_channels[channel];
        access = get_bits(val, 4, 2);
        switch(access) {
        case 0:
            s->latched_count = pit_get_count(s);
            s->rw_state = RW_STATE_LATCHED_WORD0;
            break;
        default:
            s->mode = get_bits(val, 1, 3);
            s->bcd = val & 1;
            s->rw_state = access - 1 +  RW_STATE_LSB;
            break;
        }
    } else {
        s = &pit->pit_channels[addr];
        switch(s->rw_state) {
        case RW_STATE_LSB:
            pit_load_count(s, val);
            break;
        case RW_STATE_MSB:
            pit_load_count(s, val << 8);
            break;
        case RW_STATE_WORD0:
        case RW_STATE_WORD1:
            if (s->rw_state & 1) {
                pit_load_count(s,
                               concat_bits<uint32_t>(val, s->latched_count, 8));
            } else {
                s->latched_count = val;
            }
            s->rw_state ^= 1;
            break;
        }
    }
}

uint32_t PITState::Read(uint32_t offset, int size_log2)
{
    PITState *pit = this;
    PITChannel *s;
    int ret, count, addr;
    
    addr = offset & 3;
    if (addr == 3)
        return 0xff;

    s = &pit->pit_channels[addr];
    switch(s->rw_state) {
    case RW_STATE_LSB:
    case RW_STATE_MSB:
    case RW_STATE_WORD0:
    case RW_STATE_WORD1:
        count = pit_get_count(s);
        if (s->rw_state & 1)
            ret = get_bits(count, 8, 8);
        else
            ret = get_bits(count, 0, 8);
        if (s->rw_state & 2)
            s->rw_state ^= 1;
        break;
    default:
    case RW_STATE_LATCHED_WORD0:
    case RW_STATE_LATCHED_WORD1:
        if (s->rw_state & 1)
            ret = s->latched_count >> 8;
        else
            ret = get_bits(s->latched_count, 0, 8);
        s->rw_state ^= 1;
        break;
    }
#ifdef DEBUG_PIT
    printf("pit_read: off=%d val=0x%02x\n", addr, ret);
#endif
    return ret;
}

void PITState::SpeakerWrite(uint32_t offset, uint32_t val, int size_log2)
{
    PITState *pit = this;
    pit->speaker_data_on = get_bit(val, 1);
    pit->pit_channels[2].gate = val & 1;
}

uint32_t PITState::SpeakerRead(uint32_t offset, int size_log2)
{
    PITState *pit = this;
    PITChannel *s;
    int out, val;

    s = &pit->pit_channels[2];
    out = pit_get_out(s);
    val = (pit->speaker_data_on << 1) | s->gate | (out << 5);
#ifdef DEBUG_PIT
    //    console.log("speaker_read: addr=" + toHex2(addr) + " val=" + toHex2(val));
#endif
    return val;
}

/* Channel 0's ticks until its next interrupt, which may be due already, or
   INT64_MAX once a one-shot count has fired. */
static int64_t pit_next_ticks(PITChannel *s)
{
    int64_t d = pit_get_time() - s->count_load_time;

    switch(s->mode) {
    default:
    case 0:
    case 1:
    case 4:
    case 5:
        return s->last_irq_time == 0 ? s->count - d : INT64_MAX;
    case 2: /* mode used by Linux */
    case 3:
        return s->last_irq_time + s->count - d;
    }
}

/* The microseconds until channel 0's next interrupt, rounded up, or -1. */
static int64_t pit_delay_us(PITState *pit)
{
    int64_t ticks = pit_next_ticks(&pit->pit_channels[0]);

    if (ticks == INT64_MAX)
        return -1;
    if (ticks <= 0)
        return 0;
    return (ticks * 1000000 + PIT_FREQ - 1) / PIT_FREQ;
}

/* set the IRQ if necessary and return the delay in us until the next
   IRQ, or -1. Note: The code does not handle all the PIT configurations.
   Without 'deliver' the line is someone else's: the counting goes on, the
   interrupts are dropped. */
static int64_t pit_update_irq(PITState *pit, bool deliver)
{
    PITChannel *s = &pit->pit_channels[0];
    int64_t d = pit_get_time() - s->count_load_time;

    switch(s->mode) {
    default:
    case 0:
    case 1:
    case 4:
    case 5:
        if (s->last_irq_time == 0 && s->count <= d) {
            if (deliver) {
                pit->irq->Set(1);
                pit->irq->Set(0);
            }
            s->last_irq_time = d;
        }
        break;
    case 2: /* mode used by Linux */
    case 3:
        if (s->last_irq_time + s->count <= d) {
            if (deliver) {
                pit->irq->Set(1);
                pit->irq->Set(0);
            }
            s->last_irq_time += s->count;
        }
        break;
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
