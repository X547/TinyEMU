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
#pragma once

#include <stdint.h>

#include "iomem.h"

class Device;
class VirtMachine;
struct DeviceContext;


/* Where a PC's chipset has it. */
#define HPET_ADDR 0xfed00000
#define HPET_SIZE 0x400

/* Three comparators, which is the fewest a guest may count on. */
#define HPET_TIMERS 3


/* A 64 bit main counter at 100 MHz and HPET_TIMERS comparators, each one-shot
   or periodic, edge or level triggered. In legacy replacement mode the first
   two take over the PIT's line and the RTC's; otherwise each goes to the
   IOAPIC input it is routed to. There is no FSB (MSI) delivery. */
class HPET final: public DeviceIO {
private:
    struct Timer {
        uint64_t config = 0;
        uint64_t comparator = ~0ULL;
        uint64_t period = 0;
        /* the counter the comparator was last checked against */
        uint64_t last = 0;
        /* the high half of a 64 bit write whose low half set the value */
        bool setValueHigh = false;
        /* the line a level interrupt holds up, or LINE_NONE */
        int raisedLine;
    };

    VirtMachine *fMachine;
    IRQSignal *fIrq0 = nullptr;
    IRQSignal *fIrq8 = nullptr;
    IRQTarget *fIoapic = nullptr;
    uint32_t fRouteMask = 0;

    uint64_t fConfig = 0;
    uint64_t fStatus = 0;
    /* the counter when fBaseUs was taken, or the whole of it while halted */
    uint64_t fCounterBase = 0;
    uint64_t fBaseUs = 0;
    /* the counter a read of its low half returned, and when */
    uint64_t fLowRead = 0;
    uint64_t fLowReadUs = 0;
    Timer fTimers[HPET_TIMERS];

    bool Enabled() const;
    uint64_t Counter(uint64_t now) const;
    void SetCounter(uint64_t counter, uint64_t now);
    void Process(uint64_t now);
    void Fire(int index);
    int LineOf(int index) const;
    void SetLine(int line, int level);
    void UpdateLevel(int index);
    uint32_t TimerRead(int index, uint32_t reg);
    void TimerWrite(int index, uint32_t reg, uint32_t val, uint64_t now);

public:
    HPET(VirtMachine *machine);

    /* The two lines legacy replacement drives, and the IOAPIC inputs
       'route_mask' names, reached through 'ioapic'. */
    void SetOutputs(IRQSignal *irq0, IRQSignal *irq8, IRQTarget *ioapic,
                    uint32_t route_mask);

    /* Whether the PIT's and the RTC's own interrupts are cut off. */
    bool LegacyReplacement() const;

    /* The low half of the capabilities, which the ACPI table repeats. */
    uint32_t BlockId() const;

    /* Raises the interrupts that are due and returns the microseconds until
       the next one, or -1 if there is none. */
    int64_t RunTimers();

    /* DeviceIO */
    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;
};


/* The "hpet" configuration node: the HPET at HPET_ADDR. */
Device *hpet_node_create(DeviceContext *ctx);

/* The HPET of a node hpet_node_create() made, or nullptr for another
   device. */
HPET *hpet_node_state(Device *dev);
