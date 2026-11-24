#include "VmHandle.h"

#include <new>

#include <AutoDeleter.h>

#include <team.h>
#include <util/AutoLock.h>
#include <util/atomic.h>

#include "Arch.h"
#include "Common.h"
#include "Vcpu.h"
#include "Vm.h"
#include "tvmm_abi.h"


VmHandle::VmHandle()
{
	mutex_init(&fLock, "tvmm handle");
}


// Called once no operation is under way any more.
VmHandle::~VmHandle()
{
	delete fVm;
	mutex_destroy(&fLock);
}


status_t VmHandle::Open(const char *name, uint32 flags, VmHandle *&handle)
{
	(void)name;
	(void)flags;
	handle = new(std::nothrow) VmHandle();
	if (handle == nullptr) {
		return B_NO_MEMORY;
	}
	return B_OK;
}


// Operations may still be under way on other threads; they return soon.
status_t VmHandle::Close()
{
	if (Vm *vm = GetVm()) {
		vm->Close();
	}
	return B_OK;
}


Vm *VmHandle::GetVm()
{
	return atomic_pointer_get(&fVm);
}


status_t VmHandle::GetVcpu(const void *data, size_t length, Vcpu *&vcpu)
{
	TvmmVcpuOp args;
	CHECK_RET(CopyArgsIn(args, data, length));
	Vm *vm = GetVm();
	if (vm == nullptr) {
		return B_NO_INIT;
	}
	vcpu = vm->GetVcpu(args.vcpu);
	if (vcpu == nullptr) {
		return B_BAD_VALUE;
	}
	return B_OK;
}


status_t VmHandle::Control(uint32 op, void *data, size_t length)
{
	status_t res = B_DEV_INVALID_IOCTL;
	switch (op) {
		case TvmmOpGetInfo:
			res = HandleGetInfo(data, length);
			break;
		case TvmmOpCreateVm:
			res = HandleCreateVm(data, length);
			break;
		case TvmmOpAllocRam:
			res = HandleAllocRam(data, length);
			break;
		case TvmmOpFreeRam:
			res = HandleFreeRam(data, length);
			break;
		case TvmmOpMapRam:
			res = HandleMapRam(data, length);
			break;
		case TvmmOpCreateVcpu:
			res = HandleCreateVcpu(data, length);
			break;
		case TvmmOpRun:
			res = HandleRun(data, length);
			break;
		case TvmmOpKick:
			res = HandleKick(data, length);
			break;
		case TvmmOpGetState:
			res = HandleGetState(data, length);
			break;
		case TvmmOpSetState:
			res = HandleSetState(data, length);
			break;
		case TvmmOpSetCpuid:
			res = HandleSetCpuid(data, length);
			break;
		case TvmmOpSendMsi:
			res = HandleSendMsi(data, length);
			break;
		case TvmmOpResetVcpu:
			res = HandleResetVcpu(data, length);
			break;
		case TvmmOpGetStats:
			res = HandleGetStats(data, length);
			break;
		case TvmmOpGetDirtyLog:
			res = HandleGetDirtyLog(data, length);
			break;
		case TvmmOpSetIrqLine:
			res = HandleSetIrqLine(data, length);
			break;
		case TvmmOpResetIoApic:
			res = HandleResetIoApic(data, length);
			break;
		case TvmmOpGetHostIsa:
			res = HandleGetHostIsa(data, length);
			break;
		default:
			break;
	}
	return res;
}


status_t VmHandle::HandleGetInfo(void *data, size_t length)
{
	TvmmInfo info {};
	CHECK_RET(CopyArgsIn(info, data, length));
	info = {};
	info.abiVersion = kTvmmAbiVersion;
	info.maxSlots = GuestMemory::kMaxSlots;
	Arch::Probe(info);
	return CopyArgsOut(data, info);
}


status_t VmHandle::HandleGetHostIsa(void *data, size_t length)
{
	// too large for the kernel stack
	ObjectDeleter<TvmmHostIsa> deleter(new(std::nothrow) TvmmHostIsa);
	if (!deleter.IsSet()) {
		return B_NO_MEMORY;
	}
	TvmmHostIsa &isa = *deleter.Get();
	CHECK_RET(CopyArgsIn(isa, data, length));
	isa = {};
	CHECK_RET(Arch::GetHostIsa(isa));
	return CopyArgsOut(data, isa);
}


status_t VmHandle::HandleCreateVm(void *data, size_t length)
{
	TvmmCreateVm args;
	CHECK_RET(CopyArgsIn(args, data, length));
	if ((args.flags & ~uint32(TvmmVmLocalApic | TvmmVmIoApic)) != 0) {
		return B_BAD_VALUE;
	}
	// the IOAPIC's messages go to the kernel's local APICs
	if ((args.flags & TvmmVmIoApic) != 0 && (args.flags & TvmmVmLocalApic) == 0) {
		return B_BAD_VALUE;
	}

	MutexLocker locker(fLock);
	if (fVm != nullptr) {
		return B_FILE_EXISTS;
	}
	auto *vm = new(std::nothrow) Vm(team_get_current_team_id());
	if (vm == nullptr) {
		return B_NO_MEMORY;
	}
	const status_t res = vm->Init(args.flags);
	if (res < B_OK) {
		delete vm;
		return res;
	}
	atomic_pointer_set(&fVm, vm);
	return B_OK;
}


