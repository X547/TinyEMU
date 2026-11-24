#include "RiscVVcpu.h"

#include <string.h>

#include <smp.h>
#include <util/AutoLock.h>
#include <util/atomic.h>

#include "Common.h"
#include "GStage.h"
#include "RiscVCsr.h"
#include "RiscVHost.h"
#include "RiscVSbi.h"
#include "RiscVVm.h"
#include "Vcpu.h"
#include "Vm.h"

using namespace RiscV;


RiscVVcpu::RiscVVcpu(RiscVVm &riscVVm, Vcpu &vcpu):
	fRiscVVm(riscVVm),
	fVm(riscVVm.GetVm()),
	fVcpu(vcpu),
	fControl(riscVVm.Hart(vcpu.Index())),
	fId(RiscVHost::NewVcpuId())
{
	fTimer.user_data = this;
	Reset();
}


RiscVVcpu::~RiscVVcpu()
{
	// waits for a hook running elsewhere
	cancel_timer(&fTimer);
}


//#pragma mark - timer


uint64 RiscVVcpu::GuestTime() const
{
	return ReadTime() + fRiscVVm.TimeDelta();
}


bool RiscVVcpu::TimerDue() const
{
	return fTimerDeadline != UINT64_MAX && GuestTime() >= fTimerDeadline;
}


// The SBI's set_timer, which also clears the interrupt until the deadline.
// The host's timer only wakes the vcpu; whether the interrupt is pending is
// the time counter's to say, before each entry.
void RiscVVcpu::SetTimer(uint64 deadline)
{
	cancel_timer(&fTimer);
	atomic_set(&fTimerFired, 0);
	fTimerDeadline = deadline;
	if (deadline == UINT64_MAX) {
		return;
	}
	const uint64 now = GuestTime();
	if (deadline <= now) {
		return;
	}
	const uint64 frequency = RiscVHost::TimebaseFrequency();
	const uint64 ticks = deadline - now;
	// a deadline years away, as a guest sets to have none, needs no timer
	if (ticks / frequency > (1ull << 31)) {
		return;
	}
	const uint64 us = ticks / frequency * 1000000
		+ ((ticks % frequency) * 1000000 + frequency - 1) / frequency;
	add_timer(&fTimer, TimerHook, bigtime_t(us), B_ONE_SHOT_RELATIVE_TIMER);
}


int32 RiscVVcpu::TimerHook(timer *vcpuTimer)
{
	auto *vcpu = static_cast<RiscVVcpu*>(vcpuTimer->user_data);
	atomic_set(&vcpu->fTimerFired, 1);
	vcpu->fVcpu.Wake();
	return B_HANDLED_INTERRUPT;
}


uint64 RiscVVcpu::PendingInterrupts()
{
	if (atomic_get_and_set(&fControl.softPending, 0) != 0) {
		fSoftPending = kIntVsSoft;
	}
	uint64 hvip = fSoftPending;
	if (TimerDue()) {
		hvip |= kIntVsTimer;
	}
	if (atomic_get(&fControl.externalLevel) != 0) {
		hvip |= kIntVsExternal;
	}
	return hvip;
}


// What ends a WFI: an interrupt pending that vsie enables, as vsie has
// them one bit below hvip.
bool RiscVVcpu::InterruptPending()
{
	return ((PendingInterrupts() >> 1) & fCsrs.vsie) != 0;
}


//#pragma mark - entry


void RiscVVcpu::LoadFpu()
{
	if (fFpuLoaded) {
		return;
	}
	tvmm_riscv_save_fpu(&fHostFpu);
	tvmm_riscv_load_fpu(&fGuestFpu);
	fFpuLoaded = true;
}


void RiscVVcpu::UnloadFpu()
{
	if (!fFpuLoaded) {
		return;
	}
	tvmm_riscv_save_fpu(&fGuestFpu);
	tvmm_riscv_load_fpu(&fHostFpu);
	fFpuLoaded = false;
}


