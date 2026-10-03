/*
 * HPET
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
#include "hpet.h"

#include "bits.h"
#include "devices.h"
#include "host_time.h"
#include "machine.h"

/* general registers */
#define HPET_CAP     0x000
#define HPET_CONFIG  0x010
#define HPET_STATUS  0x020
#define HPET_COUNTER 0x0f0
#define HPET_TIMER_BASE   0x100
#define HPET_TIMER_STRIDE 0x20

/* each timer's */
#define TIMER_CONFIG     0x00
#define TIMER_COMPARATOR 0x08

/* capabilities */
#define CAP_REV_ID       1
#define CAP_COUNT_SIZE   bit_at(13)
#define CAP_LEGACY_ROUTE bit_at(15)
#define CAP_VENDOR_ID    0x8086
/* 10 ns, in femtoseconds */
#define COUNTER_PERIOD_FS 10000000
#define TICKS_PER_US      100

/* general configuration */
#define CONFIG_ENABLE bit_at<uint64_t>(0)
#define CONFIG_LEGACY bit_at<uint64_t>(1)

/* timer configuration */
#define TN_LEVEL        bit_at<uint64_t>(1)
#define TN_INT_ENABLE   bit_at<uint64_t>(2)
#define TN_PERIODIC     bit_at<uint64_t>(3)
#define TN_PERIODIC_CAP bit_at<uint64_t>(4)
#define TN_SIZE_CAP     bit_at<uint64_t>(5)
#define TN_SET_VALUE    bit_at<uint64_t>(6)
#define TN_32BIT        bit_at<uint64_t>(8)
#define TN_ROUTE_POS    9
#define TN_ROUTE_LEN    5
#define TN_WRITABLE (TN_LEVEL | TN_INT_ENABLE | TN_PERIODIC | TN_SET_VALUE | \
                     TN_32BIT | field_mask<uint64_t>(TN_ROUTE_POS, TN_ROUTE_LEN))

/* where a timer's interrupt goes besides an IOAPIC input */
#define LINE_NONE -1
#define LINE_IRQ0 -2
#define LINE_IRQ8 -3

/* A comparator that has already fired is only reached again when the counter
   wraps, so the wait is capped rather than computed. */
#define MAX_DELAY_US 1000000

/* A 64 bit read reaches the device as its low half, then its high half. The
   high half read this soon after a low half one belongs with it. */
#define SPLIT_READ_US 10


static uint64_t width_mask(bool narrow)
{
    return narrow ? 0xffffffffULL : ~0ULL;
}


/* Whether 'target' lies in (last, now], counting modulo the timer's width. */
static bool crossed(uint64_t last, uint64_t now, uint64_t target, bool narrow)
{
    uint64_t mask = width_mask(narrow);

    return ((target - last - 1) & mask) < ((now - last) & mask);
}


HPET::HPET(VirtMachine *machine):
    fMachine(machine)
{
    for (Timer &t : fTimers)
        t.raisedLine = LINE_NONE;
}


void HPET::SetOutputs(IRQSignal *irq0, IRQSignal *irq8, IRQTarget *ioapic,
                      uint32_t route_mask)
{
    fIrq0 = irq0;
    fIrq8 = irq8;
    fIoapic = ioapic;
    fRouteMask = route_mask;
}


bool HPET::Enabled() const
{
    return fConfig & CONFIG_ENABLE;
}


bool HPET::LegacyReplacement() const
{
    return Enabled() && (fConfig & CONFIG_LEGACY);
}


uint32_t HPET::BlockId() const
{
    return CAP_REV_ID | ((HPET_TIMERS - 1) << 8) | CAP_COUNT_SIZE |
        CAP_LEGACY_ROUTE | (CAP_VENDOR_ID << 16);
}


uint64_t HPET::Counter(uint64_t now) const
{
    if (!Enabled())
        return fCounterBase;
    return fCounterBase + (now - fBaseUs) * TICKS_PER_US;
}


/* The comparators are checked from the new value on. */
void HPET::SetCounter(uint64_t counter, uint64_t now)
{
    fCounterBase = counter;
    fBaseUs = now;
    for (Timer &t : fTimers)
        t.last = counter;
}


/* Fires the comparators the counter went past since they were last checked.
   A periodic one that fell behind by several periods fires once. */
