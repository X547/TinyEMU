#include "SvmVcpu.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include <KernelExport.h>

#include <arch/x86/arch_cpu.h>
#include <smp.h>

#include "Common.h"
#include "SvmHost.h"
#include "SvmVm.h"
#include "Vcpu.h"
#include "Vm.h"
#include "Vmcb.h"


struct SvmRunContext {
	uint64 guestVmcb;
	uint64 hostSave;
	uint64 *gprs;
	void *hostFpu;
	void *guestFpu;
	uint64 hostXcr0;
	uint64 guestXcr0;
};

static_assert(offsetof(SvmRunContext, gprs) == 16);
static_assert(offsetof(SvmRunContext, guestXcr0) == 48);

extern "C" void tvmm_svm_run(SvmRunContext *context);


static constexpr uint64 kCr0Pe = 1ull << 0;
static constexpr uint64 kCr0Pg = 1ull << 31;
static constexpr uint64 kCr4Pse = 1ull << 4;
static constexpr uint64 kCr4Pae = 1ull << 5;
static constexpr uint64 kCr4La57 = 1ull << 12;
static constexpr uint64 kCr4Osxsave = 1ull << 18;
static constexpr uint64 kEferLma = 1ull << 10;
static constexpr uint64 kEferSvme = 1ull << 12;
static constexpr uint64 kRflagsDf = 1ull << 10;
static constexpr uint64 kRflagsVm = 1ull << 17;
static constexpr uint16 kSegAttribLong = 1 << 9;
static constexpr uint16 kSegAttribDefault32 = 1 << 10;
static constexpr uint64 kResetPat = 0x0007040600070406ull;

static constexpr uint64 kPtePresent = 1ull << 0;
static constexpr uint64 kPteLarge = 1ull << 7;
static constexpr uint64 kPteAddressMask = 0x000ffffffffff000ull;

static constexpr uint32 kMsrTsc = 0x10;
static constexpr uint32 kMsrPat = 0x277;
static constexpr uint32 kMsrEfer = 0xc0000080;

static constexpr uint8 kExceptionUd = 6;
static constexpr uint8 kExceptionGp = 13;
static constexpr uint8 kExceptionPf = 14;
static constexpr uint8 kExceptionAc = 17;

// XSAVE area layout
static constexpr size_t kXsaveMxcsr = 24;
static constexpr size_t kXsaveXmm = 160;
static constexpr size_t kXsaveHeader = 512;
static constexpr size_t kXsaveLegacyAndHeader = 576;
static constexpr uint64 kXstateSse = 1ull << 1;


// A field of the run page the client may change at any time, read once.
template <typename T>
static T ReadOnce(const T &field)
{
	return *static_cast<const volatile T*>(&field);
}


static uint64 ReadTsc()
{
	uint32 low;
	uint32 high;
	asm volatile("rdtsc" : "=a" (low), "=d" (high));
	return (uint64(high) << 32) | low;
}


static void Cpuid(uint32 leaf, uint32 subleaf, uint32 regs[4])
{
	asm volatile(
		"cpuid"
		: "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3])
		: "a" (leaf), "c" (subleaf)
	);
}


static uint64 SizeMask(uint8 size)
{
	return size >= 8 ? ~0ull : (1ull << (size * 8)) - 1;
}


static constexpr uint64 kFlagCf = 1 << 0;
static constexpr uint64 kFlagPf = 1 << 2;
static constexpr uint64 kFlagAf = 1 << 4;
static constexpr uint64 kFlagZf = 1 << 6;
static constexpr uint64 kFlagSf = 1 << 7;
static constexpr uint64 kFlagOf = 1 << 11;


// RFLAGS after a TEST (a AND b) or a CMP (a minus b) of 'size' bytes.
static uint64 ArithmeticFlags(uint64 rflags, uint64 a, uint64 b, uint8 size, bool subtract)
{
	const uint64 mask = SizeMask(size);
	const uint64 sign = 1ull << (size * 8 - 1);
	a &= mask;
	b &= mask;
	const uint64 result = (subtract ? a - b : a & b) & mask;
	uint64 flags = rflags & ~(kFlagCf | kFlagPf | kFlagAf | kFlagZf | kFlagSf | kFlagOf);
	if (subtract) {
		if (a < b) {
			flags |= kFlagCf;
		}
		if (((a ^ b) & (a ^ result) & sign) != 0) {
			flags |= kFlagOf;
		}
		if (((a ^ b ^ result) & 0x10) != 0) {
			flags |= kFlagAf;
		}
	}
	if (result == 0) {
		flags |= kFlagZf;
	}
	if ((result & sign) != 0) {
		flags |= kFlagSf;
	}
	if (__builtin_parityll(result & 0xff) == 0) {
		flags |= kFlagPf;
	}
	return flags;
}


SvmVcpu::SvmVcpu(SvmVm &svmVm, Vcpu &vcpu):
	fSvmVm(svmVm),
	fVm(svmVm.GetVm()),
	fVcpu(vcpu)
{
}


SvmVcpu::~SvmVcpu()
{
	if (fAsid != 0) {
		SvmHost::FreeAsid(fAsid);
	}
	free(fGuestFpu);
	free(fHostFpu);
}


status_t SvmVcpu::Init()
{
	fAsid = SvmHost::AllocAsid();
	if (fAsid == 0) {
		return B_BUSY;
	}
	CHECK_RET(fVmcbArea.Create("tvmm vmcb", kPageSize, B_FULL_LOCK));
	CHECK_RET(fVmcbArea.PhysicalAddress(0, fVmcbPhys));
	fVmcb = static_cast<Vmcb*>(fVmcbArea.Address());

	const size_t fpuSize = sizeof(TvmmX86State::fpu);
	fGuestFpu = memalign(64, fpuSize);
	fHostFpu = memalign(64, fpuSize);
	if (fGuestFpu == nullptr || fHostFpu == nullptr) {
		return B_NO_MEMORY;
	}
	memset(fHostFpu, 0, fpuSize);

	SetupControl();
	ResetState();
	return B_OK;
}