// hideleg goes before vsie, whose bits it makes writable.
void RiscVVcpu::LoadCsrs(uint64 hvip)
{
	TVMM_CSR_WRITE(TVMM_CSR_HEDELEG, kGuestExceptions);
	TVMM_CSR_WRITE(TVMM_CSR_HIDELEG, kIntVsAll);
	TVMM_CSR_WRITE(TVMM_CSR_HCOUNTEREN, kCounterenBase);
	TVMM_CSR_WRITE(TVMM_CSR_HENVCFG, RiscVHost::Henvcfg());
	TVMM_CSR_WRITE(TVMM_CSR_HTIMEDELTA, fRiscVVm.TimeDelta());
	TVMM_CSR_WRITE(TVMM_CSR_HGATP, (RiscVHost::HgatpMode() << kHgatpModeShift)
		| (fRiscVVm.GetGStage().RootPhys() >> 12));

	TVMM_CSR_WRITE(TVMM_CSR_VSSTATUS, fCsrs.vsstatus);
	TVMM_CSR_WRITE(TVMM_CSR_VSIE, fCsrs.vsie);
	TVMM_CSR_WRITE(TVMM_CSR_VSTVEC, fCsrs.vstvec);
	TVMM_CSR_WRITE(TVMM_CSR_VSSCRATCH, fCsrs.vsscratch);
	TVMM_CSR_WRITE(TVMM_CSR_VSEPC, fCsrs.vsepc);
	TVMM_CSR_WRITE(TVMM_CSR_VSCAUSE, fCsrs.vscause);
	TVMM_CSR_WRITE(TVMM_CSR_VSTVAL, fCsrs.vstval);
	TVMM_CSR_WRITE(TVMM_CSR_VSATP, fCsrs.vsatp);
	TVMM_CSR_WRITE(TVMM_CSR_HVIP, hvip);
}


void RiscVVcpu::SaveCsrs()
{
	fCsrs.vsstatus = TVMM_CSR_READ(TVMM_CSR_VSSTATUS);
	fCsrs.vsie = TVMM_CSR_READ(TVMM_CSR_VSIE);
	fCsrs.vstvec = TVMM_CSR_READ(TVMM_CSR_VSTVEC);
	fCsrs.vsscratch = TVMM_CSR_READ(TVMM_CSR_VSSCRATCH);
	fCsrs.vsepc = TVMM_CSR_READ(TVMM_CSR_VSEPC);
	fCsrs.vscause = TVMM_CSR_READ(TVMM_CSR_VSCAUSE);
	fCsrs.vstval = TVMM_CSR_READ(TVMM_CSR_VSTVAL);
	fCsrs.vsatp = TVMM_CSR_READ(TVMM_CSR_VSATP);
	// the guest clears its software interrupt itself
	fSoftPending = TVMM_CSR_READ(TVMM_CSR_HVIP) & kIntVsSoft;
}


// The instruction of an MMIO access the hart did not transform into htinst,
// or of a virtual instruction exception without it in stval. Read while the
// guest's translation is still in the CSRs, as the guest has it.
void RiscVVcpu::ReadExitInsn()
{
	fExitInsnValid = false;
	bool needed = false;
	switch (fContext.scause) {
		case kCauseLoadGuestPageFault:
		case kCauseStoreGuestPageFault:
			needed = (fContext.htinst & 1) == 0;
			break;
		case kCauseVirtualInsn:
			if (fContext.stval != 0) {
				fExitInsn = uint32(fContext.stval);
				fExitInsnValid = true;
				return;
			}
			needed = true;
			break;
		default:
			break;
	}
	if (!needed) {
		return;
	}
	const uint64 hstatus = TVMM_CSR_READ(TVMM_CSR_HSTATUS);
	TVMM_CSR_WRITE(TVMM_CSR_HSTATUS, fMode == TvmmRiscVModeSupervisor
		? hstatus | kHstatusSpvp : hstatus & ~kHstatusSpvp);
	RiscVTrap trap {};
	const uint32 insn = tvmm_riscv_read_insn(fContext.pc, &trap);
	TVMM_CSR_WRITE(TVMM_CSR_HSTATUS, hstatus);
	if (!trap.taken) {
		fExitInsn = insn;
		fExitInsnValid = true;
	}
}


