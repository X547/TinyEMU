#include "GuestMemory.h"

#include <stdlib.h>
#include <string.h>

#include <new>

#include <KernelExport.h>

#include "Arch.h"
#include "Common.h"
#include "tvmm_abi.h"


//#pragma mark - RamObject

RamObject::~RamObject()
{
	free(fRuns);
}


status_t RamObject::Init(uint64 size)
{
	if (size == 0 || size != RoundUpToPage(size)) {
		return B_BAD_VALUE;
	}
	CHECK_RET(fArea.Create("tvmm guest ram", size, B_FULL_LOCK));
	return CollectRuns();
}


// The area is wired, so its pages are where they are until it goes.
status_t RamObject::CollectRuns()
{
	uint32 capacity = 16;
	for (;;) {
		auto *entries = static_cast<physical_entry*>(malloc(capacity * sizeof(physical_entry)));
		if (entries == nullptr) {
			return B_NO_MEMORY;
		}
		uint32 count = capacity;
		const status_t res = get_memory_map_etc(B_SYSTEM_TEAM, fArea.Address(), fArea.Size(), entries, &count);
		if (res == B_BUFFER_OVERFLOW) {
			free(entries);
			capacity *= 4;
			continue;
		}
		if (res < B_OK) {
			free(entries);
			return res;
		}

		fRuns = static_cast<PhysicalRun*>(malloc(count * sizeof(PhysicalRun)));
		if (fRuns == nullptr) {
			free(entries);
			return B_NO_MEMORY;
		}
		uint64 offset = 0;
		for (uint32 i = 0; i < count; i++) {
			fRuns[i] = {offset, entries[i].address, entries[i].size};
			offset += entries[i].size;
		}
		fRunCount = count;
		free(entries);
		if (offset != fArea.Size()) {
			return B_ERROR;
		}
		return B_OK;
	}
}


phys_addr_t RamObject::PhysicalPage(uint64 offset) const
{
	uint32 low = 0;
	uint32 high = fRunCount;
	while (high - low > 1) {
		const uint32 mid = (low + high) / 2;
		if (fRuns[mid].offset <= offset) {
			low = mid;
		} else {
			high = mid;
		}
	}
	const PhysicalRun &run = fRuns[low];
	return run.address + (offset - run.offset);
}


//#pragma mark - GuestMemory

// A dirty log's size: one bit per page, in 64 bit words.
static uint64 BitmapWords(uint64 size)
{
	return (size / kPageSize + 63) / 64;
}


GuestMemory::~GuestMemory()
{
	for (Slot &slot: fSlots) {
		free(slot.written);
	}
	while (RamObject *ram = fRam.RemoveHead()) {
		delete ram;
	}
}


RamObject *GuestMemory::FindRam(uint32 id) const
{
	for (auto it = fRam.GetIterator(); RamObject *ram = it.Next();) {
		if (ram->Id() == id) {
			return ram;
		}
	}
	return nullptr;
}


status_t GuestMemory::AllocRam(uint64 size, RamObject *&ram)
{
	auto *newRam = new(std::nothrow) RamObject(fNextRamId);
	if (newRam == nullptr) {
		return B_NO_MEMORY;
	}
	const status_t res = newRam->Init(size);
	if (res < B_OK) {
		delete newRam;
		return res;
	}
	fNextRamId++;
	fRam.Add(newRam);
	ram = newRam;
	return B_OK;
}


status_t GuestMemory::FreeRam(uint32 id)
{
	RamObject *ram = FindRam(id);
	if (ram == nullptr) {
		return B_BAD_VALUE;
	}
	if (ram->MapCount() != 0) {
		return B_BUSY;
	}
	fRam.Remove(ram);
	delete ram;
	return B_OK;
}


void GuestMemory::UnmapSlot(Slot &slot)
{
	if (slot.size == 0) {
		return;
	}
	fArch->UnmapPages(slot.gpa, slot.size);
	slot.ram->RemoveMapping();
	free(slot.written);
	slot = Slot();
}


