#pragma once

#include <KernelExport.h>

#include "Arch.h"
#include "GStage.h"
#include "Vm.h"
#include "tvmm_abi.h"

class Vcpu;


// What others ask of a hart, which it takes before it next enters.
struct RiscVHartControl {
	spinlock lock = B_SPINLOCK_INITIALIZER;
	// TvmmRiscVHart*, under the lock
	int32 state = TvmmRiscVHartStopped;
	// for TvmmRiscVHartStartPending, under the lock
	uint64 startPc = 0;
	uint64 startOpaque = 0;
	// an IPI came
	int32 softPending = 0;
	// the client's level of the supervisor external interrupt
	int32 externalLevel = 0;
};


class RiscVVm final: public ArchVm {
private:
	Vm &fVm;
	GStage fGStage;
	// the guest's time counter less the host's, which starts it at 0
	uint64 fTimeDelta = 0;
	RiscVHartControl fHarts[Vm::kMaxVcpus];

public:
	explicit RiscVVm(Vm &vm): fVm(vm) {}

	status_t Init();

	Vm &GetVm() const {return fVm;}
	const GStage &GetGStage() const {return fGStage;}
	GStage &GetGStage() {return fGStage;}
	uint64 TimeDelta() const {return fTimeDelta;}
	RiscVHartControl &Hart(uint32 index) {return fHarts[index];}

	// The harts, numbered from 0 up to the first vcpu not created.
	uint32 HartCount();
	void SendIpi(uint32 hart);
	// SBI HSM: the result of hart_start and hart_get_status
	int64 StartHart(uint32 hart, uint64 pc, uint64 opaque);
	int64 HartStatus(uint32 hart, uint64 &status);

	status_t MapPages(uint64 gpa, uint64 size, const RamObject &ram, uint64 offset, uint32 flags) final;
	void UnmapPages(uint64 gpa, uint64 size) final;
	bool TakeDirty(uint64 gpa, uint64 size, uint64 *bitmap) final;
	status_t CreateVcpu(Vcpu &vcpu, ArchVcpu *&arch) final;

	status_t SetCpuid(const TvmmCpuidEntry *entries, uint32 count) final;
	status_t SendMsi(uint64 address, uint32 data) final;
	status_t SetIrqLine(uint32 pin, bool level) final;
	status_t ResetIoApic() final;
};
