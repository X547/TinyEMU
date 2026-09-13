/*
 * IO memory handling
 *
 * Copyright (c) 2016-2017 Fabrice Bellard
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
#include "iomem.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#ifdef __HAIKU__
#include <OS.h>
#else
#include <sys/mman.h>
#endif

#include "cutils.h"


//#pragma mark - PhysMemoryRange

const uint32_t *PhysMemoryRange::DirtyBits()
{
    return map->GetDirtyBits(this);
}


/* reset the dirty bit of one page at 'offset' inside this range */
void PhysMemoryRange::ResetDirtyBit(size_t offset)
{
    if (dirty_bits == nullptr) {
        return;
    }
    size_t page_index = offset >> DEVRAM_PAGE_SIZE_LOG2;
    uint32_t mask = 1 << (page_index & 0x1f);
    uint32_t *dirty_bits_ptr = dirty_bits + (page_index >> 5);
    if ((*dirty_bits_ptr & mask) == 0) {
        return;
    }
    *dirty_bits_ptr &= ~mask;
    /* invalidate the corresponding CPU write TLBs */
    map->FlushTlbWriteRange(phys_mem + (offset & ~(DEVRAM_PAGE_SIZE - 1)),
                            DEVRAM_PAGE_SIZE);
}


void PhysMemoryRange::SetAddr(uint64_t addr, bool enabled)
{
    if (!is_ram) {
        /* device ranges never need the RAM-specific mapping hooks */
        map->PhysMemoryMap::SetRamAddr(this, addr, enabled);
    } else {
        map->SetRamAddr(this, addr, enabled);
    }
}


//#pragma mark - PhysMemoryMap

PhysMemoryMap::~PhysMemoryMap()
{
    for (int i = 0; i < fRangeCount; i++) {
        PhysMemoryRange *pr = &fRanges[i];
        if (pr->is_ram) {
            FreeRam(pr);
        }
    }
}


void PhysMemoryMap::FlushTlbWriteRange(uint8_t *ram_addr, size_t ram_size)
{
    if (fTlbFlushTarget == nullptr) {
        return;
    }
    fTlbFlushTarget->FlushTlbWriteRange(ram_addr, ram_size);
}


/* XXX: optimize */
PhysMemoryRange *PhysMemoryMap::FindRange(uint64_t paddr)
{
    for (int i = 0; i < fRangeCount; i++) {
        PhysMemoryRange *pr = &fRanges[i];
        if (paddr >= pr->addr && paddr < pr->addr + pr->size) {
            return pr;
        }
    }
    return nullptr;
}


uint8_t *PhysMemoryMap::GetRamPtr(uint64_t paddr, bool is_rw)
{
    PhysMemoryRange *pr = FindRange(paddr);
    if (pr == nullptr || !pr->is_ram) {
        return nullptr;
    }
    uintptr_t offset = paddr - pr->addr;
    if (is_rw) {
        pr->SetDirtyBit(offset);
    }
    return pr->phys_mem + offset;
}


PhysMemoryRange *PhysMemoryMap::RegisterRamEntry(uint64_t addr, uint64_t size,
                                                 int devram_flags)
{
    assert(fRangeCount < PHYS_MEM_RANGE_MAX);
    assert((size & (DEVRAM_PAGE_SIZE - 1)) == 0 && size != 0);

    PhysMemoryRange *pr = &fRanges[fRangeCount++];
    pr->map = this;
    pr->is_ram = true;
    pr->devram_flags = devram_flags & ~DEVRAM_FLAG_DISABLED;
    pr->addr = addr;
    pr->org_size = size;
    if (devram_flags & DEVRAM_FLAG_DISABLED) {
        pr->size = 0;
    } else {
        pr->size = pr->org_size;
    }
    pr->phys_mem = nullptr;
    pr->dirty_bits = nullptr;
    return pr;
}


PhysMemoryRange *PhysMemoryMap::RegisterRam(uint64_t addr, uint64_t size,
                                            int devram_flags)
{
    PhysMemoryRange *pr = RegisterRamEntry(addr, size, devram_flags);

#ifdef __HAIKU__
    if (create_area("VM memory", reinterpret_cast<void **>(&pr->phys_mem),
                    B_ANY_ADDRESS, size, B_NO_LOCK,
                    B_READ_AREA | B_WRITE_AREA) < B_OK) {
        pr->phys_mem = nullptr;
    }
#else
    pr->phys_mem = static_cast<uint8_t *>(mmap(nullptr, size,
                                               PROT_READ | PROT_WRITE,
                                               MAP_PRIVATE | MAP_ANONYMOUS,
                                               -1, 0));
    if (pr->phys_mem == MAP_FAILED) {
        pr->phys_mem = nullptr;
    }
#endif

    if (pr->phys_mem == nullptr) {
        fprintf(stderr, "Could not allocate VM memory\n");
        exit(1);
    }

    if (devram_flags & DEVRAM_FLAG_DIRTY_BITS) {
        size_t nb_pages = size >> DEVRAM_PAGE_SIZE_LOG2;
        pr->dirty_bits_size = ((nb_pages + 31) / 32) * sizeof(uint32_t);
        pr->dirty_bits_index = 0;
        for (int i = 0; i < 2; i++) {
            pr->dirty_bits_tab[i] = std::make_unique<uint32_t[]>(
                pr->dirty_bits_size / sizeof(uint32_t));
        }
        pr->dirty_bits = pr->dirty_bits_tab[pr->dirty_bits_index].get();
    }
    return pr;
}


