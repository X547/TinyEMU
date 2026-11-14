#pragma once

#include <SupportDefs.h>


// The instructions a guest uses on memory that is not RAM, decoded far enough
// to carry out their one memory access. Pure logic, without the kernel.


enum class X86CpuMode {
	Real16,
	Protected16,
	Protected32,
	Long64,
};


enum class X86MemOp {
	// register <- memory, with 'extend'
	Load,
	// memory <- register
	Store,
	// memory <- immediate
	StoreImmediate,
	// XMM register <- memory, the rest of the register cleared when 'size'
	// is below 16
	LoadXmm,
	// memory <- XMM register
	StoreXmm,
	// the string instructions, one element each time
	Movs,
	Stos,
	Lods,
	// the flags of memory AND the operand
	Test,
	// the flags of memory minus the operand, or with 'reversed' the operand
	// minus memory
	Cmp,
	// memory pushed onto the stack
	Push,
};


enum class X86Extend {
	None,
	Zero,
	Sign,
};


enum {
	X86SegEs, X86SegCs, X86SegSs, X86SegDs, X86SegFs, X86SegGs,
	X86SegNone = -1,
};


struct X86MemInsn {
	X86MemOp op;
	uint8 length;
	// bytes of memory accessed: 1, 2, 4, 8 or 16
	uint8 size;
	// the register operand: a general register or an XMM register
	uint8 reg;
	// AH, CH, DH or BH
	bool highByte;
	X86Extend extend;
	// bytes of register an extending load writes
	uint8 regSize;
	uint64 immediate;
	// Test and Cmp: the operand is 'immediate' rather than the register
	bool immediateOperand;
	bool reversed;
	// string instructions
	uint8 addressSize;
	bool rep;
	// the segment override, X86SegNone for the default
	int8 segment;
};


// false for an instruction this does not know or that has no memory operand.
bool X86DecodeMemInsn(const uint8 *bytes, uint32 count, X86CpuMode mode, X86MemInsn &insn);
