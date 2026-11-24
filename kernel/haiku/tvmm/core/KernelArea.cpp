#include "KernelArea.h"

#include <string.h>

#include <KernelExport.h>

#include <vm/vm.h>

#include "Common.h"


KernelArea::~KernelArea()
{
	if (fArea >= 0) {
		delete_area(fArea);
	}
}


status_t KernelArea::Create(const char *name, size_t size, uint32 lock)
{
	size = RoundUpToPage(size);
	void *address = nullptr;
	const area_id area = create_area(
		name,
		&address,
		B_ANY_KERNEL_ADDRESS,
		size,
		lock,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA
	);
	CHECK_RET(area);
	fArea = area;
	fAddress = address;
	fSize = size;
	// An anonymous area's pages come zeroed, but the guest relies on it, so
	// it is not left to that.
	memset(fAddress, 0, fSize);
	return B_OK;
}


status_t KernelArea::CreateAligned(const char *name, size_t size, size_t alignment)
{
	size = RoundUpToPage(size);
	virtual_address_restrictions virtualRestrictions = {};
	virtualRestrictions.address_specification = B_ANY_KERNEL_ADDRESS;
	physical_address_restrictions physicalRestrictions = {};
	physicalRestrictions.alignment = alignment;
	void *address = nullptr;
	const area_id area = create_area_etc(
		B_SYSTEM_TEAM,
		name,
		size,
		B_CONTIGUOUS,
		B_KERNEL_READ_AREA | B_KERNEL_WRITE_AREA,
		0,
		0,
		&virtualRestrictions,
		&physicalRestrictions,
		&address
	);
	CHECK_RET(area);
	fArea = area;
	fAddress = address;
	fSize = size;
	memset(fAddress, 0, fSize);
	return B_OK;
}


status_t KernelArea::MapIntoTeam(team_id team, const char *name, uint64 &address) const
{
	void *clientAddress = nullptr;
	const area_id clone = vm_clone_area(
		team,
		name,
		&clientAddress,
		B_ANY_ADDRESS,
		B_READ_AREA | B_WRITE_AREA,
		REGION_NO_PRIVATE_MAP,
		fArea,
		true
	);
	CHECK_RET(clone);
	address = reinterpret_cast<addr_t>(clientAddress);
	return B_OK;
}


status_t KernelArea::PhysicalAddress(size_t offset, phys_addr_t &address) const
{
	if (offset >= fSize) {
		return B_BAD_VALUE;
	}
	physical_entry entry;
	uint32 count = 1;
	CHECK_RET(get_memory_map_etc(
		B_SYSTEM_TEAM,
		static_cast<uint8*>(fAddress) + (offset & ~(kPageSize - 1)),
		kPageSize,
		&entry,
		&count
	));
	address = entry.address;
	return B_OK;
}
