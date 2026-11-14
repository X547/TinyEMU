#include <string.h>

#include <KernelExport.h>

#include "Arch.h"
#include "Common.h"
#include "Drivers_cpp.h"
#include "VmHandle.h"
#include "tvmm_abi.h"


static constexpr device_hooks kVmHooks = GetLegacyDeviceHooks<VmHandle>();
static constexpr const char *kDeviceName = "misc/tvmm";


class TvmmDriver {
public:
	static status_t InitDriver();
	static const char **PublishDevices();
	static const device_hooks *FindDevice(const char *name);
};


// The device is published even on a host that cannot run guests, so that a
// client can ask why.
status_t TvmmDriver::InitDriver()
{
	TvmmInfo info {};
	Arch::Probe(info);
	TVMM_LOG("arch %u, host status %s, features %#x\n",
		static_cast<uint32>(info.arch), strerror(info.hostStatus), info.features);
	return B_OK;
}


const char **TvmmDriver::PublishDevices()
{
	static const char *devices[] = {
		kDeviceName,
		nullptr
	};
	return devices;
}


const device_hooks *TvmmDriver::FindDevice(const char *name)
{
	if (strcmp(name, kDeviceName) == 0) {
		return &kVmHooks;
	}
	return nullptr;
}


LEGACY_DRIVER_ENTRY_POINTS(TvmmDriver)
