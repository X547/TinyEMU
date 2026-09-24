/*
 * PC emulator
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
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include <algorithm>

#include "bits.h"
#include "cutils.h"
#include "host_time.h"
#include "host_x86_hypervisor.h"
#include "iomem.h"
#include "devices.h"
#include "simplefb.h"
#include "virtio.h"
#include "uart.h"
#include "x86_cpu.h"
#include "machine.h"
#include "pci.h"
#include "pci_host_i440fx.h"
#include "pc_acpi.h"
#include "ioapic.h"

//#define DEBUG_BIOS
//#define DUMP_IOPORT

/***********************************************************/
/* cmos emulation */

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
    /* used for the periodic irq */
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

std::unique_ptr<CMOSState> cmos_init(PhysMemoryMap *port_map, int addr,
                                     IRQSignal *irq, bool use_local_time)
{
    std::unique_ptr<CMOSState> s;
    
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
    return s;
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
    }
}

/* XXX: could return a delay, but we don't need high precision
   (Windows 2000 uses it for delay calibration) */
static void cmos_update_irq(CMOSState *s)
{
    uint32_t d;
    if (s->cmos_data[RTC_REG_B] & REG_B_PIE) {
        d = cmos_get_timer(s) - s->irq_timeout;
        if ((int32_t)d >= 0) {
            /* this is not what the real RTC does. Here we sent the IRQ
               immediately */
            s->cmos_data[RTC_REG_C] |= 0xc0;
            s->irq->Set(1);
            /* update for the next irq */
            s->irq_timeout += s->irq_period;
        }
    }
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

/***********************************************************/
/* 8259 pic emulation */

//#define DEBUG_PIC

/* Implemented by the cascade controller that owns the two 8259s. */
class PICUpdateTarget {
public:
    virtual ~PICUpdateTarget() = default;

    virtual void UpdatePICIRQ() = 0;
};

struct PICState {
    uint8_t last_irr; /* edge detection */
    uint8_t irr; /* interrupt request register */
    uint8_t imr; /* interrupt mask register */
    uint8_t isr; /* interrupt service register */
    uint8_t priority_add; /* used to compute irq priority */
    uint8_t irq_base;
    uint8_t read_reg_select;
    uint8_t special_mask;
    uint8_t init_state;
    uint8_t auto_eoi;
    uint8_t rotate_on_autoeoi;
    uint8_t init4; /* true if 4 byte init */
    uint8_t elcr; /* PIIX edge/trigger selection*/
    uint8_t elcr_mask;
    PICUpdateTarget *update_target;

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t val, int size_log2);
    uint32_t ElcrRead(uint32_t offset, int size_log2);
    void ElcrWrite(uint32_t offset, uint32_t val, int size_log2);

    DeviceIOAdapter<PICState, &PICState::Read, &PICState::Write> fIo {*this};
    DeviceIOAdapter<PICState, &PICState::ElcrRead,
                    &PICState::ElcrWrite> fElcrIo {*this};
};

static void pic_reset(PICState *s);

std::unique_ptr<PICState> pic_init(PhysMemoryMap *port_map, int port,
                                   int elcr_port,
                                   int elcr_mask,
                                   PICUpdateTarget *update_target)
{
    std::unique_ptr<PICState> s;

    s = std::make_unique<PICState>();
    s->elcr_mask = elcr_mask;
    s->update_target = update_target;
    port_map->RegisterDevice(port, 2, &s->fIo, DEVIO_SIZE8);
    port_map->RegisterDevice(elcr_port, 1, &s->fElcrIo, DEVIO_SIZE8);
    pic_reset(s.get());
    return s;
}

static void pic_reset(PICState *s)
{
    /* all 8 bit registers */
    s->last_irr = 0; /* edge detection */
    s->irr = 0; /* interrupt request register */
    s->imr = 0; /* interrupt mask register */
    s->isr = 0; /* interrupt service register */
    s->priority_add = 0; /* used to compute irq priority */
    s->irq_base = 0;
    s->read_reg_select = 0;
    s->special_mask = 0;
    s->init_state = 0;
    s->auto_eoi = 0;
    s->rotate_on_autoeoi = 0;
    s->init4 = 0; /* true if 4 byte init */
}

/* set irq level. If an edge is detected, then the IRR is set to 1 */
static void pic_set_irq1(PICState *s, int irq, int level)
{
    int mask;
    mask = bit_at(irq);
    if (s->elcr & mask) {
        /* level triggered */
        if (level) {
            s->irr |= mask;
            s->last_irr |= mask;
        } else {
            s->irr &= ~mask;
            s->last_irr &= ~mask;
        }
    } else {
        /* edge triggered */
        if (level) {
            if ((s->last_irr & mask) == 0)
                s->irr |= mask;
            s->last_irr |= mask;
        } else {
            s->last_irr &= ~mask;
        }
    }
}
    
/* The rotated offset of the highest priority input set in 'mask', or 8 if
   none is. Offset 0 is the highest priority one, as on the part: input 0 wins
   over input 7, and the rotation registers move which input offset 0 names. */
static int pic_get_priority(PICState *s, int mask)
{
    int priority;
    if (mask == 0)
        return 8;
    priority = 0;
    while ((mask & (1 << ((priority + s->priority_add) & 7))) == 0)
        priority++;
    return priority;
}

/* return the pic wanted interrupt. return -1 if none */
static int pic_get_irq(PICState *s)
{
    int mask, cur_priority, priority;

    mask = s->irr & ~s->imr;
    priority = pic_get_priority(s, mask);
    if (priority == 8)
        return -1;
    /* compute current priority */
    cur_priority = pic_get_priority(s, s->isr);
    if (priority < cur_priority) {
        /* higher priority found: an irq should be generated */
        return (priority + s->priority_add) & 7;
    } else {
        return -1;
    }
}
    
/* acknowledge interrupt 'irq' */
static void pic_intack(PICState *s, int irq)
{
    if (s->auto_eoi) {
        if (s->rotate_on_autoeoi)
            s->priority_add = (irq + 1) & 7;
    } else {
        s->isr = set_bit(s->isr, irq, true);
    }
    /* We don't clear a level sensitive interrupt here */
    if (!get_bit(s->elcr, irq))
        s->irr = set_bit(s->irr, irq, false);
}

void PICState::Write(uint32_t offset, uint32_t val, int size_log2)
{
    PICState *s = this;
    int priority, addr;
    
    addr = get_bit(offset, 0);
#ifdef DEBUG_PIC
    console.log("pic_write: addr=" + toHex2(addr) + " val=" + toHex2(val));
#endif
    if (addr == 0) {
        if (get_bit(val, 4)) {
            /* init */
            pic_reset(s);
            s->init_state = 1;
            s->init4 = get_bit(val, 0);
            if (get_bit(val, 1))
                abort(); /* "single mode not supported" */
            if (get_bit(val, 3))
                abort(); /* "level sensitive irq not supported" */
        } else if (get_bit(val, 3)) {
            if (get_bit(val, 1))
                s->read_reg_select = get_bit(val, 0);
            if (get_bit(val, 6)) {
                s->special_mask = get_bit(val, 5);
                /* which requests outrank what is in service has changed */
                s->update_target->UpdatePICIRQ();
            }
        } else {
            /* Ending an interrupt, or rotating the priorities, lets a
               request that was outranked through, so the processor is asked
               again once the command has been carried out. */
            switch(val) {
            case 0x00:
            case 0x80:
                s->rotate_on_autoeoi = get_bit(val, 7);
                break;
            case 0x20: /* end of interrupt */
            case 0xa0:
                priority = pic_get_priority(s, s->isr);
                if (priority < 8) {
                    s->isr &= ~(1 << ((priority + s->priority_add) & 7));
                }
                if (val == 0xa0)
                    s->priority_add = (s->priority_add + 1) & 7;
                break;
            case 0x60:
            case 0x61:
            case 0x62:
            case 0x63:
            case 0x64:
            case 0x65:
            case 0x66:
            case 0x67:
                priority = val & 7;
                s->isr = set_bit(s->isr, priority, false);
                break;
            case 0xc0:
            case 0xc1:
            case 0xc2:
            case 0xc3:
            case 0xc4:
            case 0xc5:
            case 0xc6:
            case 0xc7:
                s->priority_add = (val + 1) & 7;
                break;
            case 0xe0:
            case 0xe1:
            case 0xe2:
            case 0xe3:
            case 0xe4:
            case 0xe5:
            case 0xe6:
            case 0xe7:
                priority = val & 7;
                s->isr = set_bit(s->isr, priority, false);
                s->priority_add = (priority + 1) & 7;
                break;
            }
            s->update_target->UpdatePICIRQ();
        }
    } else {
        switch(s->init_state) {
        case 0:
            /* normal mode */
            s->imr = val;
            s->update_target->UpdatePICIRQ();
            break;
        case 1:
            s->irq_base = set_bits(val, 0, 3, 0);
            s->init_state = 2;
            break;
        case 2:
            if (s->init4) {
                s->init_state = 3;
            } else {
                s->init_state = 0;
            }
            break;
        case 3:
            s->auto_eoi = get_bit(val, 1);
            s->init_state = 0;
            break;
        }
    }
}

