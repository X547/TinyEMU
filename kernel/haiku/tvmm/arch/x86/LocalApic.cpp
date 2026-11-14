#include "LocalApic.h"

#include <OS.h>


// registers, by offset in the page
enum {
	RegId = 0x20,
	RegVersion = 0x30,
	RegTpr = 0x80,
	RegApr = 0x90,
	RegPpr = 0xa0,
	RegEoi = 0xb0,
	RegLdr = 0xd0,
	RegDfr = 0xe0,
	RegSvr = 0xf0,
	RegIsr = 0x100,
	RegTmr = 0x180,
	RegIrr = 0x200,
	RegEsr = 0x280,
	RegLvtCmci = 0x2f0,
	RegIcrLow = 0x300,
	RegIcrHigh = 0x310,
	RegLvtTimer = 0x320,
	RegLvtError = 0x370,
	RegInitialCount = 0x380,
	RegCurrentCount = 0x390,
	RegDivide = 0x3e0,
	// x2APIC only
	RegSelfIpi = 0x3f0,
};

// fLvt indexes
enum {
	LvtCmci,
	LvtTimer,
	LvtThermal,
	LvtPerf,
	LvtLint0,
	LvtLint1,
	LvtError,
};

// LVT, ICR and message fields
constexpr uint32 kDeliveryModeShift = 8;
constexpr uint32 kDeliveryStatus = 1u << 12;
constexpr uint32 kLevelAssert = 1u << 14;
constexpr uint32 kTriggerLevel = 1u << 15;
constexpr uint32 kLvtMasked = 1u << 16;
constexpr uint32 kTimerPeriodic = 1u << 17;
constexpr uint32 kIcrShorthandShift = 18;
constexpr uint32 kIcrLogical = 1u << 11;

enum {
	ModeFixed = 0,
	ModeLowest = 1,
	ModeInit = 5,
	ModeStartup = 6,
	ModeExtInt = 7,
};

enum {
	ShorthandNone,
	ShorthandSelf,
	ShorthandAll,
	ShorthandOthers,
};

constexpr uint64 kBaseBsp = 1ull << 8;
constexpr uint64 kBaseX2Apic = 1ull << 10;
constexpr uint64 kBaseEnable = 1ull << 11;

constexpr uint32 kSvrEnable = 1u << 8;

// the bits of the x2APIC ICR's low half a write must leave clear
constexpr uint32 kX2ApicIcrReserved = 0xfff32000u;

// ESR bits, by the Intel SDM
constexpr uint32 kEsrSendIllegalVector = 1u << 5;
constexpr uint32 kEsrReceiveIllegalVector = 1u << 6;
constexpr uint32 kEsrIllegalRegister = 1u << 7;

// The bits each LVT keeps, indexed like fLvt: vector, delivery mode,
// polarity and trigger where the entry has them, the mask, and the timer's
// periodic mode. The delivery status and remote IRR are read only and never
// set here.
static const uint32 kLvtWritable[7] = {
	0x000107ff,     // CMCI
	0x000300ff,     // timer, without TSC deadline mode
	0x000107ff,     // thermal
	0x000107ff,     // performance counters
	0x0001a7ff,     // LINT0
	0x0001a7ff,     // LINT1
	0x000100ff,     // error
};

// version 0x14 with 7 LVT entries
constexpr uint32 kVersion = 0x00060014;

constexpr uint64 kTimerHz = 1000000000;
constexpr uint64 kTicksPerUs = kTimerHz / 1000000;


static uint32 Bits(uint32 value, uint32 shift, uint32 count)
{
	return (value >> shift) & ((1u << count) - 1);
}


// The highest set bit of a 256 bit register, or -1.
static int HighestVector(const uint32 *reg)
{
	for (int i = 7; i >= 0; i--) {
		if (reg[i] != 0) {
			return i * 32 + 31 - __builtin_clz(reg[i]);
		}
	}
	return -1;
}


