/*
 * Guest memory map backed by a host hypervisor
 *
 * Copyright (c) 2011-2017 Fabrice Bellard
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hypervisor_memory_map.h"


/* XXX: should check overlapping mappings */
void HypervisorPhysMemoryMap::MapRam(PhysMemoryRange *pr)
{
    fHypervisor.MapRam(IndexOfRange(pr), pr->addr, pr->size, pr->phys_mem,
                       (pr->devram_flags & DEVRAM_FLAG_ROM) != 0,
                       (pr->devram_flags & DEVRAM_FLAG_DIRTY_BITS) != 0);
}

/* The base destructor cannot reach FreeRam() here, so the memory goes back
   to the hypervisor first. */
HypervisorPhysMemoryMap::~HypervisorPhysMemoryMap()
{
    for (int i = 0; i < RangeCount(); i++) {
        PhysMemoryRange *pr = RangeAt(i);
        if (pr->is_ram && pr->phys_mem != nullptr) {
            fHypervisor.FreeRam(pr->phys_mem, pr->org_size);
            pr->phys_mem = nullptr;
        }
    }
}

PhysMemoryRange *HypervisorPhysMemoryMap::RegisterRam(uint64_t addr,
                                                      uint64_t size,
                                                      int devram_flags)
{
    PhysMemoryRange *pr;

    pr = RegisterRamEntry(addr, size, devram_flags);

    pr->phys_mem = fHypervisor.AllocRam(size);
    if (pr->phys_mem == nullptr) {
        fprintf(stderr, "Could not allocate VM memory\n");
        exit(1);
    }
    if (devram_flags & DEVRAM_FLAG_DIRTY_BITS) {
        /* The hypervisor fills whole 64 bit words of the second bitmap with
           what the guest wrote; the first holds what devices wrote. */
        int n_pages = size >> DEVRAM_PAGE_SIZE_LOG2;
        pr->dirty_bits_size = ((n_pages + 63) / 64) * 8;
        for (int i = 0; i < 2; i++) {
            pr->dirty_bits_tab[i] = std::make_unique<uint32_t[]>(
                pr->dirty_bits_size / sizeof(uint32_t));
        }
        pr->dirty_bits = pr->dirty_bits_tab[0].get();
    }

    if (pr->size != 0) {
        MapRam(pr);
    }
    return pr;
}

void HypervisorPhysMemoryMap::SetRamAddr(PhysMemoryRange *pr, uint64_t addr,
                                         bool enabled)
{
    if (enabled) {
        if (pr->size == 0 || addr != pr->addr) {
            /* move or create the region */
            pr->size = pr->org_size;
            pr->addr = addr;
            MapRam(pr);
        }
    } else {
        if (pr->size != 0) {
            pr->addr = 0;
            pr->size = 0;
            /* map a zero size region to disable */
            MapRam(pr);
        }
    }
}

/* The pages the guest wrote, from the hypervisor's log, and those devices
   wrote, such as a 2D engine drawing into video memory. */
const uint32_t *HypervisorPhysMemoryMap::GetDirtyBits(PhysMemoryRange *pr)
{
    uint32_t *log = pr->dirty_bits_tab[1].get();

    if (pr->size == 0) {
        /* not mapped: the guest wrote nothing */
        memset(log, 0, pr->dirty_bits_size);
    } else {
        fHypervisor.GetDirtyLog(IndexOfRange(pr), log);
    }
    size_t n = pr->dirty_bits_size / sizeof(uint32_t);
    for (size_t i = 0; i < n; i++)
        log[i] |= __atomic_exchange_n(&pr->dirty_bits[i], 0, __ATOMIC_ACQ_REL);
    return log;
}
