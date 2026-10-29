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

#include "cutils.h"
#include "host_memory.h"


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
    uint32_t mask = bit_at(page_index % 32);
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


/* Whether an access goes to the device as it is. */
static bool passes_on(int devio_flags, uint32_t offset, int size_log2)
{
    return get_bit(devio_flags, size_log2) &&
        ((devio_flags & DEVIO_UNALIGNED) ||
         (offset & bit_mask(size_log2)) == 0);
}


/* Whether an access that does not pass on is split into halves; if not, it
   is narrower than any width taken. */
static bool splits(int devio_flags, uint32_t offset, int size_log2)
{
    return (devio_flags & bit_mask(size_log2)) != 0 ||
        (offset & bit_mask(size_log2)) != 0;
}


/* The narrowest width taken, and the register of that width holding
   'offset'. */
static int narrowest(int devio_flags, uint32_t offset, uint32_t *reg)
{
    int wide = __builtin_ctz(devio_flags & DEVIO_SIZES);
    *reg = offset & ~bit_mask(wide);
    return wide;
}


bool PhysMemoryRange::DeviceRead(uint32_t offset, int size_log2,
                                 uint64_t *val)
{
    int bits = 8 << size_log2;

    assert(size_log2 <= 3);
    if ((devio_flags & DEVIO_SIZES) == 0)
        return false;
    if (passes_on(devio_flags, offset, size_log2)) {
        *val = get_bits(io->DeviceRead(offset, size_log2), 0, bits);
    } else if (splits(devio_flags, offset, size_log2)) {
        uint64_t low, high;
        DeviceRead(offset, size_log2 - 1, &low);
        DeviceRead(offset + bits / 16, size_log2 - 1, &high);
        *val = concat_bits(high, low, bits / 2);
    } else {
        uint32_t reg;
        int wide = narrowest(devio_flags, offset, &reg);
        *val = get_bits(io->DeviceRead(reg, wide), (offset - reg) * 8, bits);
    }
    return true;
}


bool PhysMemoryRange::DeviceWrite(uint32_t offset, uint64_t val,
                                  int size_log2)
{
    int bits = 8 << size_log2;

    assert(size_log2 <= 3);
    if ((devio_flags & DEVIO_SIZES) == 0)
        return false;
    val = get_bits(val, 0, bits);
    if (passes_on(devio_flags, offset, size_log2)) {
        io->DeviceWrite(offset, val, size_log2);
        return true;
    }
    if (splits(devio_flags, offset, size_log2)) {
        bool low = DeviceWrite(offset, val, size_log2 - 1);
        bool high = DeviceWrite(offset + bits / 16, val >> (bits / 2),
                                size_log2 - 1);
        return low && high;
    }
    if (!(devio_flags & DEVIO_WRITE_MERGE))
        return false;
    uint32_t reg;
    int wide = narrowest(devio_flags, offset, &reg);
    val = set_bits(io->DeviceRead(reg, wide), (offset - reg) * 8, bits, val);
    io->DeviceWrite(reg, get_bits(val, 0, 8 << wide), wide);
    return true;
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
    assert(DeviceLock::IsHeld());
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

    pr->phys_mem = host_ram_alloc(size);
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
    if (pr->phys_mem != nullptr) {
        host_ram_free(pr->phys_mem, pr->org_size);
    }
}


/* Return a pointer to the bitmap of dirty bits and reset them. Processors
   on other threads may be setting bits meanwhile: the cleared bitmap takes
   over before the write TLBs are flushed, so that a page written through a
   stale entry is in the returned one. */
const uint32_t *PhysMemoryMap::GetDirtyBits(PhysMemoryRange *pr)
{
    uint32_t *dirty_bits = pr->dirty_bits;

    pr->dirty_bits_index ^= 1;
    uint32_t *next = pr->dirty_bits_tab[pr->dirty_bits_index].get();
    memset(next, 0, pr->dirty_bits_size);
    __atomic_store_n(&pr->dirty_bits, next, __ATOMIC_SEQ_CST);

    bool has_dirty_bits = false;
    size_t n = pr->dirty_bits_size / sizeof(uint32_t);
    for (size_t i = 0; i < n; i++) {
        if (__atomic_load_n(&dirty_bits[i], __ATOMIC_RELAXED) != 0) {
            has_dirty_bits = true;
            break;
        }
    }
    if (has_dirty_bits && pr->size != 0) {
        /* invalidate the corresponding CPU write TLBs */
        FlushTlbWriteRange(pr->phys_mem, pr->org_size);
    }
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


PhysMemoryRange *PhysMemoryMap::RegisterRomDevice(uint64_t addr, uint64_t size,
                                                  DeviceIO *io, int devio_flags,
                                                  int devram_flags)
{
    PhysMemoryRange *pr = RegisterRam(addr, size,
                                      devram_flags | DEVRAM_FLAG_ROM);
    pr->io = io;
    pr->devio_flags = devio_flags;
    return pr;
}


uint32_t PhysMemoryMap::IoRead(uint64_t addr, int size_log2)
{
    assert(DeviceLock::IsHeld());
    PhysMemoryRange *pr = FindRange(addr);
    if (pr == nullptr || pr->is_ram) {
        return -1;
    }
    uint64_t val;
    if (!pr->DeviceRead(addr - pr->addr, size_log2, &val)) {
        return -1;
    }
    return val;
}


void PhysMemoryMap::IoWrite(uint64_t addr, uint32_t val, int size_log2)
{
    assert(DeviceLock::IsHeld());
    PhysMemoryRange *pr = FindRange(addr);
    if (pr == nullptr || pr->is_ram) {
        return;
    }
    pr->DeviceWrite(addr - pr->addr, val, size_log2);
}