static void SetVectorBit(uint32 *reg, int vector, bool value)
{
	const uint32 bit = 1u << (vector % 32);
	if (value) {
		reg[vector / 32] |= bit;
	} else {
		reg[vector / 32] &= ~bit;
	}
}


static bool VectorBit(const uint32 *reg, int vector)
{
	return (reg[vector / 32] & (1u << (vector % 32))) != 0;
}


// The offsets no register answers at in xAPIC mode, by the Intel SDM.
static bool ReservedRegister(uint32 offset)
{
	return offset <= 0x010 || (offset >= 0x040 && offset <= 0x070)
		|| offset == 0x0c0 || (offset >= 0x290 && offset <= 0x2e0)
		|| (offset >= 0x3a0 && offset <= 0x3d0) || offset == 0x3f0;
}


// An xAPIC message's 8 bit destination as the bus has it: physical 0xff is
// the broadcast.
static uint32 XapicDestination(uint32 dest, bool logical)
{
	return !logical && dest == 0xff ? kApicBroadcast : dest;
}


// What an x2APIC MSR allows, by the offset of its register in the xAPIC
// page; 0 for an MSR no register answers at.
enum {
	X2ApicRead = 1,
	X2ApicWrite = 2,
};

static int X2ApicAccess(uint32 offset)
{
	int access = 0;
	switch (offset) {
		case RegId:
		case RegVersion:
		case RegPpr:
		case RegLdr:
		case RegCurrentCount:
			access = X2ApicRead;
			break;
		case RegEoi:
		case RegSelfIpi:
			access = X2ApicWrite;
			break;
		case RegTpr:
		case RegSvr:
		case RegEsr:
		case RegLvtCmci:
		case RegIcrLow:
		case RegInitialCount:
		case RegDivide:
			access = X2ApicRead | X2ApicWrite;
			break;
		default:
			// ISR, TMR and IRR
			if (offset >= RegIsr && offset < RegIrr + 0x80) {
				access = X2ApicRead;
			} else if (offset >= RegLvtTimer && offset <= RegLvtError) {
				access = X2ApicRead | X2ApicWrite;
			}
			break;
	}
	return access;
}


//#pragma mark - LocalApic

LocalApic::LocalApic(ApicBus &bus, LocalApicHost &host, uint32 id):
	fBus(bus),
	fHost(host),
	fId(id)
{
}


void LocalApic::Reset()
{
	fBase = kApicBase | kBaseEnable | (fId == 0 ? kBaseBsp : 0);
	Init();
	if (fId == 0) {
		fLvt[LvtLint0] = ModeExtInt << kDeliveryModeShift;
		Update();
	}
}


void LocalApic::Init()
{
	fTpr = 0;
	fLdr = 0;
	fDfr = 0xffffffff;
	fSvr = 0xff;
	fIcrLow = 0;
	fIcrHigh = 0;
	for (uint32 &lvt: fLvt) {
		lvt = kLvtMasked;
	}
	fDivide = 0;
	fInitialCount = 0;
	fTimerArmed = false;
	fEsr = 0;
	fEsrPending = 0;
	for (int i = 0; i < 8; i++) {
		fIrr[i] = 0;
		fIsr[i] = 0;
		fTmr[i] = 0;
	}
	fHost.ApicTimerChanged(UINT64_MAX);
	Update();
}


bool LocalApic::Enabled() const
{
	return (fBase & kBaseEnable) != 0 && (fSvr & kSvrEnable) != 0;
}


bool LocalApic::X2Apic() const
{
	return (fBase & kBaseEnable) != 0 && (fBase & kBaseX2Apic) != 0;
}


bool LocalApic::PageEnabled() const
{
	return (fBase & kBaseEnable) != 0 && (fBase & kBaseX2Apic) == 0;
}


// In x2APIC mode the LDR follows from the ID: a cluster of 16 and a bit in
// it.
uint32 LocalApic::X2ApicLdr() const
{
	return ((fId >> 4) << 16) | (1u << (fId & 0xf));
}