void SvmVcpu::SetupControl()
{
	VmcbControl &control = fVmcb->control;
	// Debug registers 0-3 are the host's while the guest runs.
	control.interceptDr = 0x000f000f;
	// #AC can be raised endlessly while being delivered, which would hold the
	// CPU in the guest.
	control.interceptExceptions = 1u << kExceptionAc;
	control.interceptMisc1 = SvmInterceptIntr | SvmInterceptNmi | SvmInterceptSmi
		| SvmInterceptInit | SvmInterceptCpuid | SvmInterceptInvd | SvmInterceptHlt
		| SvmInterceptInvlpga | SvmInterceptIoioProt | SvmInterceptMsrProt
		| SvmInterceptShutdown;
	control.interceptMisc2 = SvmInterceptVmrun | SvmInterceptVmmcall | SvmInterceptVmload
		| SvmInterceptVmsave | SvmInterceptStgi | SvmInterceptClgi | SvmInterceptSkinit
		| SvmInterceptMonitor | SvmInterceptMwait | SvmInterceptMwaitCond
		| SvmInterceptXsetbv | SvmInterceptRdpru;
	control.iopmBasePa = fSvmVm.IopmPhys();
	control.msrpmBasePa = fSvmVm.MsrpmPhys();
	control.asid = fAsid;
	// The guest's IF masks virtual interrupts only; physical ones always exit.
	control.vintr = kSvmVintrMasking;
	control.npControl = kSvmNpEnable;
	control.nCr3 = fSvmVm.NptRoot();
	control.tscOffset = -ReadTsc();
}


// The processor as at power on.
void SvmVcpu::ResetState()
{
	VmcbSave &save = fVmcb->save;
	const VmcbSegment data = {0, 0x93, 0xffff, 0};
	save.es = data;
	save.cs = {0xf000, 0x9b, 0xffff, 0xffff0000};
	save.ss = data;
	save.ds = data;
	save.fs = data;
	save.gs = data;
	save.gdtr = {0, 0, 0xffff, 0};
	save.idtr = {0, 0, 0xffff, 0};
	save.ldtr = {0, 0x82, 0xffff, 0};
	save.tr = {0, 0x8b, 0xffff, 0};
	save.cpl = 0;
	save.efer = kEferSvme;
	save.cr0 = 0x60000010;
	save.cr2 = 0;
	save.cr3 = 0;
	save.cr4 = 0;
	save.dr6 = 0xffff0ff0;
	save.dr7 = 0x400;
	save.rflags = 0x2;
	save.rip = 0xfff0;
	save.rsp = 0;
	save.rax = 0;
	save.gPat = kResetPat;

	VmcbControl &control = fVmcb->control;
	control.vintr = kSvmVintrMasking;
	control.interruptShadow = 0;
	control.eventInj = 0;
	control.interceptMisc1 &= ~SvmInterceptVintr;

	memset(fGprs, 0, sizeof(fGprs));
	memset(fDr, 0, sizeof(fDr));
	fGuestXcr0 = 1;
	fPending = Pending::None;
	ResetFpu();
}


// The x87 and SSE state after a processor reset. MXCSR must be valid with
// every exception masked; a zero MXCSR would unmask them all.
void SvmVcpu::ResetFpu()
{
	auto *area = static_cast<uint8*>(fGuestFpu);
	memset(area, 0, sizeof(TvmmX86State::fpu));
	const uint16 fcw = 0x0040;
	memcpy(area, &fcw, sizeof(fcw));
	const uint32 mxcsr = 0x1f80;
	memcpy(area + kXsaveMxcsr, &mxcsr, sizeof(mxcsr));
	const uint64 xstateBv = 0x3;
	memcpy(area + kXsaveHeader, &xstateBv, sizeof(xstateBv));
}


//#pragma mark - registers

uint64 &SvmVcpu::Gpr(uint32 index)
{
	if (index == TvmmX86GprRax) {
		return fVmcb->save.rax;
	}
	if (index == TvmmX86GprRsp) {
		return fVmcb->save.rsp;
	}
	return fGprs[index];
}


// A write of 'size' bytes, as an instruction makes it: 8 and 16 bit writes
// keep the rest of the register, 32 bit ones clear it.
void SvmVcpu::WriteGpr(uint8 reg, uint8 size, bool highByte, uint64 value)
{
	uint64 &target = Gpr(reg);
	switch (size) {
		case 1:
			if (highByte) {
				target = (target & ~0xff00ull) | ((value & 0xff) << 8);
			} else {
				target = (target & ~0xffull) | (value & 0xff);
			}
			break;
		case 2:
			target = (target & ~0xffffull) | (value & 0xffff);
			break;
		case 4:
			target = uint32(value);
			break;
		default:
			target = value;
			break;
	}
}


uint64 SvmVcpu::ReadGpr(uint8 reg, uint8 size, bool highByte)
{
	const uint64 value = Gpr(reg);
	if (highByte) {
		return (value >> 8) & 0xff;
	}
	return value & SizeMask(size);
}


uint8 *SvmVcpu::Xmm(uint8 reg)
{
	return static_cast<uint8*>(fGuestFpu) + kXsaveXmm + 16 * reg;
}


// A load into an XMM register clears what it does not fill.
void SvmVcpu::WriteXmm(uint8 reg, const uint8 *data, uint8 size)
{
	uint8 *xmm = Xmm(reg);
	memcpy(xmm, data, size);
	memset(xmm + size, 0, 16 - size);
	// XRSTOR loads the registers only with their component marked present.
	auto *area = static_cast<uint8*>(fGuestFpu);
	uint64 xstateBv;
	memcpy(&xstateBv, area + kXsaveHeader, sizeof(xstateBv));
	xstateBv |= kXstateSse;
	memcpy(area + kXsaveHeader, &xstateBv, sizeof(xstateBv));
}


//#pragma mark - guest addresses

X86CpuMode SvmVcpu::CpuMode() const
{
	const VmcbSave &save = fVmcb->save;
	if ((save.cr0 & kCr0Pe) == 0 || (save.rflags & kRflagsVm) != 0) {
		return X86CpuMode::Real16;
	}
	if ((save.efer & kEferLma) != 0 && (save.cs.attrib & kSegAttribLong) != 0) {
		return X86CpuMode::Long64;
	}
	return (save.cs.attrib & kSegAttribDefault32) != 0 ? X86CpuMode::Protected32 : X86CpuMode::Protected16;
}


uint64 SvmVcpu::Linear(int segment, uint64 offset, uint8 addressSize) const
{
	const VmcbSegment *segs = &fVmcb->save.es;
	offset &= SizeMask(addressSize);
	if (CpuMode() == X86CpuMode::Long64) {
		const uint64 base = segment == X86SegFs || segment == X86SegGs ? segs[segment].base : 0;
		return base + offset;
	}
	return (segs[segment].base + offset) & 0xffffffff;
}