void RiscVVcpu::SetReg(uint32 index, uint64 value)
{
	if (index != 0) {
		fContext.x[index] = value;
	}
}


// An exception the guest takes as if the hart had raised it in VS-mode.
void RiscVVcpu::RedirectTrap(uint64 cause, uint64 tval)
{
	fCsrs.vsepc = fContext.pc;
	fCsrs.vscause = cause;
	fCsrs.vstval = tval;
	uint64 status = fCsrs.vsstatus & ~(kStatusSpp | kStatusSpie);
	if (fMode == TvmmRiscVModeSupervisor) {
		status |= kStatusSpp;
	}
	if ((fCsrs.vsstatus & kStatusSie) != 0) {
		status |= kStatusSpie;
	}
	fCsrs.vsstatus = status & ~kStatusSie;
	fMode = TvmmRiscVModeSupervisor;
	fContext.pc = fCsrs.vstvec & ~3ull;
}


ExitAction RiscVVcpu::ToClient(TvmmRun &run, TvmmExit reason)
{
	UnloadFpu();
	run.exitReason = reason;
	return ExitAction::ToClient;
}


ExitAction RiscVVcpu::Unhandled(TvmmRun &run)
{
	fExitStat = TvmmRiscVStatOther;
	TvmmExitUnhandled &exit = run.exit.unhandled;
	memset(&exit, 0, sizeof(exit));
	exit.code = fContext.scause;
	exit.info1 = fContext.stval;
	exit.info2 = fContext.htval << 2;
	exit.rip = fContext.pc;
	if (fExitInsnValid) {
		exit.insnLength = (fExitInsn & 3) == 3 ? 4 : 2;
		memcpy(exit.insnBytes, &fExitInsn, exit.insnLength);
	}
	return ToClient(run, TvmmExit::Unhandled);
}


// A run that cannot enter until something wakes the vcpu, which a kick ends.
ExitAction RiscVVcpu::Idle(TvmmRun &run)
{
	if (fVcpu.TakeKick()) {
		CancelRun(run);
		return ExitAction::ToClient;
	}
	return ExitAction::Wait;
}


ExitAction RiscVVcpu::BeforeEntry(TvmmRun &run)
{
	bool stopped;
	{
		InterruptsSpinLocker locker(fControl.lock);
		// what the SBI's hart_start leaves defined, and the FPU on, as
		// firmware leaves it
		if (fControl.state == TvmmRiscVHartStartPending) {
			fContext.pc = fControl.startPc;
			fContext.x[10] = fVcpu.Index();
			fContext.x[11] = fControl.startOpaque;
			fMode = TvmmRiscVModeSupervisor;
			fCsrs.vsstatus &= ~kStatusSie;
			if ((fCsrs.vsstatus & kStatusFsMask) == 0) {
				fCsrs.vsstatus |= kStatusFsInitial;
			}
			fCsrs.vsatp = 0;
			fInWfi = false;
			fControl.state = TvmmRiscVHartStarted;
		}
		stopped = fControl.state == TvmmRiscVHartStopped;
	}
	if (stopped) {
		return Idle(run);
	}
	// a timer that went off early goes again
	if (atomic_get_and_set(&fTimerFired, 0) != 0 && fTimerDeadline != UINT64_MAX && !TimerDue()) {
		SetTimer(fTimerDeadline);
	}
	if (fInWfi) {
		if (!InterruptPending()) {
			return Idle(run);
		}
		fInWfi = false;
	}
	return ExitAction::Resume;
}


