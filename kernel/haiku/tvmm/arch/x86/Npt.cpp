#include "Npt.h"

#include <stdlib.h>

#include <new>

#include <util/atomic.h>

#include "Common.h"
#include "KernelArea.h"


static constexpr uint64 kPtePresent = 1ull << 0;
static constexpr uint64 kPteWritable = 1ull << 1;
// The processor walks nested tables as user accesses.
static constexpr uint64 kPteUser = 1ull << 2;
static constexpr uint64 kPteAccessed = 1ull << 5;
static constexpr uint64 kPteDirty = 1ull << 6;
static constexpr uint64 kPteAddressMask = 0x000ffffffffff000ull;

static constexpr int kLevels = 4;


Npt::~Npt()
{
	if (fRoot != nullptr) {
		FreeTable(fRoot, kLevels);
	}
	while (Chunk *chunk = fChunks.RemoveHead()) {
		delete chunk->area;
		delete chunk;
	}
}


void Npt::FreeTable(Table *table, int level)
{
	if (table->children != nullptr) {
		for (uint32 i = 0; i < kEntries; i++) {
			if (table->children[i] != nullptr) {
				FreeTable(table->children[i], level - 1);
			}
		}
		free(table->children);
	}
	delete table;
}


status_t Npt::Init()
{
	return AllocTable(false, fRoot);
}


// Table pages come from small physically contiguous chunks, so that the
// physical address of each is known without asking for it.
status_t Npt::AllocTable(bool leafLevel, Table *&table)
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
		const status_t res = chunk->area->Create("tvmm npt", kPagesPerChunk * kPageSize, B_CONTIGUOUS);
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


status_t Npt::Walk(uint64 gpa, bool create, uint64 *&entry)
{
	Table *table = fRoot;
	for (int level = kLevels; level > 1; level--) {
		const uint32 index = (gpa >> (12 + 9 * (level - 1))) & (kEntries - 1);
		Table *child = table->children[index];
		if (child == nullptr) {
			if (!create) {
				entry = nullptr;
				return B_ENTRY_NOT_FOUND;
			}
			CHECK_RET(AllocTable(level == 2, child));
			table->children[index] = child;
			// The table is complete before the processor can see it.
			atomic_set64(reinterpret_cast<int64*>(&table->entries[index]),
				child->phys | kPtePresent | kPteWritable | kPteUser | kPteAccessed);
		}
		table = child;
	}
	entry = &table->entries[(gpa >> 12) & (kEntries - 1)];
	return B_OK;
}


status_t Npt::Map(uint64 gpa, phys_addr_t hpa, bool writable)
{
	uint64 *entry = nullptr;
	CHECK_RET(Walk(gpa, true, entry));
	uint64 value = (hpa & kPteAddressMask) | kPtePresent | kPteUser | kPteAccessed;
	if (writable) {
		value |= kPteWritable;
	}
	atomic_set64(reinterpret_cast<int64*>(entry), value);
	return B_OK;
}


void Npt::Unmap(uint64 gpa)
{
	uint64 *entry = nullptr;
	if (Walk(gpa, false, entry) < B_OK) {
		return;
	}
	atomic_set64(reinterpret_cast<int64*>(entry), 0);
}


// The processor sets the dirty bit with a locked update, so clearing it the
// same way loses no write that comes meanwhile.
bool Npt::TakeDirty(uint64 gpa, uint64 pages, uint64 *bitmap)
{
	bool any = false;
	for (uint64 i = 0; i < pages; i++) {
		uint64 *entry = nullptr;
		if (Walk(gpa + i * kPageSize, false, entry) < B_OK) {
			continue;
		}
		auto *value = reinterpret_cast<int64*>(entry);
		if ((uint64(atomic_get64(value)) & kPteDirty) == 0) {
			continue;
		}
		if ((uint64(atomic_and64(value, ~int64(kPteDirty))) & kPteDirty) != 0) {
			bitmap[i / 64] |= 1ull << (i % 64);
			any = true;
		}
	}
	return any;
}