// The guest's own translation of a linear address. Access rights are not
// checked; the guest made the access itself.
SvmVcpu::Walk SvmVcpu::Translate(uint64 linear, bool write, uint64 &gpa)
{
	const VmcbSave &save = fVmcb->save;
	if ((save.cr0 & kCr0Pg) == 0) {
		gpa = linear;
		return Walk::Ok;
	}

	auto fault = [&]() {
		fVmcb->save.cr2 = linear;
		const uint32 error = (write ? 2 : 0) | (save.cpl == 3 ? 4 : 0);
		InjectException(kExceptionPf, true, error);
		return Walk::Fault;
	};
	// tables of 512 entries of 8 bytes, from 'levels' down to the page
	auto walk = [&](uint64 table, int levels) {
		for (int level = levels; level >= 1; level--) {
			const uint32 shift = 12 + 9 * (level - 1);
			uint64 entry;
			if (!fVm.ReadGuest(table + ((linear >> shift) & 511) * 8, &entry, sizeof(entry))) {
				return Walk::Error;
			}
			if ((entry & kPtePresent) == 0) {
				return fault();
			}
			if (level == 1 || ((level == 2 || level == 3) && (entry & kPteLarge) != 0)) {
				const uint64 pageMask = (1ull << shift) - 1;
				gpa = (entry & kPteAddressMask & ~pageMask) | (linear & pageMask);
				return Walk::Ok;
			}
			table = entry & kPteAddressMask;
		}
		return Walk::Error;
	};

	if ((save.efer & kEferLma) != 0) {
		return walk(save.cr3 & kPteAddressMask, (save.cr4 & kCr4La57) != 0 ? 5 : 4);
	}
	linear &= 0xffffffff;
	if ((save.cr4 & kCr4Pae) != 0) {
		uint64 pdpte;
		if (!fVm.ReadGuest((save.cr3 & 0xffffffe0) + ((linear >> 30) & 3) * 8, &pdpte, sizeof(pdpte))) {
			return Walk::Error;
		}
		if ((pdpte & kPtePresent) == 0) {
			return fault();
		}
		return walk(pdpte & kPteAddressMask, 2);
	}

	uint32 pde;
	if (!fVm.ReadGuest((save.cr3 & 0xfffff000) + ((linear >> 22) & 1023) * 4, &pde, sizeof(pde))) {
		return Walk::Error;
	}
	if ((pde & kPtePresent) == 0) {
		return fault();
	}
	if ((pde & kPteLarge) != 0 && (save.cr4 & kCr4Pse) != 0) {
		gpa = (pde & 0xffc00000) | (uint64((pde >> 13) & 0xff) << 32) | (linear & 0x3fffff);
		return Walk::Ok;
	}
	uint32 pte;
	if (!fVm.ReadGuest((pde & 0xfffff000) + ((linear >> 12) & 1023) * 4, &pte, sizeof(pte))) {
		return Walk::Error;
	}
	if ((pte & kPtePresent) == 0) {
		return fault();
	}
	gpa = (pte & 0xfffff000) | (linear & 0xfff);
	return Walk::Ok;
}


// For an exit without the instruction bytes: they are read from guest RAM.
bool SvmVcpu::FetchInstruction(uint8 *bytes, uint32 &count)
{
	const uint8 addressSize = CpuMode() == X86CpuMode::Long64 ? 8 : 4;
	const uint64 linear = Linear(X86SegCs, fVmcb->save.rip, addressSize);
	const uint32 wanted = 15;
	uint32 done = 0;
	while (done < wanted) {
		const uint64 address = linear + done;
		const uint32 chunk = std::min<uint32>(wanted - done, kPageSize - (address & (kPageSize - 1)));
		uint64 gpa;
		const uint64 savedEvent = fVmcb->control.eventInj;
		const uint64 savedCr2 = fVmcb->save.cr2;
		if (Translate(address, false, gpa) != Walk::Ok) {
			// The guest just ran this, so no fault of its own belongs here.
			fVmcb->control.eventInj = savedEvent;
			fVmcb->save.cr2 = savedCr2;
			break;
		}
		if (!fVm.ReadGuest(gpa, bytes + done, chunk)) {
			break;
		}
		done += chunk;
	}
	count = done;
	return done > 0;
}


// One element of a string instruction done: the index registers move on, and
// with REP the count goes down, the instruction running again until it is 0.
void SvmVcpu::AdvanceString(const X86MemInsn &insn, bool source, bool destination)
{
	const uint64 mask = SizeMask(insn.addressSize);
	auto update = [&](uint32 reg, int64 delta) {
		uint64 &target = Gpr(reg);
		const uint64 value = (target + delta) & mask;
		target = insn.addressSize == 2 ? (target & ~0xffffull) | value : value;
	};
	const int64 step = (fVmcb->save.rflags & kRflagsDf) != 0 ? -int64(insn.size) : int64(insn.size);
	if (source) {
		update(TvmmX86GprRsi, step);
	}
	if (destination) {
		update(TvmmX86GprRdi, step);
	}
	if (insn.rep) {
		update(TvmmX86GprRcx, -1);
		if ((Gpr(TvmmX86GprRcx) & mask) != 0) {
			return;
		}
	}
	fVmcb->save.rip += insn.length;
}


//#pragma mark - exits

void SvmVcpu::InjectException(uint8 vector, bool hasError, uint32 error)
{
	uint64 event = vector | (kSvmEventTypeException << kSvmEventTypeShift) | kSvmEventValid;
	if (hasError) {
		event |= kSvmEventErrorValid | (uint64(error) << 32);
	}
	fVmcb->control.eventInj = event;
}


void SvmVcpu::FillExitState(TvmmRun &run) const
{
	const VmcbControl &control = fVmcb->control;
	run.rflags = fVmcb->save.rflags;
	run.cr8 = control.vintr & 0xf;
	run.interruptShadow = (control.interruptShadow & 1) != 0;
	run.eventPending = (control.eventInj & kSvmEventValid) != 0;
}


ExitAction SvmVcpu::ToClient(TvmmRun &run, TvmmExit reason)
{
	FillExitState(run);
	run.exitReason = reason;
	return ExitAction::ToClient;
}


ExitAction SvmVcpu::Unhandled(TvmmRun &run)
{
	const VmcbControl &control = fVmcb->control;
	TvmmExitUnhandled &exit = run.exit.unhandled;
	exit.code = control.exitCode;
	exit.info1 = control.exitInfo1;
	exit.info2 = control.exitInfo2;
	exit.rip = fVmcb->save.rip;
	exit.insnLength = control.insnLength;
	memcpy(exit.insnBytes, control.insnBytes, sizeof(exit.insnBytes));
	return ToClient(run, TvmmExit::Unhandled);
}