status_t RiscVVcpu::PrepareRun(TvmmRun &run)
{
	switch (fPending) {
		case Pending::None:
			break;
		case Pending::MmioLoad: {
			uint8 data[8];
			memcpy(data, run.exit.mmio.data, sizeof(data));
			uint64 value = 0;
			memcpy(&value, data, fPendingInsn.size);
			if (fPendingInsn.signExtend) {
				const uint32 shift = 64 - 8 * fPendingInsn.size;
				value = uint64(int64(value << shift) >> shift);
			}
			SetReg(fPendingInsn.reg, value);
			fContext.pc += fPendingInsn.length;
			break;
		}
		case Pending::Sbi:
			SetReg(10, ReadOnce(run.exit.sbi.a[0]));
			SetReg(11, ReadOnce(run.exit.sbi.a[1]));
			break;
		case Pending::LegacySbi:
			SetReg(10, ReadOnce(run.exit.sbi.a[0]));
			break;
	}
	fPending = Pending::None;
	run.exitReason = TvmmExit::None;
	return B_OK;
}


void RiscVVcpu::CancelRun(TvmmRun &run)
{
	ToClient(run, TvmmExit::Canceled);
}


ExitAction RiscVVcpu::EnterGuest(TvmmRun &run)
{
	const ExitAction before = BeforeEntry(run);
	if (before != ExitAction::Resume) {
		return before;
	}

	const cpu_status state = disable_interrupts();
	const int32 cpu = smp_get_current_cpu();
	int64 generation = 0;
	if (!fVcpu.BeginGuestEntry(cpu, generation)) {
		restore_interrupts(state);
		CancelRun(run);
		return ExitAction::ToClient;
	}

	LoadFpu();
	// After BeginGuestEntry(): what comes from now on interrupts the guest,
	// so none waits for the next exit.
	LoadCsrs(PendingInterrupts());
	const uint64 hostScounteren = TVMM_CSR_READ(TVMM_CSR_SCOUNTEREN);
	const uint64 hostSenvcfg = TVMM_CSR_READ(TVMM_CSR_SENVCFG);
	TVMM_CSR_WRITE(TVMM_CSR_SCOUNTEREN, fCsrs.scounteren);
	TVMM_CSR_WRITE(TVMM_CSR_SENVCFG, fCsrs.senvcfg);

	// The guests share VMID 0: a CPU that ran another vcpu, or this one
	// before the VM's mappings changed, forgets their translations, and
	// instructions another CPU may have written.
	if (cpu != fLastCpu || generation != fTlbGeneration || RiscVHost::LoadedVcpu(cpu) != fId) {
		tvmm_riscv_hfence_all();
		asm volatile("fence.i" ::: "memory");
		RiscVHost::SetLoadedVcpu(cpu, fId);
		fLastCpu = cpu;
		fTlbGeneration = generation;
	}

	const bool supervisor = fMode == TvmmRiscVModeSupervisor;
	const uint64 hostSstatus = TVMM_CSR_READ(TVMM_CSR_SSTATUS);
	const uint64 hostHstatus = TVMM_CSR_READ(TVMM_CSR_HSTATUS);
	fContext.sstatus = (hostSstatus & ~(kStatusSpp | kStatusSpie | kStatusSie))
		| (supervisor ? kStatusSpp : 0);
	fContext.hstatus = (hostHstatus & ~(kHstatusGva | kHstatusSpvp | kHstatusHu
			| kHstatusVgeinMask | kHstatusVtvm | kHstatusVtw | kHstatusVtsr | kHstatusVsxlMask))
		| kHstatusSpv | (supervisor ? kHstatusSpvp : 0) | kHstatusVtw
		| (kXlen64 << kHstatusVsxlShift);

	const uint64 entered = ReadTime();
	tvmm_riscv_run(&fContext);
	const uint64 exited = ReadTime();

	fMode = (fContext.sstatus & kStatusSpp) != 0 ? TvmmRiscVModeSupervisor : TvmmRiscVModeUser;
	SaveCsrs();
	ReadExitInsn();
	// the guest writes these, which have no VS copies, as they are
	fCsrs.scounteren = TVMM_CSR_READ(TVMM_CSR_SCOUNTEREN);
	fCsrs.senvcfg = TVMM_CSR_READ(TVMM_CSR_SENVCFG);
	TVMM_CSR_WRITE(TVMM_CSR_SCOUNTEREN, hostScounteren);
	TVMM_CSR_WRITE(TVMM_CSR_SENVCFG, hostSenvcfg);
	fVcpu.EndGuestEntry();
	restore_interrupts(state);

	TvmmVcpuStats &stats = fVcpu.Stats();
	AddStat(stats.entries, ElapsedCycles(entered, exited));
	const ExitAction action = HandleExit(run);
	AddStat(stats.archExits[fExitStat], ElapsedCycles(exited, ReadTime()));
	return action;
}


