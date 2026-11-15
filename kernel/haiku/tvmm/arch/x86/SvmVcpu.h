#pragma once

#include <KernelExport.h>

#include "Arch.h"
#include "KernelArea.h"
#include "LocalApic.h"
#include "X86Decoder.h"
#include "tvmm_abi.h"

class SvmVm;
class Vcpu;
class Vm;
struct Vmcb;


class SvmVcpu final: public ArchVcpu, private LocalApicHost {
private:
	// with a local APIC: whether the processor runs
	enum class MpState {
		Running,
		// at HLT, until an interrupt it can take
		Halted,
		// after INIT, until a STARTUP
		WaitStartup,
	};

	// an exit the client handles, which the next run finishes
	enum class Pending {
		None,
		IoIn,
		// INS: the value goes to fPendingGpa
		InsIn,
		MsrRead,
		MsrWrite,
		// a load from memory that is not RAM
		MmioLoad,
		// MOVS from memory that is not RAM into RAM at fPendingGpa
		MovsLoad,
	};

	enum class Walk {
		Ok,
		// the guest's own page fault, which is injected
		Fault,
		// the guest's page tables are not in RAM
		Error,
	};

	SvmVm &fSvmVm;
	Vm &fVm;
	Vcpu &fVcpu;
	KernelArea fVmcbArea;
	Vmcb *fVmcb = nullptr;
	phys_addr_t fVmcbPhys = 0;
	uint32 fAsid = 0;

	// the general registers the VMCB does not hold, in encoding order
	uint64 fGprs[TvmmX86GprCount] {};
	void *fGuestFpu = nullptr;
	void *fHostFpu = nullptr;
	uint64 fGuestXcr0 = 1;
	uint64 fDr[4] {};
	// guest TSC minus CPU 0's TSC; the VMCB's offset is for the CPU entered
	uint64 fTscOffset = 0;

	// the CPU and VM TLB generation the guest last entered with
	int32 fLastCpu = -1;
	int64 fTlbGeneration = 0;

	Pending fPending = Pending::None;
	uint64 fPendingNextRip = 0;
	uint64 fPendingGpa = 0;
	// INS: the elements
	uint32 fPendingCount = 0;
	// the size, string and register details of the pending access
	X86MemInsn fPendingInsn {};
	// the client asked to exit once the guest can take an interrupt
	bool fClientWindow = false;
	// what the exit being handled counts as, TvmmX86Stat*
	uint32 fExitStat = TvmmX86StatOther;
	// the APIC register it accessed, -1 for none
	int32 fExitApicReg = -1;
	bool fExitApicWrite = false;

	// The local APIC, without which HLT and the processors' start are the
	// client's. Its state is under SvmVm::ApicLock().
	LocalApic *fApic = nullptr;
	MpState fMpState = MpState::Running;
	timer fApicTimer {};
	// set from any context, taken by the vcpu's thread
	int32 fTimerExpired = 0;
	int32 fInitPending = 0;
	// the STARTUP vector, with bit 8 set while one is pending
	int32 fStartupPending = 0;
	// a level triggered vector the APIC ended, for the client
	int fEoiVector = -1;

	uint64 &Gpr(uint32 index);
	void WriteGpr(uint8 reg, uint8 size, bool highByte, uint64 value);
	uint64 ReadGpr(uint8 reg, uint8 size, bool highByte);
	uint8 *Xmm(uint8 reg);
	void WriteXmm(uint8 reg, const uint8 *data, uint8 size);

	void SetupControl();
	void ResetState();
	void ResetFpu();

	// the VM's, timed
	bool GuestIsRam(uint64 gpa, bool write);
	bool GuestRead(uint64 gpa, void *buffer, size_t size);
	bool GuestWrite(uint64 gpa, const void *buffer, size_t size);

	X86CpuMode CpuMode() const;
	uint64 Linear(int segment, uint64 offset, uint8 addressSize) const;
	Walk Translate(uint64 linear, bool write, uint64 &gpa);
	bool FetchInstruction(uint8 *bytes, uint32 &count);
	bool FetchInstructionBytes(uint8 *bytes, uint32 &count);
	// 'count' elements done; with REP the instruction runs again until RCX
	// is 0
	void AdvanceString(const X86MemInsn &insn, bool source, bool destination, uint32 count = 1);
	uint32 StringIoCount(const X86MemInsn &insn, bool in, uint64 linear);

	void InjectException(uint8 vector, bool hasError = false, uint32 error = 0);
	void FillExitState(TvmmRun &run) const;
	ExitAction ToClient(TvmmRun &run, TvmmExit reason);
	ExitAction Unhandled(TvmmRun &run);
	ExitAction MmioRead(TvmmRun &run, uint64 gpa, uint8 size);
	ExitAction MmioWrite(TvmmRun &run, uint64 gpa, uint8 size, const void *data);

	ExitAction HandleExit(TvmmRun &run);
	ExitAction HandleIo(TvmmRun &run);
	ExitAction HandleStringIo(TvmmRun &run);
	ExitAction HandleNpf(TvmmRun &run);
	ExitAction HandleMsr(TvmmRun &run);
	ExitAction HandleCr8Write(TvmmRun &run);
	void HandleCpuid();
	void HandleXsetbv();
	ExitAction HandleDr(TvmmRun &run);

	void CompleteLoad(const uint8 *data);
	bool Push(uint64 value, uint8 size);
	bool ValidXcr0(uint64 value) const;
	bool ValidFpu(const uint8 *area) const;

	// the local APIC
	ExitAction ApicBeforeEntry(TvmmRun &run);
	bool InjectApicInterrupt();
	void PullTaskPriority();
	void SyncTaskPriority();
	bool ApicMmio(const X86MemInsn &insn, uint64 gpa, TvmmRun &run, ExitAction &action);
	bool ApicMsr(uint32 msr, bool write, uint64 &value, bool &ok, bool &extIntBefore);
	ExitAction AfterApicAccess(TvmmRun &run, bool extIntBefore);
	static int32 ApicTimerHook(timer *apicTimer);

	// LocalApicHost
	void ApicInterruptChanged(bool pending) final;
	void ApicEoi(int vector) final;
	void ApicInit() final;
	void ApicStartup(int vector) final;
	void ApicTimerChanged(uint64 due) final;

public:
	SvmVcpu(SvmVm &svmVm, Vcpu &vcpu);
	~SvmVcpu() final;

	status_t Init();

	status_t PrepareRun(TvmmRun &run) final;
	ExitAction EnterGuest(TvmmRun &run) final;
	void CancelRun(TvmmRun &run) final;
	void Reset() final;

	size_t StateSize() const final {return sizeof(TvmmX86State);}
	void GetState(uint32 classes, void *state) final;
	status_t SetState(uint32 classes, const void *state) final;
};