ExitAction SvmVcpu::MmioRead(TvmmRun &run, uint64 gpa, uint8 size)
{
	TvmmExitMmio &exit = run.exit.mmio;
	exit.gpa = gpa;
	exit.size = size;
	exit.write = 0;
	memset(exit.data, 0, sizeof(exit.data));
	return ToClient(run, TvmmExit::Mmio);
}


ExitAction SvmVcpu::MmioWrite(TvmmRun &run, uint64 gpa, uint8 size, const void *data)
{
	TvmmExitMmio &exit = run.exit.mmio;
	exit.gpa = gpa;
	exit.size = size;
	exit.write = 1;
	memset(exit.data, 0, sizeof(exit.data));
	memcpy(exit.data, data, size);
	return ToClient(run, TvmmExit::Mmio);
}


//#pragma mark - running

// What the client read for a pending load goes where the instruction puts it.
void SvmVcpu::CompleteLoad(const uint8 *data)
{
	const X86MemInsn &insn = fPendingInsn;
	uint64 value = 0;
	memcpy(&value, data, std::min<uint8>(insn.size, sizeof(value)));
	switch (insn.op) {
		case X86MemOp::Load:
			if (insn.extend == X86Extend::Sign) {
				const uint32 shift = 64 - 8 * insn.size;
				value = uint64(int64(value << shift) >> shift);
			}
			WriteGpr(insn.reg, insn.regSize, insn.highByte, value);
			fVmcb->save.rip += insn.length;
			break;
		case X86MemOp::LoadXmm:
			WriteXmm(insn.reg, data, insn.size);
			fVmcb->save.rip += insn.length;
			break;
		case X86MemOp::Lods:
			WriteGpr(TvmmX86GprRax, insn.size, false, value);
			AdvanceString(insn, true, false);
			break;
		case X86MemOp::Test:
		case X86MemOp::Cmp: {
			const uint64 operand = insn.immediateOperand
				? insn.immediate : ReadGpr(insn.reg, insn.size, insn.highByte);
			const bool subtract = insn.op == X86MemOp::Cmp;
			fVmcb->save.rflags = insn.reversed
				? ArithmeticFlags(fVmcb->save.rflags, operand, value, insn.size, subtract)
				: ArithmeticFlags(fVmcb->save.rflags, value, operand, insn.size, subtract);
			fVmcb->save.rip += insn.length;
			break;
		}
		case X86MemOp::Push:
			if (Push(value, insn.size)) {
				fVmcb->save.rip += insn.length;
			}
			break;
		default:
			break;
	}
}


// False after a fault of the guest's own, which is injected.
bool SvmVcpu::Push(uint64 value, uint8 size)
{
	const VmcbSave &save = fVmcb->save;
	uint8 stackSize = (save.ss.attrib & kSegAttribDefault32) != 0 ? 4 : 2;
	if (CpuMode() == X86CpuMode::Long64) {
		stackSize = 8;
	}
	const uint64 mask = SizeMask(stackSize);
	uint64 &rsp = Gpr(TvmmX86GprRsp);
	const uint64 top = (rsp - size) & mask;
	const uint64 linear = Linear(X86SegSs, top, stackSize);
	uint64 gpa;
	if (Translate(linear, true, gpa) != Walk::Ok) {
		return false;
	}
	if ((linear & (kPageSize - 1)) + size > kPageSize || !fVm.WriteGuest(gpa, &value, size)) {
		// a stack that is not RAM, or across pages: the push is lost
		TVMM_LOG("push to %#" B_PRIx64 " not done\n", linear);
	}
	rsp = stackSize == 2 ? (rsp & ~0xffffull) | top : top;
	return true;
}


status_t SvmVcpu::PrepareRun(TvmmRun &run)
{
	VmcbSave &save = fVmcb->save;
	switch (fPending) {
		case Pending::None:
			break;
		case Pending::IoIn: {
			const uint32 value = ReadOnce(run.exit.io.data);
			WriteGpr(TvmmX86GprRax, fPendingInsn.size, false, value);
			save.rip = fPendingNextRip;
			break;
		}
		case Pending::InsIn: {
			const uint32 value = ReadOnce(run.exit.io.data);
			fVm.WriteGuest(fPendingGpa, &value, fPendingInsn.size);
			AdvanceString(fPendingInsn, false, true);
			break;
		}
		case Pending::MsrRead: {
			if (!ReadOnce(run.exit.msr.ok)) {
				InjectException(kExceptionGp, true, 0);
				break;
			}
			const uint64 value = ReadOnce(run.exit.msr.value);
			save.rax = uint32(value);
			fGprs[TvmmX86GprRdx] = value >> 32;
			save.rip = fPendingNextRip;
			break;
		}
		case Pending::MsrWrite:
			if (!ReadOnce(run.exit.msr.ok)) {
				InjectException(kExceptionGp, true, 0);
				break;
			}
			save.rip = fPendingNextRip;
			break;
		case Pending::MmioLoad: {
			uint8 data[16];
			memcpy(data, run.exit.mmio.data, sizeof(data));
			CompleteLoad(data);
			break;
		}
		case Pending::MovsLoad: {
			uint8 data[8];
			memcpy(data, run.exit.mmio.data, sizeof(data));
			fVm.WriteGuest(fPendingGpa, data, fPendingInsn.size);
			AdvanceString(fPendingInsn, true, true);
			break;
		}
	}
	fPending = Pending::None;

	VmcbControl &control = fVmcb->control;
	const uint8 injectType = ReadOnce(run.injectType);
	const uint8 injectVector = ReadOnce(run.injectVector);
	const bool window = ReadOnce(run.requestInterruptWindow) != 0;
	const bool tprExits = ReadOnce(run.tprExits) != 0;
	const bool setCr8 = ReadOnce(run.setCr8) != 0;
	const uint8 cr8Value = ReadOnce(run.cr8Value);
	if (injectType != TvmmInjectNone) {
		if ((control.eventInj & kSvmEventValid) != 0) {
			return B_BUSY;
		}
		switch (injectType) {
			case TvmmInjectInterrupt:
				control.eventInj = injectVector | (kSvmEventTypeIntr << kSvmEventTypeShift) | kSvmEventValid;
				break;
			case TvmmInjectNmi:
				control.eventInj = 2 | (kSvmEventTypeNmi << kSvmEventTypeShift) | kSvmEventValid;
				break;
			default:
				return B_BAD_VALUE;
		}
	}
	// An interrupt the guest can never take, which exits as soon as the
	// guest could take one.
	const uint64 windowBits = kSvmVintrIrq | kSvmVintrIgnoreTpr | (uint64(0xf) << kSvmVintrPrioShift);
	if (window) {
		control.vintr |= windowBits;
		control.interceptMisc1 |= SvmInterceptVintr;
	} else {
		control.vintr &= ~windowBits;
		control.interceptMisc1 &= ~SvmInterceptVintr;
	}
	if (tprExits) {
		control.interceptCr |= kSvmInterceptCr8Write;
	} else {
		control.interceptCr &= ~kSvmInterceptCr8Write;
	}
	if (setCr8) {
		control.vintr = (control.vintr & ~kSvmVintrTprMask) | (cr8Value & 0xf);
	}
	run.exitReason = TvmmExit::None;
	return B_OK;
}