//#pragma mark - exits


ExitAction RiscVVcpu::HandleExit(TvmmRun &run)
{
	fExitStat = TvmmRiscVStatOther;
	const uint64 cause = fContext.scause;
	if ((cause & kCauseInterrupt) != 0) {
		// the host takes it once interrupts are enabled
		fExitStat = TvmmRiscVStatIntr;
		return ExitAction::Resume;
	}
	switch (cause) {
		case kCauseVsEcall:
			return HandleSbi(run);
		case kCauseInsnGuestPageFault:
		case kCauseLoadGuestPageFault:
		case kCauseStoreGuestPageFault:
			return HandleGuestPageFault(run);
		case kCauseVirtualInsn:
			return HandleVirtualInsn(run);
		default:
			return Unhandled(run);
	}
}


ExitAction RiscVVcpu::HandleGuestPageFault(TvmmRun &run)
{
	const uint64 cause = fContext.scause;
	const uint64 gpa = (fContext.htval << 2) | (fContext.stval & 3);
	const bool write = cause == kCauseStoreGuestPageFault;
	// RAM mapped since, or a page the dirty log took W from
	if (fVm.IsRam(gpa, write)) {
		if (write) {
			fRiscVVm.GetGStage().SetWritable(gpa);
		}
		fExitStat = TvmmRiscVStatGpfRam;
		return ExitAction::Resume;
	}
	if (cause == kCauseInsnGuestPageFault) {
		return Unhandled(run);
	}

	RiscVMemInsn insn;
	if (!RiscVDecodeTransformedInsn(uint32(fContext.htinst), insn)
		&& !(fExitInsnValid && RiscVDecodeMemInsn(fExitInsn, insn))) {
		return Unhandled(run);
	}
	// an atomic or a floating point access
	if (insn.store != write) {
		return Unhandled(run);
	}
	fExitStat = TvmmRiscVStatMmio;
	TvmmExitMmio &exit = run.exit.mmio;
	exit.gpa = gpa;
	exit.size = insn.size;
	exit.write = write;
	memset(exit.data, 0, sizeof(exit.data));
	if (write) {
		const uint64 value = Reg(insn.reg);
		memcpy(exit.data, &value, insn.size);
		fContext.pc += insn.length;
	} else {
		fPending = Pending::MmioLoad;
		fPendingInsn = insn;
	}
	return ToClient(run, TvmmExit::Mmio);
}


// WFI, which VTW makes trap here, waits in the kernel. Anything else the
// guest may not do: the guest finds it illegal.
ExitAction RiscVVcpu::HandleVirtualInsn(TvmmRun &run)
{
	if (!fExitInsnValid) {
		return Unhandled(run);
	}
	if (fExitInsn == kInsnWfi) {
		fContext.pc += 4;
		fInWfi = true;
		fExitStat = TvmmRiscVStatWfi;
		return ExitAction::Resume;
	}
	fExitStat = TvmmRiscVStatVirtualInsn;
	RedirectTrap(kCauseIllegalInsn, fExitInsn);
	return ExitAction::Resume;
}


//#pragma mark - SBI


