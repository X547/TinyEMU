#pragma once

#include <SupportDefs.h>


// The local APICs, in xAPIC or x2APIC mode, as TinyEMU's own model in
// src/device/intc/lapic.cpp has them: the timer counts at 1 GHz from the host
// clock, NMI, SMI and ExtINT messages are not modelled, nor the TSC deadline
// mode. Pure logic: the caller holds the lock that covers every APIC of a VM.


constexpr uint64 kApicBase = 0xfee00000;
constexpr uint32 kApicMsrBase = 0x800;
constexpr uint32 kApicMsrCount = 0x100;
// the destination of a message to every APIC
constexpr uint32 kApicBroadcast = 0xffffffff;


// What one local APIC needs of the processor it belongs to.
class LocalApicHost {
public:
	virtual ~LocalApicHost() = default;

	// Whether the APIC has an interrupt for the processor.
	virtual void ApicInterruptChanged(bool pending) = 0;
	// An EOI of a level triggered vector, for the IOAPIC.
	virtual void ApicEoi(int vector) = 0;
	// An INIT, and a STARTUP with its vector.
	virtual void ApicInit() = 0;
	virtual void ApicStartup(int vector) = 0;
	// The timer was programmed: it is due at 'due', in system_time(), or
	// never for UINT64_MAX.
	virtual void ApicTimerChanged(uint64 due) = 0;
};

class ApicBus;


class LocalApic {
private:
	ApicBus &fBus;
	LocalApicHost &fHost;
	uint32 fId;
	uint64 fBase = 0;
	uint32 fTpr = 0;
	uint32 fLdr = 0;
	uint32 fDfr = 0xffffffff;
	uint32 fSvr = 0xff;
	uint32 fIcrHigh = 0;
	uint32 fIcrLow = 0;
	uint32 fLvt[7] {};
	uint32 fDivide = 0;
	uint32 fIrr[8] {};
	uint32 fIsr[8] {};
	uint32 fTmr[8] {};
	// The errors software was shown at its last write of the ESR, and those
	// found since.
	uint32 fEsr = 0;
	uint32 fEsrPending = 0;

	// the timer
	uint32 fInitialCount = 0;
	// a count runs and will interrupt
	bool fTimerArmed = false;
	// system_time() it started at
	uint64 fTimerStart = 0;
	// periods counted so far
	uint64 fTimerFired = 0;

	bool Enabled() const;
	uint32 X2ApicLdr() const;
	bool Deliverable() const;
	void Update();
	void Accept(int vector, bool level);
	void Error(uint32 bit);
	bool MatchesLogical(uint32 dest) const;
	void SendIpi();
	uint32 TimerDivisor() const;
	uint64 TimerTicks(uint64 now) const;
	uint32 CurrentCount(uint64 now) const;
	uint64 TimerDue() const;
	uint32 ReadRegister(uint32 offset);
	void WriteRegister(uint32 offset, uint32 value);
	void WriteLvt(int lvt, uint32 value);
	void Eoi();

public:
	LocalApic(ApicBus &bus, LocalApicHost &host, uint32 id);

	// As at power on: enabled, the first processor's LINT0 ready for the
	// 8259s as firmware would leave it.
	void Reset();
	// An INIT resets all but the ID and IA32_APIC_BASE.
	void Init();

	uint32 Id() const {return fId;}
	bool X2Apic() const;
	// Its register page answers memory accesses.
	bool PageEnabled() const;
	// The 8259s' INTR reaches the processor: the APIC is disabled, or LINT0
	// is unmasked in ExtINT mode.
	bool AcceptsExtInt() const;

	// The register page in xAPIC mode, 32 bit accesses.
	uint32 PageRead(uint32 offset);
	void PageWrite(uint32 offset, uint32 value);
	// The registers in x2APIC mode, by MSR; false for an access that raises
	// #GP.
	bool MsrRead(uint32 msr, uint64 &value);
	bool MsrWrite(uint32 msr, uint64 value);

	uint64 Base() const {return fBase;}
	// IA32_APIC_BASE; false for a value the APIC cannot take
	bool SetBase(uint64 value);
	// CR8
	int TaskPriority() const {return fTpr >> 4;}
	void SetTaskPriority(int cr8);

	// Whether a message to 'dest', an APIC ID, a logical destination or
	// kApicBroadcast, reaches this APIC.
	bool IsDestination(uint32 dest, bool logical) const;
	// A message with delivery mode 'mode'; 'level' for level triggered,
	// 'assert' for its level.
	void Receive(int mode, int vector, bool level, bool assert);
	// The processor priority, which lowest priority delivery goes by.
	int Ppr() const;

	// The vector the processor takes now, which goes from IRR to ISR, or -1
	// with none deliverable.
	int Acknowledge();
	bool HasDeliverable() const {return Deliverable();}

	// Fires the timer if due.
	void RunTimer(uint64 now);
};


// The local APICs of a VM, and the messages between them.
class ApicBus {
public:
	static constexpr uint32 kMaxApics = 256;

private:
	LocalApic *fApics[kMaxApics] {};

public:
	void Attach(uint32 id, LocalApic *apic) {fApics[id] = apic;}
	void Detach(uint32 id) {fApics[id] = nullptr;}
	LocalApic *Apic(uint32 id) const {return id < kMaxApics ? fApics[id] : nullptr;}

	// An MSI or an IOAPIC message.
	void Deliver(uint64 address, uint32 data);
	// A message from APIC 'source' by ICR shorthand 'shorthand': to itself,
	// to all, to all others, or to 'dest' as IsDestination() takes it.
	void Send(uint32 source, int shorthand, uint32 dest, bool logical, int mode, int vector, bool level,
		bool assert);
};
