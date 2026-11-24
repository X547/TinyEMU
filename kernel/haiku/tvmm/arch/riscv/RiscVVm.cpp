#include "RiscVVm.h"

#include <new>

#include <util/AutoLock.h>
#include <util/atomic.h>

#include "Common.h"
#include "GuestMemory.h"
#include "RiscVCsr.h"
#include "RiscVHost.h"
#include "RiscVSbi.h"
#include "RiscVVcpu.h"
#include "Vcpu.h"
#include "tvmm_abi.h"


status_t RiscVVm::Init()
{
	fTimeDelta = -RiscV::ReadTime();
	return fGStage.Init(RiscVHost::HgatpMode());
}


uint32 RiscVVm::HartCount()
{
	uint32 count = 0;
	while (count < Vm::kMaxVcpus && fVm.GetVcpu(count) != nullptr) {
		count++;
	}
	return count;
}


void RiscVVm::SendIpi(uint32 hart)
{
	Vcpu *vcpu = fVm.GetVcpu(hart);
	if (vcpu == nullptr) {
		return;
	}
	atomic_set(&fHarts[hart].softPending, 1);
	vcpu->Wake();
}


int64 RiscVVm::StartHart(uint32 hart, uint64 pc, uint64 opaque)
{
	Vcpu *vcpu = hart < Vm::kMaxVcpus ? fVm.GetVcpu(hart) : nullptr;
	if (vcpu == nullptr) {
		return Sbi::kErrInvalidParam;
	}
	RiscVHartControl &control = fHarts[hart];
	{
		InterruptsSpinLocker locker(control.lock);
		if (control.state != TvmmRiscVHartStopped) {
			return Sbi::kErrAlreadyAvailable;
		}
		control.startPc = pc;
		control.startOpaque = opaque;
		control.state = TvmmRiscVHartStartPending;
	}
	vcpu->Wake();
	return Sbi::kSuccess;
}


int64 RiscVVm::HartStatus(uint32 hart, uint64 &status)
{
	if (hart >= Vm::kMaxVcpus || fVm.GetVcpu(hart) == nullptr) {
		return Sbi::kErrInvalidParam;
	}
	RiscVHartControl &control = fHarts[hart];
	InterruptsSpinLocker locker(control.lock);
	status = uint32(control.state);
	return Sbi::kSuccess;
}


status_t RiscVVm::MapPages(uint64 gpa, uint64 size, const RamObject &ram, uint64 offset, uint32 flags)
{
	const uint64 limit = fGStage.Limit();
	if (gpa >= limit || size > limit - gpa) {
		return B_BAD_VALUE;
	}
	const bool writable = (flags & TvmmMapReadOnly) == 0;
	for (uint64 done = 0; done < size; done += kPageSize) {
		CHECK_RET(fGStage.Map(gpa + done, ram.PhysicalPage(offset + done), writable));
	}
	return B_OK;
}


void RiscVVm::UnmapPages(uint64 gpa, uint64 size)
{
	for (uint64 done = 0; done < size; done += kPageSize) {
		fGStage.Unmap(gpa + done);
	}
}


bool RiscVVm::TakeDirty(uint64 gpa, uint64 size, uint64 *bitmap)
{
	return fGStage.TakeDirty(gpa, size / kPageSize, bitmap);
}


status_t RiscVVm::CreateVcpu(Vcpu &vcpu, ArchVcpu *&arch)
{
	auto *riscVVcpu = new(std::nothrow) RiscVVcpu(*this, vcpu);
	if (riscVVcpu == nullptr) {
		return B_NO_MEMORY;
	}
	arch = riscVVcpu;
	return B_OK;
}


status_t RiscVVm::SetCpuid(const TvmmCpuidEntry *entries, uint32 count)
{
	(void)entries;
	(void)count;
	return B_NOT_SUPPORTED;
}


status_t RiscVVm::SendMsi(uint64 address, uint32 data)
{
	(void)address;
	(void)data;
	return B_NOT_SUPPORTED;
}


// The level the client's interrupt controller drives, which the vcpu looks
// at before it enters.
status_t RiscVVm::SetIrqLine(uint32 pin, bool level)
{
	if (pin >= Vm::kMaxVcpus) {
		return B_BAD_VALUE;
	}
	atomic_set(&fHarts[pin].externalLevel, level ? 1 : 0);
	if (Vcpu *vcpu = fVm.GetVcpu(pin)) {
		vcpu->Wake();
	}
	return B_OK;
}


status_t RiscVVm::ResetIoApic()
{
	return B_NOT_SUPPORTED;
}