bool RiscVVcpu::HartSet(uint64 mask, uint64 base, uint64 set[4])
{
	memset(set, 0, 4 * sizeof(uint64));
	const uint64 count = fRiscVVm.HartCount();
	if (base == UINT64_MAX) {
		for (uint64 hart = 0; hart < count; hart++) {
			set[hart / 64] |= 1ull << (hart % 64);
		}
		return true;
	}
	for (uint64 i = 0; i < 64; i++) {
		if (((mask >> i) & 1) == 0) {
			continue;
		}
		if (base >= count || i >= count - base) {
			return false;
		}
		const uint64 hart = base + i;
		set[hart / 64] |= 1ull << (hart % 64);
	}
	return true;
}


void RiscVVcpu::Stop()
{
	InterruptsSpinLocker locker(fControl.lock);
	fControl.state = TvmmRiscVHartStopped;
	fInWfi = false;
}


ExitAction RiscVVcpu::SbiToClient(TvmmRun &run, bool legacy)
{
	fExitStat = TvmmRiscVStatSbiClient;
	for (uint32 i = 0; i < 8; i++) {
		run.exit.sbi.a[i] = Reg(10 + i);
	}
	fPending = legacy ? Pending::LegacySbi : Pending::Sbi;
	return ToClient(run, TvmmExit::Sbi);
}


ExitAction RiscVVcpu::SbiReset(TvmmRun &run, uint64 type, uint64 reason, int64 &error)
{
	if ((reason != Sbi::kSrstReasonNone && reason != Sbi::kSrstReasonFailure)
		|| (type != Sbi::kSrstShutdown && type != Sbi::kSrstColdReboot && type != Sbi::kSrstWarmReboot)) {
		error = Sbi::kErrInvalidParam;
		return ExitAction::Resume;
	}
	// the call does not return
	Stop();
	SetReg(10, Sbi::kSuccess);
	run.exit.systemEvent.type = uint32(type);
	run.exit.systemEvent.reason = uint32(reason);
	return ToClient(run, TvmmExit::SystemEvent);
}


int64 RiscVVcpu::SbiBase(uint32 fid, uint64 &value)
{
	switch (fid) {
		case 0:
			value = Sbi::kSpecVersion;
			break;
		case 1:
			value = Sbi::kImplId;
			break;
		case 2:
			value = Sbi::kImplVersion;
			break;
		case 3: {
			const uint64 extension = Reg(10);
			switch (extension) {
				case Sbi::kExtBase:
				case Sbi::kExtTime:
				case Sbi::kExtIpi:
				case Sbi::kExtRfence:
				case Sbi::kExtHsm:
				case Sbi::kExtSrst:
				case Sbi::kExtDbcn:
					value = 1;
					break;
				default:
					value = extension <= Sbi::kExtLegacyLast && extension != Sbi::kLegacySendIpi;
					break;
			}
			break;
		}
		case 4:
		case 5:
		case 6:
			// mvendorid, marchid and mimpid, which the guest has none of
			value = 0;
			break;
		default:
			return Sbi::kErrNotSupported;
	}
	return Sbi::kSuccess;
}


int64 RiscVVcpu::SbiHsm(uint32 fid, uint64 &value, bool &stop)
{
	const uint64 hart = Reg(10);
	switch (fid) {
		case 0:
			if (hart >= Vm::kMaxVcpus) {
				return Sbi::kErrInvalidParam;
			}
			return fRiscVVm.StartHart(uint32(hart), Reg(11), Reg(12));
		case 1:
			stop = true;
			return Sbi::kSuccess;
		case 2:
			if (hart >= Vm::kMaxVcpus) {
				return Sbi::kErrInvalidParam;
			}
			return fRiscVVm.HartStatus(uint32(hart), value);
		case 3:
			// a retentive suspend is a WFI; the call returns once it ends
			if (Reg(10) == Sbi::kHsmSuspendRetentive) {
				fInWfi = true;
				return Sbi::kSuccess;
			}
			return (Reg(10) & 0x80000000) != 0 ? Sbi::kErrNotSupported : Sbi::kErrInvalidParam;
		default:
			return Sbi::kErrNotSupported;
	}
}