bool LocalApic::AcceptsExtInt() const
{
	const uint32 lint0 = fLvt[LvtLint0];
	return (fBase & kBaseEnable) == 0
		|| ((lint0 & kLvtMasked) == 0 && Bits(lint0, kDeliveryModeShift, 3) == ModeExtInt);
}


int LocalApic::Ppr() const
{
	const int isr = HighestVector(fIsr);
	const int isrClass = isr < 0 ? 0 : isr & 0xf0;
	return int(fTpr & 0xf0) >= isrClass ? int(fTpr & 0xff) : isrClass;
}


// An accepted vector above the processor priority.
bool LocalApic::Deliverable() const
{
	const int irr = HighestVector(fIrr);
	return irr >= 0 && Enabled() && (irr & 0xf0) > (Ppr() & 0xf0);
}


void LocalApic::Update()
{
	fHost.ApicInterruptChanged(Deliverable());
}


void LocalApic::Accept(int vector, bool level)
{
	// vectors 0-15 are illegal
	if (vector < 16) {
		Error(kEsrReceiveIllegalVector);
		return;
	}
	SetVectorBit(fIrr, vector, true);
	SetVectorBit(fTmr, vector, level);
	Update();
}


// An error is recorded for the next ESR write to show, and interrupts
// through the error LVT the first time it is found. Its own vector being
// illegal is then an error already recorded, which ends there.
void LocalApic::Error(uint32 bit)
{
	if ((fEsrPending & bit) != 0) {
		return;
	}
	fEsrPending |= bit;
	if ((fLvt[LvtError] & kLvtMasked) == 0) {
		Accept(Bits(fLvt[LvtError], 0, 8), false);
	}
}


// Flat model: a bit per APIC; cluster model: a cluster and 4 bits.
bool LocalApic::MatchesLogical(uint32 dest) const
{
	const uint32 ldr = Bits(fLdr, 24, 8);
	if (Bits(fDfr, 28, 4) == 0xf) {
		return (ldr & dest) != 0;
	}
	const uint32 cluster = Bits(dest, 4, 4);
	return (cluster == 0xf || cluster == Bits(ldr, 4, 4)) && (Bits(dest, 0, 4) & Bits(ldr, 0, 4)) != 0;
}


// All ones is everyone in either mode. An xAPIC takes the low 8 bits of a
// logical destination; an x2APIC in logical mode matches the cluster and any
// of its bits.
bool LocalApic::IsDestination(uint32 dest, bool logical) const
{
	if (dest == kApicBroadcast) {
		return true;
	}
	if (!logical) {
		return dest == fId;
	}
	if (!X2Apic()) {
		return MatchesLogical(dest & 0xff);
	}
	const uint32 ldr = X2ApicLdr();
	return (dest >> 16) == (ldr >> 16) && (dest & 0xffff & ldr) != 0;
}


void LocalApic::Receive(int mode, int vector, bool level, bool assert)
{
	switch (mode) {
		case ModeFixed:
		case ModeLowest:
			Accept(vector, level);
			break;
		case ModeInit:
			// the de-assert of a level triggered INIT does nothing
			if (level && !assert) {
				break;
			}
			Init();
			fHost.ApicInit();
			break;
		case ModeStartup:
			fHost.ApicStartup(vector);
			break;
		default:
			break;
	}
}


int LocalApic::Acknowledge()
{
	if (!Deliverable()) {
		return -1;
	}
	const int vector = HighestVector(fIrr);
	SetVectorBit(fIrr, vector, false);
	SetVectorBit(fIsr, vector, true);
	Update();
	return vector;
}


