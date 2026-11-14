#pragma once

#include <module.h>


// Helpers shared by the driver API wrappers. Not part of the public interface.
namespace DriversCppPrivate {


template <typename Impl, typename Cookie>
inline Impl *FromCookie(Cookie *cookie)
{
	return reinterpret_cast<Impl*>(cookie);
}


template <typename Cookie, typename Impl>
inline Cookie *ToCookie(Impl *impl)
{
	return reinterpret_cast<Cookie*>(impl);
}


template <typename Impl>
status_t ModuleInit()
{
	if constexpr (requires {Impl::ModuleInit();}) {
		return Impl::ModuleInit();
	} else {
		return B_OK;
	}
}


template <typename Impl>
status_t ModuleUninit()
{
	if constexpr (requires {Impl::ModuleUninit();}) {
		return Impl::ModuleUninit();
	} else {
		return B_OK;
	}
}


// A function template rather than a lambda: GCC can not convert a C-variadic lambda
// to a function pointer.
template <typename Impl>
status_t StdOps(int32 op, ...)
{
	status_t res = B_ERROR;
	switch (op) {
		case B_MODULE_INIT:
			res = ModuleInit<Impl>();
			break;
		case B_MODULE_UNINIT:
			res = ModuleUninit<Impl>();
			break;
		default:
			break;
	}
	return res;
}


// Default release of an object created by a static `Init*`/`Open` method, used when
// `Impl` has no release method of its own (`UninitDriver`, `Free`, ...).
template <typename Impl, typename Cookie>
inline void Delete(Cookie *cookie)
{
	delete FromCookie<Impl>(cookie);
}


} // DriversCppPrivate


// Builds `module_info`. `std_ops` forwards `B_MODULE_INIT` and `B_MODULE_UNINIT` to
// optional `static status_t Impl::ModuleInit()` and `Impl::ModuleUninit()`.
template <typename Impl>
constexpr module_info GetModuleInfo(const char *name, uint32 flags = 0)
{
	module_info info {};
	info.name = name;
	info.flags = flags;
	info.std_ops = DriversCppPrivate::StdOps<Impl>;
	return info;
}
