#pragma once

#include <SupportDefs.h>

#include <lock.h>

class Vcpu;
class Vm;


// One open file descriptor of the device, which holds at most one VM.
class VmHandle {
private:
	mutex fLock;
	// set once by TvmmOpCreateVm, then stays until Free()
	Vm *fVm = nullptr;

	Vm *GetVm();
	status_t GetVcpu(const void *data, size_t length, Vcpu *&vcpu);

	status_t HandleGetInfo(void *data, size_t length);
	status_t HandleGetHostIsa(void *data, size_t length);
	status_t HandleCreateVm(void *data, size_t length);
	status_t HandleAllocRam(void *data, size_t length);
	status_t HandleFreeRam(void *data, size_t length);
	status_t HandleMapRam(void *data, size_t length);
	status_t HandleCreateVcpu(void *data, size_t length);
	status_t HandleRun(void *data, size_t length);
	status_t HandleKick(void *data, size_t length);
	status_t HandleGetState(void *data, size_t length);
	status_t HandleSetState(void *data, size_t length);
	status_t HandleSetCpuid(void *data, size_t length);
	status_t HandleSendMsi(void *data, size_t length);
	status_t HandleResetVcpu(void *data, size_t length);
	status_t HandleGetStats(void *data, size_t length);
	status_t HandleGetDirtyLog(void *data, size_t length);
	status_t HandleSetIrqLine(void *data, size_t length);
	status_t HandleResetIoApic(void *data, size_t length);

public:
	VmHandle();
	~VmHandle();

	static status_t Open(const char *name, uint32 flags, VmHandle *&handle);
	status_t Close();
	status_t Control(uint32 op, void *data, size_t length);
};
