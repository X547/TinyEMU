#pragma once

#include <OS.h>


// A wired area in the kernel team, deleted with the object. It can be mapped
// into a client team as well; that mapping is the client's to remove.
class KernelArea {
private:
	area_id fArea = -1;
	void *fAddress = nullptr;
	size_t fSize = 0;

public:
	KernelArea() = default;
	~KernelArea();

	KernelArea(const KernelArea &other) = delete;
	KernelArea &operator=(const KernelArea &other) = delete;

	// 'lock' is B_FULL_LOCK or B_CONTIGUOUS. The memory is zeroed.
	status_t Create(const char *name, size_t size, uint32 lock);
	status_t MapIntoTeam(team_id team, const char *name, uint64 &address) const;

	// The physical address of the page at 'offset'.
	status_t PhysicalAddress(size_t offset, phys_addr_t &address) const;

	area_id Id() const {return fArea;}
	void *Address() const {return fAddress;}
	size_t Size() const {return fSize;}
};
