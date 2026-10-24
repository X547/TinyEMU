/*
 * 8259 PIC
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

#include "bits.h"
#include "i8259.h"

//#define DEBUG_PIC

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
    uint8_t single; /* initialized without a cascade, so no ICW3 */
    uint8_t elcr; /* PIIX edge/trigger selection*/
    uint8_t elcr_mask;
    I8259Pair *update_target;

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t val, int size_log2);
    uint32_t ElcrRead(uint32_t offset, int size_log2);
    void ElcrWrite(uint32_t offset, uint32_t val, int size_log2);

    DeviceIOAdapter<PICState, &PICState::Read, &PICState::Write> fIo {*this};
    DeviceIOAdapter<PICState, &PICState::ElcrRead,
                    &PICState::ElcrWrite> fElcrIo {*this};
};

static void pic_reset(PICState *s);

static std::unique_ptr<PICState> pic_init(PhysMemoryMap *port_map, int port,
                                          int elcr_port,
                                          int elcr_mask,
                                          I8259Pair *update_target)
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
    s->single = 0;
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
    printf("pic_write: addr=%d val=0x%02x\n", addr, val);
#endif
    if (addr == 0) {
        if (get_bit(val, 4)) {
            /* init */
            pic_reset(s);
            s->init_state = 1;
            s->init4 = get_bit(val, 0);
            /* SNGL: a lone 8259, which takes no ICW3. LTIM is ignored, as on
               the PIIX: the ELCRs choose edge or level per input. */
            s->single = get_bit(val, 1);
        } else if (get_bit(val, 3)) {
            if (get_bit(val, 1))
                s->read_reg_select = get_bit(val, 0);
            if (get_bit(val, 6)) {
                s->special_mask = get_bit(val, 5);
                /* which requests outrank what is in service has changed */
                s->update_target->Update();
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
            s->update_target->Update();
        }
    } else {
        switch(s->init_state) {
        case 0:
            /* normal mode */
            s->imr = val;
            s->update_target->Update();
            break;
        case 1:
            s->irq_base = set_bits(val, 0, 3, 0);
            if (!s->single)
                s->init_state = 2;
            else
                s->init_state = s->init4 ? 3 : 0;
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
    printf("pic_read: addr=%d val=0x%02x\n", addr, ret);
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

I8259Pair::I8259Pair(PhysMemoryMap *port_map, uint32_t addr0, uint32_t addr1,
                     uint32_t elcr_addr0, uint32_t elcr_addr1,
                     CPUIRQTarget &cpu, IRQSignal *irqs):
    fCpu(cpu)
{
    for (int i = 0; i < I8259_IRQ_COUNT; i++)
        irqs[i].Init(this, i);
    fPics[0] = pic_init(port_map, addr0, elcr_addr0, 0xf8, this);
    fPics[1] = pic_init(port_map, addr1, elcr_addr1, 0xde, this);
}

I8259Pair::~I8259Pair() = default;

void I8259Pair::SetElcr(const uint8_t *elcr)
{
    int i;
    for(i = 0; i < 2; i++) {
        fPics[i]->elcr = elcr[i] & fPics[i]->elcr_mask;
    }
}

/* raise irq to CPU if necessary. must be called every time the active
   irq may change */
void I8259Pair::Update()
{
    int irq2, irq;

    /* first look at slave pic */
    irq2 = pic_get_irq(fPics[1].get());
    if (irq2 >= 0) {
        /* if irq request by slave pic, signal master PIC */
        pic_set_irq1(fPics[0].get(), 2, 1);
        pic_set_irq1(fPics[0].get(), 2, 0);
    }
    /* look at requested irq */
    irq = pic_get_irq(fPics[0].get());
    if (irq >= 0) {
        /* raise IRQ request on the CPU */
        fCpu.SetCPUIRQ(1);
    } else {
        /* lower irq */
        fCpu.SetCPUIRQ(0);
    }
}

void I8259Pair::SetIRQ(int irq, int level)
{
#if defined(DEBUG_PIC)
    if (irq != 0)
        printf("pic_set_irq: irq=%d level=%d\n", irq, level);
#endif
    pic_set_irq1(fPics[irq >> 3].get(), irq & 7, level);
    Update();
}

/* called from the CPU to get the hardware interrupt number */
int I8259Pair::Acknowledge()
{
    int irq, irq2, intno;

    irq = pic_get_irq(fPics[0].get());
    if (irq >= 0) {
        pic_intack(fPics[0].get(), irq);
        if (irq == 2) {
            irq2 = pic_get_irq(fPics[1].get());
            if (irq2 >= 0) {
                pic_intack(fPics[1].get(), irq2);
            } else {
                /* spurious IRQ on slave controller */
                irq2 = 7;
            }
            intno = fPics[1]->irq_base + irq2;
            irq = irq2 + 8;
        } else {
            intno = fPics[0]->irq_base + irq;
        }
    } else {
        /* spurious IRQ on host controller */
        irq = 7;
        intno = fPics[0]->irq_base + irq;
    }
    Update();

#if defined(DEBUG_PIC)
    if (irq != 0 && irq != 14)
        printf("pic_interrupt: irq=%d\n", irq);
#endif
    return intno;
}