uint32_t PICState::Read(uint32_t offset, int size_log2)
{
    PICState *s = this;
    int addr, ret;

    addr = get_bit(offset, 0);
    if (addr == 0) {
        if (s->read_reg_select)
            ret = s->isr;
        else
            ret = s->irr;
    } else {
        ret = s->imr;
    }
#ifdef DEBUG_PIC
    console.log("pic_read: addr=" + toHex2(addr1) + " val=" + toHex2(ret));
#endif
    return ret;
}

void PICState::ElcrWrite(uint32_t offset, uint32_t val, int size_log2)
{
    PICState *s = this;
    s->elcr = val & s->elcr_mask;
}

uint32_t PICState::ElcrRead(uint32_t offset, int size_log2)
{
    PICState *s = this;
    return s->elcr;
}

/* Implemented by the machine: raises or lowers the CPU's INTR line. */
class CPUIRQTarget {
public:
    virtual ~CPUIRQTarget() = default;

    virtual void SetCPUIRQ(int level) = 0;
};

struct PIC2State: public IRQTarget, public PICUpdateTarget {
    std::unique_ptr<PICState> pics[2];
    int irq_requested = 0;
    CPUIRQTarget *cpu_irq_target = nullptr;
#if defined(DEBUG_PIC)
    uint8_t irq_level[16];
#endif
    IRQSignal *irqs = nullptr;

    void SetIRQ(int irq, int level) override;
    void UpdatePICIRQ() override;
};

std::unique_ptr<PIC2State> pic2_init(PhysMemoryMap *port_map, uint32_t addr0,
                                     uint32_t addr1,
                                     uint32_t elcr_addr0, uint32_t elcr_addr1,
                                     CPUIRQTarget *cpu_irq_target,
                                     IRQSignal *irqs)
{
    std::unique_ptr<PIC2State> s;
    int i;
    
    s = std::make_unique<PIC2State>();

    for(i = 0; i < 16; i++) {
        irqs[i].Init(s.get(), i);
    }
    s->cpu_irq_target = cpu_irq_target;
    s->pics[0] = pic_init(port_map, addr0, elcr_addr0, 0xf8, s.get());
    s->pics[1] = pic_init(port_map, addr1, elcr_addr1, 0xde, s.get());
    s->irq_requested = 0;
    return s;
}

void pic2_set_elcr(PIC2State *s, const uint8_t *elcr)
{
    int i;
    for(i = 0; i < 2; i++) {
        s->pics[i]->elcr = elcr[i] & s->pics[i]->elcr_mask;
    }
}

/* raise irq to CPU if necessary. must be called every time the active
   irq may change */
void PIC2State::UpdatePICIRQ()
{
    PIC2State *s = this;
    int irq2, irq;

    /* first look at slave pic */
    irq2 = pic_get_irq(s->pics[1].get());
    if (irq2 >= 0) {
        /* if irq request by slave pic, signal master PIC */
        pic_set_irq1(s->pics[0].get(), 2, 1);
        pic_set_irq1(s->pics[0].get(), 2, 0);
    }
    /* look at requested irq */
    irq = pic_get_irq(s->pics[0].get());
#if 0
    console.log("irr=" + toHex2(s->pics[0].irr) + " imr=" + toHex2(s->pics[0].imr) + " isr=" + toHex2(s->pics[0].isr) + " irq="+ irq);
#endif
    if (irq >= 0) {
        /* raise IRQ request on the CPU */
        s->cpu_irq_target->SetCPUIRQ(1);
    } else {
        /* lower irq */
        s->cpu_irq_target->SetCPUIRQ(0);
    }
}

void PIC2State::SetIRQ(int irq, int level)
{
    PIC2State *s = this;
#if defined(DEBUG_PIC)
    if (irq != 0 && level != s->irq_level[irq]) {
        console.log("pic_set_irq: irq=" + irq + " level=" + level);
        s->irq_level[irq] = level;
    }
#endif
    pic_set_irq1(s->pics[irq >> 3].get(), irq & 7, level);
    s->UpdatePICIRQ();
}

/* called from the CPU to get the hardware interrupt number */
static int pic2_get_hard_intno(PIC2State *s)
{
    int irq, irq2, intno;

    irq = pic_get_irq(s->pics[0].get());
    if (irq >= 0) {
        pic_intack(s->pics[0].get(), irq);
        if (irq == 2) {
            irq2 = pic_get_irq(s->pics[1].get());
            if (irq2 >= 0) {
                pic_intack(s->pics[1].get(), irq2);
            } else {
                /* spurious IRQ on slave controller */
                irq2 = 7;
            }
            intno = s->pics[1]->irq_base + irq2;
            irq = irq2 + 8;
        } else {
            intno = s->pics[0]->irq_base + irq;
        }
    } else {
        /* spurious IRQ on host controller */
        irq = 7;
        intno = s->pics[0]->irq_base + irq;
    }
    s->UpdatePICIRQ();

#if defined(DEBUG_PIC)
    if (irq != 0 && irq != 14)
        printf("pic_interrupt: irq=%d\n", irq);
#endif
    return intno;
}

/***********************************************************/
/* 8253 PIT emulation */

#define PIT_FREQ 1193182

#define RW_STATE_LSB 0
#define RW_STATE_MSB 1
#define RW_STATE_WORD0 2
#define RW_STATE_WORD1 3
#define RW_STATE_LATCHED_WORD0 4
#define RW_STATE_LATCHED_WORD1 5

//#define DEBUG_PIT


typedef struct PITState PITState;

typedef struct {
    PITState *pit_state;
    uint32_t count;
    uint32_t latched_count;
    uint8_t rw_state;
    uint8_t mode;
    uint8_t bcd;
    uint8_t gate;
    int64_t count_load_time;
    int64_t last_irq_time;
} PITChannel;

/* Implemented by the machine: the PIT counts in CPU ticks. */
class PITTickSource {
public:
    virtual ~PITTickSource() = default;

    virtual int64_t Ticks() = 0;
};

