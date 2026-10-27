/*
 * Local APIC
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
#include "lapic.h"

#include "bits.h"
#include "host_time.h"

/* registers, by offset in the page */
enum {
    REG_ID = 0x20,
    REG_VERSION = 0x30,
    REG_TPR = 0x80,
    REG_APR = 0x90,
    REG_PPR = 0xa0,
    REG_EOI = 0xb0,
    REG_LDR = 0xd0,
    REG_DFR = 0xe0,
    REG_SVR = 0xf0,
    REG_ISR = 0x100,
    REG_TMR = 0x180,
    REG_IRR = 0x200,
    REG_ESR = 0x280,
    REG_LVT_CMCI = 0x2f0,
    REG_ICR_LOW = 0x300,
    REG_ICR_HIGH = 0x310,
    REG_LVT_TIMER = 0x320,
    REG_LVT_ERROR = 0x370,
    REG_INITIAL_COUNT = 0x380,
    REG_CURRENT_COUNT = 0x390,
    REG_DIVIDE = 0x3e0,
};

/* fLvt indexes */
enum {
    LVT_CMCI,
    LVT_TIMER,
    LVT_THERMAL,
    LVT_PERF,
    LVT_LINT0,
    LVT_LINT1,
    LVT_ERROR,
};

/* LVT, ICR and message fields */
enum {
    DELIVERY_MODE = 8,  /* 3 bits */
    DELIVERY_STATUS = 12,
    LEVEL_ASSERT = 14,
    TRIGGER_LEVEL = 15,
    LVT_MASKED = 16,
    TIMER_PERIODIC = 17,
    ICR_SHORTHAND = 18, /* 2 bits */
    ICR_LOGICAL = 11,
};

enum {
    MODE_FIXED = 0,
    MODE_LOWEST = 1,
    MODE_INIT = 5,
    MODE_STARTUP = 6,
    MODE_EXTINT = 7,
};

enum {
    SHORTHAND_NONE,
    SHORTHAND_SELF,
    SHORTHAND_ALL,
    SHORTHAND_OTHERS,
};

enum {
    BASE_BSP = 8,
    BASE_X2APIC = 10,
    BASE_ENABLE = 11,
};

#define SVR_ENABLE 8

/* ESR bits, by the Intel SDM */
#define ESR_SEND_ILLEGAL_VECTOR bit_at(5)
#define ESR_RECV_ILLEGAL_VECTOR bit_at(6)
#define ESR_ILLEGAL_REGISTER    bit_at(7)

/* The bits each LVT keeps, indexed like fLvt: vector, delivery mode,
   polarity and trigger where the entry has them, the mask, and the
   timer's periodic mode. The delivery status and remote IRR are read only
   and never set here. */
static const uint32_t kLvtWritable[7] = {
    0x000107ff, /* CMCI */
    0x000300ff, /* timer, without TSC deadline mode */
    0x000107ff, /* thermal */
    0x000107ff, /* performance counters */
    0x0001a7ff, /* LINT0 */
    0x0001a7ff, /* LINT1 */
    0x000100ff, /* error */
};

/* version 0x14 with 7 LVT entries */
#define LAPIC_VERSION 0x00060014

#define TIMER_HZ 1000000000


LocalApic::LocalApic(LocalApicBus &bus, LocalApicHost &host, uint32_t id):
    fBus(bus), fHost(host), fId(id)
{
    Reset();
}


/* The state after a reset, processor 0 with LINT0 ready for the 8259s as
   firmware would leave it. */
void LocalApic::Reset()
{
    fBase = LAPIC_BASE | bit_at(BASE_ENABLE) | (fId == 0 ? bit_at(BASE_BSP) : 0);
    Init();
    if (fId == 0) {
        fLvt[LVT_LINT0] = set_bits(0, DELIVERY_MODE, 3, MODE_EXTINT);
        Update();
    }
}


/* An INIT resets all but the ID and IA32_APIC_BASE. */
void LocalApic::Init()
{
    fTpr = 0;
    fLdr = 0;
    fDfr = UINT32_MAX;
    fSvr = 0xff;
    fIcrLow = fIcrHigh = 0;
    for (uint32_t &lvt : fLvt)
        lvt = bit_at(LVT_MASKED);
    fDivide = 0;
    fInitialCount = 0;
    fTimerArmed = false;
    fEsr = fEsrPending = 0;
    for (int i = 0; i < 8; i++)
        fIrr[i] = fIsr[i] = fTmr[i] = 0;
    Update();
}


bool LocalApic::Enabled() const
{
    return get_bit(fBase, BASE_ENABLE) && get_bit(fSvr, SVR_ENABLE);
}


