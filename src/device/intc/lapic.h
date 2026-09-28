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

#include <memory>
#include <vector>

#include "iomem.h"


#define LAPIC_BASE 0xfee00000
#define LAPIC_SIZE 0x1000

/* Acknowledge() for an interrupt the 8259s give the vector of. */
#define LAPIC_EXTINT (-1)


/* What the local APICs need of the machine. Processors are named by their
   APIC IDs, which count from 0. */
class LocalApicHost {
public:
    virtual ~LocalApicHost() = default;

    /* Whether processor 'id' has an interrupt to take. */
    virtual void SetApicInterrupt(uint32_t id, bool pending) = 0;
    /* An EOI for a level triggered vector, for the IOAPIC. */
    virtual void ApicEoi(int vector) = 0;
    /* An INIT, and a STARTUP IPI with its vector, for processor 'id'. */
    virtual void ApicInit(uint32_t id) = 0;
    virtual void ApicStartup(uint32_t id, int vector) = 0;
    /* A timer was programmed and may be due sooner than RunTimers() last
       said. */
    virtual void ApicTimerChanged() = 0;
};

class LocalApicBus;


/* The xAPIC of one processor. The timer counts at 1 GHz, the bus clock a
   hypervisor's APIC has, from the host clock. NMI, SMI and ExtINT
   messages are not modelled. The device lock covers every call. */
class LocalApic final: public DeviceIO {
private:
    LocalApicBus &fBus;
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
    bool Deliverable() const;
    void Update();
    void Accept(int vector, bool level);
    bool MatchesLogical(uint32_t dest) const;
    void Init();
    void SendIpi();
    uint32_t TimerDivisor() const;
    uint64_t TimerTicks(uint64_t now) const;
    uint32_t CurrentCount(uint64_t now) const;

public:
    LocalApic(LocalApicBus &bus, LocalApicHost &host, uint32_t id);

    void Reset();

    /* DeviceIO: the register page */
    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;

    /* Whether a message to 'dest' reaches this APIC. */
    bool IsDestination(uint32_t dest, bool logical) const;
    /* A message with delivery mode 'mode'; 'level' for level triggered,
       'assert' for its level. */
    void Receive(int mode, int vector, bool level, bool assert);
    /* The processor priority, which lowest priority delivery goes by. */
    int Ppr() const;
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


/* The local APICs of the processors, and the messages between them. The
   device lock covers every call. */
class LocalApicBus {
private:
    std::vector<std::unique_ptr<LocalApic>> fApics;

public:
    LocalApicBus(LocalApicHost &host, int count);

    int Count() const {return (int)fApics.size();}
    LocalApic &Apic(uint32_t id) {return *fApics[id];}

    /* An MSI or an IOAPIC message. */
    void Deliver(uint64_t addr, uint32_t data);
    /* A message from APIC 'source' by ICR shorthand 'shorthand': to
       itself, to all, to all others, or to 'dest'. */
    void Send(uint32_t source, int shorthand, uint32_t dest, bool logical,
              int mode, int vector, bool level, bool assert);
    /* LocalApic::RunTimer() for each; the soonest of their results. */
    int64_t RunTimers();
};