void SvmVcpu::CancelRun(TvmmRun &run)
{
	ToClient(run, TvmmExit::Canceled);
}


ExitAction SvmVcpu::EnterGuest(TvmmRun &run)
{
	const cpu_status state = disable_interrupts();
	const int32 cpu = smp_get_current_cpu();
	int64 generation = 0;
	if (!fVcpu.BeginGuestEntry(cpu, generation)) {
		restore_interrupts(state);
		CancelRun(run);
		return ExitAction::ToClient;
	}

	VmcbControl &control = fVmcb->control;
	control.tlbControl = kSvmTlbNothing;
	if (cpu != fLastCpu || generation != fTlbGeneration) {
		control.tlbControl = SvmHost::HasFeature(TvmmSvmFeatureFlushByAsid)
			? kSvmTlbFlushAsid : kSvmTlbFlushAll;
		fLastCpu = cpu;
		fTlbGeneration = generation;
	}
	control.cleanBits = 0;

	SvmRunContext context = {
		fVmcbPhys,
		SvmHost::HostSavePage(cpu),
		fGprs,
		fHostFpu,
		fGuestFpu,
		SvmHost::HostXcr0(),
		fGuestXcr0
	};
	tvmm_svm_run(&context);

	fVcpu.EndGuestEntry();
	restore_interrupts(state);
	return HandleExit(run);
}


ExitAction SvmVcpu::HandleExit(TvmmRun &run)
{
	VmcbControl &control = fVmcb->control;
	VmcbSave &save = fVmcb->save;

	// An event whose delivery the exit cut short is delivered again.
	if ((control.exitIntInfo & kSvmEventValid) != 0) {
		control.eventInj = control.exitIntInfo;
	}

	const uint64 code = control.exitCode;
	if (code >= SvmExitReadDr0 && code < SvmExitWriteDr0 + 0x10) {
		return HandleDr(run);
	}

	ExitAction action = ExitAction::Resume;
	switch (code) {
		case SvmExitIntr:
		case SvmExitNmi:
		case SvmExitSmi:
		case SvmExitInit:
		case SvmExitPause:
			break;
		case SvmExitVintr:
			control.vintr &= ~(kSvmVintrIrq | kSvmVintrIgnoreTpr | (uint64(0xf) << kSvmVintrPrioShift));
			control.interceptMisc1 &= ~SvmInterceptVintr;
			action = ToClient(run, TvmmExit::InterruptWindow);
			break;
		case SvmExitWriteCr8:
			action = HandleCr8Write(run);
			break;
		case SvmExitCpuid:
			HandleCpuid();
			break;
		case SvmExitHlt:
			save.rip = control.nextRip;
			action = ToClient(run, TvmmExit::Halted);
			break;
		case SvmExitIoio:
			action = HandleIo(run);
			break;
		case SvmExitNpf:
			action = HandleNpf(run);
			break;
		case SvmExitMsr:
			action = HandleMsr(run);
			break;
		case SvmExitXsetbv:
			HandleXsetbv();
			break;
		case SvmExitInvd:
			save.rip = control.nextRip;
			break;
		case SvmExitShutdown:
			action = ToClient(run, TvmmExit::Shutdown);
			break;
		case SvmExitVmrun:
		case SvmExitVmmcall:
		case SvmExitVmload:
		case SvmExitVmsave:
		case SvmExitStgi:
		case SvmExitClgi:
		case SvmExitSkinit:
		case SvmExitInvlpga:
		case SvmExitMonitor:
		case SvmExitMwait:
		case SvmExitMwaitCond:
		case SvmExitRdpru:
			InjectException(kExceptionUd);
			break;
		case SvmExitException0 + kExceptionAc:
			InjectException(kExceptionAc, true, uint32(control.exitInfo1));
			break;
		case SvmExitInvalid:
			run.exit.failEntry.code = code;
			action = ToClient(run, TvmmExit::FailEntry);
			break;
		default:
			action = Unhandled(run);
			break;
	}
	return action;
}


ExitAction SvmVcpu::HandleIo(TvmmRun &run)
{
	const VmcbControl &control = fVmcb->control;
	VmcbSave &save = fVmcb->save;
	const uint64 info = control.exitInfo1;
	if ((info & kSvmIoioString) != 0) {
		return HandleStringIo(run);
	}
	const uint8 size = (info >> kSvmIoioSizeShift) & 7;
	const bool in = (info & kSvmIoioIn) != 0;

	TvmmExitIo &exit = run.exit.io;
	exit.port = uint16(info >> kSvmIoioPortShift);
	exit.size = size;
	exit.in = in;
	if (in) {
		exit.data = 0;
		fPending = Pending::IoIn;
		fPendingInsn = {};
		fPendingInsn.size = size;
		fPendingNextRip = control.exitInfo2;
	} else {
		exit.data = uint32(save.rax & SizeMask(size));
		save.rip = control.exitInfo2;
	}
	return ToClient(run, TvmmExit::Io);
}