/* A disabled APIC passes INTR straight through. */
bool LocalApic::AcceptsExtInt() const
{
    uint32_t lint0 = fLvt[LVT_LINT0];
    return !get_bit(fBase, BASE_ENABLE) ||
        (!get_bit(lint0, LVT_MASKED) &&
         get_bits(lint0, DELIVERY_MODE, 3) == MODE_EXTINT);
}


/* The offsets no register answers at in xAPIC mode, by the Intel SDM. */
static bool reserved_register(uint32_t offset)
{
    return offset <= 0x010 || (offset >= 0x040 && offset <= 0x070) ||
        offset == 0x0c0 || (offset >= 0x290 && offset <= 0x2e0) ||
        (offset >= 0x3a0 && offset <= 0x3d0) || offset == 0x3f0;
}


/* The highest set bit of a 256 bit register, or -1. */
static int highest_vector(const uint32_t *reg)
{
    for (int i = 7; i >= 0; i--) {
        if (reg[i] != 0)
            return i * 32 + 31 - __builtin_clz(reg[i]);
    }
    return -1;
}


int LocalApic::Ppr() const
{
    int isr = highest_vector(fIsr);
    int isr_class = isr < 0 ? 0 : isr & 0xf0;
    return (fTpr & 0xf0) >= isr_class ? fTpr & 0xff : isr_class;
}


/* An accepted vector above the processor priority. */
bool LocalApic::Deliverable() const
{
    int irr = highest_vector(fIrr);
    return irr >= 0 && Enabled() && (irr & 0xf0) > (Ppr() & 0xf0);
}


void LocalApic::Update()
{
    fHost.SetApicInterrupt(fId, Deliverable() || (fLint0 && AcceptsExtInt()));
}


void LocalApic::Accept(int vector, bool level)
{
    /* vectors 0-15 are illegal */
    if (vector < 16) {
        Error(ESR_RECV_ILLEGAL_VECTOR);
        return;
    }
    int word = vector / 32, bit = vector % 32;
    fIrr[word] = set_bit(fIrr[word], bit, true);
    fTmr[word] = set_bit(fTmr[word], bit, level);
    Update();
}


/* An error is recorded for the next ESR write to show, and interrupts
   through the error LVT the first time it is found. Its own vector being
   illegal is then an error already recorded, which ends there. */
void LocalApic::Error(uint32_t bit)
{
    if (fEsrPending & bit)
        return;
    fEsrPending |= bit;
    if (!get_bit(fLvt[LVT_ERROR], LVT_MASKED))
        Accept(get_bits(fLvt[LVT_ERROR], 0, 8), false);
}


/* Flat model: a bit per APIC; cluster model: a cluster and 4 bits. */
bool LocalApic::MatchesLogical(uint32_t dest) const
{
    uint32_t ldr = get_bits(fLdr, 24, 8);
    if (get_bits(fDfr, 28, 4) == 0xf)
        return (ldr & dest) != 0;
    uint32_t cluster = get_bits(dest, 4, 4);
    return (cluster == 0xf || cluster == get_bits(ldr, 4, 4)) &&
        (get_bits(dest, 0, 4) & get_bits(ldr, 0, 4)) != 0;
}


bool LocalApic::IsDestination(uint32_t dest, bool logical) const
{
    return logical ? MatchesLogical(dest) : dest == fId || dest == 0xff;
}


void LocalApic::Receive(int mode, int vector, bool level, bool assert)
{
    switch (mode) {
    case MODE_FIXED:
    case MODE_LOWEST:
        Accept(vector, level);
        break;
    case MODE_INIT:
        /* the de-assert of a level triggered INIT does nothing today */
        if (level && !assert)
            break;
        Init();
        fHost.ApicInit(fId);
        break;
    case MODE_STARTUP:
        fHost.ApicStartup(fId, vector);
        break;
    default:
        break;
    }
}


void LocalApic::SetLint0(bool level)
{
    fLint0 = level;
    Update();
}


int LocalApic::Acknowledge()
{
    if (Deliverable()) {
        int vector = highest_vector(fIrr);
        int word = vector / 32, bit = vector % 32;
        fIrr[word] = set_bit(fIrr[word], bit, false);
        fIsr[word] = set_bit(fIsr[word], bit, true);
        Update();
        return vector;
    }
    if (fLint0 && AcceptsExtInt())
        return LAPIC_EXTINT;
    return get_bits(fSvr, 0, 8);
}


void LocalApic::SendIpi()
{
    int mode = get_bits(fIcrLow, DELIVERY_MODE, 3);
    /* a fixed or lowest priority message names a vector, which 0-15 is not;
       such a message is not sent */
    if ((mode == MODE_FIXED || mode == MODE_LOWEST) &&
        get_bits(fIcrLow, 0, 8) < 16) {
        Error(ESR_SEND_ILLEGAL_VECTOR);
        return;
    }
    fBus.Send(fId, get_bits(fIcrLow, ICR_SHORTHAND, 2),
              get_bits(fIcrHigh, 24, 8), get_bit(fIcrLow, ICR_LOGICAL),
              get_bits(fIcrLow, DELIVERY_MODE, 3), get_bits(fIcrLow, 0, 8),
              get_bit(fIcrLow, TRIGGER_LEVEL), get_bit(fIcrLow, LEVEL_ASSERT));
}


