#pragma once

#include <SupportDefs.h>

#include <util/DoublyLinkedList.h>

class KernelArea;


// The G-stage tables: guest physical to host physical addresses, Sv39x4 or
// Sv48x4, with 4 KB pages. Tables are kept until the object goes, so that a
// walk the hart makes meanwhile never reads a freed one.
//
// Without Svadu the hart faults rather than set an A or D bit, so leaves
// have both set. A writable page of a slot with a dirty log is mapped
// without W after its log is taken, and gets W back on its first write: the
// pages with W are the ones written.
class GStage {
private:
	static constexpr uint32 kEntries = 512;
	static constexpr uint32 kRootEntries = 2048;
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

	// 3 for Sv39x4, 4 for Sv48x4
	int fLevels = 0;
	DoublyLinkedList<Chunk> fChunks;
	// 16 KB, aligned to that
	KernelArea *fRootArea = nullptr;
	Table *fRoot = nullptr;

	status_t AllocTable(bool leafLevel, Table *&table);
	void FreeTable(Table *table, int level);
	status_t Walk(uint64 gpa, bool create, uint64 *&entry);

public:
	~GStage();

	// 'mode' is hgatp's, kHgatpModeSv39x4 or kHgatpModeSv48x4.
	status_t Init(uint64 mode);
	phys_addr_t RootPhys() const {return fRoot->phys;}
	// the guest physical addresses the tables cover
	uint64 Limit() const;

	status_t Map(uint64 gpa, phys_addr_t hpa, bool writable);
	void Unmap(uint64 gpa);
	// The guest wrote to a page of a writable slot that is mapped without
	// W; any thread. False when no page is mapped there.
	bool SetWritable(uint64 gpa);
	// Sets the bit of each of 'pages' pages from 'gpa' that has W, and takes
	// it away. True when one had it; the guest's TLBs may then still let it
	// write.
	bool TakeDirty(uint64 gpa, uint64 pages, uint64 *bitmap);
};
