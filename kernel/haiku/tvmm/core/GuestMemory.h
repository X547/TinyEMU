#pragma once

#include <SupportDefs.h>

#include <util/DoublyLinkedList.h>

#include "KernelArea.h"

class ArchVm;
struct TvmmMapRam;


// Guest RAM the kernel owns. Its pages are wired for its whole life, so the
// physical addresses the nested page tables hold stay valid.
class RamObject: public DoublyLinkedListLinkImpl<RamObject> {
private:
	struct PhysicalRun {
		uint64 offset;
		phys_addr_t address;
		uint64 size;
	};

	uint32 fId;
	KernelArea fArea;
	PhysicalRun *fRuns = nullptr;
	uint32 fRunCount = 0;
	// slots that map it
	uint32 fMapCount = 0;

	status_t CollectRuns();

public:
	explicit RamObject(uint32 id): fId(id) {}
	~RamObject();

	status_t Init(uint64 size);

	uint32 Id() const {return fId;}
	uint64 Size() const {return fArea.Size();}
	uint8 *KernelAddress() const {return static_cast<uint8*>(fArea.Address());}
	const KernelArea &Area() const {return fArea;}

	// The physical address of the page at 'offset', which is below Size().
	phys_addr_t PhysicalPage(uint64 offset) const;

	uint32 MapCount() const {return fMapCount;}
	void AddMapping() {fMapCount++;}
	void RemoveMapping() {fMapCount--;}
};


// The guest physical address space: which RAM backs which range. Called with
// the VM's lock held.
class GuestMemory {
public:
	static constexpr uint32 kMaxSlots = 256;
	// a dirty log of 64 GB
	static constexpr uint64 kMaxBitmapSize = 2 * 1024 * 1024;

private:
	struct Slot {
		uint64 gpa = 0;
		uint64 size = 0;
		uint64 offset = 0;
		RamObject *ram = nullptr;
		uint32 flags = 0;
		// with TvmmMapLogDirty: the pages written other than through the
		// nested page tables, by the kernel's emulation, one bit each
		uint64 *written = nullptr;
	};

	ArchVm *fArch = nullptr;
	DoublyLinkedList<RamObject> fRam;
	uint32 fNextRamId = 1;
	Slot fSlots[kMaxSlots];

	RamObject *FindRam(uint32 id) const;
	void UnmapSlot(Slot &slot);

public:
	~GuestMemory();

	void SetArch(ArchVm *arch) {fArch = arch;}

	status_t AllocRam(uint64 size, RamObject *&ram);
	status_t FreeRam(uint32 id);
	// The caller flushes the guest TLBs once it returns.
	status_t MapRam(const TvmmMapRam &args, bool &unmapped);

	// The kernel's address of guest RAM at 'gpa', for 'size' bytes that stay
	// within one slot; nullptr where there is none.
	uint8 *KernelAddress(uint64 gpa, uint64 size, bool write) const;
	// The kernel wrote guest RAM at 'gpa', as KernelAddress() gave it.
	void MarkWritten(uint64 gpa, uint64 size);

	// TvmmOpGetDirtyLog into 'bitmap', of 'words' 64 bit words, which must
	// be the slot's. 'flush' tells the caller to flush the guest TLBs.
	status_t TakeDirtyLog(uint32 slot, uint64 *bitmap, uint64 words, bool &flush);
};
