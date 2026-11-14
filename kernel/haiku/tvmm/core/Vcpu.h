#pragma once

#include <SupportDefs.h>

#include <lock.h>

#include "KernelArea.h"

class ArchVcpu;
class Vm;
struct TvmmRun;
struct TvmmStateOp;


// One virtual processor. Its client runs it on a thread of its own with
// TvmmOpRun; any thread may kick it out of a run.
class Vcpu {
private:
	Vm &fVm;
	uint32 fIndex;
	mutex fLock;
	KernelArea fRunArea;
	TvmmRun *fRun = nullptr;
	ArchVcpu *fArch = nullptr;
	// where the state is staged between the client and the processor
	void *fStateBuffer = nullptr;

	// the CPU the guest runs on, -1 outside the guest
	int32 fGuestCpu = -1;
	int32 fKickPending = 0;
	// the VM's TLB generation the guest last entered with
	int64 fEntryTlbGeneration = 0;

public:
	Vcpu(Vm &vm, uint32 index);
	~Vcpu();

	status_t Init(team_id team, uint64 &runAddress);

	Vm &GetVm() const {return fVm;}
	uint32 Index() const {return fIndex;}

	status_t Run();
	// Makes a run return soon with TvmmExit::Canceled, or the next one if
	// none is under way. Any thread.
	void Kick();

	status_t GetState(const TvmmStateOp &op);
	status_t SetState(const TvmmStateOp &op);

	// Waits until the guest no longer runs with a TLB generation older than
	// 'generation'. Any thread.
	void SyncTlbGeneration(int64 generation);

	// For the architecture's entry, with interrupts disabled: false when a kick
	// is pending, which it consumes. Otherwise returns the TLB generation to
	// enter with.
	bool BeginGuestEntry(int32 cpu, int64 &tlbGeneration);
	void EndGuestEntry();
};
