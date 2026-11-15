#include "GuestMemory.h"

#include <stdlib.h>
#include <string.h>

#include <new>

#include <KernelExport.h>

#include <util/AutoLock.h>

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


// Readers see the slot no more once this takes it for writing; none is left
// using its RAM then, which the slot's removal lets go.
void GuestMemory::UnmapSlot(Slot &slot)
{
	if (slot.size == 0) {
		return;
	}
	Slot old;
	{
		InterruptsWriteSpinLocker locker(fSlotLock);
		old = slot;
		slot = Slot();
	}
	fArch->UnmapPages(old.gpa, old.size);
	old.ram->RemoveMapping();
	free(old.written);
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

	// The slot is there before its pages: a nested page fault on one of them
	// meanwhile finds RAM, and the guest tries again until it is mapped.
	Slot mapped;
	mapped.gpa = args.gpa;
	mapped.size = args.size;
	mapped.offset = args.offset;
	mapped.ram = ram;
	mapped.flags = args.flags;
	mapped.written = written;
	ram->AddMapping();
	{
		InterruptsWriteSpinLocker locker(fSlotLock);
		slot = mapped;
	}
	const status_t res = fArch->MapPages(args.gpa, args.size, *ram, args.offset, args.flags);
	if (res < B_OK) {
		UnmapSlot(slot);
		unmapped = true;
		return res;
	}
	return B_OK;
}


// Logged with a locked update, as TakeDirtyLog() takes the words.
void GuestMemory::MarkWritten(const Slot &slot, uint64 gpa, uint64 size)
{
	if (slot.written == nullptr) {
		return;
	}
	const uint64 first = (gpa - slot.gpa) / kPageSize;
	const uint64 last = (gpa - slot.gpa + size - 1) / kPageSize;
	for (uint64 page = first; page <= last; page++) {
		atomic_or64(reinterpret_cast<int64*>(&slot.written[page / 64]), int64(1ull << (page % 64)));
	}
}


status_t GuestMemory::TakeDirtyLog(uint32 index, uint64 *bitmap, uint64 words, bool &flush)
{
	flush = false;
	if (index >= kMaxSlots) {
		return B_BAD_VALUE;
	}
	// with the VM's lock, the slot stays as it is
	const Slot &slot = fSlots[index];
	if (slot.written == nullptr || words != BitmapWords(slot.size)) {
		return B_BAD_VALUE;
	}
	for (uint64 i = 0; i < words; i++) {
		bitmap[i] = uint64(atomic_get_and_set64(reinterpret_cast<int64*>(&slot.written[i]), 0));
	}
	flush = fArch->TakeDirty(slot.gpa, slot.size, bitmap);
	return B_OK;
}


// With the slots' lock held.
const GuestMemory::Slot *GuestMemory::FindSlot(uint64 gpa, uint64 size, bool write) const
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
		return &slot;
	}
	return nullptr;
}


uint8 *GuestMemory::KernelAddress(const Slot &slot, uint64 gpa)
{
	return slot.ram->KernelAddress() + slot.offset + (gpa - slot.gpa);
}


bool GuestMemory::IsRam(uint64 gpa, bool write)
{
	InterruptsReadSpinLocker locker(fSlotLock);
	return FindSlot(gpa, 1, write) != nullptr;
}


// The RAM is wired, so the copy faults on nothing.
bool GuestMemory::Read(uint64 gpa, void *buffer, size_t size)
{
	InterruptsReadSpinLocker locker(fSlotLock);
	const Slot *slot = FindSlot(gpa, size, false);
	if (slot == nullptr) {
		return false;
	}
	memcpy(buffer, KernelAddress(*slot, gpa), size);
	return true;
}


bool GuestMemory::Write(uint64 gpa, const void *buffer, size_t size)
{
	InterruptsReadSpinLocker locker(fSlotLock);
	const Slot *slot = FindSlot(gpa, size, true);
	if (slot == nullptr) {
		return false;
	}
	memcpy(KernelAddress(*slot, gpa), buffer, size);
	MarkWritten(*slot, gpa, size);
	return true;
}
