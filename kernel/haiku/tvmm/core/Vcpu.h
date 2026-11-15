#pragma once

#include <SupportDefs.h>

#include <condition_variable.h>
#include <lock.h>

#include "KernelArea.h"
#include "tvmm_abi.h"

class ArchVcpu;
class Vm;


// The statistics have one writer, the vcpu's run, and readers on any thread:
// each value is stored and loaded whole.
inline void AddStatCount(TvmmStat &stat)
{
	__atomic_store_n(&stat.count, stat.count + 1, __ATOMIC_RELAXED);
}


inline void AddStatCycles(TvmmStat &stat, uint64 cycles)
{
	__atomic_store_n(&stat.cycles, stat.cycles + cycles, __ATOMIC_RELAXED);
}


inline void AddStat(TvmmStat &stat, uint64 cycles)
{
	AddStatCount(stat);
	AddStatCycles(stat, cycles);
}


// The cycles from 'start' to 'end', none when a CPU change put them out of
// order.
inline uint64 ElapsedCycles(uint64 start, uint64 end)
{
	return end > start ? end - start : 0;
}


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
	// something for the vcpu to look at came since its run last did
	int32 fWakePending = 0;
	ConditionVariable fWakeCondition;

	TvmmVcpuStats fStats {};
	// when the last run returned to the client, 0 before the first, and with
	// which exit
	uint64 fClientSince = 0;
	uint32 fClientExit = 0;

	status_t RunLoop();
	bool WaitForWake();

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
	// Takes a pending kick, for a run that ends without entering the guest.
	bool TakeKick();
	// Makes the run look at the vcpu's state again: the guest leaves, or the
	// wait the architecture asked for ends. Any context, interrupts included.
	void Wake();

	status_t GetState(const TvmmStateOp &op);
	status_t SetState(const TvmmStateOp &op);
	void Reset();

	// For the architecture's run, which counts the processor's exits.
	TvmmVcpuStats &Stats() {return fStats;}
	// Any thread, without the vcpu's lock.
	void GetStats(TvmmVcpuStats &stats) const;

	// Waits until the guest no longer runs with a TLB generation older than
	// 'generation'. Any thread.
	void SyncTlbGeneration(int64 generation);

	// For the architecture's entry, with interrupts disabled: false when a kick
	// is pending, which it consumes. Otherwise returns the TLB generation to
	// enter with.
	bool BeginGuestEntry(int32 cpu, int64 &tlbGeneration);
	void EndGuestEntry();
};
