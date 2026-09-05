/*
 * Device resource records and allocation
 *
 * Copyright (c) 2016-2018 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#pragma once

#include <stdint.h>

/* A PCI host bridge is the greediest device: an ECAM window, a memory
   aperture and one line per INTx pin. */
#define RESOURCE_MAX_PER_DEVICE 8
#define ALLOCATOR_MAX_ENTRIES 128


typedef enum {
    RES_NONE,
    RES_MMIO, /* host physical address space */
    RES_IRQ,  /* interrupt controller input line */
} ResourceTypeEnum;


/* One resource a device needs. The device fills in type/size/align (and base
   when the placement is architectural); the bus fills in base and marks it
   assigned. Devices read back 'base' in Realize() and when emitting FDT, so
   the mapping and its description can never drift apart. */
struct Resource {
    ResourceTypeEnum type = RES_NONE;
    const char *name = nullptr;
    uint64_t base = 0;
    uint64_t size = 0;
    uint64_t align = 0; /* 0 means "natural": align to size */
    bool fixed = false; /* base is preassigned and must be honoured */
    bool assigned = false;

    uint64_t End() const {return base + size;}
};


/* A claimed interval, kept for overlap reporting. */
struct AllocEntry {
    uint64_t base;
    uint64_t size;
    const char *owner;
};


/* Tracks one linear space: the host MMIO map, or the interrupt controller's
   input lines. Only statically placed resources go in here. Guest-programmed
   PCI BARs are deliberately absent: the guest may map them wherever it likes,
   including on top of RAM, and the PCI host bridge instead reserves the whole
   aperture it advertises so that the aperture itself cannot overlap anything
   static. */
class RangeAllocator {
private:
    const char *fName;
    uint64_t fWindowBase = 0;
    uint64_t fWindowSize = 0;
    int fCount = 0;
    AllocEntry fEntries[ALLOCATOR_MAX_ENTRIES] {};

    int InsertIndex(uint64_t base) const;
    bool Insert(int index, uint64_t base, uint64_t size, const char *owner);

public:
    RangeAllocator(const char *name): fName(name) {}

    /* Region new dynamic allocations are taken from. Claims outside it are
       still allowed; they just are not candidates for Alloc(). */
    void SetWindow(uint64_t base, uint64_t size);

    /* Reserve a fixed interval. Returns false and reports the conflicting
       owner if it overlaps something already claimed. */
    bool Claim(uint64_t base, uint64_t size, const char *owner);

    /* Place 'size' bytes inside the window. 'align' of 0 means natural
       alignment. Returns false if the window is exhausted. */
    bool Alloc(uint64_t size, uint64_t align, const char *owner,
               uint64_t *base_out);

    /* Assign one resource record, honouring res->fixed. */
    bool Assign(Resource *res, const char *owner);

    const AllocEntry *FindOverlap(uint64_t base, uint64_t size) const;

    int Count() const {return fCount;}
    const AllocEntry &At(int index) const {return fEntries[index];}

    void Dump() const;
};