void LocalApic::SendIpi()
{
	const int mode = Bits(fIcrLow, kDeliveryModeShift, 3);
	// a fixed or lowest priority message names a vector, which 0-15 is not;
	// such a message is not sent
	if ((mode == ModeFixed || mode == ModeLowest) && Bits(fIcrLow, 0, 8) < 16) {
		Error(kEsrSendIllegalVector);
		return;
	}
	const bool logical = (fIcrLow & kIcrLogical) != 0;
	const uint32 dest = X2Apic() ? fIcrHigh : XapicDestination(Bits(fIcrHigh, 24, 8), logical);
	fBus.Send(fId, Bits(fIcrLow, kIcrShorthandShift, 2), dest, logical, mode, Bits(fIcrLow, 0, 8),
		(fIcrLow & kTriggerLevel) != 0, (fIcrLow & kLevelAssert) != 0);
}


// The page stays where it is. x2APIC mode is entered from xAPIC mode and left
// by disabling the APIC.
bool LocalApic::SetBase(uint64 value)
{
	const uint64 address = value & ~0xfffull;
	const bool enable = (value & kBaseEnable) != 0;
	const bool x2apic = (value & kBaseX2Apic) != 0;
	if (address != kApicBase || (value & 0xfff & ~(kBaseBsp | kBaseEnable | kBaseX2Apic)) != 0) {
		return false;
	}
	// x2APIC mode disabled, entered from disabled, or left for xAPIC
	if ((x2apic && !enable) || (x2apic && (fBase & kBaseEnable) == 0) || (X2Apic() && enable && !x2apic)) {
		return false;
	}
	fBase = (fBase & ~(kBaseEnable | kBaseX2Apic)) | (enable ? kBaseEnable : 0) | (x2apic ? kBaseX2Apic : 0);
	Update();
	return true;
}


void LocalApic::SetTaskPriority(int cr8)
{
	fTpr = uint32(cr8) << 4;
	Update();
}


//#pragma mark - timer

uint32 LocalApic::TimerDivisor() const
{
	const uint32 n = Bits(fDivide, 0, 2) | (Bits(fDivide, 3, 1) << 2);
	return n == 7 ? 1 : 2u << n;
}


uint64 LocalApic::TimerTicks(uint64 now) const
{
	return (now - fTimerStart) * kTicksPerUs / TimerDivisor();
}


uint32 LocalApic::CurrentCount(uint64 now) const
{
	if (fInitialCount == 0) {
		return 0;
	}
	const uint64 ticks = TimerTicks(now);
	if ((fLvt[LvtTimer] & kTimerPeriodic) != 0) {
		return fInitialCount - uint32(ticks % fInitialCount);
	}
	return ticks >= fInitialCount ? 0 : fInitialCount - uint32(ticks);
}


// When the period after the ones counted so far ends, rounded up to a
// microsecond, or UINT64_MAX with no count running.
uint64 LocalApic::TimerDue() const
{
	if (!fTimerArmed) {
		return UINT64_MAX;
	}
	const uint64 ticks = (fTimerFired + 1) * fInitialCount * TimerDivisor();
	return fTimerStart + (ticks + kTicksPerUs - 1) / kTicksPerUs;
}


// Periods that ran out while nobody looked are one interrupt.
void LocalApic::RunTimer(uint64 now)
{
	if (!fTimerArmed) {
		return;
	}
	const uint64 periods = TimerTicks(now) / fInitialCount;
	const bool periodic = (fLvt[LvtTimer] & kTimerPeriodic) != 0;
	if (periods > fTimerFired) {
		fTimerFired = periods;
		if ((fLvt[LvtTimer] & kLvtMasked) == 0) {
			Accept(Bits(fLvt[LvtTimer], 0, 8), false);
		}
		// a one-shot count stays in the initial count register, and the
		// current count stops at 0
		if (!periodic) {
			fTimerArmed = false;
		}
	}
	fHost.ApicTimerChanged(TimerDue());
}


//#pragma mark - registers

