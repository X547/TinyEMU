#include "Vm.h"

#include <stdlib.h>
#include <string.h>

#include <new>

#include <AutoDeleter.h>

#include <util/AutoLock.h>
#include <util/atomic.h>

#include "Arch.h"
#include "Common.h"
#include "Vcpu.h"
#include "tvmm_abi.h"


Vm::Vm(team_id team):
	fTeam(team)
{
	mutex_init(&fLock, "tvmm vm");
}


Vm::~Vm()
{
	for (Vcpu *&vcpu: fVcpus) {
		delete vcpu;
		vcpu = nullptr;
	}
	// The nested page tables go with the architecture's half, so the RAM
	// fMemory frees after this needs no unmapping first.
	delete fArch;
	fArch = nullptr;
	fMemory.SetArch(nullptr);
	if (fArchAcquired) {
		Arch::Release();
	}
	mutex_destroy(&fLock);
}


status_t Vm::Init(uint32 flags)
{
	CHECK_RET(Arch::HostStatus());
	CHECK_RET(Arch::Acquire());
	fArchAcquired = true;
	CHECK_RET(Arch::CreateVm(*this, flags, fArch));
	fMemory.SetArch(fArch);
	return B_OK;
}


int64 Vm::TlbGeneration() const
{
	return atomic_get64(const_cast<int64*>(&fTlbGeneration));
}


bool Vm::Closing() const
{
	return atomic_get(const_cast<int32*>(&fClosing)) != 0;
}


// After a mapping is removed, no guest may go on using a translation of it.
void Vm::FlushGuestTlbs()
{
	const int64 generation = atomic_add64(&fTlbGeneration, 1) + 1;
	for (Vcpu *vcpu: fVcpus) {
		if (vcpu != nullptr) {
			vcpu->SyncTlbGeneration(generation);
		}
	}
}


status_t Vm::AllocRam(TvmmAllocRam &args)
{
	MutexLocker locker(fLock);
	RamObject *ram = nullptr;
	CHECK_RET(fMemory.AllocRam(args.size, ram));
	uint64 address = 0;
	const status_t res = ram->Area().MapIntoTeam(fTeam, "tvmm guest ram", address);
	if (res < B_OK) {
		fMemory.FreeRam(ram->Id());
		return res;
	}
	args.ramId = ram->Id();
	args.address = address;
	return B_OK;
}


status_t Vm::FreeRam(uint32 id)
{
	MutexLocker locker(fLock);
	return fMemory.FreeRam(id);
}


status_t Vm::MapRam(const TvmmMapRam &args)
{
	MutexLocker locker(fLock);
	bool unmapped = false;
	const status_t res = fMemory.MapRam(args, unmapped);
	if (unmapped) {
		FlushGuestTlbs();
	}
	return res;
}


// The guest's TLBs may remember the pages the log had as written, and write
// them without saying so again: they are flushed before the client, told of
// the pages, reads them.
status_t Vm::GetDirtyLog(const TvmmGetDirtyLog &args)
{
	void *clientBitmap = reinterpret_cast<void*>(addr_t(args.bitmapAddress));
	if (args.bitmapSize == 0 || args.bitmapSize % sizeof(uint64) != 0
		|| args.bitmapSize > GuestMemory::kMaxBitmapSize) {
		return B_BAD_VALUE;
	}
	if (!IS_USER_ADDRESS(clientBitmap)) {
		return B_BAD_ADDRESS;
	}
	MemoryDeleter bitmap(malloc(args.bitmapSize));
	if (!bitmap.IsSet()) {
		return B_NO_MEMORY;
	}
	{
		MutexLocker locker(fLock);
		bool flush = false;
		CHECK_RET(fMemory.TakeDirtyLog(args.slot, static_cast<uint64*>(bitmap.Get()),
			args.bitmapSize / sizeof(uint64), flush));
		if (flush) {
			FlushGuestTlbs();
		}
	}
	return user_memcpy(clientBitmap, bitmap.Get(), args.bitmapSize);
}


status_t Vm::CreateVcpu(TvmmCreateVcpu &args)
{
	if (args.index >= kMaxVcpus || args.index >= Arch::MaxVcpus()) {
		return B_BAD_VALUE;
	}
	MutexLocker locker(fLock);
	if (fVcpus[args.index] != nullptr) {
		return B_FILE_EXISTS;
	}
	auto *vcpu = new(std::nothrow) Vcpu(*this, args.index);
	if (vcpu == nullptr) {
		return B_NO_MEMORY;
	}
	uint64 runAddress = 0;
	const status_t res = vcpu->Init(fTeam, runAddress);
	if (res < B_OK) {
		delete vcpu;
		return res;
	}
	atomic_pointer_set(&fVcpus[args.index], vcpu);
	args.runAddress = runAddress;
	return B_OK;
}


Vcpu *Vm::GetVcpu(uint32 index)
{
	if (index >= kMaxVcpus) {
		return nullptr;
	}
	return atomic_pointer_get(&fVcpus[index]);
}


status_t Vm::SetCpuid(const TvmmSetCpuid &args)
{
	if (args.count > kTvmmMaxCpuidEntries) {
		return B_BAD_VALUE;
	}
	const auto *clientEntries = reinterpret_cast<const TvmmCpuidEntry*>(addr_t(args.entriesAddress));
	if (args.count != 0 && !IS_USER_ADDRESS(clientEntries)) {
		return B_BAD_ADDRESS;
	}
	TvmmCpuidEntry entries[kTvmmMaxCpuidEntries];
	CHECK_RET(user_memcpy(entries, clientEntries, args.count * sizeof(TvmmCpuidEntry)));
	MutexLocker locker(fLock);
	return fArch->SetCpuid(entries, args.count);
}


bool Vm::IsRam(uint64 gpa, bool write)
{
	MutexLocker locker(fLock);
	return fMemory.KernelAddress(gpa, 1, write) != nullptr;
}


bool Vm::ReadGuest(uint64 gpa, void *buffer, size_t size)
{
	MutexLocker locker(fLock);
	const uint8 *address = fMemory.KernelAddress(gpa, size, false);
	if (address == nullptr) {
		return false;
	}
	memcpy(buffer, address, size);
	return true;
}


bool Vm::WriteGuest(uint64 gpa, const void *buffer, size_t size)
{
	MutexLocker locker(fLock);
	uint8 *address = fMemory.KernelAddress(gpa, size, true);
	if (address == nullptr) {
		return false;
	}
	memcpy(address, buffer, size);
	fMemory.MarkWritten(gpa, size);
	return true;
}


void Vm::Close()
{
	atomic_set(&fClosing, 1);
	for (uint32 i = 0; i < kMaxVcpus; i++) {
		if (Vcpu *vcpu = GetVcpu(i)) {
			vcpu->Kick();
		}
	}
}
