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
}


Vcpu::~Vcpu()
{
	delete fArch;
	free(fStateBuffer);
	mutex_destroy(&fLock);
}


status_t Vcpu::Init(team_id team, uint64 &runAddress)
{
	static_assert(sizeof(TvmmRun) <= kPageSize);
	CHECK_RET(fRunArea.Create("tvmm vcpu run", kPageSize, B_FULL_LOCK));
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
		if (fArch->EnterGuest(run) == ExitAction::ToClient) {
			return B_OK;
		}
	}
}


void Vcpu::Kick()
{
	atomic_set(&fKickPending, 1);
	const int32 cpu = atomic_get(&fGuestCpu);
	if (cpu >= 0) {
		call_single_cpu(cpu, NoOp, nullptr);
	}
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
