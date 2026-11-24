#include "RiscVDecoder.h"


static constexpr uint32 kOpLoad = 0x03;
static constexpr uint32 kOpStore = 0x23;


static uint32 Bits(uint32 value, int shift, int count)
{
	return (value >> shift) & ((1u << count) - 1);
}


static bool Decode32(uint32 insn, RiscVMemInsn &out)
{
	const uint32 funct3 = Bits(insn, 12, 3);
	out.length = 4;
	switch (insn & 0x7f) {
		case kOpLoad:
			if (funct3 == 7) {
				return false;
			}
			out.store = false;
			out.size = 1 << (funct3 & 3);
			// LB, LH, LW sign extend; LD has nothing to extend
			out.signExtend = funct3 < 3;
			out.reg = Bits(insn, 7, 5);
			return true;
		case kOpStore:
			if (funct3 > 3) {
				return false;
			}
			out.store = true;
			out.size = 1 << funct3;
			out.signExtend = false;
			out.reg = Bits(insn, 20, 5);
			return true;
		default:
			return false;
	}
}


static bool Decode16(uint32 insn, RiscVMemInsn &out)
{
	const uint32 funct3 = Bits(insn, 13, 3);
	out.length = 2;
	out.signExtend = false;
	switch (insn & 3) {
		case 0:
			// C.LW, C.LD, C.SW, C.SD: x8 to x15
			switch (funct3) {
				case 2:
				case 3:
					out.store = false;
					out.size = funct3 == 2 ? 4 : 8;
					out.signExtend = funct3 == 2;
					out.reg = 8 + Bits(insn, 2, 3);
					return true;
				case 6:
				case 7:
					out.store = true;
					out.size = funct3 == 6 ? 4 : 8;
					out.reg = 8 + Bits(insn, 2, 3);
					return true;
				default:
					return false;
			}
		case 2:
			// C.LWSP, C.LDSP, C.SWSP, C.SDSP
			switch (funct3) {
				case 2:
				case 3:
					out.store = false;
					out.size = funct3 == 2 ? 4 : 8;
					out.signExtend = funct3 == 2;
					out.reg = Bits(insn, 7, 5);
					return out.reg != 0;
				case 6:
				case 7:
					out.store = true;
					out.size = funct3 == 6 ? 4 : 8;
					out.reg = Bits(insn, 2, 5);
					return true;
				default:
					return false;
			}
		default:
			return false;
	}
}


bool RiscVDecodeMemInsn(uint32 insn, RiscVMemInsn &out)
{
	if ((insn & 3) == 3) {
		return Decode32(insn, out);
	}
	return Decode16(insn & 0xffff, out);
}


bool RiscVDecodeTransformedInsn(uint32 htinst, RiscVMemInsn &out)
{
	if ((htinst & 1) == 0) {
		return false;
	}
	if (!Decode32(htinst | 3, out)) {
		return false;
	}
	out.length = (htinst & 2) != 0 ? 4 : 2;
	return true;
}
