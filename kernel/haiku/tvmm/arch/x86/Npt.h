#pragma once

#include <SupportDefs.h>

#include <util/DoublyLinkedList.h>

class KernelArea;


// Nested page tables: guest physical to host physical addresses, in the
// host's four level long mode format, with 4 KB pages. Tables are kept until
// the object goes, so a walk the processor makes concurrently never reads a
// freed one.
class Npt {
private:
	static constexpr uint32 kEntries = 512;
	static constexpr uint32 kPagesPerChunk = 16;

	struct Table {
		uint64 *entries;
		phys_addr_t phys;
		// levels above the last: the tables the entries point to
		Table **children;
	};

	struct Chunk: DoublyLinkedListLinkImpl<Chunk> {
		KernelArea *area;
		uint32 used;
	};

	DoublyLinkedList<Chunk> fChunks;
	Table *fRoot = nullptr;

	status_t AllocTable(bool leafLevel, Table *&table);
	void FreeTable(Table *table, int level);
	status_t Walk(uint64 gpa, bool create, uint64 *&entry);

public:
	~Npt();

	status_t Init();
	phys_addr_t RootPhys() const {return fRoot->phys;}

	status_t Map(uint64 gpa, phys_addr_t hpa, bool writable);
	void Unmap(uint64 gpa);
};