void HPET::Process(uint64_t now)
{
    if (!Enabled())
        return;

    uint64_t counter = Counter(now);
    for (int i = 0; i < HPET_TIMERS; i++) {
        Timer &t = fTimers[i];
        bool narrow = t.config & TN_32BIT;
        uint64_t mask = width_mask(narrow);

        if (crossed(t.last, counter, t.comparator, narrow)) {
            Fire(i);
            uint64_t period = t.period & mask;
            if ((t.config & TN_PERIODIC) && period != 0) {
                uint64_t behind = (counter - t.comparator) & mask;
                t.comparator = (t.comparator +
                                (behind / period + 1) * period) & mask;
            }
        }
        t.last = counter;
    }
}


void HPET::Fire(int index)
{
    Timer &t = fTimers[index];

    if (t.config & TN_LEVEL) {
        fStatus |= bit_at<uint64_t>(index);
        UpdateLevel(index);
    } else if (t.config & TN_INT_ENABLE) {
        int line = LineOf(index);
        SetLine(line, 1);
        SetLine(line, 0);
    }
}


int HPET::LineOf(int index) const
{
    if (LegacyReplacement() && index < 2)
        return index == 0 ? LINE_IRQ0 : LINE_IRQ8;

    int route = get_bits(fTimers[index].config, TN_ROUTE_POS, TN_ROUTE_LEN);
    return get_bit(fRouteMask, route) ? route : LINE_NONE;
}


void HPET::SetLine(int line, int level)
{
    switch (line) {
    case LINE_NONE:
        break;
    case LINE_IRQ0:
        fIrq0->Set(level);
        break;
    case LINE_IRQ8:
        fIrq8->Set(level);
        break;
    default:
        fIoapic->SetIRQ(line, level);
        break;
    }
}


/* A level interrupt holds its line up while its status bit is set, and
   follows the timer to another line when it is rerouted. */
void HPET::UpdateLevel(int index)
{
    Timer &t = fTimers[index];
    bool on = Enabled() && (t.config & TN_LEVEL) &&
        (t.config & TN_INT_ENABLE) && get_bit(fStatus, index);
    int line = on ? LineOf(index) : LINE_NONE;

    if (line != t.raisedLine) {
        SetLine(t.raisedLine, 0);
        SetLine(line, 1);
        t.raisedLine = line;
    }
}


int64_t HPET::RunTimers()
{
    uint64_t now = host_monotonic_us();

    Process(now);
    if (!Enabled())
        return -1;

    uint64_t counter = Counter(now);
    int64_t delay = -1;
    for (const Timer &t : fTimers) {
        if (!(t.config & TN_INT_ENABLE))
            continue;
        uint64_t ticks = (t.comparator - counter) &
            width_mask(t.config & TN_32BIT);
        if (ticks == 0)
            continue;
        uint64_t us = (ticks + TICKS_PER_US - 1) / TICKS_PER_US;
        if (us > MAX_DELAY_US)
            us = MAX_DELAY_US;
        if (delay < 0 || (int64_t)us < delay)
            delay = us;
    }
    return delay;
}


uint32_t HPET::TimerRead(int index, uint32_t reg)
{
    const Timer &t = fTimers[index];
    uint64_t val;

    switch (reg & ~7) {
    case TIMER_CONFIG:
        val = t.config | TN_PERIODIC_CAP | TN_SIZE_CAP |
            ((uint64_t)fRouteMask << 32);
        break;
    case TIMER_COMPARATOR:
        val = t.comparator;
        break;
    default:
        val = 0;
        break;
    }
    return val >> ((reg & 4) * 8);
}


/* In periodic mode a comparator write sets the period, and also the
   comparator when the set value bit asks for it. That bit is spent by the
   write, but a 64 bit write arrives as two halves, so it carries over to the
   high half that follows. */
void HPET::TimerWrite(int index, uint32_t reg, uint32_t val, uint64_t now)
{
    Timer &t = fTimers[index];
    bool periodic = t.config & TN_PERIODIC;
    bool set_value;

    switch (reg) {
    case TIMER_CONFIG:
        t.config = val & TN_WRITABLE;
        if (!(t.config & TN_LEVEL))
            fStatus &= ~bit_at<uint64_t>(index);
        if (t.config & TN_32BIT) {
            t.comparator &= 0xffffffff;
            t.period &= 0xffffffff;
        }
        UpdateLevel(index);
        break;
    case TIMER_COMPARATOR:
        set_value = t.config & TN_SET_VALUE;
        if (!periodic || set_value)
            t.comparator = set_bits(t.comparator, 0, 32, val);
        if (periodic)
            t.period = set_bits(t.period, 0, 32, val);
        t.config &= ~TN_SET_VALUE;
        t.setValueHigh = set_value;
        t.last = Counter(now);
        break;
    case TIMER_COMPARATOR + 4:
        set_value = (t.config & TN_SET_VALUE) || t.setValueHigh;
        t.config &= ~TN_SET_VALUE;
        t.setValueHigh = false;
        if (t.config & TN_32BIT)
            break;
        if (!periodic || set_value)
            t.comparator = set_bits(t.comparator, 32, 32, val);
        if (periodic)
            t.period = set_bits(t.period, 32, 32, val);
        t.last = Counter(now);
        break;
    }
}


