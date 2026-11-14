#pragma once

#include "Arch.h"
#include "KernelArea.h"
#include "Npt.h"
#include "tvmm_abi.h"

class Vm;


class SvmVm final: public ArchVm {
private:
	Vm &fVm;
	Npt fNpt;
	// every port access exits
	KernelArea fIopm;
	phys_addr_t fIopmPhys = 0;
	// every MSR access exits, but for those the VMCB holds
	KernelArea fMsrpm;
	phys_addr_t fMsrpmPhys = 0;
	TvmmCpuidEntry fCpuid[kTvmmMaxCpuidEntries];
	uint32 fCpuidCount = 0;

	void PassThroughMsr(uint32 msr);

public:
	explicit SvmVm(Vm &vm): fVm(vm) {}

	status_t Init();

	Vm &GetVm() const {return fVm;}
	phys_addr_t NptRoot() const {return fNpt.RootPhys();}
	phys_addr_t IopmPhys() const {return fIopmPhys;}
	phys_addr_t MsrpmPhys() const {return fMsrpmPhys;}

	status_t MapPages(uint64 gpa, uint64 size, const RamObject &ram, uint64 offset, uint32 flags) final;
	void UnmapPages(uint64 gpa, uint64 size) final;
	status_t CreateVcpu(Vcpu &vcpu, ArchVcpu *&arch) final;
	status_t SetCpuid(const TvmmCpuidEntry *entries, uint32 count) final;

	// nullptr when the leaf is the host's
	const TvmmCpuidEntry *CpuidEntry(uint32 leaf) const;
};
