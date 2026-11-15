#include "Vcpu.h"

#include <malloc.h>
#include <stdlib.h>
#include <string.h>

#include <KernelExport.h>

#include <smp.h>
#include <thread.h>
#include <util/AutoLock.h>
#include <util/atomic.h>

#include "Arch.h"
#include "Common.h"
#include "Vm.h"
#include "tvmm_abi.h"


// An interrupt from another CPU is all it takes to leave the guest.
static void NoOp(void *cookie, int cpu)
{
	(void)cookie;
	(void)cpu;
}


Vcpu::Vcpu(Vm &vm, uint32 index):
	fVm(vm),
	fIndex(index)
{
	mutex_init(&fLock, "tvmm vcpu");
	fWakeCondition.Init(this, "tvmm vcpu wake");
}


Vcpu::~Vcpu()
{
	delete fArch;
	free(fStateBuffer);
	mutex_destroy(&fLock);
}


status_t Vcpu::Init(team_id team, uint64 &runAddress)
{
	static_assert(sizeof(TvmmRun) <= kTvmmRunDataOffset);
	CHECK_RET(fRunArea.Create("tvmm vcpu run", kTvmmRunAreaSize, B_FULL_LOCK));
	fRun = static_cast<TvmmRun*>(fRunArea.Address());
	CHECK_RET(fVm.Arch().CreateVcpu(*this, fArch));
	fStateBuffer = memalign(64, fArch->StateSize());
	if (fStateBuffer == nullptr) {
		return B_NO_MEMORY;
	}
	return fRunArea.MapIntoTeam(team, "tvmm vcpu run", runAddress);
}


status_t Vcpu::Run()
{
	MutexLocker locker(fLock);
	const uint64 start = Arch::Cycles();
	if (fClientSince != 0) {
		AddStatCycles(fStats.clientExits[fClientExit], ElapsedCycles(fClientSince, start));
		fClientSince = 0;
	}
	const status_t res = RunLoop();
	const uint64 end = Arch::Cycles();
	AddStat(fStats.runs, ElapsedCycles(start, end));
	if (res == B_OK) {
		// the client may change the page; a reason out of range goes as None
		const uint32 exit = uint32(ReadOnce(fRun->exitReason));
		fClientExit = exit < kTvmmExitCount ? exit : 0;
		AddStatCount(fStats.clientExits[fClientExit]);
		fClientSince = end;
	}
	return res;
}


status_t Vcpu::RunLoop()
{
	TvmmRun &run = *fRun;
	if (fVm.Closing()) {
		fArch->CancelRun(run);
		return B_OK;
	}
	CHECK_RET(fArch->PrepareRun(run));
	Thread *thread = thread_get_current_thread();
	for (;;) {
		// A signal for the thread is taken on the way back to userland. The
		// test is racy, which only delays it to the next exit.
		if (fVm.Closing() || thread_is_interrupted(thread, B_CAN_INTERRUPT)) {
			fArch->CancelRun(run);
			return B_OK;
		}
		// a wake from now on is for what EnterGuest() looks at next
		atomic_set(&fWakePending, 0);
		const ExitAction action = fArch->EnterGuest(run);
		if (action == ExitAction::ToClient) {
			return B_OK;
		}
		if (action == ExitAction::Wait) {
			const uint64 start = Arch::Cycles();
			const bool woken = WaitForWake();
			AddStat(fStats.waits, ElapsedCycles(start, Arch::Cycles()));
			if (!woken) {
				fArch->CancelRun(run);
				return B_OK;
			}
		}
	}
}


// False when a signal or the VM closing ends the wait.
bool Vcpu::WaitForWake()
{
	ConditionVariableEntry entry;
	fWakeCondition.Add(&entry);
	// a wake that came before the entry was added is not lost
	if (atomic_get(&fWakePending) != 0 || atomic_get(&fKickPending) != 0) {
		return true;
	}
	if (fVm.Closing()) {
		return false;
	}
	return entry.Wait(B_CAN_INTERRUPT) != B_INTERRUPTED;
}


void Vcpu::Kick()
{
	atomic_set(&fKickPending, 1);
	Wake();
}


