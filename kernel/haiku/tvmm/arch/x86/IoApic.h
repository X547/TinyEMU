#pragma once

#include <SupportDefs.h>

class ApicBus;


// An 82093AA, as TinyEMU's own model in src/device/intc/ioapic.cpp has it: 24
// inputs, each sent to the local APICs as the message its redirection entry
// describes. The input polarity is not modelled: a line the client raises is
// asserted, whichever way the guest says it is wired. Pure logic: the caller
// holds the lock that covers the VM's APICs.


constexpr uint64 kIoApicBase = 0xfec00000;
constexpr uint32 kIoApicPins = 24;


class IoApic {
private:
	ApicBus &fBus;
	uint32 fId = 0;
	uint8 fSelect = 0;
	uint64 fRedirection[kIoApicPins] {};
	// bit n is the level of input n
	uint32 fLevels = 0;

	void Send(uint32 pin);
	void Service(uint32 pin);

public:
	explicit IoApic(ApicBus &bus);

	// Every input masked, as at power on; the inputs' levels are what drives
	// them, not the IOAPIC's.
	void Reset();

	void SetLine(uint32 pin, bool level);
	// A local APIC ended an interrupt with this vector: the level triggered
	// inputs waiting on it may send again.
	void Eoi(int vector);

	// The page's registers, by offset, 32 bits.
	uint32 PageRead(uint32 offset);
	void PageWrite(uint32 offset, uint32 value);
};