uint32_t HPET::DeviceRead(uint32_t offset, int size_log2)
{
    uint64_t now = host_monotonic_us();
    uint64_t val;

    Process(now);
    if (offset >= HPET_TIMER_BASE) {
        int index = (offset - HPET_TIMER_BASE) / HPET_TIMER_STRIDE;
        if (index >= HPET_TIMERS)
            return 0;
        return TimerRead(index, (offset - HPET_TIMER_BASE) % HPET_TIMER_STRIDE);
    }

    switch (offset & ~7) {
    case HPET_CAP:
        val = BlockId() | ((uint64_t)COUNTER_PERIOD_FS << 32);
        break;
    case HPET_CONFIG:
        val = fConfig;
        break;
    case HPET_STATUS:
        val = fStatus;
        break;
    case HPET_COUNTER:
        /* the halves of one read agree, even across a carry */
        if (!(offset & 4)) {
            fLowRead = Counter(now);
            fLowReadUs = now;
            val = fLowRead;
        } else if (now - fLowReadUs < SPLIT_READ_US) {
            val = fLowRead;
        } else {
            val = Counter(now);
        }
        break;
    default:
        val = 0;
        break;
    }
    return val >> ((offset & 4) * 8);
}


void HPET::DeviceWrite(uint32_t offset, uint32_t val, int size_log2)
{
    uint64_t now = host_monotonic_us();
    uint64_t counter;

    Process(now);
    if (offset >= HPET_TIMER_BASE) {
        int index = (offset - HPET_TIMER_BASE) / HPET_TIMER_STRIDE;
        if (index < HPET_TIMERS) {
            TimerWrite(index, (offset - HPET_TIMER_BASE) % HPET_TIMER_STRIDE,
                       val, now);
        }
    } else {
        switch (offset) {
        case HPET_CONFIG: {
            /* halting keeps the count, and starting again goes on from it */
            counter = Counter(now);
            uint64_t old = fConfig;
            fConfig = val & (CONFIG_ENABLE | CONFIG_LEGACY);
            if ((old ^ fConfig) & CONFIG_ENABLE)
                SetCounter(counter, now);
            for (int i = 0; i < HPET_TIMERS; i++)
                UpdateLevel(i);
            break;
        }
        case HPET_STATUS:
            fStatus &= ~(uint64_t)val;
            for (int i = 0; i < HPET_TIMERS; i++)
                UpdateLevel(i);
            break;
        case HPET_COUNTER:
            SetCounter(set_bits(Counter(now), 0, 32, val), now);
            break;
        case HPET_COUNTER + 4:
            SetCounter(set_bits(Counter(now), 32, 32, val), now);
            break;
        }
    }
    /* the next deadline may have moved */
    fMachine->Kick();
}


//#pragma mark - configuration node


class HPETDevice final: public Device {
private:
    HPET fHpet;
    Resource *fRegs = nullptr;

public:
    HPETDevice(DeviceContext *ctx): Device("hpet"), fHpet(ctx->machine) {}

    HPET *State() {return &fHpet;}

    bool Prepare() override
    {
        SystemBus *sys = dynamic_cast<SystemBus *>(ParentBus());
        if (sys == nullptr || !sys->IsPortBased()) {
            vm_error("%s: must be attached to a PC system bus\n", Name());
            return false;
        }
        fRegs = AddFixedResource(RES_MMIO, HPET_ADDR, HPET_SIZE);
        return fRegs != nullptr;
    }

    bool Realize() override
    {
        SystemBus *sys = static_cast<SystemBus *>(ParentBus());

        sys->MemMap()->RegisterDevice(fRegs->base, fRegs->size, &fHpet,
                                      DEVIO_SIZE32);
        return true;
    }
};


/* The HPET at HPET_ADDR. */
class HPETClass final: public DeviceClass {
public:
    HPETClass(): DeviceClass("hpet") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        (void)cfg;
        return new HPETDevice(ctx);
    }
};

static const HPETClass sHPETClass;


HPET *hpet_node_state(Device *dev)
{
    HPETDevice *node = dynamic_cast<HPETDevice *>(dev);

    return node != nullptr ? node->State() : nullptr;
}
