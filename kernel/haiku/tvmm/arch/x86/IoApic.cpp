#include "IoApic.h"

#include "LocalApic.h"


// the two registers in the window
static constexpr uint32 kIoRegSel = 0x00;
static constexpr uint32 kIoWin = 0x10;

// indirect registers
static constexpr uint8 kRegId = 0x00;
static constexpr uint8 kRegVersion = 0x01;
static constexpr uint8 kRegArbitration = 0x02;
static constexpr uint8 kRegRedirection = 0x10;

static constexpr uint32 kVersion = 0x11;

// redirection entry
static constexpr uint64 kRteDeliveryStatus = 1ull << 12;
static constexpr uint64 kRteRemoteIrr = 1ull << 14;
static constexpr uint64 kRteLevel = 1ull << 15;
static constexpr uint64 kRteMasked = 1ull << 16;
static constexpr uint64 kRteReadOnly = kRteDeliveryStatus | kRteRemoteIrr;
// What a write sets: the destination, and in the low word everything but the
// read only bits and the reserved 17-31.
static constexpr uint32 kRteWritableLow = 0x0001afff;
static constexpr uint32 kRteWritableHigh = 0xff000000;


IoApic::IoApic(ApicBus &bus):
	fBus(bus)
{
	Reset();
}


void IoApic::Reset()
{
	fId = 0;
	fSelect = 0;
	for (uint64 &rte: fRedirection) {
		rte = kRteMasked;
	}
}


// The message is the one an MSI to the same destination would be.
void IoApic::Send(uint32 pin)
{
	const uint64 rte = fRedirection[pin];
	const uint64 address = 0xfee00000 | (((rte >> 56) & 0xff) << 12) | (((rte >> 11) & 1) << 2);
	uint32 data = uint32(rte & 0x7ff);
	if ((rte & kRteLevel) != 0) {
		// level, asserted
		data |= (1u << 15) | (1u << 14);
		fRedirection[pin] |= kRteRemoteIrr;
	}
	fBus.Deliver(address, data);
}


// Sends a level triggered input that is asserted and not already waiting for
// its EOI.
void IoApic::Service(uint32 pin)
{
	const uint64 rte = fRedirection[pin];
	if ((rte & kRteLevel) != 0 && (rte & (kRteMasked | kRteRemoteIrr)) == 0
		&& (fLevels & (1u << pin)) != 0) {
		Send(pin);
	}
}


void IoApic::SetLine(uint32 pin, bool level)
{
	const bool was = (fLevels & (1u << pin)) != 0;
	if (level) {
		fLevels |= 1u << pin;
	} else {
		fLevels &= ~(1u << pin);
	}
	if ((fRedirection[pin] & kRteLevel) != 0) {
		Service(pin);
	} else if (level && !was && (fRedirection[pin] & kRteMasked) == 0) {
		// an edge while masked is lost, as on the real part
		Send(pin);
	}
}


void IoApic::Eoi(int vector)
{
	for (uint32 pin = 0; pin < kIoApicPins; pin++) {
		const uint64 rte = fRedirection[pin];
		if ((rte & kRteRemoteIrr) != 0 && int(rte & 0xff) == vector) {
			fRedirection[pin] &= ~kRteRemoteIrr;
			Service(pin);
		}
	}
}


uint32 IoApic::PageRead(uint32 offset)
{
	if (offset == kIoRegSel) {
		return fSelect;
	}
	if (offset != kIoWin) {
		return 0;
	}
	switch (fSelect) {
		case kRegId:
		case kRegArbitration:
			return fId << 24;
		case kRegVersion:
			return ((kIoApicPins - 1) << 16) | kVersion;
		default:
			if (fSelect >= kRegRedirection && fSelect < kRegRedirection + 2 * kIoApicPins) {
				const uint32 pin = (fSelect - kRegRedirection) / 2;
				return uint32(fRedirection[pin] >> ((fSelect & 1) * 32));
			}
			return 0;
	}
}


void IoApic::PageWrite(uint32 offset, uint32 value)
{
	if (offset == kIoRegSel) {
		fSelect = uint8(value);
		return;
	}
	if (offset != kIoWin) {
		return;
	}
	if (fSelect == kRegId) {
		fId = (value >> 24) & 0xf;
	} else if (fSelect >= kRegRedirection && fSelect < kRegRedirection + 2 * kIoApicPins) {
		const uint32 pin = (fSelect - kRegRedirection) / 2;
		uint64 rte = fRedirection[pin];
		if ((fSelect & 1) != 0) {
			rte = (rte & 0xffffffffull) | (uint64(value & kRteWritableHigh) << 32);
		} else {
			rte = (rte & ~0xffffffffull) | (rte & kRteReadOnly) | (value & kRteWritableLow);
			// an edge triggered entry has nothing to wait for
			if ((rte & kRteLevel) == 0) {
				rte &= ~kRteRemoteIrr;
			}
		}
		fRedirection[pin] = rte;
		// unmasking a level triggered input that is asserted sends it
		Service(pin);
	}
}
