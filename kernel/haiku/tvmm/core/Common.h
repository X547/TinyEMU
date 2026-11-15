#pragma once

#include <KernelExport.h>
#include <SupportDefs.h>

#include <kernel.h>


// The kernel is built without exceptions, so failure travels as status_t,
// returned early at each step.
#define CHECK_RET(err) {status_t _err = (err); if (_err < B_OK) return _err;}


#define TVMM_LOG(format...) dprintf("tvmm: " format)


constexpr size_t kPageSize = B_PAGE_SIZE;


inline constexpr uint64 RoundUpToPage(uint64 value)
{
	return (value + kPageSize - 1) & ~uint64(kPageSize - 1);
}


// A field of the run page the client may change at any time, read once.
template <typename T>
inline T ReadOnce(const T &field)
{
	return *static_cast<const volatile T*>(&field);
}


// Copies an operation's argument structure in from the client, requiring the
// size the client passed to be the structure's.
template <typename Args>
status_t CopyArgsIn(Args &args, const void *data, size_t length)
{
	if (length != sizeof(Args) || data == nullptr) {
		return B_BAD_VALUE;
	}
	if (!IS_USER_ADDRESS(data)) {
		return B_BAD_ADDRESS;
	}
	return user_memcpy(&args, data, sizeof(Args));
}


template <typename Args>
status_t CopyArgsOut(void *data, const Args &args)
{
	return user_memcpy(data, &args, sizeof(Args));
}
