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
#pragma once

#include <stdint.h>

#include "iomem.h"


#define LAPIC_BASE 0xfee00000
#define LAPIC_SIZE 0x1000

/* Acknowledge() for an interrupt the 8259s give the vector of. */
#define LAPIC_EXTINT (-1)


/* What a local APIC needs of the machine. */
class LocalApicHost {
public:
    virtual ~LocalApicHost() = default;

    /* Whether the processor has an interrupt to take. */
    virtual void SetApicInterrupt(bool pending) = 0;
    /* An EOI for a level triggered vector, for the IOAPIC. */
    virtual void ApicEoi(int vector) = 0;
};


/* The xAPIC of a processor without other processors: messages it is not
   the destination of, and IPIs to others, are dropped. The timer counts
   at 1 GHz, the bus clock a hypervisor's APIC has, from the host clock.
   The device lock covers every call. */
class LocalApic final: public DeviceIO {
private:
    LocalApicHost &fHost;
    uint32_t fId;
    uint64_t fBase;
    uint32_t fTpr = 0;
    uint32_t fLdr = 0;
    uint32_t fDfr = UINT32_MAX;
    uint32_t fSvr = 0xff;
    uint32_t fIcrHigh = 0;
    uint32_t fIcrLow = 0;
    uint32_t fLvt[7];
    uint32_t fDivide = 0;
    uint32_t fIrr[8] = {};
    uint32_t fIsr[8] = {};
    uint32_t fTmr[8] = {};
    bool fLint0 = false;

    /* the timer */
    uint32_t fInitialCount = 0;
    uint64_t fTimerStart = 0;   /* host microseconds */
    uint64_t fTimerFired = 0;   /* periods counted so far */

    bool Enabled() const;
    bool AcceptsExtInt() const;
    int Ppr() const;
    bool Deliverable() const;
    void Update();
    void Accept(int vector, bool level);
    bool MatchesLogical(uint32_t dest) const;
    void SendIpi();
    uint32_t TimerDivisor() const;
    uint64_t TimerTicks(uint64_t now) const;
    uint32_t CurrentCount(uint64_t now) const;

public:
    LocalApic(LocalApicHost &host, uint32_t id);

    void Reset();

    /* DeviceIO: the register page */
    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;

    /* An MSI or an IOAPIC message. */
    void Deliver(uint64_t addr, uint32_t data);
    /* The 8259s' INTR, on LINT0. */
    void SetLint0(bool level);

    /* The processor takes the interrupt it was told of: a vector, or
       LAPIC_EXTINT, or the spurious vector if it went away. */
    int Acknowledge();

    uint32_t Id() const {return fId;}
    uint64_t Base() const {return fBase;}
    /* IA32_APIC_BASE; false for a value the APIC cannot take */
    bool SetBase(uint64_t val);
    /* CR8 */
    int TaskPriority() const {return fTpr >> 4;}
    void SetTaskPriority(int cr8);

    /* Fires the timer if due; returns the microseconds until it is due
       next, or -1. */
    int64_t RunTimer();
};
