#pragma once

#include <SupportDefs.h>


// A load or store of an integer register, as MMIO is accessed.
struct RiscVMemInsn {
	bool store;
	// 1, 2, 4 or 8
	uint8 size;
	// a load sign extends
	bool signExtend;
	// rd of a load, rs2 of a store
	uint8 reg;
	// 2 or 4
	uint8 length;
};


// The instruction as the guest has it, 16 or 32 bits. False for anything but
// a load or store of an integer register.
bool RiscVDecodeMemInsn(uint32 insn, RiscVMemInsn &out);

// htinst's transformed instruction for a guest page fault, which has bit 0
// set; bit 1 is clear when the guest's instruction was a compressed one.
// False for anything else, a pseudoinstruction or 0 included.
bool RiscVDecodeTransformedInsn(uint32 htinst, RiscVMemInsn &out);