bool LocalApic::SetBase(uint64_t val)
{
    uint64_t addr = val & ~(uint64_t)0xfff;
    /* no x2APIC, and the page stays where the machine put it */
    if (get_bit(val, BASE_X2APIC) || addr != LAPIC_BASE ||
        (val & 0xfff & ~(bit_at(BASE_BSP) | bit_at(BASE_ENABLE))) != 0)
        return false;
    fBase = set_bit(fBase, BASE_ENABLE, get_bit(val, BASE_ENABLE));
    Update();
    return true;
}


void LocalApic::SetTaskPriority(int cr8)
{
    fTpr = cr8 << 4;
    Update();
}


//#pragma mark - timer

uint32_t LocalApic::TimerDivisor() const
{
    int n = get_bits(fDivide, 0, 2) | get_bit(fDivide, 3) << 2;
    return n == 7 ? 1 : 2 << n;
}


uint64_t LocalApic::TimerTicks(uint64_t now) const
{
    return (now - fTimerStart) * (TIMER_HZ / 1000000) / TimerDivisor();
}


uint32_t LocalApic::CurrentCount(uint64_t now) const
{
    if (fInitialCount == 0)
        return 0;
    uint64_t ticks = TimerTicks(now);
    if (get_bit(fLvt[LVT_TIMER], TIMER_PERIODIC))
        return fInitialCount - ticks % fInitialCount;
    return ticks >= fInitialCount ? 0 : fInitialCount - ticks;
}


/* When the period after the ones counted so far ends, rounded up to a
   microsecond, or UINT64_MAX with no count running. */
uint64_t LocalApic::TimerDue() const
{
    if (!fTimerArmed)
        return UINT64_MAX;
    uint64_t ticks = (fTimerFired + 1) * fInitialCount * TimerDivisor();
    return fTimerStart + (ticks + TIMER_HZ / 1000000 - 1) /
        (TIMER_HZ / 1000000);
}


/* Periods that ran out while nobody looked are one interrupt. */
int64_t LocalApic::RunTimer()
{
    if (!fTimerArmed)
        return -1;
    uint64_t now = host_monotonic_us();
    uint64_t periods = TimerTicks(now) / fInitialCount;
    bool periodic = get_bit(fLvt[LVT_TIMER], TIMER_PERIODIC);
    if (periods > fTimerFired) {
        fTimerFired = periods;
        if (!get_bit(fLvt[LVT_TIMER], LVT_MASKED))
            Accept(get_bits(fLvt[LVT_TIMER], 0, 8), false);
        /* a one-shot count stays in the initial count register, and the
           current count stops at 0 */
        if (!periodic) {
            fTimerArmed = false;
            return -1;
        }
    }
    uint64_t due = TimerDue();
    return due > now ? due - now : 0;
}


//#pragma mark - registers

uint64_t LocalApic::DeviceRead(uint32_t offset, int size_log2)
{
    offset &= 0xff0;
    switch (offset) {
    case REG_ID:
        return fId << 24;
    case REG_VERSION:
        return LAPIC_VERSION;
    case REG_TPR:
        return fTpr;
    case REG_PPR:
        return Ppr();
    case REG_LDR:
        return fLdr;
    case REG_DFR:
        return fDfr;
    case REG_SVR:
        return fSvr;
    case REG_ICR_LOW:
        /* IPIs are sent at once */
        return set_bit(fIcrLow, DELIVERY_STATUS, false);
    case REG_ICR_HIGH:
        return fIcrHigh;
    case REG_LVT_CMCI:
        return fLvt[LVT_CMCI];
    case REG_INITIAL_COUNT:
        return fInitialCount;
    case REG_CURRENT_COUNT:
        return CurrentCount(host_monotonic_us());
    case REG_DIVIDE:
        return fDivide;
    }
    if (offset >= REG_ISR && offset < REG_ISR + 0x80)
        return fIsr[(offset - REG_ISR) / 16];
    if (offset >= REG_TMR && offset < REG_TMR + 0x80)
        return fTmr[(offset - REG_TMR) / 16];
    if (offset >= REG_IRR && offset < REG_IRR + 0x80)
        return fIrr[(offset - REG_IRR) / 16];
    if (offset >= REG_LVT_TIMER && offset <= REG_LVT_ERROR)
        return fLvt[LVT_TIMER + (offset - REG_LVT_TIMER) / 16];
    if (offset == REG_ESR)
        return fEsr;
    if (reserved_register(offset))
        Error(ESR_ILLEGAL_REGISTER);
    /* APR, EOI and the reserved registers */
    return 0;
}