void PhysMemoryMap::FreeRam(PhysMemoryRange *pr)
{
#ifdef __HAIKU__
    delete_area(area_for(pr->phys_mem));
#else
    munmap(pr->phys_mem, pr->org_size);
#endif
}


/* return a pointer to the bitmap of dirty bits and reset them */
const uint32_t *PhysMemoryMap::GetDirtyBits(PhysMemoryRange *pr)
{
    uint32_t *dirty_bits = pr->dirty_bits;

    bool has_dirty_bits = false;
    size_t n = pr->dirty_bits_size / sizeof(uint32_t);
    for (size_t i = 0; i < n; i++) {
        if (dirty_bits[i] != 0) {
            has_dirty_bits = true;
            break;
        }
    }
    if (has_dirty_bits && pr->size != 0) {
        /* invalidate the corresponding CPU write TLBs */
        FlushTlbWriteRange(pr->phys_mem, pr->org_size);
    }

    pr->dirty_bits_index ^= 1;
    pr->dirty_bits = pr->dirty_bits_tab[pr->dirty_bits_index].get();
    memset(pr->dirty_bits, 0, pr->dirty_bits_size);
    return dirty_bits;
}


void PhysMemoryMap::SetRamAddr(PhysMemoryRange *pr, uint64_t addr, bool enabled)
{
    if (enabled) {
        if (pr->size == 0 || pr->addr != addr) {
            /* enable or move mapping */
            if (pr->is_ram) {
                FlushTlbWriteRange(pr->phys_mem, pr->org_size);
            }
            pr->addr = addr;
            pr->size = pr->org_size;
        }
    } else {
        if (pr->size != 0) {
            /* disable mapping */
            if (pr->is_ram) {
                FlushTlbWriteRange(pr->phys_mem, pr->org_size);
            }
            pr->addr = 0;
            pr->size = 0;
        }
    }
}


PhysMemoryRange *PhysMemoryMap::RegisterDevice(uint64_t addr, uint64_t size,
                                               DeviceIO *io, int devio_flags)
{
    assert(fRangeCount < PHYS_MEM_RANGE_MAX);
    assert(size <= 0xffffffff);

    PhysMemoryRange *pr = &fRanges[fRangeCount++];
    pr->map = this;
    pr->addr = addr;
    pr->org_size = size;
    if (devio_flags & DEVIO_DISABLED) {
        pr->size = 0;
    } else {
        pr->size = pr->org_size;
    }
    pr->is_ram = false;
    pr->io = io;
    pr->devio_flags = devio_flags;
    return pr;
}


uint32_t PhysMemoryMap::IoRead(uint64_t addr, int size_log2)
{
    PhysMemoryRange *pr = FindRange(addr);
    if (pr == nullptr || pr->is_ram) {
        return -1;
    }
    uint64_t offset = addr - pr->addr;
    if ((pr->devio_flags >> size_log2) & 1) {
        return pr->io->DeviceRead(offset, size_log2);
    }
    /* A halfword access to a range that only decodes bytes is split rather
       than dropped: a device declaring one width still has to answer the
       other, because the guest picks the width. */
    if (size_log2 == 1 && (pr->devio_flags & DEVIO_SIZE8)) {
        uint32_t val = pr->io->DeviceRead(offset, 0) & 0xff;
        val |= (pr->io->DeviceRead(offset + 1, 0) & 0xff) << 8;
        return val;
    }
    return -1;
}


void PhysMemoryMap::IoWrite(uint64_t addr, uint32_t val, int size_log2)
{
    PhysMemoryRange *pr = FindRange(addr);
    if (pr == nullptr || pr->is_ram) {
        return;
    }
    uint64_t offset = addr - pr->addr;
    if ((pr->devio_flags >> size_log2) & 1) {
        pr->io->DeviceWrite(offset, val, size_log2);
    } else if (size_log2 == 1 && (pr->devio_flags & DEVIO_SIZE8)) {
        pr->io->DeviceWrite(offset, val & 0xff, 0);
        pr->io->DeviceWrite(offset + 1, (val >> 8) & 0xff, 0);
    }
}