bool Vcpu::TakeKick()
{
	return atomic_get_and_set(&fKickPending, 0) != 0;
}


void Vcpu::Wake()
{
	atomic_set(&fWakePending, 1);
	const int32 cpu = atomic_get(&fGuestCpu);
	if (cpu >= 0) {
		call_single_cpu(cpu, NoOp, nullptr);
	}
	fWakeCondition.NotifyAll();
}


bool Vcpu::BeginGuestEntry(int32 cpu, int64 &tlbGeneration)
{
	// Kick() and SyncTlbGeneration() store their side, then look at
	// fGuestCpu; this stores fGuestCpu, then looks at theirs. Either they see
	// the guest and interrupt it, or it sees what they stored.
	atomic_set(&fGuestCpu, cpu);
	if (atomic_get_and_set(&fKickPending, 0) != 0) {
		atomic_set(&fGuestCpu, -1);
		return false;
	}
	tlbGeneration = fVm.TlbGeneration();
	atomic_set64(&fEntryTlbGeneration, tlbGeneration);
	return true;
}


void Vcpu::EndGuestEntry()
{
	atomic_set(&fGuestCpu, -1);
}


void Vcpu::SyncTlbGeneration(int64 generation)
{
	for (;;) {
		const int32 cpu = atomic_get(&fGuestCpu);
		if (cpu < 0 || atomic_get64(&fEntryTlbGeneration) >= generation) {
			return;
		}
		call_single_cpu(cpu, NoOp, nullptr);
		while (atomic_get(&fGuestCpu) == cpu && atomic_get64(&fEntryTlbGeneration) < generation) {
			cpu_pause();
		}
	}
}


status_t Vcpu::GetState(const TvmmStateOp &op)
{
	const size_t size = fArch->StateSize();
	if (op.stateSize != size) {
		return B_BAD_VALUE;
	}
	void *clientState = reinterpret_cast<void*>(addr_t(op.stateAddress));
	if (!IS_USER_ADDRESS(clientState)) {
		return B_BAD_ADDRESS;
	}
	MutexLocker locker(fLock);
	// The classes not asked for go back as the client had them.
	CHECK_RET(user_memcpy(fStateBuffer, clientState, size));
	fArch->GetState(op.classes, fStateBuffer);
	return user_memcpy(clientState, fStateBuffer, size);
}


static void ReadStats(const TvmmStat *from, TvmmStat *to, size_t count)
{
	for (size_t i = 0; i < count; i++) {
		to[i].count = __atomic_load_n(&from[i].count, __ATOMIC_RELAXED);
		to[i].cycles = __atomic_load_n(&from[i].cycles, __ATOMIC_RELAXED);
	}
}


void Vcpu::GetStats(TvmmVcpuStats &stats) const
{
	ReadStats(&fStats.runs, &stats.runs, 1);
	ReadStats(&fStats.entries, &stats.entries, 1);
	ReadStats(&fStats.waits, &stats.waits, 1);
	ReadStats(fStats.clientExits, stats.clientExits, kTvmmMaxClientExits);
	ReadStats(fStats.archExits, stats.archExits, kTvmmMaxArchExits);
	ReadStats(fStats.apicReads, stats.apicReads, kTvmmApicRegCount);
	ReadStats(fStats.apicWrites, stats.apicWrites, kTvmmApicRegCount);
	ReadStats(&fStats.apicLock, &stats.apicLock, 1);
	ReadStats(&fStats.guestMemory, &stats.guestMemory, 1);
	ReadStats(&fStats.insnFetches, &stats.insnFetches, 1);
}


void Vcpu::Reset()
{
	MutexLocker locker(fLock);
	fArch->Reset();
}


status_t Vcpu::SetState(const TvmmStateOp &op)
{
	const size_t size = fArch->StateSize();
	if (op.stateSize != size) {
		return B_BAD_VALUE;
	}
	const void *clientState = reinterpret_cast<const void*>(addr_t(op.stateAddress));
	if (!IS_USER_ADDRESS(clientState)) {
		return B_BAD_ADDRESS;
	}
	MutexLocker locker(fLock);
	CHECK_RET(user_memcpy(fStateBuffer, clientState, size));
	return fArch->SetState(op.classes, fStateBuffer);
}
