#include "GStage.h"

#include <stdlib.h>

#include <new>

#include <util/atomic.h>

#include "Common.h"
#include "KernelArea.h"
#include "RiscVCsr.h"


static constexpr uint64 kPteValid = 1ull << 0;
static constexpr uint64 kPteRead = 1ull << 1;
static constexpr uint64 kPteWrite = 1ull << 2;
static constexpr uint64 kPteExecute = 1ull << 3;
// G-stage accesses are all user accesses
static constexpr uint64 kPteUser = 1ull << 4;
static constexpr uint64 kPteAccessed = 1ull << 6;
static constexpr uint64 kPteDirty = 1ull << 7;
static constexpr int kPtePpnShift = 10;

static constexpr size_t kRootSize = 16 * 1024;


static uint64 PteFor(phys_addr_t phys)
{
	return (uint64(phys) >> 12) << kPtePpnShift;
}


GStage::~GStage()
{
	if (fRoot != nullptr) {
		FreeTable(fRoot, fLevels);
	}
	delete fRootArea;
	while (Chunk *chunk = fChunks.RemoveHead()) {
		delete chunk->area;
		delete chunk;
	}
}


void GStage::FreeTable(Table *table, int level)
{
	if (table->children != nullptr) {
		const uint32 count = level == fLevels ? kRootEntries : kEntries;
		for (uint32 i = 0; i < count; i++) {
			if (table->children[i] != nullptr) {
				FreeTable(table->children[i], level - 1);
			}
		}
		free(table->children);
	}
	delete table;
}


status_t GStage::Init(uint64 mode)
{
	if (mode == RiscV::kHgatpModeSv39x4) {
		fLevels = 3;
	} else if (mode == RiscV::kHgatpModeSv48x4) {
		fLevels = 4;
	} else {
		return B_NOT_SUPPORTED;
	}

	fRootArea = new(std::nothrow) KernelArea();
	fRoot = new(std::nothrow) Table();
	if (fRootArea == nullptr || fRoot == nullptr) {
		return B_NO_MEMORY;
	}
	fRoot->children = static_cast<Table**>(calloc(kRootEntries, sizeof(Table*)));
	if (fRoot->children == nullptr) {
		return B_NO_MEMORY;
	}
	CHECK_RET(fRootArea->CreateAligned("tvmm g-stage root", kRootSize, kRootSize));
	fRoot->entries = static_cast<uint64*>(fRootArea->Address());
	return fRootArea->PhysicalAddress(0, fRoot->phys);
}


uint64 GStage::Limit() const
{
	// 2 more bits than the guest's own translation of as many levels
	return 1ull << (12 + 9 * fLevels + 2);
}


// Table pages come from small physically contiguous chunks, so that the
// physical address of each is known without asking for it.
status_t GStage::AllocTable(bool leafLevel, Table *&table)
{
	Chunk *chunk = fChunks.Head();
	if (chunk == nullptr || chunk->used == kPagesPerChunk) {
		chunk = new(std::nothrow) Chunk();
		if (chunk == nullptr) {
			return B_NO_MEMORY;
		}
		chunk->area = new(std::nothrow) KernelArea();
		chunk->used = 0;
		if (chunk->area == nullptr) {
			delete chunk;
			return B_NO_MEMORY;
		}
		const status_t res = chunk->area->Create("tvmm g-stage", kPagesPerChunk * kPageSize, B_CONTIGUOUS);
		if (res < B_OK) {
			delete chunk->area;
			delete chunk;
			return res;
		}
		fChunks.Insert(chunk, false);
	}

	auto *newTable = new(std::nothrow) Table();
	if (newTable == nullptr) {
		return B_NO_MEMORY;
	}
	newTable->children = nullptr;
	if (!leafLevel) {
		newTable->children = static_cast<Table**>(calloc(kEntries, sizeof(Table*)));
		if (newTable->children == nullptr) {
			delete newTable;
			return B_NO_MEMORY;
		}
	}
	const size_t offset = chunk->used * kPageSize;
	phys_addr_t phys;
	const status_t res = chunk->area->PhysicalAddress(offset, phys);
	if (res < B_OK) {
		free(newTable->children);
		delete newTable;
		return res;
	}
	newTable->entries = reinterpret_cast<uint64*>(static_cast<uint8*>(chunk->area->Address()) + offset);
	newTable->phys = phys;
	chunk->used++;
	table = newTable;
	return B_OK;
}


status_t GStage::Walk(uint64 gpa, bool create, uint64 *&entry)
{
	Table *table = fRoot;
	for (int level = fLevels; level > 1; level--) {
		// the root's index has 2 bits more than the others'
		const uint32 entries = level == fLevels ? kRootEntries : kEntries;
		const uint32 index = (gpa >> (12 + 9 * (level - 1))) & (entries - 1);
		Table *child = table->children[index];
		if (child == nullptr) {
			if (!create) {
				entry = nullptr;
				return B_ENTRY_NOT_FOUND;
			}
			CHECK_RET(AllocTable(level == 2, child));
			// The table is complete before the hart or another walk can see
			// it.
			atomic_pointer_set(&table->children[index], child);
			atomic_set64(reinterpret_cast<int64*>(&table->entries[index]),
				int64(PteFor(child->phys) | kPteValid));
		}
		table = child;
	}
	entry = &table->entries[(gpa >> 12) & (kEntries - 1)];
	return B_OK;
}


status_t GStage::Map(uint64 gpa, phys_addr_t hpa, bool writable)
{
	uint64 *entry = nullptr;
	CHECK_RET(Walk(gpa, true, entry));
	uint64 value = PteFor(hpa) | kPteValid | kPteRead | kPteExecute | kPteUser
		| kPteAccessed | kPteDirty;
	if (writable) {
		value |= kPteWrite;
	}
	atomic_set64(reinterpret_cast<int64*>(entry), int64(value));
	return B_OK;
}


void GStage::Unmap(uint64 gpa)
{
	uint64 *entry = nullptr;
	if (Walk(gpa, false, entry) < B_OK) {
		return;
	}
	atomic_set64(reinterpret_cast<int64*>(entry), 0);
}


// Only what is still mapped gets W, should the page be unmapped meanwhile.
bool GStage::SetWritable(uint64 gpa)
{
	uint64 *entry = nullptr;
	if (Walk(gpa, false, entry) < B_OK) {
		return false;
	}
	auto *value = reinterpret_cast<int64*>(entry);
	for (;;) {
		const int64 old = atomic_get64(value);
		if ((uint64(old) & kPteValid) == 0) {
			return false;
		}
		if ((uint64(old) & kPteWrite) != 0
			|| atomic_test_and_set64(value, int64(uint64(old) | kPteWrite), old) == old) {
			return true;
		}
	}
}


bool GStage::TakeDirty(uint64 gpa, uint64 pages, uint64 *bitmap)
{
	bool any = false;
	for (uint64 i = 0; i < pages; i++) {
		uint64 *entry = nullptr;
		if (Walk(gpa + i * kPageSize, false, entry) < B_OK) {
			continue;
		}
		auto *value = reinterpret_cast<int64*>(entry);
		if ((uint64(atomic_get64(value)) & kPteWrite) == 0) {
			continue;
		}
		if ((uint64(atomic_and64(value, ~int64(kPteWrite))) & kPteWrite) != 0) {
			bitmap[i / 64] |= 1ull << (i % 64);
			any = true;
		}
	}
	return any;
}