status_t GuestMemory::MapRam(const TvmmMapRam &args, bool &unmapped)
{
	unmapped = false;
	if (args.slot >= kMaxSlots) {
		return B_BAD_VALUE;
	}
	Slot &slot = fSlots[args.slot];
	if ((args.flags & ~uint32(TvmmMapReadOnly | TvmmMapLogDirty)) != 0) {
		return B_BAD_VALUE;
	}

	RamObject *ram = nullptr;
	uint64 *written = nullptr;
	if (args.size != 0) {
		ram = FindRam(args.ramId);
		if (ram == nullptr) {
			return B_BAD_VALUE;
		}
		const uint64 pageMask = kPageSize - 1;
		if (((args.gpa | args.size | args.offset) & pageMask) != 0
			|| args.offset > ram->Size() || args.size > ram->Size() - args.offset
			|| args.gpa + args.size < args.gpa) {
			return B_BAD_VALUE;
		}
		for (const Slot &other: fSlots) {
			if (&other != &slot && other.size != 0
				&& args.gpa < other.gpa + other.size && other.gpa < args.gpa + args.size) {
				return B_BAD_VALUE;
			}
		}
		if ((args.flags & TvmmMapLogDirty) != 0) {
			// a slot logged from now on was written as far as anyone knows
			const uint64 words = BitmapWords(args.size);
			written = static_cast<uint64*>(malloc(words * sizeof(uint64)));
			if (written == nullptr) {
				return B_NO_MEMORY;
			}
			// the bits of the slot's pages, none past them
			memset(written, 0xff, words * sizeof(uint64));
			const uint64 pages = args.size / kPageSize;
			if (pages % 64 != 0) {
				written[words - 1] = (1ull << (pages % 64)) - 1;
			}
		}
	}

	if (slot.size != 0) {
		UnmapSlot(slot);
		unmapped = true;
	}
	if (args.size == 0) {
		return B_OK;
	}

	const status_t res = fArch->MapPages(args.gpa, args.size, *ram, args.offset, args.flags);
	if (res < B_OK) {
		fArch->UnmapPages(args.gpa, args.size);
		free(written);
		unmapped = true;
		return res;
	}
	ram->AddMapping();
	slot.gpa = args.gpa;
	slot.size = args.size;
	slot.offset = args.offset;
	slot.ram = ram;
	slot.flags = args.flags;
	slot.written = written;
	return B_OK;
}


void GuestMemory::MarkWritten(uint64 gpa, uint64 size)
{
	for (Slot &slot: fSlots) {
		if (slot.written == nullptr || gpa < slot.gpa || gpa - slot.gpa >= slot.size) {
			continue;
		}
		const uint64 first = (gpa - slot.gpa) / kPageSize;
		const uint64 last = (gpa - slot.gpa + size - 1) / kPageSize;
		for (uint64 page = first; page <= last; page++) {
			slot.written[page / 64] |= 1ull << (page % 64);
		}
		return;
	}
}


status_t GuestMemory::TakeDirtyLog(uint32 index, uint64 *bitmap, uint64 words, bool &flush)
{
	flush = false;
	if (index >= kMaxSlots) {
		return B_BAD_VALUE;
	}
	Slot &slot = fSlots[index];
	if (slot.written == nullptr || words != BitmapWords(slot.size)) {
		return B_BAD_VALUE;
	}
	memcpy(bitmap, slot.written, words * sizeof(uint64));
	memset(slot.written, 0, words * sizeof(uint64));
	flush = fArch->TakeDirty(slot.gpa, slot.size, bitmap);
	return B_OK;
}


uint8 *GuestMemory::KernelAddress(uint64 gpa, uint64 size, bool write) const
{
	for (const Slot &slot: fSlots) {
		if (slot.size == 0 || gpa < slot.gpa || gpa - slot.gpa >= slot.size) {
			continue;
		}
		if (size > slot.size - (gpa - slot.gpa)) {
			return nullptr;
		}
		if (write && (slot.flags & TvmmMapReadOnly) != 0) {
			return nullptr;
		}
		return slot.ram->KernelAddress() + slot.offset + (gpa - slot.gpa);
	}
	return nullptr;
}