// The v0.1 calls return in a0 and keep a1. Those that take a hart mask take
// its address, which only the hart's own translation can read: IPIs are not
// done, and the fences fence every hart.
ExitAction RiscVVcpu::SbiLegacy(TvmmRun &run, uint32 eid)
{
	int64 error = Sbi::kSuccess;
	switch (eid) {
		case Sbi::kLegacySetTimer:
			SetTimer(Reg(10));
			break;
		case Sbi::kLegacyPutchar:
		case Sbi::kLegacyGetchar:
			return SbiToClient(run, true);
		case Sbi::kLegacyClearIpi:
			fSoftPending = 0;
			atomic_set(&fControl.softPending, 0);
			break;
		case Sbi::kLegacyFenceI:
		case Sbi::kLegacySfenceVma:
		case Sbi::kLegacySfenceAsid:
			fVm.FlushGuestTlbs();
			break;
		case Sbi::kLegacyShutdown:
			return SbiReset(run, Sbi::kSrstShutdown, Sbi::kSrstReasonNone, error);
		default:
			error = Sbi::kErrNotSupported;
			break;
	}
	SetReg(10, error);
	return ExitAction::Resume;
}


ExitAction RiscVVcpu::HandleSbi(TvmmRun &run)
{
	fContext.pc += 4;
	fExitStat = TvmmRiscVStatSbi;
	const uint64 eid = Reg(17);
	const uint32 fid = uint32(Reg(16));
	if (eid <= Sbi::kExtLegacyLast) {
		return SbiLegacy(run, uint32(eid));
	}

	int64 error = Sbi::kErrNotSupported;
	uint64 value = 0;
	uint64 set[4];
	switch (eid) {
		case Sbi::kExtBase:
			error = SbiBase(fid, value);
			break;
		case Sbi::kExtTime:
			if (fid == 0) {
				SetTimer(Reg(10));
				error = Sbi::kSuccess;
			}
			break;
		case Sbi::kExtIpi:
			if (fid != 0) {
				break;
			}
			if (!HartSet(Reg(10), Reg(11), set)) {
				error = Sbi::kErrInvalidParam;
				break;
			}
			for (uint32 hart = 0; hart < Vm::kMaxVcpus; hart++) {
				if (((set[hart / 64] >> (hart % 64)) & 1) != 0) {
					fRiscVVm.SendIpi(hart);
				}
			}
			error = Sbi::kSuccess;
			break;
		case Sbi::kExtRfence:
			// FENCE.I, SFENCE.VMA and SFENCE.VMA with an ASID; the guest has
			// no H for the others
			if (fid > 2) {
				break;
			}
			if (!HartSet(Reg(10), Reg(11), set)) {
				error = Sbi::kErrInvalidParam;
				break;
			}
			fVm.FlushGuestTlbs();
			error = Sbi::kSuccess;
			break;
		case Sbi::kExtHsm: {
			bool stop = false;
			error = SbiHsm(fid, value, stop);
			if (stop) {
				Stop();
				return ExitAction::Resume;
			}
			break;
		}
		case Sbi::kExtSrst:
			if (fid == 0) {
				const ExitAction action = SbiReset(run, Reg(10), Reg(11), error);
				if (action != ExitAction::Resume) {
					return action;
				}
			}
			break;
		default:
			// the debug console, and what the client may know more of
			return SbiToClient(run, false);
	}
	SetReg(10, uint64(error));
	SetReg(11, value);
	return ExitAction::Resume;
}


//#pragma mark - state


void RiscVVcpu::Reset()
{
	cancel_timer(&fTimer);
	atomic_set(&fTimerFired, 0);
	fTimerDeadline = UINT64_MAX;
	fContext = {};
	fMode = TvmmRiscVModeSupervisor;
	fCsrs = {};
	// as firmware leaves it for the supervisor
	fCsrs.vsstatus = kStatusFsInitial;
	fSoftPending = 0;
	fGuestFpu = {};
	fInWfi = false;
	fLastCpu = -1;
	fPending = Pending::None;
	{
		InterruptsSpinLocker locker(fControl.lock);
		fControl.state = TvmmRiscVHartStopped;
	}
	atomic_set(&fControl.softPending, 0);
}


