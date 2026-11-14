#include "X86Decoder.h"


namespace {


class Decoder {
private:
	const uint8 *fBytes;
	uint32 fCount;
	uint32 fPos = 0;
	bool fOverflow = false;

public:
	Decoder(const uint8 *bytes, uint32 count): fBytes(bytes), fCount(count) {}

	uint32 Pos() const {return fPos;}
	bool Overflow() const {return fOverflow;}

	uint8 Peek()
	{
		if (fPos >= fCount) {
			fOverflow = true;
			return 0;
		}
		return fBytes[fPos];
	}

	uint64 Fetch(uint32 size)
	{
		uint64 value = 0;
		for (uint32 i = 0; i < size; i++) {
			value |= uint64(Peek()) << (8 * i);
			fPos++;
		}
		return value;
	}
};


struct Prefixes {
	bool operandSize = false;
	bool addressSize = false;
	bool rep = false;
	bool repne = false;
	int8 segment = X86SegNone;
	uint8 rex = 0;
};


struct ModRm {
	uint8 mod;
	uint8 reg;
	uint8 rm;
};


// Skips the addressing bytes after ModRM; false for a register operand.
bool SkipAddress(Decoder &decoder, const ModRm &modRm, uint8 addressSize)
{
	if (modRm.mod == 3) {
		return false;
	}
	if (addressSize == 2) {
		if (modRm.mod == 0 && modRm.rm == 6) {
			decoder.Fetch(2);
		} else if (modRm.mod == 1) {
			decoder.Fetch(1);
		} else if (modRm.mod == 2) {
			decoder.Fetch(2);
		}
		return true;
	}
	if (modRm.rm == 4) {
		const uint8 sib = uint8(decoder.Fetch(1));
		if (modRm.mod == 0 && (sib & 7) == 5) {
			decoder.Fetch(4);
		}
	}
	if (modRm.mod == 0 && modRm.rm == 5) {
		decoder.Fetch(4);
	} else if (modRm.mod == 1) {
		decoder.Fetch(1);
	} else if (modRm.mod == 2) {
		decoder.Fetch(4);
	}
	return true;
}


} // namespace