// INS and OUTS, one element each time; with REP the instruction runs again
// for the next.
ExitAction SvmVcpu::HandleStringIo(TvmmRun &run)
{
	const VmcbControl &control = fVmcb->control;
	VmcbSave &save = fVmcb->save;
	const uint64 info = control.exitInfo1;
	const bool in = (info & kSvmIoioIn) != 0;

	X86MemInsn insn {};
	insn.size = (info >> kSvmIoioSizeShift) & 7;
	insn.addressSize = (info & (1 << 9)) != 0 ? 8 : (info & (1 << 8)) != 0 ? 4 : 2;
	insn.rep = (info & kSvmIoioRep) != 0;
	insn.length = uint8(control.exitInfo2 - save.rip);
	if (insn.rep && (Gpr(TvmmX86GprRcx) & SizeMask(insn.addressSize)) == 0) {
		save.rip = control.exitInfo2;
		return ExitAction::Resume;
	}

	const uint64 linear = in
		? Linear(X86SegEs, Gpr(TvmmX86GprRdi), insn.addressSize)
		: Linear(int((info >> 10) & 7), Gpr(TvmmX86GprRsi), insn.addressSize);
	if ((linear & (kPageSize - 1)) + insn.size > kPageSize) {
		return Unhandled(run);
	}
	uint64 gpa;
	const Walk walk = Translate(linear, in, gpa);
	if (walk == Walk::Fault) {
		return ExitAction::Resume;
	}
	if (walk == Walk::Error) {
		return Unhandled(run);
	}

	TvmmExitIo &exit = run.exit.io;
	exit.port = uint16(info >> kSvmIoioPortShift);
	exit.size = insn.size;
	exit.in = in;
	if (in) {
		if (!fVm.IsRam(gpa, true)) {
			return Unhandled(run);
		}
		exit.data = 0;
		fPending = Pending::InsIn;
		fPendingGpa = gpa;
		fPendingInsn = insn;
	} else {
		uint32 value = 0;
		if (!fVm.ReadGuest(gpa, &value, insn.size)) {
			return Unhandled(run);
		}
		exit.data = value;
		AdvanceString(insn, true, false);
	}
	return ToClient(run, TvmmExit::Io);
}


// An access to guest physical memory that is not RAM, or a write to RAM
// mapped read only: the instruction is decoded and its access goes to the
// client.
ExitAction SvmVcpu::HandleNpf(TvmmRun &run)
{
	const VmcbControl &control = fVmcb->control;
	VmcbSave &save = fVmcb->save;
	const uint64 gpa = control.exitInfo2;
	const bool write = (control.exitInfo1 & 2) != 0;
	if (fVm.IsRam(gpa, write)) {
		// the mapping changed while the guest ran; it tries again
		return ExitAction::Resume;
	}

	uint8 bytes[15];
	uint32 count = control.insnLength;
	memcpy(bytes, control.insnBytes, sizeof(bytes));
	if (count == 0 && !FetchInstruction(bytes, count)) {
		return Unhandled(run);
	}
	X86MemInsn insn;
	if (!X86DecodeMemInsn(bytes, count, CpuMode(), insn)) {
		return Unhandled(run);
	}

	ExitAction action = ExitAction::ToClient;
	switch (insn.op) {
		case X86MemOp::Load:
		case X86MemOp::LoadXmm:
		case X86MemOp::Lods:
		case X86MemOp::Test:
		case X86MemOp::Cmp:
		case X86MemOp::Push:
			fPending = Pending::MmioLoad;
			fPendingInsn = insn;
			action = MmioRead(run, gpa, insn.size);
			break;
		case X86MemOp::Store: {
			const uint64 value = ReadGpr(insn.reg, insn.size, insn.highByte);
			save.rip += insn.length;
			action = MmioWrite(run, gpa, insn.size, &value);
			break;
		}
		case X86MemOp::StoreImmediate:
			save.rip += insn.length;
			action = MmioWrite(run, gpa, insn.size, &insn.immediate);
			break;
		case X86MemOp::StoreXmm: {
			uint8 data[16];
			memcpy(data, Xmm(insn.reg), sizeof(data));
			save.rip += insn.length;
			action = MmioWrite(run, gpa, insn.size, data);
			break;
		}
		case X86MemOp::Stos: {
			const uint64 value = ReadGpr(TvmmX86GprRax, insn.size, false);
			AdvanceString(insn, false, true);
			action = MmioWrite(run, gpa, insn.size, &value);
			break;
		}
		case X86MemOp::Movs: {
			const int sourceSegment = insn.segment != X86SegNone ? int(insn.segment) : int(X86SegDs);
			const uint64 linear = write
				? Linear(sourceSegment, Gpr(TvmmX86GprRsi), insn.addressSize)
				: Linear(X86SegEs, Gpr(TvmmX86GprRdi), insn.addressSize);
			uint64 other;
			const Walk walk = Translate(linear, !write, other);
			if (walk == Walk::Fault) {
				action = ExitAction::Resume;
				break;
			}
			if (walk == Walk::Error || (linear & (kPageSize - 1)) + insn.size > kPageSize) {
				action = Unhandled(run);
				break;
			}
			if (write) {
				// from RAM to the device
				uint64 value = 0;
				if (!fVm.ReadGuest(other, &value, insn.size)) {
					action = Unhandled(run);
					break;
				}
				AdvanceString(insn, true, true);
				action = MmioWrite(run, gpa, insn.size, &value);
			} else {
				// from the device to RAM
				if (!fVm.IsRam(other, true)) {
					action = Unhandled(run);
					break;
				}
				fPending = Pending::MovsLoad;
				fPendingGpa = other;
				fPendingInsn = insn;
				action = MmioRead(run, gpa, insn.size);
			}
			break;
		}
	}
	return action;
}


ExitAction SvmVcpu::HandleMsr(TvmmRun &run)
{
	const VmcbControl &control = fVmcb->control;
	VmcbSave &save = fVmcb->save;
	const uint32 msr = uint32(fGprs[TvmmX86GprRcx]);
	const bool write = control.exitInfo1 == 1;
	const uint64 value = (fGprs[TvmmX86GprRdx] << 32) | uint32(save.rax);

	bool handled = true;
	bool ok = true;
	uint64 result = 0;
	switch (msr) {
		case kMsrEfer:
			if (write) {
				save.efer = value | kEferSvme;
			} else {
				result = save.efer & ~kEferSvme;
			}
			break;
		case kMsrPat:
			if (write) {
				for (int i = 0; i < 8; i++) {
					const uint8 type = (value >> (i * 8)) & 0xff;
					if (type == 2 || type == 3 || type > 7) {
						ok = false;
					}
				}
				if (ok) {
					save.gPat = value;
				}
			} else {
				result = save.gPat;
			}
			break;
		case kMsrTsc:
			if (write) {
				fVmcb->control.tscOffset = value - ReadTsc();
			} else {
				result = ReadTsc() + fVmcb->control.tscOffset;
			}
			break;
		default:
			handled = false;
			break;
	}

	if (handled) {
		if (!ok) {
			InjectException(kExceptionGp, true, 0);
			return ExitAction::Resume;
		}
		if (!write) {
			save.rax = uint32(result);
			fGprs[TvmmX86GprRdx] = result >> 32;
		}
		save.rip = control.nextRip;
		return ExitAction::Resume;
	}

	TvmmExitMsr &exit = run.exit.msr;
	exit.msr = msr;
	exit.write = write;
	exit.ok = 1;
	exit.value = write ? value : 0;
	fPending = write ? Pending::MsrWrite : Pending::MsrRead;
	fPendingNextRip = control.nextRip;
	return ToClient(run, TvmmExit::Msr);
}