void RiscVVcpu::GetState(uint32 classes, void *buffer)
{
	auto &state = *static_cast<TvmmRiscVState*>(buffer);
	if ((classes & TvmmRiscVStateCore) != 0) {
		state.x[0] = 0;
		for (uint32 i = 1; i < 32; i++) {
			state.x[i] = fContext.x[i];
		}
		state.pc = fContext.pc;
		state.mode = fMode;
	}
	if ((classes & TvmmRiscVStateCsrs) != 0) {
		state.sstatus = fCsrs.vsstatus;
		state.sie = fCsrs.vsie;
		state.stvec = fCsrs.vstvec;
		state.sscratch = fCsrs.vsscratch;
		state.sepc = fCsrs.vsepc;
		state.scause = fCsrs.vscause;
		state.stval = fCsrs.vstval;
		state.satp = fCsrs.vsatp;
		state.scounteren = fCsrs.scounteren;
		state.senvcfg = fCsrs.senvcfg;
		state.sip = PendingInterrupts() >> 1;
	}
	if ((classes & TvmmRiscVStateFp) != 0) {
		memcpy(state.f, fGuestFpu.f, sizeof(state.f));
		state.fcsr = fGuestFpu.fcsr;
	}
	if ((classes & TvmmRiscVStateTimer) != 0) {
		state.timerDeadline = fTimerDeadline;
		state.timeDelta = fRiscVVm.TimeDelta();
	}
	if ((classes & TvmmRiscVStateMp) != 0) {
		InterruptsSpinLocker locker(fControl.lock);
		state.mpState = uint32(fControl.state);
	}
}


status_t RiscVVcpu::SetState(uint32 classes, const void *buffer)
{
	const auto &state = *static_cast<const TvmmRiscVState*>(buffer);
	if ((classes & ~uint32(TvmmRiscVStateAll)) != 0) {
		return B_BAD_VALUE;
	}
	if ((classes & TvmmRiscVStateCore) != 0
		&& state.mode != TvmmRiscVModeUser && state.mode != TvmmRiscVModeSupervisor) {
		return B_BAD_VALUE;
	}
	if ((classes & TvmmRiscVStateMp) != 0
		&& state.mpState != TvmmRiscVHartStarted && state.mpState != TvmmRiscVHartStopped) {
		return B_BAD_VALUE;
	}

	if ((classes & TvmmRiscVStateCore) != 0) {
		for (uint32 i = 1; i < 32; i++) {
			fContext.x[i] = state.x[i];
		}
		fContext.pc = state.pc;
		fMode = state.mode;
		fPending = Pending::None;
	}
	if ((classes & TvmmRiscVStateCsrs) != 0) {
		fCsrs.vsstatus = state.sstatus;
		fCsrs.vsie = state.sie;
		fCsrs.vstvec = state.stvec;
		fCsrs.vsscratch = state.sscratch;
		fCsrs.vsepc = state.sepc;
		fCsrs.vscause = state.scause;
		fCsrs.vstval = state.stval;
		fCsrs.vsatp = state.satp;
		fCsrs.scounteren = state.scounteren;
		fCsrs.senvcfg = state.senvcfg;
		// the others are what the timer and the client drive
		fSoftPending = (state.sip << 1) & kIntVsSoft;
	}
	if ((classes & TvmmRiscVStateFp) != 0) {
		memcpy(fGuestFpu.f, state.f, sizeof(fGuestFpu.f));
		fGuestFpu.fcsr = state.fcsr;
	}
	if ((classes & TvmmRiscVStateTimer) != 0) {
		SetTimer(state.timerDeadline);
	}
	if ((classes & TvmmRiscVStateMp) != 0) {
		InterruptsSpinLocker locker(fControl.lock);
		fControl.state = int32(state.mpState);
		fInWfi = false;
	}
	return B_OK;
}
