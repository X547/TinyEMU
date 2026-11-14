#pragma once

#include <Drivers.h>

#include "DriversCommon_cpp.h"


// C++ wrappers for the legacy driver API (`device_hooks` and driver entry points).
//
// Caller side: `LegacyDevice` and `LegacyDeviceHandle` call into an existing
// `device_hooks` table.
//
// Implementation side: `GetLegacyDeviceHooks<HandleImpl>()` builds a `device_hooks`
// table from a C++ class, and `LEGACY_DRIVER_ENTRY_POINTS(DriverImpl)` exports the
// driver entry points.


class LegacyDeviceHandle;


class LegacyDevice {
public:
	const device_hooks *fCls = nullptr;

public:
	status_t Open(const char *name, uint32 flags, LegacyDeviceHandle &handle) const;
};


class LegacyDeviceHandle {
public:
	const device_hooks *fCls = nullptr;
	void *fInst = nullptr;

public:
	LegacyDevice Device() const {return {fCls};}

	status_t Close() const {return fCls->close(fInst);}
	status_t Free();

	bool HasControl() const {return fCls->control != nullptr;}
	status_t Control(uint32 op, void *data, size_t length) const {return fCls->control(fInst, op, data, length);}

	bool HasRead() const {return fCls->read != nullptr;}
	status_t Read(off_t pos, void *data, size_t &length) const {return fCls->read(fInst, pos, data, &length);}

	bool HasWrite() const {return fCls->write != nullptr;}
	status_t Write(off_t pos, const void *data, size_t &length) const {return fCls->write(fInst, pos, data, &length);}

	bool HasSelect() const {return fCls->select != nullptr;}
	status_t Select(uint8 event, uint32 ref, selectsync *sync) const {return fCls->select(fInst, event, ref, sync);}

	bool HasDeselect() const {return fCls->deselect != nullptr;}
	status_t Deselect(uint8 event, selectsync *sync) const {return fCls->deselect(fInst, event, sync);}

	bool HasReadV() const {return fCls->readv != nullptr;}
	status_t ReadV(off_t pos, const iovec *vecs, size_t count, size_t &length) const {return fCls->readv(fInst, pos, vecs, count, &length);}

	bool HasWriteV() const {return fCls->writev != nullptr;}
	status_t WriteV(off_t pos, const iovec *vecs, size_t count, size_t &length) const {return fCls->writev(fInst, pos, vecs, count, &length);}
};


// #pragma mark - LegacyDevice

inline status_t LegacyDevice::Open(const char *name, uint32 flags, LegacyDeviceHandle &handle) const
{
	handle.fCls = fCls;
	handle.fInst = nullptr;
	return fCls->open(name, flags, &handle.fInst);
}


// #pragma mark - LegacyDeviceHandle

inline status_t LegacyDeviceHandle::Free()
{
	const status_t res = fCls->free(fInst);
	fInst = nullptr;
	return res;
}


// #pragma mark - implementation side