// Only with TvmmRun::tprExits, for a task priority kept by the client.
ExitAction SvmVcpu::HandleCr8Write(TvmmRun &run)
{
	VmcbControl &control = fVmcb->control;
	const uint64 info = control.exitInfo1;
	if ((info & (1ull << 63)) == 0) {
		return Unhandled(run);
	}
	const uint64 value = Gpr(info & 0xf) & 0xf;
	control.vintr = (control.vintr & ~kSvmVintrTprMask) | value;
	fVmcb->save.rip = control.nextRip;
	return ToClient(run, TvmmExit::TprChanged);
}


// What of the host's processor a guest cannot have, as the bits of each
// register to clear: instructions that are made to raise #UD, and features
// whose MSRs or state are not virtualized. All bits of a leaf listed as
// cleared entirely.
struct CpuidHidden {
	uint32 leaf;
	uint32 regs[4];
};

static const CpuidHidden kCpuidHidden[] = {
	// MONITOR, VMX, SMX, EST, TM2, PDCM, DCA; ACPI, TM, PBE
	{1, {0, 0, (1u << 3) | (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 15) | (1u << 18),
		(1u << 22) | (1u << 29) | (1u << 31)}},
	// MONITOR and MWAIT, thermal and power management
	{5, {~0u, ~0u, ~0u, ~0u}},
	{6, {~0u, ~0u, ~0u, ~0u}},
	// TSC_ADJUST, SGX; WAITPKG, CET shadow stacks, SGX_LC; CET IBT
	{7, {0, (1u << 1) | (1u << 2), (1u << 5) | (1u << 7) | (1u << 30), 1u << 20}},
	// SVM, SKINIT, WDT, LWP, the performance counter extensions, MONITORX
	{0x80000001, {0, 0, (1u << 2) | (1u << 12) | (1u << 13) | (1u << 15) | (1u << 23)
		| (1u << 24) | (1u << 28) | (1u << 29), 0}},
	// SVM's own leaf
	{0x8000000a, {~0u, ~0u, ~0u, ~0u}},
};


// The host's processor as the client's table changes it, less what this
// cannot give a guest, with what differs per processor filled in.
void SvmVcpu::HandleCpuid()
{
	VmcbSave &save = fVmcb->save;
	const uint32 leaf = uint32(save.rax);
	const uint32 subleaf = uint32(fGprs[TvmmX86GprRcx]);
	uint32 regs[4];
	Cpuid(leaf, subleaf, regs);
	if (const TvmmCpuidEntry *entry = fSvmVm.CpuidEntry(leaf)) {
		for (int i = 0; i < 4; i++) {
			regs[i] = (regs[i] & ~entry->del[i]) | entry->set[i];
		}
	}
	for (const CpuidHidden &hidden: kCpuidHidden) {
		if (hidden.leaf == leaf) {
			for (int i = 0; i < 4; i++) {
				regs[i] &= ~hidden.regs[i];
			}
		}
	}

	switch (leaf) {
		case 1:
			regs[1] = (regs[1] & 0x00ffffff) | (fVcpu.Index() << 24);
			regs[2] &= ~(1u << 27);                 // OSXSAVE
			if ((save.cr4 & kCr4Osxsave) != 0) {
				regs[2] |= 1u << 27;
			}
			break;
		case 0xb:
		case 0x1f:
			regs[3] = fVcpu.Index();
			break;
		case 0xd:
			if (subleaf == 0) {
				const uint64 supported = SvmHost::HostXcr0();
				regs[0] &= uint32(supported);
				regs[3] &= uint32(supported >> 32);
				// the area for the XCR0 the guest has set
				uint32 size = kXsaveLegacyAndHeader;
				for (uint32 i = 2; i < 64; i++) {
					if ((fGuestXcr0 & (1ull << i)) != 0) {
						uint32 component[4];
						Cpuid(0xd, i, component);
						size = std::max(size, component[1] + component[0]);
					}
				}
				regs[1] = size;
			} else if (subleaf == 1) {
				regs[0] &= ~(1u << 3);              // XSAVES, without IA32_XSS
				regs[1] = 0;
			}
			break;
		default:
			break;
	}

	save.rax = regs[0];
	fGprs[TvmmX86GprRbx] = regs[1];
	fGprs[TvmmX86GprRcx] = regs[2];
	fGprs[TvmmX86GprRdx] = regs[3];
	save.rip = fVmcb->control.nextRip;
}


bool SvmVcpu::ValidXcr0(uint64 value) const
{
	const uint64 avx512 = 0x7ull << 5;
	return (value & 1) != 0
		&& (value & ~SvmHost::HostXcr0()) == 0
		&& ((value & 4) == 0 || (value & 2) != 0)
		&& ((value & avx512) == 0 || ((value & avx512) == avx512 && (value & 4) != 0));
}


void SvmVcpu::HandleXsetbv()
{
	VmcbSave &save = fVmcb->save;
	const uint32 index = uint32(fGprs[TvmmX86GprRcx]);
	const uint64 value = (fGprs[TvmmX86GprRdx] << 32) | uint32(save.rax);
	if (save.cpl != 0 || index != 0 || !ValidXcr0(value)) {
		InjectException(kExceptionGp, true, 0);
		return;
	}
	fGuestXcr0 = value;
	save.rip = fVmcb->control.nextRip;
}


ExitAction SvmVcpu::HandleDr(TvmmRun &run)
{
	const VmcbControl &control = fVmcb->control;
	const uint64 code = control.exitCode;
	// the decode assist names the general register
	const uint64 info = control.exitInfo1;
	const uint32 dr = code & 0xf;
	const uint32 gpr = info & 0xf;
	if (dr >= 4) {
		return Unhandled(run);
	}
	if (code < SvmExitWriteDr0) {
		Gpr(gpr) = fDr[dr];
	} else {
		fDr[dr] = Gpr(gpr);
	}
	fVmcb->save.rip = control.nextRip;
	return ExitAction::Resume;
}


//#pragma mark - state