struct PITState {
    PITChannel pit_channels[3] {};
    uint8_t speaker_data_on = 0;
    PITTickSource *tick_source = nullptr;
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

std::unique_ptr<PITState> pit_init(PhysMemoryMap *port_map, int addr0,
                                   int addr1, IRQSignal *irq,
                                   PITTickSource *tick_source)
{
    std::unique_ptr<PITState> s;
    PITChannel *pc;
    int i;

    s = std::make_unique<PITState>();

    s->irq = irq;
    s->tick_source = tick_source;
    
    for(i = 0; i < 3; i++) {
        pc = &s->pit_channels[i];
        pc->pit_state = s.get();
        pc->mode = 3;
        pc->gate = (i != 2) >> 0;
        pit_load_count(pc, 0);
    }
    s->speaker_data_on = 0;

    port_map->RegisterDevice(addr0, 4, &s->fIo, DEVIO_SIZE8);
    port_map->RegisterDevice(addr1, 1, &s->fSpeakerIo, DEVIO_SIZE8);
    return s;
}

/* unit = PIT frequency  */
static int64_t pit_get_time(PITChannel *pc)
{
    PITState *s = pc->pit_state;
    return s->tick_source->Ticks();
}

static uint32_t pit_get_count(PITChannel *pc)
{
    uint32_t counter;
    uint64_t d;
    
    d = pit_get_time(pc) - pc->count_load_time;
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
    
    d = pit_get_time(pc) - pc->count_load_time;
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
    s->count_load_time = pit_get_time(s);
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

/* set the IRQ if necessary and return the delay in us until the next
   IRQ. Note: The code does not handle all the PIT configurations. */
static int64_t pit_update_irq(PITState *pit)
{
    PITChannel *s;
    int64_t d, delay;
    
    s = &pit->pit_channels[0];
    
    delay = PIT_FREQ; /* could be infinity delay */
    
    d = pit_get_time(s) - s->count_load_time;
    switch(s->mode) {
    default:
    case 0:
    case 1:
    case 4:
    case 5:
        if (s->last_irq_time == 0) {
            delay = s->count - d;
            if (delay <= 0) {
                pit->irq->Set(1);
                pit->irq->Set(0);
                s->last_irq_time = d;
            }
        }
        break;
    case 2: /* mode used by Linux */
    case 3:
        delay = s->last_irq_time + s->count - d;
        if (delay <= 0) {
            pit->irq->Set(1);
            pit->irq->Set(0);
            s->last_irq_time += s->count;
        }
        break;
    }

    if (delay <= 0)
        return 0;
    else
        return delay * 1000000 / PIT_FREQ;
}
    

#ifdef DEBUG_BIOS

#endif

class PCMachine;

/* With a hypervisor the host reads guest RAM directly and keeps the dirty
   log, so the memory map tells it about each mapping instead of managing
   them itself. */
class HypervisorPhysMemoryMap final: public PhysMemoryMap {
private:
    HostX86Hypervisor &fHypervisor;

    void MapRam(PhysMemoryRange *pr);

public:
    HypervisorPhysMemoryMap(HostX86Hypervisor &hypervisor):
        fHypervisor(hypervisor) {}
    ~HypervisorPhysMemoryMap() override;

    PhysMemoryRange *RegisterRam(uint64_t addr, uint64_t size,
                                 int devram_flags) override;
    const uint32_t *GetDirtyBits(PhysMemoryRange *pr) override;
    void SetRamAddr(PhysMemoryRange *pr, uint64_t addr, bool enabled) override;
};

/* With a hypervisor the interrupt controller is the host's, so device IRQs go
   straight there rather than through the emulated 8259s. */
class HypervisorIRQTarget final: public IRQTarget {
private:
    PCMachine &fMachine;

public:
    HypervisorIRQTarget(PCMachine &machine): fMachine(machine) {}

    void SetIRQ(int irq_num, int level) override;
};

/* With an IOAPIC, each line reaches both it and the 8259s, as on a PIIX,
   and the PIT's IRQ 0 is IOAPIC input 2. */
class PCIrqFanout final: public IRQTarget {
private:
    PCMachine &fMachine;

public:
    PCIrqFanout(PCMachine &machine): fMachine(machine) {}

    void SetIRQ(int irq_num, int level) override;
};

class PCMachine final:
    public VirtMachine,
    public TlbFlushTarget,
    public CPUIRQTarget,
    public PITTickSource,
    public X86HardIntnoSource,
    public X86TscSource,
    public X86HypervisorTarget,
    public PCIMsiTarget {
public:
    uint64_t ram_size;
    PhysMemoryMap *mem_map;
    PhysMemoryMap *port_map;
    
    X86CPUState *cpu_state;
    std::unique_ptr<PIC2State> pic_state;
    IRQSignal pic_irq[16];
    std::unique_ptr<PITState> pit_state;
    std::unique_ptr<CMOSState> cmos_state;
    /* what the ACPI tables of a kernel boot describe */
    std::unique_ptr<AcpiPmBlock> fAcpiPm;

    /* The configuration's devices, and what realizing them produced. */
    SystemBus *bus = nullptr;
    I440FXState *i440fx_state = nullptr;
    FBDevice *fb_dev = nullptr;
    /* The device answering the VMware backdoor port, and where it is. */
    VMPortTarget *vmport = nullptr;
    uint64_t fb_base = 0;

    /* runs the processor instead of the interpreter when the host has one */
    std::unique_ptr<HostX86Hypervisor> hypervisor;
    /* the hypervisor provides local APICs, and MSIs go to them */
    bool fLocalApic = false;
    /* the hypervisor has the 8259s and the 8254 rather than the machine */
    bool fHypervisorIrqchip = false;
    HypervisorIRQTarget fHypervisorIrqTarget {*this};
    /* the machine's IOAPIC, when the local APICs are the hypervisor's and
       the rest is the machine's */
    std::unique_ptr<IOAPIC> fIoApic;
    PCIrqFanout fIrqFanout {*this};
    /* Otherwise the machine's: INTR as the 8259s drive it, read without the
       lock, and how long the hypervisor may run before the PIT is due. */
    std::atomic<bool> fCpuIrq {false};
    int64_t fTimerDelayUs = -1;

    /* fixed-function port stubs and the VMware backdoor port */
    uint32_t Port80Read(uint32_t offset, int size_log2);
    void Port80Write(uint32_t offset, uint32_t val, int size_log2);
    uint32_t Port92Read(uint32_t offset, int size_log2);
    void Port92Write(uint32_t offset, uint32_t val, int size_log2);
    uint32_t VmPortRead(uint32_t addr, int size_log2);
    void VmPortWrite(uint32_t addr, uint32_t val, int size_log2);
    uint32_t BiosDebugRead(uint32_t offset, int size_log2);
    void BiosDebugWrite(uint32_t offset, uint32_t val, int size_log2);
    /* X86HypervisorTarget; the interpreter reaches the port map through
       fPortIo too */
    uint32_t PortRead(uint32_t port, int size_log2) override;
    void PortWrite(uint32_t port, uint32_t val, int size_log2) override;
    void MmioRead(uint64_t addr, uint8_t *data, int len) override;
    void MmioWrite(uint64_t addr, const uint8_t *data, int len) override;
    bool InterruptRequested() override;
    int AcknowledgeInterrupt() override;
    void ApicEoi(int vector) override;
    /* PCIMsiTarget */
    void SendMsi(uint64_t addr, uint32_t data) override;

    DeviceIOAdapter<PCMachine, &PCMachine::Port80Read,
                    &PCMachine::Port80Write> fPort80Io {*this};
    DeviceIOAdapter<PCMachine, &PCMachine::Port92Read,
                    &PCMachine::Port92Write> fPort92Io {*this};
    DeviceIOAdapter<PCMachine, &PCMachine::VmPortRead,
                    &PCMachine::VmPortWrite> fVmPortIo {*this};
    DeviceIOAdapter<PCMachine, &PCMachine::BiosDebugRead,
                    &PCMachine::BiosDebugWrite> fBiosDebugIo {*this};
    DeviceIOAdapter<PCMachine, &PCMachine::PortRead,
                    &PCMachine::PortWrite> fPortIo {*this};

    ~PCMachine() override;

    DeviceLock &Lock() {return *fDeviceLock;}

    /* TlbFlushTarget */
    void FlushTlbWriteRange(uint8_t *ram_addr, size_t ram_size) override;
    /* CPUIRQTarget */
    void SetCPUIRQ(int level) override;
    /* PITTickSource */
    int64_t Ticks() override;
    /* X86HardIntnoSource */
    int HardIntno() override;
    /* X86TscSource */
    uint64_t Tsc() override;

    /* VirtMachine */
    void ProcessorThreadStarted() override;
    int64_t RunTimers() override;
    bool Idle() override;
    void Interp(int max_exec_cycle) override;
    void InterruptExecution() override;
};

/* Where a kernel boot routes PIRQA-D: two lines no ISA device of a PC
   owns, leaving 9 for the SCI and 12 for the PS/2 mouse. */
static const uint8_t kKernelPciIrqs[4] = { 10, 11, 10, 11 };

static void copy_kernel(PCMachine *s, const uint8_t *buf, int buf_len,
                        const char *cmd_line);
static void pc_acpi_setup(PCMachine *s);
static uint8_t *get_ram_range_ptr(PCMachine *s, uint64_t addr, uint64_t len);
static void set_flat_gdt(uint8_t *gdt);
static void start_flat_protected_mode(PCMachine *s, uint32_t gdt_addr,
                                      uint32_t entry, int reg,
                                      uint32_t reg_val);
static void map_pci_interrupts(PCMachine *s);
static bool is_elf(const uint8_t *buf, int buf_len);
static bool pvh_load(PCMachine *s, const uint8_t *buf, int buf_len,
                     const uint8_t *initrd, int initrd_len,
                     const char *cmd_line, uint32_t bios_size,
                     uint64_t rsdp);

void PCMachine::BiosDebugWrite(uint32_t offset, uint32_t val, int size_log2)
{
    (void)offset;
    (void)size_log2;
    putchar(get_bits(val, 0, 8));
}

uint32_t PCMachine::BiosDebugRead(uint32_t offset, int size_log2)
{
    (void)offset;
    (void)size_log2;
    return 0;
}

void PCMachine::Port80Write(uint32_t offset, uint32_t val, int size_log2)
{
    (void)offset;
    (void)val;
    (void)size_log2;
}

uint32_t PCMachine::Port80Read(uint32_t offset, int size_log2)
{
    (void)offset;
    (void)size_log2;
    return 0xff;
}

void PCMachine::Port92Write(uint32_t offset, uint32_t val, int size_log2)
{
    (void)offset;
    (void)val;
    (void)size_log2;
}

uint32_t PCMachine::Port92Read(uint32_t offset, int size_log2)
{
    (void)offset;
    (void)size_log2;
    int a20 = 1; /* A20=0 is not supported */
    return a20 << 1;
}

#define VMPORT_MAGIC   0x564D5868
#define REG_EAX 0
#define REG_EBX 1
#define REG_ECX 2
#define REG_EDX 3
#define REG_ESI 4
#define REG_EDI 5

uint32_t PCMachine::VmPortRead(uint32_t addr, int size_log2)
{
    PCMachine *s = this;
    uint32_t regs[6];

    if (s->hypervisor) {
        HostX86Regs r;

        s->hypervisor->GetRegs(&r);
        regs[REG_EAX] = r.gpr[0];
        regs[REG_EBX] = r.gpr[3];
        regs[REG_ECX] = r.gpr[1];
        regs[REG_EDX] = r.gpr[2];
        regs[REG_ESI] = r.gpr[6];
        regs[REG_EDI] = r.gpr[7];

        if (regs[REG_EAX] == VMPORT_MAGIC) {

            s->vmport->VMPortCommand(regs);

            /* Note: in 64 bits the high parts are reset to zero
               in all cases. */
            r.gpr[0] = regs[REG_EAX];
            r.gpr[3] = regs[REG_EBX];
            r.gpr[1] = regs[REG_ECX];
            r.gpr[2] = regs[REG_EDX];
            r.gpr[6] = regs[REG_ESI];
            r.gpr[7] = regs[REG_EDI];
            s->hypervisor->SetRegs(r);
        }
    } else {
        regs[REG_EAX] = x86_cpu_get_reg(s->cpu_state, 0);
        regs[REG_EBX] = x86_cpu_get_reg(s->cpu_state, 3);
        regs[REG_ECX] = x86_cpu_get_reg(s->cpu_state, 1);
        regs[REG_EDX] = x86_cpu_get_reg(s->cpu_state, 2);
        regs[REG_ESI] = x86_cpu_get_reg(s->cpu_state, 6);
        regs[REG_EDI] = x86_cpu_get_reg(s->cpu_state, 7);

        if (regs[REG_EAX] == VMPORT_MAGIC) {
            s->vmport->VMPortCommand(regs);

            x86_cpu_set_reg(s->cpu_state, 0, regs[REG_EAX]);
            x86_cpu_set_reg(s->cpu_state, 3, regs[REG_EBX]);
            x86_cpu_set_reg(s->cpu_state, 1, regs[REG_ECX]);
            x86_cpu_set_reg(s->cpu_state, 2, regs[REG_EDX]);
            x86_cpu_set_reg(s->cpu_state, 6, regs[REG_ESI]);
            x86_cpu_set_reg(s->cpu_state, 7, regs[REG_EDI]);
        }
    }
    return regs[REG_EAX];
}

void PCMachine::VmPortWrite(uint32_t addr, uint32_t val, int size_log2)
{
    (void)addr;
    (void)val;
    (void)size_log2;
}

void PCMachine::SetCPUIRQ(int level)
{
    if (hypervisor) {
        bool raised = level && !fCpuIrq.load();
        fCpuIrq.store(level != 0);
        /* The processor thread looks at INTR before each run, so only a
           request from elsewhere has to stop a run in progress. */
        if (raised && !OnProcessorThread()) {
            hypervisor->InterruptRun();
        }
    } else {
        x86_cpu_set_irq(cpu_state, level);
    }
    Kick();
}

int PCMachine::HardIntno()
{
    return pic2_get_hard_intno(pic_state.get());
}

bool PCMachine::InterruptRequested()
{
    return fCpuIrq.load();
}

int PCMachine::AcknowledgeInterrupt()
{
    return pic2_get_hard_intno(pic_state.get());
}

/* Only a write to the local APICs' page is an interrupt; anything else is
   the memory write an MSI really is. */
void PCMachine::SendMsi(uint64_t addr, uint32_t data)
{
    if ((addr >> 20) == 0xfee)
        hypervisor->SendMsi(addr, data);
    else
        mem_map->IoWrite(addr, data, 2);
}

int64_t PCMachine::Ticks()
{
    uint64_t us = host_monotonic_us();

    return (us / 1000000) * PIT_FREQ + (us % 1000000) * PIT_FREQ / 1000000;
}

/* The two PICs give sixteen lines, and the port space is what a 16 bit port
   number can name. */
#define PC_IRQ_COUNT 16
#define PC_IO_SPACE_SIZE 0x10000

/* Where a device that wants host address space rather than ports is placed.
   A PC has almost nothing of the kind -- the framebuffer of a machine booting
   a kernel without firmware is the one thing -- so the window is the hole
   below the BIOS and nothing else is expected to compete for it. */
#define FRAMEBUFFER_BASE_ADDR 0xf0400000
#define PC_DEVICE_WINDOW_SIZE 0x08000000 /* 128 MB */

static uint8_t *get_ram_ptr(PCMachine *s, uint64_t paddr)
{
    PhysMemoryRange *pr;
    pr = s->mem_map->FindRange(paddr);
    if (!pr || !pr->is_ram)
        return NULL;
    return pr->phys_mem + (uintptr_t)(paddr - pr->addr);
}

#ifdef DUMP_IOPORT
static bool dump_port(int port)
{
    return !((port >= 0x1f0 && port <= 0x1f7) ||
             (port >= 0x20 && port <= 0x21) ||
             (port >= 0xa0 && port <= 0xa1));
}
#endif

void PCMachine::PortWrite(uint32_t port, uint32_t val, int size_log2)
{
#ifdef DUMP_IOPORT
    if (dump_port(port))
        printf("write port=0x%x val=0x%x s=%d\n", port, val, 1 << size_log2);
#endif
    port_map->IoWrite(port, val, size_log2);
}

uint32_t PCMachine::PortRead(uint32_t port, int size_log2)
{
    uint32_t val = port_map->IoRead(port, size_log2);
#ifdef DUMP_IOPORT
    if (dump_port(port))
        printf("read port=0x%x val=0x%x s=%d\n", port, val, 1 << size_log2);
#endif
    return val;
}

/* XXX: should check overlapping mappings */
void HypervisorPhysMemoryMap::MapRam(PhysMemoryRange *pr)
{
    fHypervisor.MapRam(IndexOfRange(pr), pr->addr, pr->size, pr->phys_mem,
                       (pr->devram_flags & DEVRAM_FLAG_ROM) != 0,
                       (pr->devram_flags & DEVRAM_FLAG_DIRTY_BITS) != 0);
}

/* The base destructor cannot reach FreeRam() here, so the memory goes back
   to the hypervisor first. */
HypervisorPhysMemoryMap::~HypervisorPhysMemoryMap()
{
    for (int i = 0; i < RangeCount(); i++) {
        PhysMemoryRange *pr = RangeAt(i);
        if (pr->is_ram && pr->phys_mem != nullptr) {
            fHypervisor.FreeRam(pr->phys_mem, pr->org_size);
            pr->phys_mem = nullptr;
        }
    }
}

PhysMemoryRange *HypervisorPhysMemoryMap::RegisterRam(uint64_t addr,
                                                      uint64_t size,
                                                      int devram_flags)
{
    PhysMemoryRange *pr;

    pr = RegisterRamEntry(addr, size, devram_flags);

    pr->phys_mem = fHypervisor.AllocRam(size);
    if (pr->phys_mem == nullptr) {
        fprintf(stderr, "Could not allocate VM memory\n");
        exit(1);
    }
    if (devram_flags & DEVRAM_FLAG_DIRTY_BITS) {
        /* the hypervisor fills whole 64 bit words */
        int n_pages = size >> DEVRAM_PAGE_SIZE_LOG2;
        pr->dirty_bits_size = ((n_pages + 63) / 64) * 8;
        pr->dirty_bits_tab[0] = std::make_unique<uint32_t[]>(
            pr->dirty_bits_size / sizeof(uint32_t));
        pr->dirty_bits = pr->dirty_bits_tab[0].get();
    }

    if (pr->size != 0) {
        MapRam(pr);
    }
    return pr;
}

void HypervisorPhysMemoryMap::SetRamAddr(PhysMemoryRange *pr, uint64_t addr,
                                         bool enabled)
{
    if (enabled) {
        if (pr->size == 0 || addr != pr->addr) {
            /* move or create the region */
            pr->size = pr->org_size;
            pr->addr = addr;
            MapRam(pr);
        }
    } else {
        if (pr->size != 0) {
            pr->addr = 0;
            pr->size = 0;
            /* map a zero size region to disable */
            MapRam(pr);
        }
    }
}

const uint32_t *HypervisorPhysMemoryMap::GetDirtyBits(PhysMemoryRange *pr)
{
    if (pr->size == 0) {
        /* not mapped: we assume no modification was made */
        memset(pr->dirty_bits, 0, pr->dirty_bits_size);
    } else {
        fHypervisor.GetDirtyLog(IndexOfRange(pr), pr->dirty_bits);
    }
    return pr->dirty_bits;
}

void HypervisorIRQTarget::SetIRQ(int irq_num, int level)
{
    fMachine.hypervisor->SetIRQ(irq_num, level);
}

void PCIrqFanout::SetIRQ(int irq_num, int level)
{
    fMachine.pic_state->SetIRQ(irq_num, level);
    if (irq_num != 2)
        fMachine.fIoApic->SetIRQ(irq_num == 0 ? 2 : irq_num, level);
}

void PCMachine::ApicEoi(int vector)
{
    if (fIoApic)
        fIoApic->Eoi(vector);
}

void PCMachine::MmioWrite(uint64_t paddr, const uint8_t *data, int len)
{
    PhysMemoryRange *pr;
    uint64_t addr;

    pr = mem_map->FindRange(paddr);
    if (!pr)
        return;
    addr = paddr - pr->addr;
    if (pr->is_ram) {
        if (!(pr->devram_flags & DEVRAM_FLAG_ROM) && addr + len <= pr->size) {
            memcpy(mem_map->GetRamPtr(paddr, true), data, len);
        }
        return;
    }
    switch(len) {
    case 1:
        if (pr->devio_flags & DEVIO_SIZE8) {
            pr->io->DeviceWrite(addr, *(const uint8_t *)data, 0);
        }
        break;
    case 2:
        if (pr->devio_flags & DEVIO_SIZE16) {
            pr->io->DeviceWrite(addr, *(const uint16_t *)data, 1);
        }
        break;
    case 4:
        if (pr->devio_flags & DEVIO_SIZE32) {
            pr->io->DeviceWrite(addr, *(const uint32_t *)data, 2);
        }
        break;
    case 8:
        if (pr->devio_flags & DEVIO_SIZE32) {
            pr->io->DeviceWrite(addr, *(const uint32_t *)data, 2);
            pr->io->DeviceWrite(addr + 4, *(const uint32_t *)(data + 4), 2);
        }
        break;
    default:
        abort();
    }
}

void PCMachine::MmioRead(uint64_t paddr, uint8_t *data, int len)
{
    PhysMemoryRange *pr;
    uint64_t addr;

    pr = mem_map->FindRange(paddr);
    if (!pr)
        goto no_dev;
    addr = paddr - pr->addr;
    if (pr->is_ram) {
        if (addr + len > pr->size)
            goto no_dev;
        memcpy(data, pr->phys_mem + addr, len);
        return;
    }
    switch(len) {
    case 1:
        if (!(pr->devio_flags & DEVIO_SIZE8))
            goto no_dev;
        *(uint8_t *)data = pr->io->DeviceRead(addr, 0);
        break;
    case 2:
        if (!(pr->devio_flags & DEVIO_SIZE16))
            goto no_dev;
        *(uint16_t *)data = pr->io->DeviceRead(addr, 1);
        break;
    case 4:
        if (!(pr->devio_flags & DEVIO_SIZE32))
            goto no_dev;
        *(uint32_t *)data = pr->io->DeviceRead(addr, 2);
        break;
    case 8:
        if (!(pr->devio_flags & DEVIO_SIZE32))
            goto no_dev;
        *(uint32_t *)data = pr->io->DeviceRead(addr, 2);
        *(uint32_t *)(data + 4) = pr->io->DeviceRead(addr + 4, 2);
        break;
    default:
        abort();
    }
    return;
 no_dev:
    memset(data, 0, len);
}

#define TSC_FREQ 100000000

uint64_t PCMachine::Tsc()
{
    uint64_t us = host_monotonic_us();

    return (us / 1000000) * TSC_FREQ + (us % 1000000) * (TSC_FREQ / 1000000);
}

void PCMachine::FlushTlbWriteRange(uint8_t *ram_addr, size_t ram_size)
{
    assert(OnProcessorThread());
    x86_cpu_flush_tlb_write_range_ram(cpu_state, ram_addr, ram_size);
}

/* The addresses and lines the chipset above holds. They are reserved so that
   a device the configuration declares cannot be placed on top of one of
   them. */
static bool pc_claim_fixed_ranges(PCMachine *s, bool acpi)
{
    RangeAllocator &io = s->bus->IoAlloc();
    RangeAllocator &irq = s->bus->IrqAlloc();

    return (!acpi || io.Claim(ACPI_PM_BASE, ACPI_PM_SIZE, "acpi")) &&
        io.Claim(0x20, 2, "pic") && io.Claim(0xa0, 2, "pic") &&
        io.Claim(0x4d0, 2, "elcr") &&
        io.Claim(0x40, 4, "pit") && io.Claim(0x61, 1, "pit") &&
        io.Claim(0x70, 2, "cmos") &&
        io.Claim(0x80, 2, "port80") && io.Claim(0x92, 2, "port92") &&
#ifdef DEBUG_BIOS
        io.Claim(0x402, 2, "bios debug") &&
#endif
        irq.Claim(0, 1, "pit") && irq.Claim(2, 1, "cascade") &&
        irq.Claim(8, 1, "rtc") && irq.Claim(13, 1, "fpu");
}


static std::unique_ptr<VirtMachine> pc_machine_init(const VirtMachineParams *p)
{
    std::unique_ptr<PCMachine> owned;
    PCMachine *s;
    DeviceContext ctx;

    if (strcmp(p->machine_name, "pc") != 0) {
        vm_error("unsupported machine: %s\n", p->machine_name);
        return nullptr;
    }
    if (p->cpu_count != 1) {
        vm_error("pc: only one processor is supported\n");
        return nullptr;
    }
    HostX86Options options;
    if (p->interrupt_controller == nullptr ||
        strcmp(p->interrupt_controller, "pic") == 0) {
        options.local_apic = false;
    } else if (strcmp(p->interrupt_controller, "apic") == 0) {
        options.local_apic = true;
    } else {
        vm_error("pc: interrupt_controller must be \"pic\" or \"apic\", "
                 "not \"%s\"\n", p->interrupt_controller);
        return nullptr;
    }
    /* The nesting in the file is the nesting of the buses, so the root one
       has to be the kind this machine provides. */
    if (p->root_bus_type != NULL && strcmp(p->root_bus_type, "pc") != 0) {
        vm_error("pc: the root bus must be a 'pc' bus, not '%s'\n",
                 p->root_bus_type);
        return nullptr;
    }

    assert(p->ram_size >= (1 << 20));

    owned = std::make_unique<PCMachine>();
    s = owned.get();
    s->SetHost(p);
    s->vmc = p->vmc;
    s->ram_size = p->ram_size;
    
    s->port_map = new PhysMemoryMap();

    if (p->accel_enable) {
        s->hypervisor = host_x86_hypervisor_open(*s, *p->device_lock,
                                                 options);
    }
    /* the interpreter has no local APIC */
    if (options.local_apic && !s->hypervisor) {
        vm_error("pc: interrupt_controller \"apic\" needs a hypervisor\n");
        return nullptr;
    }
    s->fLocalApic = options.local_apic;

    if (s->hypervisor) {
        s->mem_map = new HypervisorPhysMemoryMap(*s->hypervisor);
        s->fHypervisorIrqchip = s->hypervisor->HasInterruptControllers();
        if (s->fHypervisorIrqchip) {
            for (int i = 0; i < 16; i++) {
                s->pic_irq[i].Init(&s->fHypervisorIrqTarget, i);
            }
        }
    } else {
        s->mem_map = new PhysMemoryMap();
        s->cpu_state = x86_cpu_init(s->mem_map);
        x86_cpu_set_tsc_source(s->cpu_state, s);
        x86_cpu_set_port_io(s->cpu_state, &s->fPortIo);
        x86_cpu_set_device_lock(s->cpu_state, p->device_lock);

        /* needed to handle the RAM dirty bits */
        s->mem_map->SetTlbFlushTarget(s);
    }

    /* set the RAM mapping and leave the VGA addresses empty */
    s->mem_map->RegisterRam(0xc0000, p->ram_size - 0xc0000, 0);
    s->mem_map->RegisterRam(0, 0xa0000, 0);
    
    /* devices */
    s->port_map->RegisterDevice(0x80, 2, &s->fPort80Io, DEVIO_SIZE8);
    s->port_map->RegisterDevice(0x92, 2, &s->fPort92Io, DEVIO_SIZE8);
    
    /* setup the bios */
    if (p->files[VM_FILE_BIOS].len > 0) {
        int bios_size, bios_size1;
        uint8_t *bios_buf, *ptr;
        uint32_t bios_addr;
        
        bios_size = p->files[VM_FILE_BIOS].len;
        bios_buf = p->files[VM_FILE_BIOS].buf;
        assert((bios_size % 65536) == 0 && bios_size != 0);
        bios_addr = -bios_size;
        /* at the top of the 4GB memory */
        s->mem_map->RegisterRam(bios_addr, bios_size, DEVRAM_FLAG_ROM);
        ptr = get_ram_ptr(s, bios_addr);
        memcpy(ptr, bios_buf, bios_size);
        /* in the lower 1MB memory (currently set as RAM) */
        bios_size1 = min_int(bios_size, 128 * 1024);
        ptr = get_ram_ptr(s, 0x100000 - bios_size1);
        memcpy(ptr, bios_buf + bios_size - bios_size1, bios_size1);
#ifdef DEBUG_BIOS
        s->port_map->RegisterDevice(0x402, 2, &s->fBiosDebugIo, DEVIO_SIZE8);
#endif
    }

    if (!s->fHypervisorIrqchip) {
        s->pic_state = pic2_init(s->port_map, 0x20, 0xa0,
                                 0x4d0, 0x4d1, s, s->pic_irq);
        if (s->fLocalApic) {
            s->fIoApic = std::make_unique<IOAPIC>(*s, 0);
            s->mem_map->RegisterDevice(IOAPIC_ADDR, IOAPIC_SIZE,
                                       s->fIoApic.get(), DEVIO_SIZE32);
            for (int i = 0; i < PC_IRQ_COUNT; i++)
                s->pic_irq[i].Init(&s->fIrqFanout, i);
        }
        if (s->cpu_state) {
            x86_cpu_set_hard_intno_source(s->cpu_state, s);
        }
        s->pit_state = pit_init(s->port_map, 0x40, 0x61, &s->pic_irq[0], s);
    }

    s->cmos_state = cmos_init(s->port_map, 0x70, &s->pic_irq[8],
                              p->rtc_local_time);

    /* various cmos data */
    {
        int size;
        /* memory size */
        size = min_int((s->ram_size - (1 << 20)) >> 10, 65535);
        put_le16(s->cmos_state->cmos_data + 0x30, size);
        if (s->ram_size >= (16 << 20)) {
            size = min_int((s->ram_size - (16 << 20)) >> 16, 65535);
            put_le16(s->cmos_state->cmos_data + 0x34, size);
        }
        s->cmos_state->cmos_data[0x14] = 0x06; /* mouse + FPU present */
    }
    
    /* The devices the configuration declares. Everything above this point is
       what a PC has before any of them exists: RAM, the interrupt
       controllers, the timer and the clock. */
    s->bus = new SystemBus(s->mem_map, s->port_map, s->pic_irq,
                           PC_IRQ_COUNT);
    if (s->fLocalApic)
        s->bus->SetMsiTarget(s);
    s->bus->IoAlloc().SetWindow(0, PC_IO_SPACE_SIZE);
    s->bus->MmioAlloc().SetWindow(FRAMEBUFFER_BASE_ADDR,
                                  PC_DEVICE_WINDOW_SIZE);
    /* A kernel booted without firmware gets its ACPI tables from here. */
    bool acpi = p->files[VM_FILE_KERNEL].buf != nullptr;
    if (!pc_claim_fixed_ranges(s, acpi)) {
        return nullptr;
    }

    ctx.params = p;
    ctx.platform = p->platform;
    ctx.machine = s;

    if (!device_build_tree(s->bus, p->root_devices, &ctx) ||
        !s->bus->AllocateAll() || !s->bus->RealizeAll()) {
        return nullptr;
    }

    device_context_connect(&ctx);
    s->fb_dev = ctx.fb_dev;
    s->fb_base = ctx.fb_base;

    /* The VMware backdoor is read through the processor's registers, so the
       machine owns the port and the device only interprets the call. */
    s->vmport = ctx.vmport;
    if (s->vmport != NULL) {
        s->port_map->RegisterDevice(ctx.vmport_base, 1, &s->fVmPortIo,
                                    DEVIO_SIZE32);
    }

    /* With no firmware to program the PIRQ registers, copy_kernel() routes
       the INTx lines itself, which needs the bridge the tree built. */
    for (int i = 0; i < s->bus->DeviceCount(); i++) {
        I440FXState *fx = i440fx_node_state(s->bus->DeviceAt(i));
        if (fx != NULL) {
            s->i440fx_state = fx;
            break;
        }
    }

    if (acpi) {
        pc_acpi_setup(s);
    }

    if (p->files[VM_FILE_KERNEL].buf) {
        const VMFileEntry &kernel = p->files[VM_FILE_KERNEL];
        const VMFileEntry &initrd = p->files[VM_FILE_INITRD];

        if (is_elf(kernel.buf, kernel.len)) {
            if (!pvh_load(s, kernel.buf, kernel.len, initrd.buf, initrd.len,
                          p->cmdline ? p->cmdline : "",
                          p->files[VM_FILE_BIOS].len, PC_ACPI_ADDR)) {
                return nullptr;
            }
        } else {
            copy_kernel(s, kernel.buf, kernel.len,
                        p->cmdline ? p->cmdline : "");
        }
    }

    return owned;
}

PCMachine::~PCMachine()
{
    PCMachine *s = this;
    /* XXX: free all */
    if (s->cpu_state) {
        x86_cpu_end(s->cpu_state);
    }
    delete s->bus;
    delete s->mem_map;
    delete s->port_map;
}

struct screen_info {
} __attribute__((packed));

/* from plex86 (BSD license) */
struct  __attribute__ ((packed)) linux_params {
    /* screen_info structure */
    uint8_t  orig_x;		/* 0x00 */
    uint8_t  orig_y;		/* 0x01 */
    uint16_t ext_mem_k;	/* 0x02 */
    uint16_t orig_video_page;	/* 0x04 */
    uint8_t  orig_video_mode;	/* 0x06 */
    uint8_t  orig_video_cols;	/* 0x07 */
    uint8_t  flags;		/* 0x08 */
    uint8_t  unused2;		/* 0x09 */
    uint16_t orig_video_ega_bx;/* 0x0a */
    uint16_t unused3;		/* 0x0c */
    uint8_t  orig_video_lines;	/* 0x0e */
    uint8_t  orig_video_isVGA;	/* 0x0f */
    uint16_t orig_video_points;/* 0x10 */
    
    /* VESA graphic mode -- linear frame buffer */
    uint16_t lfb_width;	/* 0x12 */
    uint16_t lfb_height;	/* 0x14 */
    uint16_t lfb_depth;	/* 0x16 */
    uint32_t lfb_base;		/* 0x18 */
    uint32_t lfb_size;		/* 0x1c */
    uint16_t cl_magic, cl_offset; /* 0x20 */
    uint16_t lfb_linelength;	/* 0x24 */
    uint8_t  red_size;		/* 0x26 */
    uint8_t  red_pos;		/* 0x27 */
    uint8_t  green_size;	/* 0x28 */
    uint8_t  green_pos;	/* 0x29 */
    uint8_t  blue_size;	/* 0x2a */
    uint8_t  blue_pos;		/* 0x2b */
    uint8_t  rsvd_size;	/* 0x2c */
    uint8_t  rsvd_pos;		/* 0x2d */
    uint16_t vesapm_seg;	/* 0x2e */
    uint16_t vesapm_off;	/* 0x30 */
    uint16_t pages;		/* 0x32 */
    uint16_t vesa_attributes;	/* 0x34 */
    uint32_t capabilities;     /* 0x36 */
    uint32_t ext_lfb_base;	/* 0x3a */
    uint8_t  _reserved[2];	/* 0x3e */
    
  /* 0x040 */ uint8_t   apm_bios_info[20]; // struct apm_bios_info
  /* 0x054 */ uint8_t   pad2[0x80 - 0x54];

  // Following 2 from 'struct drive_info_struct' in drivers/block/cciss.h.
  // Might be truncated?
  /* 0x080 */ uint8_t   hd0_info[16]; // hd0-disk-parameter from intvector 0x41
  /* 0x090 */ uint8_t   hd1_info[16]; // hd1-disk-parameter from intvector 0x46

  // System description table truncated to 16 bytes
  // From 'struct sys_desc_table_struct' in linux/arch/i386/kernel/setup.c.
  /* 0x0a0 */ uint16_t  sys_description_len;
  /* 0x0a2 */ uint8_t   sys_description_table[14];
                        // [0] machine id
                        // [1] machine submodel id
                        // [2] BIOS revision
                        // [3] bit1: MCA bus

  /* 0x0b0 */ uint8_t   pad3[0x1e0 - 0xb0];
  /* 0x1e0 */ uint32_t  alt_mem_k;
  /* 0x1e4 */ uint8_t   pad4[4];
  /* 0x1e8 */ uint8_t   e820map_entries;
  /* 0x1e9 */ uint8_t   eddbuf_entries; // EDD_NR
  /* 0x1ea */ uint8_t   pad5[0x1f1 - 0x1ea];
  /* 0x1f1 */ uint8_t   setup_sects; // size of setup.S, number of sectors
  /* 0x1f2 */ uint16_t  mount_root_rdonly; // MOUNT_ROOT_RDONLY (if !=0)
  /* 0x1f4 */ uint16_t  sys_size; // size of compressed kernel-part in the
                                // (b)zImage-file (in 16 byte units, rounded up)
  /* 0x1f6 */ uint16_t  swap_dev; // (unused AFAIK)
  /* 0x1f8 */ uint16_t  ramdisk_flags;
  /* 0x1fa */ uint16_t  vga_mode; // (old one)
  /* 0x1fc */ uint16_t  orig_root_dev; // (high=Major, low=minor)
  /* 0x1fe */ uint8_t   pad6[1];
  /* 0x1ff */ uint8_t   aux_device_info;
  /* 0x200 */ uint16_t  jump_setup; // Jump to start of setup code,
                                  // aka "reserved" field.
  /* 0x202 */ uint8_t   setup_signature[4]; // Signature for SETUP-header, ="HdrS"
  /* 0x206 */ uint16_t  header_format_version; // Version number of header format;
  /* 0x208 */ uint8_t   setup_S_temp0[8]; // Used by setup.S for communication with
                                        // boot loaders, look there.
  /* 0x210 */ uint8_t   loader_type;
                        // 0 for old one.
                        // else 0xTV:
                        //   T=0: LILO
                        //   T=1: Loadlin
                        //   T=2: bootsect-loader
                        //   T=3: SYSLINUX
                        //   T=4: ETHERBOOT
                        //   V=version
  /* 0x211 */ uint8_t   loadflags;
                        // bit0 = 1: kernel is loaded high (bzImage)
                        // bit7 = 1: Heap and pointer (see below) set by boot
                        //   loader.
  /* 0x212 */ uint16_t  setup_S_temp1;
  /* 0x214 */ uint32_t  kernel_start;
  /* 0x218 */ uint32_t  initrd_start;
  /* 0x21c */ uint32_t  initrd_size;
  /* 0x220 */ uint8_t   setup_S_temp2[4];
  /* 0x224 */ uint16_t  setup_S_heap_end_pointer;
  /* 0x226 */ uint16_t  pad70;
  /* 0x228 */ uint32_t  cmd_line_ptr;
  /* 0x22c */ uint8_t   pad7[0x2d0 - 0x22c];

  /* 0x2d0 : Int 15, ax=e820 memory map. */
  // (linux/include/asm-i386/e820.h, 'struct e820entry')
#define E820MAX  32
#define E820_RAM  1
#define E820_RESERVED 2
#define E820_ACPI 3 /* usable as RAM once ACPI tables have been read */
#define E820_NVS  4
  struct {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
    } e820map[E820MAX];

  /* 0x550 */ uint8_t   pad8[0x600 - 0x550];

  // BIOS Enhanced Disk Drive Services.
  // (From linux/include/asm-i386/edd.h, 'struct edd_info')
  // Each 'struct edd_info is 78 bytes, times a max of 6 structs in array.
  /* 0x600 */ uint8_t   eddbuf[0x7d4 - 0x600];

  /* 0x7d4 */ uint8_t   pad9[0x800 - 0x7d4];
  /* 0x800 */ uint8_t   commandline[0x800];

  uint64_t gdt_table[4];
};

#define KERNEL_PARAMS_ADDR 0x00090000

static void copy_kernel(PCMachine *s, const uint8_t *buf, int buf_len,
                        const char *cmd_line)
{
    uint8_t *ram_ptr;
    int setup_sects, header_len, copy_len, setup_hdr_start, setup_hdr_end;
    uint32_t load_address;
    struct linux_params *params;
    FBDevice *fb_dev;
    
    if (buf_len < 1024) {
    too_small:
        fprintf(stderr, "Kernel too small\n");
        exit(1);
    }
    if (buf[0x1fe] != 0x55 || buf[0x1ff] != 0xaa) {
        fprintf(stderr, "Invalid kernel magic\n");
        exit(1);
    }
    setup_sects = buf[0x1f1];
    if (setup_sects == 0)
        setup_sects = 4;
    header_len = (setup_sects + 1) * 512;
    if (buf_len < header_len)
        goto too_small;
    if (memcmp(buf + 0x202, "HdrS", 4) != 0) {
        fprintf(stderr, "Kernel too old\n");
        exit(1);
    }
    load_address = 0x100000; /* we don't support older protocols */

    ram_ptr = get_ram_ptr(s, load_address);
    params = reinterpret_cast<struct linux_params *>(
        get_ram_ptr(s, KERNEL_PARAMS_ADDR));
    if (ram_ptr == NULL || params == NULL) {
        fprintf(stderr, "No RAM to load the kernel into\n");
        exit(1);
    }
    copy_len = buf_len - header_len;
    if (copy_len > (s->ram_size - load_address)) {
        fprintf(stderr, "Not enough RAM\n");
        exit(1);
    }
    memcpy(ram_ptr, buf + header_len, copy_len);

    memset(params, 0, sizeof(struct linux_params));

    /* copy the setup header */
    setup_hdr_start = 0x1f1;
    setup_hdr_end = 0x202 + buf[0x201];
    memcpy((uint8_t *)params + setup_hdr_start, buf + setup_hdr_start,
           setup_hdr_end - setup_hdr_start);

    strcpy((char *)params->commandline, cmd_line);

    params->mount_root_rdonly = 0;
    params->cmd_line_ptr = KERNEL_PARAMS_ADDR +
        offsetof(struct linux_params, commandline);
    params->alt_mem_k = (s->ram_size / 1024) - 1024;
    params->loader_type = 0x01;
#if 0
    if (initrd_size > 0) {
        params->initrd_start = INITRD_LOAD_ADDR;
        params->initrd_size = initrd_size;
    }
#endif
    params->orig_video_lines = 0;
    params->orig_video_cols = 0;

    fb_dev = s->fb_dev;
    if (fb_dev) {
        
        params->orig_video_isVGA = 0x23; /* VIDEO_TYPE_VLFB */

        params->lfb_depth = 32;
        params->red_size = 8;
        params->red_pos = 16;
        params->green_size = 8;
        params->green_pos = 8;
        params->blue_size = 8;
        params->blue_pos = 0;
        params->rsvd_size = 8;
        params->rsvd_pos = 24;

        params->lfb_width = fb_dev->width;
        params->lfb_height = fb_dev->height;
        params->lfb_linelength = fb_dev->stride;
        params->lfb_size = fb_dev->fb_size;
        params->lfb_base = s->fb_base;
    }
    
    set_flat_gdt((uint8_t *)params + offsetof(struct linux_params, gdt_table));
    start_flat_protected_mode(s, KERNEL_PARAMS_ADDR +
                              offsetof(struct linux_params, gdt_table),
                              load_address, 6, KERNEL_PARAMS_ADDR); /* esi */
    map_pci_interrupts(s);
}

/* The GDT a kernel entered in flat protected mode starts with: code in
   entry 2 and data in entry 3. */
#define BOOT_GDT_ENTRIES 4

static void set_flat_gdt(uint8_t *gdt)
{
    put_le64(gdt, 0);
    put_le64(gdt + 8, 0);
    put_le64(gdt + 16, 0x00cf9b000000ffffULL); /* CS */
    put_le64(gdt + 24, 0x00cf93000000ffffULL); /* DS */
}

/* Starts the processor at 'entry' in 32 bit protected mode with paging off,
   the segments flat from the GDT at 'gdt_addr', and general register 'reg'
   holding 'reg_val'. */
static void start_flat_protected_mode(PCMachine *s, uint32_t gdt_addr,
                                      uint32_t entry, int reg,
                                      uint32_t reg_val)
{
    uint16_t gdt_limit = BOOT_GDT_ENTRIES * 8 - 1;

    if (s->hypervisor) {
        HostX86Regs regs;

        s->hypervisor->SetFlatProtectedMode(gdt_addr, gdt_limit,
                                            2 << 3, 3 << 3);

        memset(&regs, 0, sizeof(regs));
        regs.rip = entry;
        regs.gpr[reg] = reg_val;
        regs.rflags = 0x2;
        s->hypervisor->SetRegs(regs);
    } else {
        int i;
        X86CPUSeg sd;
        uint32_t val;
        val = x86_cpu_get_reg(s->cpu_state, X86_CPU_REG_CR0);
        x86_cpu_set_reg(s->cpu_state, X86_CPU_REG_CR0, val | (1 << 0));

        sd.base = gdt_addr;
        sd.limit = gdt_limit;
        x86_cpu_set_seg(s->cpu_state, X86_CPU_SEG_GDT, &sd);
        sd.sel = 2 << 3;
        sd.base = 0;
        sd.limit = 0xffffffff;
        sd.flags = 0xc09b;
        x86_cpu_set_seg(s->cpu_state, X86_CPU_SEG_CS, &sd);
        sd.sel = 3 << 3;
        sd.flags = 0xc093;
        for(i = 0; i < 6; i++) {
            if (i != X86_CPU_SEG_CS) {
                x86_cpu_set_seg(s->cpu_state, i, &sd);
            }
        }

        x86_cpu_set_reg(s->cpu_state, X86_CPU_REG_EIP, entry);
        x86_cpu_set_reg(s->cpu_state, reg, reg_val);
    }
}

/* With no firmware to program the PIRQ registers, the loader routes the
   INTx lines itself. */
static void map_pci_interrupts(PCMachine *s)
{
    uint8_t elcr[2];

    if (s->i440fx_state == nullptr)
        return;
    i440fx_map_interrupts(s->i440fx_state, elcr, kKernelPciIrqs);
    /* XXX: hypervisor support */
    if (s->pic_state) {
        pic2_set_elcr(s->pic_state.get(), elcr);
    }
}

static void pc_acpi_setup(PCMachine *s)
{
    PcAcpiConfig config;
    uint8_t *mem = get_ram_range_ptr(s, PC_ACPI_ADDR, PC_ACPI_SIZE);

    config.i8042 = s->port_map->FindRange(0x64) != nullptr;
    config.apic = s->fLocalApic;
    config.pci_gsis = kKernelPciIrqs;
    /* the hole between RAM and the machine's own device window */
    config.pci_mmio_base = (s->ram_size + 0xfffff) & ~(uint64_t)0xfffff;
    config.pci_mmio_end = FRAMEBUFFER_BASE_ADDR;
    assert(mem != NULL);
    memset(mem, 0, PC_ACPI_SIZE);
    pc_acpi_build(mem, config);

    s->fAcpiPm = std::make_unique<AcpiPmBlock>(*s);
    s->port_map->RegisterDevice(ACPI_PM_BASE, ACPI_PM_SIZE, s->fAcpiPm.get(),
                                DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32);
}

/* PVH boot: an ELF kernel with a PHYS32_ENTRY note is entered there in flat
   32 bit protected mode, paging off, with EBX pointing at an hvm_start_info
   (Xen's arch/x86/hvm/start_info.h). */

#define XEN_HVM_START_MAGIC 0x336ec578
#define XEN_ELFNOTE_PHYS32_ENTRY 18

struct HvmStartInfo {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t nr_modules;
    uint64_t modlist_paddr;
    uint64_t cmdline_paddr;
    uint64_t rsdp_paddr;
    uint64_t memmap_paddr;
    uint32_t memmap_entries;
    uint32_t reserved;
};

struct HvmModlistEntry {
    uint64_t paddr;
    uint64_t size;
    uint64_t cmdline_paddr;
    uint64_t reserved;
};

struct HvmMemmapEntry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
    uint32_t reserved;
};

#define PVH_MEMMAP_MAX 8
#define PVH_CMDLINE_SIZE 2048

/* Everything the kernel is handed besides itself and the initrd, in the page
   below the VGA hole, which the memory map reports as reserved. */
struct PvhBootData {
    HvmStartInfo start_info;
    HvmModlistEntry modules[1];
    HvmMemmapEntry memmap[PVH_MEMMAP_MAX];
    uint64_t gdt[BOOT_GDT_ENTRIES];
    char cmdline[PVH_CMDLINE_SIZE];
};

#define PVH_BOOT_DATA_ADDR 0x9f000
static_assert(sizeof(PvhBootData) <= 0xa0000 - PVH_BOOT_DATA_ADDR,
              "the PVH boot data does not fit below the VGA hole");

#define ELF_CLASS32 1
#define ELF_CLASS64 2
#define ELF_EM_386 3
#define ELF_EM_X86_64 62
#define ELF_PT_LOAD 1
#define ELF_PT_NOTE 4

static bool is_elf(const uint8_t *buf, int buf_len)
{
    return buf_len >= 4 && memcmp(buf, "\x7f" "ELF", 4) == 0;
}

/* Guest RAM [addr, addr + len) when one writable RAM range holds all of it,
   otherwise NULL. */
static uint8_t *get_ram_range_ptr(PCMachine *s, uint64_t addr, uint64_t len)
{
    PhysMemoryRange *pr = s->mem_map->FindRange(addr);

    if (!pr || !pr->is_ram || (pr->devram_flags & DEVRAM_FLAG_ROM) ||
        len > pr->addr + pr->size - addr)
        return NULL;
    return pr->phys_mem + (uintptr_t)(addr - pr->addr);
}

/* The PHYS32_ENTRY value in the notes 'buf' holds, if any. */
static bool pvh_find_entry(const uint8_t *buf, uint64_t len, uint32_t *entry)
{
    uint64_t pos = 0;

    while (len - pos >= 12) {
        uint32_t namesz = get_le32(buf + pos);
        uint32_t descsz = get_le32(buf + pos + 4);
        uint32_t type = get_le32(buf + pos + 8);
        uint64_t name = pos + 12;
        uint64_t desc = name + ((namesz + 3) & ~3ULL);

        if (desc > len || ((descsz + 3) & ~3ULL) > len - desc)
            return false;
        if (namesz == 4 && memcmp(buf + name, "Xen", 4) == 0 &&
            type == XEN_ELFNOTE_PHYS32_ENTRY && descsz >= 4) {
            *entry = get_le32(buf + desc);
            return true;
        }
        pos = desc + ((descsz + 3) & ~3ULL);
    }
    return false;
}

static bool pvh_load(PCMachine *s, const uint8_t *buf, int buf_len,
                     const uint8_t *initrd, int initrd_len,
                     const char *cmd_line, uint32_t bios_size,
                     uint64_t rsdp)
{
    bool elf64;
    uint64_t phoff;
    int phentsize, phnum, machine;
    uint64_t kernel_end = 0;
    uint32_t entry;
    bool has_entry = false;

    if (buf_len < 0x40 || buf[5] != 1 /* little endian */ ||
        (buf[4] != ELF_CLASS32 && buf[4] != ELF_CLASS64)) {
        vm_error("pc: the kernel is not a little endian ELF file\n");
        return false;
    }
    elf64 = buf[4] == ELF_CLASS64;
    machine = get_le16(buf + 18);
    if (machine != (elf64 ? ELF_EM_X86_64 : ELF_EM_386)) {
        vm_error("pc: the kernel is not an x86 ELF file\n");
        return false;
    }
    /* the interpreter is an i686 */
    if (elf64 && !s->hypervisor) {
        vm_error("pc: a 64 bit kernel needs a hypervisor\n");
        return false;
    }
    if (elf64) {
        phoff = get_le64(buf + 32);
        phentsize = get_le16(buf + 54);
        phnum = get_le16(buf + 56);
    } else {
        phoff = get_le32(buf + 28);
        phentsize = get_le16(buf + 42);
        phnum = get_le16(buf + 44);
    }
    if (phentsize < (elf64 ? 56 : 32) ||
        phoff > (uint64_t)buf_len ||
        (uint64_t)phnum * phentsize > buf_len - phoff) {
        vm_error("pc: the kernel's program headers are truncated\n");
        return false;
    }

    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = buf + phoff + (uint64_t)i * phentsize;
        uint32_t type = get_le32(ph);
        uint64_t offset, paddr, filesz, memsz;

        if (elf64) {
            offset = get_le64(ph + 8);
            paddr = get_le64(ph + 24);
            filesz = get_le64(ph + 32);
            memsz = get_le64(ph + 40);
        } else {
            offset = get_le32(ph + 4);
            paddr = get_le32(ph + 12);
            filesz = get_le32(ph + 16);
            memsz = get_le32(ph + 20);
        }
        if (offset > (uint64_t)buf_len || filesz > buf_len - offset) {
            vm_error("pc: a kernel segment is past the end of the file\n");
            return false;
        }
        if (type == ELF_PT_NOTE && !has_entry) {
            has_entry = pvh_find_entry(buf + offset, filesz, &entry);
        } else if (type == ELF_PT_LOAD && memsz != 0) {
            uint8_t *ptr = NULL;

            /* below 1 MB is the boot data and the legacy areas */
            if (filesz <= memsz && paddr >= 0x100000)
                ptr = get_ram_range_ptr(s, paddr, memsz);
            if (ptr == NULL) {
                vm_error("pc: no RAM for the kernel segment at 0x%" PRIx64
                         "-0x%" PRIx64 "\n", paddr, paddr + memsz);
                return false;
            }
            memcpy(ptr, buf + offset, filesz);
            memset(ptr + filesz, 0, memsz - filesz);
            kernel_end = std::max(kernel_end, paddr + memsz);
        }
    }
    if (!has_entry) {
        vm_error("pc: the ELF kernel has no PVH entry point\n");
        return false;
    }