bool X86DecodeMemInsn(const uint8 *bytes, uint32 count, X86CpuMode mode, X86MemInsn &insn)
{
	Decoder decoder(bytes, count);
	Prefixes prefixes;
	const bool longMode = mode == X86CpuMode::Long64;
	const bool defaultWide = mode == X86CpuMode::Protected32 || longMode;

	for (bool more = true; more;) {
		const uint8 byte = decoder.Peek();
		switch (byte) {
			case 0x66:
				prefixes.operandSize = true;
				break;
			case 0x67:
				prefixes.addressSize = true;
				break;
			case 0xf2:
				prefixes.repne = true;
				break;
			case 0xf3:
				prefixes.rep = true;
				break;
			case 0xf0:
				break;
			case 0x26:
				prefixes.segment = X86SegEs;
				break;
			case 0x2e:
				prefixes.segment = X86SegCs;
				break;
			case 0x36:
				prefixes.segment = X86SegSs;
				break;
			case 0x3e:
				prefixes.segment = X86SegDs;
				break;
			case 0x64:
				prefixes.segment = X86SegFs;
				break;
			case 0x65:
				prefixes.segment = X86SegGs;
				break;
			default:
				more = false;
				break;
		}
		if (more) {
			decoder.Fetch(1);
		}
		if (decoder.Overflow() || decoder.Pos() > 14) {
			return false;
		}
	}
	if (longMode && (decoder.Peek() & 0xf0) == 0x40) {
		prefixes.rex = uint8(decoder.Fetch(1));
	}
	const bool rexW = (prefixes.rex & 8) != 0;
	const uint8 rexR = (prefixes.rex & 4) != 0 ? 8 : 0;

	uint8 operandSize;
	if (rexW) {
		operandSize = 8;
	} else if (defaultWide) {
		operandSize = prefixes.operandSize ? 2 : 4;
	} else {
		operandSize = prefixes.operandSize ? 4 : 2;
	}
	uint8 addressSize;
	if (longMode) {
		addressSize = prefixes.addressSize ? 4 : 8;
	} else if (defaultWide) {
		addressSize = prefixes.addressSize ? 2 : 4;
	} else {
		addressSize = prefixes.addressSize ? 4 : 2;
	}

	insn = {};
	insn.segment = prefixes.segment;
	insn.addressSize = addressSize;
	insn.rep = prefixes.rep || prefixes.repne;
	insn.extend = X86Extend::None;

	auto readModRm = [&]() -> ModRm {
		const uint8 byte = uint8(decoder.Fetch(1));
		return {uint8(byte >> 6), uint8(((byte >> 3) & 7) | rexR), uint8(byte & 7)};
	};
	// An 8 bit register operand without REX is AH, CH, DH or BH for 4-7.
	auto setByteReg = [&](uint8 reg) {
		if (prefixes.rex == 0 && reg >= 4 && reg < 8) {
			insn.reg = reg - 4;
			insn.highByte = true;
		} else {
			insn.reg = reg;
		}
	};

	// an immediate of the operand size, at most 32 bits, sign extended to 64
	auto fetchImmediate = [&](uint8 size) -> uint64 {
		if (size == 1) {
			return decoder.Fetch(1);
		}
		if (size == 2) {
			return decoder.Fetch(2);
		}
		return uint64(int64(int32(uint32(decoder.Fetch(4)))));
	};

	const uint8 opcode = uint8(decoder.Fetch(1));
	bool ok = true;
	switch (opcode) {
		// TEST r/m, r
		case 0x84:
		case 0x85: {
			const ModRm modRm = readModRm();
			ok = SkipAddress(decoder, modRm, addressSize);
			insn.op = X86MemOp::Test;
			insn.size = opcode == 0x84 ? 1 : operandSize;
			if (insn.size == 1) {
				setByteReg(modRm.reg);
			} else {
				insn.reg = modRm.reg;
			}
			break;
		}
		// CMP r/m, r and CMP r, r/m
		case 0x38:
		case 0x39:
		case 0x3a:
		case 0x3b: {
			const ModRm modRm = readModRm();
			ok = SkipAddress(decoder, modRm, addressSize);
			insn.op = X86MemOp::Cmp;
			insn.size = (opcode & 1) == 0 ? 1 : operandSize;
			insn.reversed = opcode >= 0x3a;
			if (insn.size == 1) {
				setByteReg(modRm.reg);
			} else {
				insn.reg = modRm.reg;
			}
			break;
		}
		// CMP r/m, imm
		case 0x80:
		case 0x81:
		case 0x83: {
			const ModRm modRm = readModRm();
			ok = SkipAddress(decoder, modRm, addressSize) && (modRm.reg & 7) == 7;
			insn.op = X86MemOp::Cmp;
			insn.size = opcode == 0x80 ? 1 : operandSize;
			insn.immediateOperand = true;
			if (opcode == 0x83) {
				insn.immediate = uint64(int64(int8(uint8(decoder.Fetch(1)))));
			} else {
				insn.immediate = fetchImmediate(insn.size);
			}
			break;
		}
		// TEST r/m, imm
		case 0xf6:
		case 0xf7: {
			const ModRm modRm = readModRm();
			ok = SkipAddress(decoder, modRm, addressSize) && (modRm.reg & 7) == 0;
			insn.op = X86MemOp::Test;
			insn.size = opcode == 0xf6 ? 1 : operandSize;
			insn.immediateOperand = true;
			insn.immediate = fetchImmediate(insn.size);
			break;
		}
		// PUSH r/m, 64 bits in long mode unless 16
		case 0xff: {
			const ModRm modRm = readModRm();
			ok = SkipAddress(decoder, modRm, addressSize) && (modRm.reg & 7) == 6;
			insn.op = X86MemOp::Push;
			insn.size = longMode && !prefixes.operandSize ? 8 : operandSize;
			break;
		}
		case 0x88:
		case 0x8a: {
			const ModRm modRm = readModRm();
			ok = SkipAddress(decoder, modRm, addressSize);
			insn.op = opcode == 0x88 ? X86MemOp::Store : X86MemOp::Load;
			insn.size = 1;
			insn.regSize = 1;
			setByteReg(modRm.reg);
			break;
		}
		case 0x89:
		case 0x8b: {
			const ModRm modRm = readModRm();
			ok = SkipAddress(decoder, modRm, addressSize);
			insn.op = opcode == 0x89 ? X86MemOp::Store : X86MemOp::Load;
			insn.size = operandSize;
			insn.regSize = operandSize;
			insn.reg = modRm.reg;
			break;
		}
		case 0xa0:
		case 0xa1:
		case 0xa2:
		case 0xa3:
			decoder.Fetch(addressSize);
			insn.op = opcode < 0xa2 ? X86MemOp::Load : X86MemOp::Store;
			insn.size = (opcode & 1) != 0 ? operandSize : 1;
			insn.regSize = insn.size;
			insn.reg = 0;
			break;
		case 0xc6:
		case 0xc7: {
			const ModRm modRm = readModRm();
			ok = SkipAddress(decoder, modRm, addressSize) && (modRm.reg & 7) == 0;
			insn.op = X86MemOp::StoreImmediate;
			if (opcode == 0xc6) {
				insn.size = 1;
				insn.immediate = decoder.Fetch(1);
			} else if (operandSize == 2) {
				insn.size = 2;
				insn.immediate = decoder.Fetch(2);
			} else {
				insn.size = operandSize;
				insn.immediate = uint64(int64(int32(uint32(decoder.Fetch(4)))));
			}
			break;
		}
		case 0xa4:
		case 0xa5:
			insn.op = X86MemOp::Movs;
			insn.size = opcode == 0xa4 ? 1 : operandSize;
			break;
		case 0xaa:
		case 0xab:
			insn.op = X86MemOp::Stos;
			insn.size = opcode == 0xaa ? 1 : operandSize;
			break;
		case 0xac:
		case 0xad:
			insn.op = X86MemOp::Lods;
			insn.size = opcode == 0xac ? 1 : operandSize;
			insn.regSize = insn.size;
			break;
		case 0x0f: {
			const uint8 opcode2 = uint8(decoder.Fetch(1));
			switch (opcode2) {
				case 0xb6:
				case 0xb7:
				case 0xbe:
				case 0xbf: {
					const ModRm modRm = readModRm();
					ok = SkipAddress(decoder, modRm, addressSize);
					insn.op = X86MemOp::Load;
					insn.size = (opcode2 & 1) != 0 ? 2 : 1;
					insn.regSize = operandSize;
					insn.reg = modRm.reg;
					insn.extend = opcode2 < 0xbe ? X86Extend::Zero : X86Extend::Sign;
					break;
				}
				// MOVUPS, MOVUPD, MOVSS, MOVSD
				case 0x10:
				case 0x11: {
					const ModRm modRm = readModRm();
					ok = SkipAddress(decoder, modRm, addressSize);
					insn.op = opcode2 == 0x10 ? X86MemOp::LoadXmm : X86MemOp::StoreXmm;
					insn.size = prefixes.rep ? 4 : prefixes.repne ? 8 : 16;
					insn.rep = false;
					insn.reg = modRm.reg;
					break;
				}
				// MOVAPS, MOVAPD, MOVNTPS, MOVNTPD
				case 0x28:
				case 0x29:
				case 0x2b: {
					const ModRm modRm = readModRm();
					ok = SkipAddress(decoder, modRm, addressSize) && !prefixes.rep && !prefixes.repne;
					insn.op = opcode2 == 0x28 ? X86MemOp::LoadXmm : X86MemOp::StoreXmm;
					insn.size = 16;
					insn.reg = modRm.reg;
					break;
				}
				// MOVDQA, MOVDQU
				case 0x6f:
				case 0x7f: {
					const ModRm modRm = readModRm();
					ok = SkipAddress(decoder, modRm, addressSize) && (prefixes.operandSize || prefixes.rep);
					insn.op = opcode2 == 0x6f ? X86MemOp::LoadXmm : X86MemOp::StoreXmm;
					insn.size = 16;
					insn.rep = false;
					insn.reg = modRm.reg;
					break;
				}
				// MOVNTDQ
				case 0xe7: {
					const ModRm modRm = readModRm();
					ok = SkipAddress(decoder, modRm, addressSize) && prefixes.operandSize;
					insn.op = X86MemOp::StoreXmm;
					insn.size = 16;
					insn.reg = modRm.reg;
					break;
				}
				// MOVD, MOVQ: xmm <- r/m
				case 0x6e: {
					const ModRm modRm = readModRm();
					ok = SkipAddress(decoder, modRm, addressSize) && prefixes.operandSize;
					insn.op = X86MemOp::LoadXmm;
					insn.size = rexW ? 8 : 4;
					insn.reg = modRm.reg;
					break;
				}
				// MOVD, MOVQ: r/m <- xmm; with F3, MOVQ xmm <- m64
				case 0x7e: {
					const ModRm modRm = readModRm();
					ok = SkipAddress(decoder, modRm, addressSize);
					if (prefixes.rep) {
						insn.op = X86MemOp::LoadXmm;
						insn.size = 8;
						insn.rep = false;
					} else {
						ok = ok && prefixes.operandSize;
						insn.op = X86MemOp::StoreXmm;
						insn.size = rexW ? 8 : 4;
					}
					insn.reg = modRm.reg;
					break;
				}
				// MOVQ m64 <- xmm
				case 0xd6: {
					const ModRm modRm = readModRm();
					ok = SkipAddress(decoder, modRm, addressSize) && prefixes.operandSize;
					insn.op = X86MemOp::StoreXmm;
					insn.size = 8;
					insn.reg = modRm.reg;
					break;
				}
				default:
					ok = false;
					break;
			}
			break;
		}
		default:
			ok = false;
			break;
	}

	if (!ok || decoder.Overflow() || decoder.Pos() > 15) {
		return false;
	}
	if (insn.op != X86MemOp::Movs && insn.op != X86MemOp::Stos && insn.op != X86MemOp::Lods) {
		insn.rep = false;
	}
	insn.length = uint8(decoder.Pos());
	return true;
}
