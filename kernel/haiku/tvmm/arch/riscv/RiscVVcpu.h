#pragma once

#include <KernelExport.h>

#include "Arch.h"
#include "RiscVContext.h"
#include "RiscVDecoder.h"
#include "tvmm_abi.h"

class RiscVVm;
class Vcpu;
class Vm;
struct RiscVHartControl;


// A guest hart, run in VS- and VU-mode. The kernel answers the SBI but for
// what only the client has, waits at WFI, and keeps the timer; the client
// drives the external interrupt.
class RiscVVcpu final: public ArchVcpu {
private:
	// an exit the client handles, which the next run finishes
	enum class Pending {
		None,
		MmioLoad,
		Sbi,
		LegacySbi,
	};

	// the guest's supervisor CSRs while it is not entered
	struct GuestCsrs {
		uint64 vsstatus;
		uint64 vsie;
		uint64 vstvec;
		uint64 vsscratch;
		uint64 vsepc;
		uint64 vscause;
		uint64 vstval;
		uint64 vsatp;
		uint64 scounteren;
		uint64 senvcfg;
	};

	RiscVVm &fRiscVVm;
	Vm &fVm;
	Vcpu &fVcpu;
	RiscVHartControl &fControl;
	// for the CPUs to tell whose translations they hold
	int64 fId;

	RiscVContext fContext {};
	// TvmmRiscVMode*
	uint64 fMode = TvmmRiscVModeSupervisor;
	GuestCsrs fCsrs {};
	// the software interrupt the guest has pending, as hvip has it
	uint64 fSoftPending = 0;

	// The guest's FPU state is in the registers from the first entry of a
	// run until the run returns; the client's waits here meanwhile.
	RiscVFpu fGuestFpu {};
	RiscVFpu fHostFpu {};
	bool fFpuLoaded = false;

	// on the guest's time counter, UINT64_MAX for none
	uint64 fTimerDeadline = UINT64_MAX;
	timer fTimer {};
	// the timer went off, maybe before the deadline as the time counter has
	// it; set from any context
	int32 fTimerFired = 0;

	// waits at WFI until an interrupt it has enabled is pending
	bool fInWfi = false;

	// the CPU and VM TLB generation the guest last entered with
	int32 fLastCpu = -1;
	int64 fTlbGeneration = 0;

	// the instruction of the last exit, when it took one to know
	uint32 fExitInsn = 0;
	bool fExitInsnValid = false;

	Pending fPending = Pending::None;
	RiscVMemInsn fPendingInsn {};
	// TvmmRiscVStat*
	uint32 fExitStat = TvmmRiscVStatOther;

	uint64 GuestTime() const;
	bool TimerDue() const;
	void SetTimer(uint64 deadline);
	static int32 TimerHook(timer *vcpuTimer);
	// the VS interrupts pending, as hvip has them
	uint64 PendingInterrupts();
	bool InterruptPending();

	void LoadFpu();
	void UnloadFpu();
	void LoadCsrs(uint64 hvip);
	void SaveCsrs();
	void ReadExitInsn();

	uint64 Reg(uint32 index) const {return index == 0 ? 0 : fContext.x[index];}
	void SetReg(uint32 index, uint64 value);
	void RedirectTrap(uint64 cause, uint64 tval);

	ExitAction ToClient(TvmmRun &run, TvmmExit reason);
	ExitAction Unhandled(TvmmRun &run);
	ExitAction Idle(TvmmRun &run);
	ExitAction BeforeEntry(TvmmRun &run);
	ExitAction HandleExit(TvmmRun &run);
	ExitAction HandleGuestPageFault(TvmmRun &run);
	ExitAction HandleVirtualInsn(TvmmRun &run);

	// the SBI
	ExitAction HandleSbi(TvmmRun &run);
	ExitAction SbiLegacy(TvmmRun &run, uint32 eid);
	ExitAction SbiToClient(TvmmRun &run, bool legacy);
	ExitAction SbiReset(TvmmRun &run, uint64 type, uint64 reason, int64 &error);
	int64 SbiBase(uint32 fid, uint64 &value);
	int64 SbiHsm(uint32 fid, uint64 &value, bool &stop);
	// The harts 'mask' from 'base' names, as a bitmap of Vm::kMaxVcpus.
	bool HartSet(uint64 mask, uint64 base, uint64 set[4]);
	void Stop();

public:
	RiscVVcpu(RiscVVm &riscVVm, Vcpu &vcpu);
	~RiscVVcpu() final;

	status_t PrepareRun(TvmmRun &run) final;
	ExitAction EnterGuest(TvmmRun &run) final;
	void CancelRun(TvmmRun &run) final;
	void Reset() final;

	size_t StateSize() const final {return sizeof(TvmmRiscVState);}
	void GetState(uint32 classes, void *state) final;
	status_t SetState(uint32 classes, const void *state) final;
};