    PvhBootData *bd = reinterpret_cast<PvhBootData *>(
        get_ram_range_ptr(s, PVH_BOOT_DATA_ADDR, sizeof(PvhBootData)));
    assert(bd != NULL);
    memset(bd, 0, sizeof(*bd));
    HvmStartInfo *si = &bd->start_info;
    si->magic = XEN_HVM_START_MAGIC;
    si->version = 1;
    si->rsdp_paddr = rsdp;

    if (strlen(cmd_line) >= sizeof(bd->cmdline)) {
        vm_error("pc: the kernel command line is longer than %d bytes\n",
                 PVH_CMDLINE_SIZE - 1);
        return false;
    }
    strcpy(bd->cmdline, cmd_line);
    si->cmdline_paddr = PVH_BOOT_DATA_ADDR + offsetof(PvhBootData, cmdline);

    /* the initrd at the top of RAM */
    if (initrd_len > 0) {
        uint64_t addr = (s->ram_size - initrd_len) & ~(uint64_t)0xfff;
        uint8_t *ptr = NULL;

        if ((uint64_t)initrd_len < s->ram_size &&
            addr >= ((kernel_end + 0xfff) & ~(uint64_t)0xfff))
            ptr = get_ram_range_ptr(s, addr, initrd_len);
        if (ptr == NULL) {
            vm_error("pc: not enough RAM above the kernel for the initrd\n");
            return false;
        }
        memcpy(ptr, initrd, initrd_len);
        bd->modules[0].paddr = addr;
        bd->modules[0].size = initrd_len;
        si->nr_modules = 1;
        si->modlist_paddr = PVH_BOOT_DATA_ADDR +
            offsetof(PvhBootData, modules);
    }