// Builds `device_hooks` from `HandleImpl` (one instance per open file descriptor):
//
//   static status_t Open(const char *name, uint32 flags, HandleImpl *&handle); // required
//   status_t Close();                                         // optional, B_OK if absent
//   status_t Free();               // optional, owns the object; `delete` if absent
//   status_t Control(uint32 op, void *data, size_t length);                 // optional
//   status_t Read(off_t pos, void *data, size_t &length);                    // optional
//   status_t Write(off_t pos, const void *data, size_t &length);             // optional
//   status_t Select(uint8 event, uint32 ref, selectsync *sync);              // optional
//   status_t Deselect(uint8 event, selectsync *sync);                        // optional
//   status_t ReadV(off_t pos, const iovec *vecs, size_t count, size_t &length);  // optional
//   status_t WriteV(off_t pos, const iovec *vecs, size_t count, size_t &length); // optional
template <typename HandleImpl>
constexpr device_hooks GetLegacyDeviceHooks()
{
	using DriversCppPrivate::FromCookie;

	device_hooks cls {};
	cls.open = [](const char *name, uint32 flags, void **cookie) -> status_t {
		HandleImpl *handle = nullptr;
		const status_t res = HandleImpl::Open(name, flags, handle);
		if (res < B_OK) {
			return res;
		}
		*cookie = handle;
		return res;
	};
	cls.close = [](void *cookie) -> status_t {
		if constexpr (requires(HandleImpl &handle) {handle.Close();}) {
			return FromCookie<HandleImpl>(cookie)->Close();
		} else {
			(void)cookie;
			return B_OK;
		}
	};
	cls.free = [](void *cookie) -> status_t {
		if constexpr (requires(HandleImpl &handle) {handle.Free();}) {
			return FromCookie<HandleImpl>(cookie)->Free();
		} else {
			DriversCppPrivate::Delete<HandleImpl>(cookie);
			return B_OK;
		}
	};
	if constexpr (requires(HandleImpl &handle, uint32 op, void *data, size_t length) {handle.Control(op, data, length);}) {
		cls.control = [](void *cookie, uint32 op, void *data, size_t length) -> status_t {
			return FromCookie<HandleImpl>(cookie)->Control(op, data, length);
		};
	}
	if constexpr (requires(HandleImpl &handle, off_t pos, void *data, size_t &length) {handle.Read(pos, data, length);}) {
		cls.read = [](void *cookie, off_t pos, void *data, size_t *length) -> status_t {
			return FromCookie<HandleImpl>(cookie)->Read(pos, data, *length);
		};
	}
	if constexpr (requires(HandleImpl &handle, off_t pos, const void *data, size_t &length) {handle.Write(pos, data, length);}) {
		cls.write = [](void *cookie, off_t pos, const void *data, size_t *length) -> status_t {
			return FromCookie<HandleImpl>(cookie)->Write(pos, data, *length);
		};
	}
	if constexpr (requires(HandleImpl &handle, uint8 event, uint32 ref, selectsync *sync) {handle.Select(event, ref, sync);}) {
		cls.select = [](void *cookie, uint8 event, uint32 ref, selectsync *sync) -> status_t {
			return FromCookie<HandleImpl>(cookie)->Select(event, ref, sync);
		};
	}
	if constexpr (requires(HandleImpl &handle, uint8 event, selectsync *sync) {handle.Deselect(event, sync);}) {
		cls.deselect = [](void *cookie, uint8 event, selectsync *sync) -> status_t {
			return FromCookie<HandleImpl>(cookie)->Deselect(event, sync);
		};
	}
	if constexpr (requires(HandleImpl &handle, off_t pos, const iovec *vecs, size_t count, size_t &length) {handle.ReadV(pos, vecs, count, length);}) {
		cls.readv = [](void *cookie, off_t pos, const iovec *vecs, size_t count, size_t *length) -> status_t {
			return FromCookie<HandleImpl>(cookie)->ReadV(pos, vecs, count, *length);
		};
	}
	if constexpr (requires(HandleImpl &handle, off_t pos, const iovec *vecs, size_t count, size_t &length) {handle.WriteV(pos, vecs, count, length);}) {
		cls.writev = [](void *cookie, off_t pos, const iovec *vecs, size_t count, size_t *length) -> status_t {
			return FromCookie<HandleImpl>(cookie)->WriteV(pos, vecs, count, *length);
		};
	}
	return cls;
}


namespace DriversCppPrivate {


template <typename DriverImpl>
status_t LegacyInitHardware()
{
	if constexpr (requires {DriverImpl::InitHardware();}) {
		return DriverImpl::InitHardware();
	} else {
		return B_OK;
	}
}


template <typename DriverImpl>
status_t LegacyInitDriver()
{
	if constexpr (requires {DriverImpl::InitDriver();}) {
		return DriverImpl::InitDriver();
	} else {
		return B_OK;
	}
}


template <typename DriverImpl>
void LegacyUninitDriver()
{
	if constexpr (requires {DriverImpl::UninitDriver();}) {
		DriverImpl::UninitDriver();
	}
}


template <typename DriverImpl>
device_hooks *LegacyFindDevice(const char *name)
{
	const LegacyDevice device {DriverImpl::FindDevice(name)};
	return const_cast<device_hooks*>(device.fCls);
}


} // DriversCppPrivate


// Exports the legacy driver entry points, forwarding them to static methods of
// `DriverImpl`:
//
//   static status_t InitHardware();                       // optional, B_OK if absent
//   static status_t InitDriver();                         // optional, B_OK if absent
//   static void UninitDriver();                           // optional
//   static const char **PublishDevices();                 // required
//   static const device_hooks *FindDevice(const char *name); // required, may also
//                                                         // return `LegacyDevice`
#define LEGACY_DRIVER_ENTRY_POINTS(DriverImpl) \
	_EXPORT int32 api_version = B_CUR_DRIVER_API_VERSION; \
	\
	_EXPORT status_t init_hardware() \
	{ \
		return DriversCppPrivate::LegacyInitHardware<DriverImpl>(); \
	} \
	\
	_EXPORT status_t init_driver() \
	{ \
		return DriversCppPrivate::LegacyInitDriver<DriverImpl>(); \
	} \
	\
	_EXPORT void uninit_driver() \
	{ \
		DriversCppPrivate::LegacyUninitDriver<DriverImpl>(); \
	} \
	\
	_EXPORT const char **publish_devices() \
	{ \
		return DriverImpl::PublishDevices(); \
	} \
	\
	_EXPORT device_hooks *find_device(const char *name) \
	{ \
		return DriversCppPrivate::LegacyFindDevice<DriverImpl>(name); \
	}
