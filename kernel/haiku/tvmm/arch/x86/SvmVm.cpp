#include "SvmVm.h"

#include <string.h>

#include <new>

#include <util/AutoLock.h>

#include "Common.h"
#include "GuestMemory.h"
#include "SvmVcpu.h"
#include "tvmm_abi.h"


static constexpr size_t kIopmSize = 3 * kPageSize;
static constexpr size_t kMsrpmSize = 2 * kPageSize;
// what four level nested tables translate
static constexpr uint64 kMaxGpa = 1ull << 48;


status_t SvmVm::Init()
{
	CHECK_RET(fNpt.Init());

	CHECK_RET(fIopm.Create("tvmm iopm", kIopmSize, B_CONTIGUOUS));
	CHECK_RET(fIopm.PhysicalAddress(0, fIopmPhys));
	memset(fIopm.Address(), 0xff, kIopmSize);

	CHECK_RET(fMsrpm.Create("tvmm msrpm", kMsrpmSize, B_CONTIGUOUS));
	CHECK_RET(fMsrpm.PhysicalAddress(0, fMsrpmPhys));
	memset(fMsrpm.Address(), 0xff, kMsrpmSize);
	// VMLOAD and VMSAVE switch these, so the guest has its own.
	static const uint32 vmcbMsrs[] = {
		0x00000174, 0x00000175, 0x00000176,     // SYSENTER_CS, _ESP, _EIP
		0xc0000081, 0xc0000082, 0xc0000083,     // STAR, LSTAR, CSTAR
		0xc0000084,                             // SFMASK
		0xc0000100, 0xc0000101, 0xc0000102,     // FS_BASE, GS_BASE, KERNEL_GS_BASE
	};
	for (uint32 msr: vmcbMsrs) {
		PassThroughMsr(msr);
	}
	return B_OK;
}


// Two bits per MSR, read then write, in three ranges of 8192 MSRs.
void SvmVm::PassThroughMsr(uint32 msr)
{
	uint32 base;
	if (msr < 0x2000) {
		base = 0;
	} else if (msr - 0xc0000000 < 0x2000) {
		base = 0x800;
		msr -= 0xc0000000;
	} else if (msr - 0xc0010000 < 0x2000) {
		base = 0x1000;
		msr -= 0xc0010000;
	} else {
		return;
	}
	auto *bitmap = static_cast<uint8*>(fMsrpm.Address());
	const uint32 bit = msr * 2;
	bitmap[base + bit / 8] &= ~(3 << (bit % 8));
}


status_t SvmVm::MapPages(uint64 gpa, uint64 size, const RamObject &ram, uint64 offset, uint32 flags)
{
	if (gpa >= kMaxGpa || size > kMaxGpa - gpa) {
		return B_BAD_VALUE;
	}
	const bool writable = (flags & TvmmMapReadOnly) == 0;
	for (uint64 done = 0; done < size; done += kPageSize) {
		CHECK_RET(fNpt.Map(gpa + done, ram.PhysicalPage(offset + done), writable));
	}
	return B_OK;
}


void SvmVm::UnmapPages(uint64 gpa, uint64 size)
{
	for (uint64 done = 0; done < size; done += kPageSize) {
		fNpt.Unmap(gpa + done);
	}
}


status_t SvmVm::SetCpuid(const TvmmCpuidEntry *entries, uint32 count)
{
	memcpy(fCpuid, entries, count * sizeof(TvmmCpuidEntry));
	fCpuidCount = count;
	return B_OK;
}


// Only a write to the local APICs' page is an interrupt message.
status_t SvmVm::SendMsi(uint64 address, uint32 data)
{
	if (!HasLocalApics()) {
		return B_NOT_SUPPORTED;
	}
	if ((address >> 20) != (kApicBase >> 20)) {
		return B_BAD_VALUE;
	}
	InterruptsSpinLocker locker(fApicLock);
	fApicBus.Deliver(address, data);
	return B_OK;
}


const TvmmCpuidEntry *SvmVm::CpuidEntry(uint32 leaf) const
{
	for (uint32 i = 0; i < fCpuidCount; i++) {
		if (fCpuid[i].leaf == leaf) {
			return &fCpuid[i];
		}
	}
	return nullptr;
}


status_t SvmVm::CreateVcpu(Vcpu &vcpu, ArchVcpu *&arch)
{
	auto *svmVcpu = new(std::nothrow) SvmVcpu(*this, vcpu);
	if (svmVcpu == nullptr) {
		return B_NO_MEMORY;
	}
	const status_t res = svmVcpu->Init();
	if (res < B_OK) {
		delete svmVcpu;
		return res;
	}
	arch = svmVcpu;
	return B_OK;
}