bool SvmVcpu::ValidFpu(const uint8 *area) const
{
	uint64 xstateBv;
	memcpy(&xstateBv, area + kXsaveHeader, sizeof(xstateBv));
	if ((xstateBv & ~SvmHost::HostXcr0()) != 0) {
		return false;
	}
	// XCOMP_BV and the rest of the header: zero in the standard format
	for (size_t i = kXsaveHeader + 8; i < kXsaveLegacyAndHeader; i++) {
		if (area[i] != 0) {
			return false;
		}
	}
	uint32 mxcsr;
	memcpy(&mxcsr, area + kXsaveMxcsr, sizeof(mxcsr));
	return (mxcsr & ~SvmHost::MxcsrMask()) == 0;
}


void SvmVcpu::GetState(uint32 classes, void *buffer)
{
	auto &state = *static_cast<TvmmX86State*>(buffer);
	const VmcbSave &save = fVmcb->save;
	const VmcbControl &control = fVmcb->control;

	if ((classes & TvmmX86StateGprs) != 0) {
		for (uint32 i = 0; i < TvmmX86GprCount; i++) {
			state.gprs[i] = Gpr(i);
		}
		state.rip = save.rip;
		state.rflags = save.rflags;
	}
	if ((classes & TvmmX86StateSegs) != 0) {
		const VmcbSegment *segs = &save.es;
		for (uint32 i = 0; i < TvmmX86SegCount; i++) {
			state.segs[i] = {segs[i].base, segs[i].limit, segs[i].selector, segs[i].attrib};
		}
	}
	if ((classes & TvmmX86StateCrs) != 0) {
		state.cr0 = save.cr0;
		state.cr2 = save.cr2;
		state.cr3 = save.cr3;
		state.cr4 = save.cr4;
		state.cr8 = control.vintr & 0xf;
		state.xcr0 = fGuestXcr0;
		state.efer = save.efer & ~kEferSvme;
	}
	if ((classes & TvmmX86StateDrs) != 0) {
		memcpy(state.dr, fDr, sizeof(state.dr));
		state.dr6 = save.dr6;
		state.dr7 = save.dr7;
	}
	if ((classes & TvmmX86StateMsrs) != 0) {
		state.msrs[TvmmX86MsrStar] = save.star;
		state.msrs[TvmmX86MsrLstar] = save.lstar;
		state.msrs[TvmmX86MsrCstar] = save.cstar;
		state.msrs[TvmmX86MsrSfmask] = save.sfmask;
		state.msrs[TvmmX86MsrKernelGsBase] = save.kernelGsBase;
		state.msrs[TvmmX86MsrSysenterCs] = save.sysenterCs;
		state.msrs[TvmmX86MsrSysenterEsp] = save.sysenterEsp;
		state.msrs[TvmmX86MsrSysenterEip] = save.sysenterEip;
		state.msrs[TvmmX86MsrPat] = save.gPat;
		state.msrs[TvmmX86MsrTsc] = ReadTsc() + control.tscOffset;
		state.msrs[TvmmX86MsrTscAux] = 0;
	}
	if ((classes & TvmmX86StateIntr) != 0) {
		state.interruptShadow = control.interruptShadow & 1;
		state.pendingEvent = control.eventInj;
	}
	if ((classes & TvmmX86StateFpu) != 0) {
		const size_t size = SvmHost::XsaveSize();
		memcpy(state.fpu, fGuestFpu, size);
		memset(state.fpu + size, 0, sizeof(state.fpu) - size);
	}
}


status_t SvmVcpu::SetState(uint32 classes, const void *buffer)
{
	const auto &state = *static_cast<const TvmmX86State*>(buffer);
	VmcbSave &save = fVmcb->save;
	VmcbControl &control = fVmcb->control;

	if ((classes & ~uint32(TvmmX86StateAll)) != 0) {
		return B_BAD_VALUE;
	}
	if ((classes & TvmmX86StateCrs) != 0 && !ValidXcr0(state.xcr0)) {
		return B_BAD_VALUE;
	}
	if ((classes & TvmmX86StateFpu) != 0 && !ValidFpu(state.fpu)) {
		return B_BAD_VALUE;
	}

	if ((classes & TvmmX86StateGprs) != 0) {
		for (uint32 i = 0; i < TvmmX86GprCount; i++) {
			Gpr(i) = state.gprs[i];
		}
		save.rip = state.rip;
		save.rflags = state.rflags;
		// the registers an exit would have finished are set now
		fPending = Pending::None;
	}
	if ((classes & TvmmX86StateSegs) != 0) {
		VmcbSegment *segs = &save.es;
		for (uint32 i = 0; i < TvmmX86SegCount; i++) {
			segs[i] = {state.segs[i].selector, state.segs[i].attrib, state.segs[i].limit, state.segs[i].base};
		}
		save.cpl = (save.ss.attrib >> 5) & 3;
	}
	if ((classes & TvmmX86StateCrs) != 0) {
		save.cr0 = state.cr0;
		save.cr2 = state.cr2;
		save.cr3 = state.cr3;
		save.cr4 = state.cr4;
		control.vintr = (control.vintr & ~kSvmVintrTprMask) | (state.cr8 & 0xf);
		fGuestXcr0 = state.xcr0;
		save.efer = state.efer | kEferSvme;
	}
	if ((classes & TvmmX86StateDrs) != 0) {
		memcpy(fDr, state.dr, sizeof(fDr));
		save.dr6 = state.dr6;
		save.dr7 = state.dr7;
	}
	if ((classes & TvmmX86StateMsrs) != 0) {
		save.star = state.msrs[TvmmX86MsrStar];
		save.lstar = state.msrs[TvmmX86MsrLstar];
		save.cstar = state.msrs[TvmmX86MsrCstar];
		save.sfmask = state.msrs[TvmmX86MsrSfmask];
		save.kernelGsBase = state.msrs[TvmmX86MsrKernelGsBase];
		save.sysenterCs = state.msrs[TvmmX86MsrSysenterCs];
		save.sysenterEsp = state.msrs[TvmmX86MsrSysenterEsp];
		save.sysenterEip = state.msrs[TvmmX86MsrSysenterEip];
		save.gPat = state.msrs[TvmmX86MsrPat];
		control.tscOffset = state.msrs[TvmmX86MsrTsc] - ReadTsc();
	}
	if ((classes & TvmmX86StateIntr) != 0) {
		control.interruptShadow = state.interruptShadow & 1;
		control.eventInj = state.pendingEvent;
	}
	if ((classes & TvmmX86StateFpu) != 0) {
		memcpy(fGuestFpu, state.fpu, SvmHost::XsaveSize());
	}
	return B_OK;
}