// A register of either mode, by its offset in the xAPIC page.
uint32 LocalApic::ReadRegister(uint32 offset)
{
	if (offset >= RegIsr && offset < RegIsr + 0x80) {
		return fIsr[(offset - RegIsr) / 16];
	}
	if (offset >= RegTmr && offset < RegTmr + 0x80) {
		return fTmr[(offset - RegTmr) / 16];
	}
	if (offset >= RegIrr && offset < RegIrr + 0x80) {
		return fIrr[(offset - RegIrr) / 16];
	}
	if (offset >= RegLvtTimer && offset <= RegLvtError) {
		return fLvt[LvtTimer + (offset - RegLvtTimer) / 16];
	}

	// APR, EOI and the reserved registers read as 0
	uint32 value = 0;
	switch (offset) {
		case RegId:
			value = X2Apic() ? fId : fId << 24;
			break;
		case RegVersion:
			value = kVersion;
			break;
		case RegTpr:
			value = fTpr;
			break;
		case RegPpr:
			value = Ppr();
			break;
		case RegLdr:
			value = X2Apic() ? X2ApicLdr() : fLdr;
			break;
		case RegDfr:
			value = fDfr;
			break;
		case RegSvr:
			value = fSvr;
			break;
		case RegIcrLow:
			// IPIs are sent at once
			value = fIcrLow & ~kDeliveryStatus;
			break;
		case RegIcrHigh:
			// the x2APIC destination is 32 bits
			value = fIcrHigh & 0xff000000;
			break;
		case RegLvtCmci:
			value = fLvt[LvtCmci];
			break;
		case RegInitialCount:
			value = fInitialCount;
			break;
		case RegCurrentCount:
			value = CurrentCount(system_time());
			break;
		case RegDivide:
			value = fDivide;
			break;
		case RegEsr:
			value = fEsr;
			break;
		default:
			break;
	}
	return value;
}


void LocalApic::Eoi()
{
	const int vector = HighestVector(fIsr);
	if (vector < 0) {
		return;
	}
	SetVectorBit(fIsr, vector, false);
	if (VectorBit(fTmr, vector)) {
		fHost.ApicEoi(vector);
	}
	Update();
}


void LocalApic::WriteLvt(int lvt, uint32 value)
{
	value &= kLvtWritable[lvt];
	if ((fSvr & kSvrEnable) == 0) {
		value |= kLvtMasked;
	}
	fLvt[lvt] = value;
	Update();
	if (lvt == LvtTimer) {
		fHost.ApicTimerChanged(TimerDue());
	}
}


void LocalApic::WriteRegister(uint32 offset, uint32 value)
{
	if (offset >= RegLvtTimer && offset <= RegLvtError) {
		WriteLvt(LvtTimer + (offset - RegLvtTimer) / 16, value);
		return;
	}

	// ID and the read only registers ignore writes
	switch (offset) {
		case RegTpr:
			fTpr = value & 0xff;
			Update();
			break;
		case RegEoi:
			Eoi();
			break;
		case RegLdr:
			fLdr = value & 0xff000000;
			break;
		case RegDfr:
			fDfr = value | 0x0fffffff;
			break;
		case RegSvr:
			fSvr = value & 0x3ff;
			// disabling masks the LVT
			if ((fSvr & kSvrEnable) == 0) {
				for (uint32 &lvt: fLvt) {
					lvt |= kLvtMasked;
				}
			}
			Update();
			break;
		case RegIcrLow:
			fIcrLow = value;
			SendIpi();
			break;
		case RegIcrHigh:
			fIcrHigh = value & 0xff000000;
			break;
		case RegEsr:
			// shows the errors found since the last write
			fEsr = fEsrPending;
			fEsrPending = 0;
			break;
		case RegLvtCmci:
			WriteLvt(LvtCmci, value);
			break;
		case RegInitialCount:
			fInitialCount = value;
			fTimerArmed = value != 0;
			fTimerStart = system_time();
			fTimerFired = 0;
			fHost.ApicTimerChanged(TimerDue());
			break;
		case RegDivide:
			fDivide = value & 0xb;
			// a count in progress starts over at the new rate
			if (fTimerArmed) {
				fTimerStart = system_time();
				fTimerFired = 0;
				fHost.ApicTimerChanged(TimerDue());
			}
			break;
		default:
			break;
	}
}