    /* The RAM there is, less the boot data page, the VGA hole and the BIOS
       area. */
    int n = 0;
    auto add_range = [bd, &n](uint64_t addr, uint64_t size, uint32_t type) {
        assert(n < PVH_MEMMAP_MAX);
        bd->memmap[n].addr = addr;
        bd->memmap[n].size = size;
        bd->memmap[n].type = type;
        n++;
    };
    add_range(0, PVH_BOOT_DATA_ADDR, E820_RAM);
    add_range(PVH_BOOT_DATA_ADDR, 0x100000 - PVH_BOOT_DATA_ADDR,
              E820_RESERVED);
    add_range(0x100000, s->ram_size - 0x100000, E820_RAM);
    if (bios_size != 0)
        add_range(0x100000000ULL - bios_size, bios_size, E820_RESERVED);
    si->memmap_paddr = PVH_BOOT_DATA_ADDR + offsetof(PvhBootData, memmap);
    si->memmap_entries = n;

    set_flat_gdt(reinterpret_cast<uint8_t *>(bd->gdt));
    start_flat_protected_mode(s, PVH_BOOT_DATA_ADDR +
                              offsetof(PvhBootData, gdt),
                              entry, 3, PVH_BOOT_DATA_ADDR); /* ebx */
    map_pci_interrupts(s);
    return true;
}

