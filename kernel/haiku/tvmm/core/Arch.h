#pragma once

#include <SupportDefs.h>

class RamObject;
class Vcpu;
class Vm;
struct TvmmCpuidEntry;
struct TvmmInfo;
struct TvmmRun;


enum class ExitAction {
	// handled in the kernel: enter the guest again
	Resume,
	// the run page describes an exit for the client
	ToClient,
};


// One vcpu's processor-specific half. Called with the vcpu's lock held.
class ArchVcpu {
public:
	virtual ~ArchVcpu() = default;

	// Before a run: finishes the exit the client has handled and takes the
	// requests from the run page.
	virtual status_t PrepareRun(TvmmRun &run) = 0;
	// Enters the guest once and handles the exit.
	virtual ExitAction EnterGuest(TvmmRun &run) = 0;
	// Ends the run without entering: a kick, a signal, or the VM closing.
	virtual void CancelRun(TvmmRun &run) = 0;

	// The architecture's state structure, TvmmX86State for x86.
	virtual size_t StateSize() const = 0;
	virtual void GetState(uint32 classes, void *state) = 0;
	virtual status_t SetState(uint32 classes, const void *state) = 0;
};


// A VM's processor-specific half: the second level address translation.
// Called with the VM's lock held.
class ArchVm {
public:
	virtual ~ArchVm() = default;

	virtual status_t MapPages(uint64 gpa, uint64 size, const RamObject &ram, uint64 offset, uint32 flags) = 0;
	// The caller flushes the guest TLBs before the pages can be reused.
	virtual void UnmapPages(uint64 gpa, uint64 size) = 0;

	virtual status_t CreateVcpu(Vcpu &vcpu, ArchVcpu *&arch) = 0;

	// x86 only
	virtual status_t SetCpuid(const TvmmCpuidEntry *entries, uint32 count) = 0;
};


// Implemented once per architecture.
namespace Arch {


// At driver load: fills the architecture fields of 'info'.
void Probe(TvmmInfo &info);
// Whether the host can run guests, as Probe() found.
status_t HostStatus();
uint32 MaxVcpus();

// Held by each VM: the processors are set up to run guests while one is.
status_t Acquire();
void Release();

status_t CreateVm(Vm &vm, ArchVm *&arch);


} // Arch