void LocalApic::DeviceWrite(uint32_t offset, uint64_t val, int size_log2)
{
    offset &= 0xff0;
    switch (offset) {
    case REG_TPR:
        fTpr = get_bits(val, 0, 8);
        Update();
        return;
    case REG_EOI: {
        int vector = highest_vector(fIsr);
        if (vector < 0)
            return;
        int word = vector / 32, bit = vector % 32;
        fIsr[word] = set_bit(fIsr[word], bit, false);
        if (get_bit(fTmr[word], bit))
            fHost.ApicEoi(vector);
        Update();
        return;
    }
    case REG_LDR:
        fLdr = val & 0xff000000;
        return;
    case REG_DFR:
        fDfr = val | 0x0fffffff;
        return;
    case REG_SVR:
        fSvr = val & 0x3ff;
        /* disabling masks the LVT */
        if (!get_bit(fSvr, SVR_ENABLE)) {
            for (uint32_t &lvt : fLvt)
                lvt = set_bit(lvt, LVT_MASKED, true);
        }
        Update();
        return;
    case REG_ICR_LOW:
        fIcrLow = val;
        SendIpi();
        return;
    case REG_ICR_HIGH:
        fIcrHigh = val & 0xff000000;
        return;
    case REG_ESR:
        /* shows the errors found since the last write */
        fEsr = fEsrPending;
        fEsrPending = 0;
        return;
    case REG_INITIAL_COUNT:
        fInitialCount = val;
        fTimerArmed = val != 0;
        fTimerStart = host_monotonic_us();
        fTimerFired = 0;
        fHost.ApicTimerChanged(TimerDue());
        return;
    case REG_DIVIDE:
        fDivide = val & 0xb;
        /* a count in progress starts over at the new rate */
        if (fTimerArmed) {
            fTimerStart = host_monotonic_us();
            fTimerFired = 0;
            fHost.ApicTimerChanged(TimerDue());
        }
        return;
    }
    int lvt = -1;
    if (offset == REG_LVT_CMCI)
        lvt = LVT_CMCI;
    else if (offset >= REG_LVT_TIMER && offset <= REG_LVT_ERROR)
        lvt = LVT_TIMER + (offset - REG_LVT_TIMER) / 16;
    if (lvt >= 0) {
        val &= kLvtWritable[lvt];
        if (!get_bit(fSvr, SVR_ENABLE))
            val = set_bit(val, LVT_MASKED, true);
        fLvt[lvt] = val;
        Update();
        if (lvt == LVT_TIMER)
            fHost.ApicTimerChanged(TimerDue());
        return;
    }
    if (reserved_register(offset))
        Error(ESR_ILLEGAL_REGISTER);
    /* ID and the read only registers ignore writes */
}


//#pragma mark - bus

LocalApicBus::LocalApicBus(LocalApicHost &host, int count)
{
    for (int i = 0; i < count; i++)
        fApics.push_back(std::make_unique<LocalApic>(*this, host, i));
}


void LocalApicBus::Deliver(uint64_t addr, uint32_t data)
{
    Send(0, SHORTHAND_NONE, get_bits(addr, 12, 8), get_bit(addr, 2),
         get_bits(data, DELIVERY_MODE, 3), get_bits(data, 0, 8),
         get_bit(data, TRIGGER_LEVEL), get_bit(data, LEVEL_ASSERT));
}


/* Lowest priority delivery picks the destination of lowest processor
   priority, the first of equals. */
void LocalApicBus::Send(uint32_t source, int shorthand, uint32_t dest,
                        bool logical, int mode, int vector, bool level,
                        bool assert)
{
    LocalApic *lowest = nullptr;
    for (auto &apic : fApics) {
        bool target;
        switch (shorthand) {
        case SHORTHAND_SELF:
            target = apic->Id() == source;
            break;
        case SHORTHAND_ALL:
            target = true;
            break;
        case SHORTHAND_OTHERS:
            target = apic->Id() != source;
            break;
        default:
            target = apic->IsDestination(dest, logical);
            break;
        }
        if (!target)
            continue;
        if (mode != MODE_LOWEST) {
            apic->Receive(mode, vector, level, assert);
        } else if (lowest == nullptr || apic->Ppr() < lowest->Ppr()) {
            lowest = apic.get();
        }
    }
    if (lowest != nullptr)
        lowest->Receive(mode, vector, level, assert);
}


int64_t LocalApicBus::RunTimers()
{
    int64_t delay = -1;
    for (auto &apic : fApics) {
        int64_t d = apic->RunTimer();
        if (d >= 0 && (delay < 0 || d < delay))
            delay = d;
    }
    return delay;
}