void PCMachine::ProcessorThreadStarted()
{
    if (hypervisor) {
        hypervisor->ProcessorThreadStarted();
    }
}

/* The CMOS periodic interrupt is polled, so it has no deadline. */
int64_t PCMachine::RunTimers()
{
    PCMachine *s = this;

    cmos_update_irq(s->cmos_state.get());
    /* the hypervisor has the PIT */
    if (s->fHypervisorIrqchip)
        return -1;
    s->fTimerDelayUs = pit_update_irq(s->pit_state.get());
    return s->fTimerDelayUs;
}

bool PCMachine::Idle()
{
    if (hypervisor)
        return hypervisor->Idle(fCpuIrq.load());
    return x86_cpu_get_power_down(cpu_state);
}

void PCMachine::Interp(int max_exec_cycles)
{
    PCMachine *s = this;
    if (s->hypervisor) {
        s->hypervisor->Run(s->fTimerDelayUs);
    } else {
        x86_cpu_interp(s->cpu_state, max_exec_cycles);
    }
}

void PCMachine::InterruptExecution()
{
    if (hypervisor) {
        hypervisor->InterruptRun();
    }
}

class PcMachineClass final: public VirtMachineClass {
public:
    const char *MachineNames() const override {return "pc";}

    void SetDefaults(VirtMachineParams *p) const override
    {
        p->accel_enable = true;
    }

    std::unique_ptr<VirtMachine> Init(const VirtMachineParams *p) const override
    {
        return pc_machine_init(p);
    }
};

static const PcMachineClass sPcMachineClass;
const VirtMachineClass &gPcMachineClass = sPcMachineClass;