// In x2APIC mode the page is not there, as with the APIC disabled.
uint32 LocalApic::PageRead(uint32 offset)
{
	if (X2Apic()) {
		return 0;
	}
	offset &= 0xff0;
	if (ReservedRegister(offset)) {
		Error(kEsrIllegalRegister);
		return 0;
	}
	return ReadRegister(offset);
}


void LocalApic::PageWrite(uint32 offset, uint32 value)
{
	if (X2Apic()) {
		return;
	}
	offset &= 0xff0;
	if (ReservedRegister(offset)) {
		Error(kEsrIllegalRegister);
		return;
	}
	WriteRegister(offset, value);
}


// Outside x2APIC mode, and for an MSR with no register or one the access
// does not suit, the processor raises #GP rather than setting the ESR.
bool LocalApic::MsrRead(uint32 msr, uint64 &value)
{
	const uint32 offset = (msr - kApicMsrBase) << 4;
	if (!X2Apic() || (X2ApicAccess(offset) & X2ApicRead) == 0) {
		return false;
	}
	value = ReadRegister(offset);
	// the ICR is one 64 bit register
	if (offset == RegIcrLow) {
		value |= uint64(fIcrHigh) << 32;
	}
	return true;
}


// A write that sets reserved bits faults where the register's are known: the
// upper half of all but the ICR, and those of the ICR, the EOI, the ESR and
// the self IPI. Elsewhere they are dropped as in xAPIC mode.
bool LocalApic::MsrWrite(uint32 msr, uint64 value)
{
	const uint32 offset = (msr - kApicMsrBase) << 4;
	if (!X2Apic() || (X2ApicAccess(offset) & X2ApicWrite) == 0) {
		return false;
	}
	if (offset == RegIcrLow) {
		if ((value & kX2ApicIcrReserved) != 0) {
			return false;
		}
		fIcrLow = uint32(value);
		fIcrHigh = uint32(value >> 32);
		SendIpi();
		return true;
	}
	if (offset == RegSelfIpi) {
		if (value > 0xff) {
			return false;
		}
		// a fixed, edge triggered IPI to itself
		if (value < 16) {
			Error(kEsrSendIllegalVector);
		} else {
			Accept(int(value), false);
		}
		return true;
	}
	if ((offset == RegEoi || offset == RegEsr) && value != 0) {
		return false;
	}
	if (value > 0xffffffff) {
		return false;
	}
	WriteRegister(offset, uint32(value));
	return true;
}


//#pragma mark - ApicBus

void ApicBus::Deliver(uint64 address, uint32 data)
{
	const bool logical = (address & 4) != 0;
	Send(0, ShorthandNone, XapicDestination(Bits(uint32(address), 12, 8), logical), logical,
		Bits(data, kDeliveryModeShift, 3), Bits(data, 0, 8), (data & kTriggerLevel) != 0,
		(data & kLevelAssert) != 0);
}


// Lowest priority delivery picks the destination of lowest processor
// priority, the first of equals.
void ApicBus::Send(uint32 source, int shorthand, uint32 dest, bool logical, int mode, int vector, bool level,
	bool assert)
{
	LocalApic *lowest = nullptr;
	for (LocalApic *apic: fApics) {
		if (apic == nullptr) {
			continue;
		}
		bool target;
		switch (shorthand) {
			case ShorthandSelf:
				target = apic->Id() == source;
				break;
			case ShorthandAll:
				target = true;
				break;
			case ShorthandOthers:
				target = apic->Id() != source;
				break;
			default:
				target = apic->IsDestination(dest, logical);
				break;
		}
		if (!target) {
			continue;
		}
		if (mode != ModeLowest) {
			apic->Receive(mode, vector, level, assert);
		} else if (lowest == nullptr || apic->Ppr() < lowest->Ppr()) {
			lowest = apic;
		}
	}
	if (lowest != nullptr) {
		lowest->Receive(mode, vector, level, assert);
	}
}