status_t VmHandle::HandleAllocRam(void *data, size_t length)
{
	TvmmAllocRam args;
	CHECK_RET(CopyArgsIn(args, data, length));
	Vm *vm = GetVm();
	if (vm == nullptr) {
		return B_NO_INIT;
	}
	CHECK_RET(vm->AllocRam(args));
	const status_t res = CopyArgsOut(data, args);
	if (res < B_OK) {
		vm->FreeRam(args.ramId);
	}
	return res;
}


status_t VmHandle::HandleFreeRam(void *data, size_t length)
{
	TvmmFreeRam args;
	CHECK_RET(CopyArgsIn(args, data, length));
	Vm *vm = GetVm();
	if (vm == nullptr) {
		return B_NO_INIT;
	}
	return vm->FreeRam(args.ramId);
}


status_t VmHandle::HandleMapRam(void *data, size_t length)
{
	TvmmMapRam args;
	CHECK_RET(CopyArgsIn(args, data, length));
	Vm *vm = GetVm();
	if (vm == nullptr) {
		return B_NO_INIT;
	}
	return vm->MapRam(args);
}


status_t VmHandle::HandleGetDirtyLog(void *data, size_t length)
{
	TvmmGetDirtyLog args;
	CHECK_RET(CopyArgsIn(args, data, length));
	Vm *vm = GetVm();
	if (vm == nullptr) {
		return B_NO_INIT;
	}
	return vm->GetDirtyLog(args);
}


status_t VmHandle::HandleCreateVcpu(void *data, size_t length)
{
	TvmmCreateVcpu args;
	CHECK_RET(CopyArgsIn(args, data, length));
	Vm *vm = GetVm();
	if (vm == nullptr) {
		return B_NO_INIT;
	}
	CHECK_RET(vm->CreateVcpu(args));
	return CopyArgsOut(data, args);
}


status_t VmHandle::HandleRun(void *data, size_t length)
{
	Vcpu *vcpu = nullptr;
	CHECK_RET(GetVcpu(data, length, vcpu));
	return vcpu->Run();
}


status_t VmHandle::HandleKick(void *data, size_t length)
{
	Vcpu *vcpu = nullptr;
	CHECK_RET(GetVcpu(data, length, vcpu));
	vcpu->Kick();
	return B_OK;
}


status_t VmHandle::HandleGetState(void *data, size_t length)
{
	TvmmStateOp args;
	CHECK_RET(CopyArgsIn(args, data, length));
	Vm *vm = GetVm();
	if (vm == nullptr) {
		return B_NO_INIT;
	}
	Vcpu *vcpu = vm->GetVcpu(args.vcpu);
	if (vcpu == nullptr) {
		return B_BAD_VALUE;
	}
	return vcpu->GetState(args);
}


status_t VmHandle::HandleSetCpuid(void *data, size_t length)
{
	TvmmSetCpuid args;
	CHECK_RET(CopyArgsIn(args, data, length));
	Vm *vm = GetVm();
	if (vm == nullptr) {
		return B_NO_INIT;
	}
	return vm->SetCpuid(args);
}


status_t VmHandle::HandleSendMsi(void *data, size_t length)
{
	TvmmSendMsi args;
	CHECK_RET(CopyArgsIn(args, data, length));
	Vm *vm = GetVm();
	if (vm == nullptr) {
		return B_NO_INIT;
	}
	return vm->Arch().SendMsi(args.address, args.data);
}


status_t VmHandle::HandleSetIrqLine(void *data, size_t length)
{
	TvmmSetIrqLine args;
	CHECK_RET(CopyArgsIn(args, data, length));
	Vm *vm = GetVm();
	if (vm == nullptr) {
		return B_NO_INIT;
	}
	return vm->Arch().SetIrqLine(args.pin, args.level != 0);
}


status_t VmHandle::HandleResetIoApic(void *data, size_t length)
{
	TvmmResetIoApic args;
	CHECK_RET(CopyArgsIn(args, data, length));
	Vm *vm = GetVm();
	if (vm == nullptr) {
		return B_NO_INIT;
	}
	return vm->Arch().ResetIoApic();
}


status_t VmHandle::HandleResetVcpu(void *data, size_t length)
{
	Vcpu *vcpu = nullptr;
	CHECK_RET(GetVcpu(data, length, vcpu));
	vcpu->Reset();
	return B_OK;
}


status_t VmHandle::HandleGetStats(void *data, size_t length)
{
	// too large for the kernel stack
	ObjectDeleter<TvmmGetStats> deleter(new(std::nothrow) TvmmGetStats);
	if (!deleter.IsSet()) {
		return B_NO_MEMORY;
	}
	TvmmGetStats &args = *deleter.Get();
	CHECK_RET(CopyArgsIn(args, data, length));
	Vm *vm = GetVm();
	if (vm == nullptr) {
		return B_NO_INIT;
	}
	Vcpu *vcpu = vm->GetVcpu(args.vcpu);
	if (vcpu == nullptr) {
		return B_BAD_VALUE;
	}
	vcpu->GetStats(args.stats);
	return CopyArgsOut(data, args);
}


status_t VmHandle::HandleSetState(void *data, size_t length)
{
	TvmmStateOp args;
	CHECK_RET(CopyArgsIn(args, data, length));
	Vm *vm = GetVm();
	if (vm == nullptr) {
		return B_NO_INIT;
	}
	Vcpu *vcpu = vm->GetVcpu(args.vcpu);
	if (vcpu == nullptr) {
		return B_BAD_VALUE;
	}
	return vcpu->SetState(args);
}
