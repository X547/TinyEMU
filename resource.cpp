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
#include "resource.h"

#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "machine.h"


void RangeAllocator::SetWindow(uint64_t base, uint64_t size)
{
    fWindowBase = base;
    fWindowSize = size;
}


int RangeAllocator::InsertIndex(uint64_t base) const
{
    int i = 0;
    while (i < fCount && fEntries[i].base < base) {
        i++;
    }
    return i;
}


const AllocEntry *RangeAllocator::FindOverlap(uint64_t base, uint64_t size) const
{
    if (size == 0) {
        return nullptr;
    }
    for (int i = 0; i < fCount; i++) {
        const AllocEntry &e = fEntries[i];
        if (base < e.base + e.size && e.base < base + size) {
            return &e;
        }
    }
    return nullptr;
}


bool RangeAllocator::Insert(int index, uint64_t base, uint64_t size,
                            const char *owner)
{
    if (fCount >= ALLOCATOR_MAX_ENTRIES) {
        vm_error("%s: too many resources (max %d)\n", fName,
                 ALLOCATOR_MAX_ENTRIES);
        return false;
    }
    memmove(&fEntries[index + 1], &fEntries[index],
            (fCount - index) * sizeof(AllocEntry));
    fEntries[index].base = base;
    fEntries[index].size = size;
    fEntries[index].owner = owner;
    fCount++;
    return true;
}


bool RangeAllocator::Claim(uint64_t base, uint64_t size, const char *owner)
{
    if (size == 0) {
        return true;
    }
    const AllocEntry *conflict = FindOverlap(base, size);
    if (conflict != nullptr) {
        vm_error("%s: %s at 0x%" PRIx64 "-0x%" PRIx64
                 " collides with %s at 0x%" PRIx64 "-0x%" PRIx64 "\n",
                 fName, owner, base, base + size - 1,
                 conflict->owner, conflict->base,
                 conflict->base + conflict->size - 1);
        return false;
    }
    return Insert(InsertIndex(base), base, size, owner);
}


bool RangeAllocator::Alloc(uint64_t size, uint64_t align, const char *owner,
                           uint64_t *base_out)
{
    if (size == 0) {
        *base_out = 0;
        return true;
    }
    if (align == 0) {
        align = size;
    }

    /* Walk the gaps between claimed intervals inside the window. The entry
       list is sorted, so one pass finds the lowest fit. */
    uint64_t candidate = (fWindowBase + align - 1) & ~(align - 1);
    uint64_t window_end = fWindowBase + fWindowSize;

    for (int i = 0; i <= fCount; i++) {
        uint64_t gap_end;
        if (i < fCount) {
            gap_end = fEntries[i].base;
        } else {
            gap_end = window_end;
        }
        if (gap_end > window_end) {
            gap_end = window_end;
        }
        if (candidate + size <= gap_end) {
            if (!Insert(InsertIndex(candidate), candidate, size, owner)) {
                return false;
            }
            *base_out = candidate;
            return true;
        }
        if (i < fCount) {
            uint64_t next = fEntries[i].base + fEntries[i].size;
            if (next > candidate) {
                candidate = (next + align - 1) & ~(align - 1);
            }
        }
    }

    vm_error("%s: no free space for %s (size 0x%" PRIx64 ", align 0x%" PRIx64
             ") in window 0x%" PRIx64 "-0x%" PRIx64 "\n",
             fName, owner, size, align, fWindowBase, window_end - 1);
    return false;
}


bool RangeAllocator::Assign(Resource *res, const char *owner)
{
    if (res->name != nullptr) {
        owner = res->name;
    }
    if (res->fixed) {
        if (!Claim(res->base, res->size, owner)) {
            return false;
        }
    } else {
        if (!Alloc(res->size, res->align, owner, &res->base)) {
            return false;
        }
    }
    res->assigned = true;
    return true;
}


void RangeAllocator::Dump() const
{
    printf("%s map:\n", fName);
    for (int i = 0; i < fCount; i++) {
        const AllocEntry &e = fEntries[i];
        printf("  0x%08" PRIx64 "-0x%08" PRIx64 "  %s\n",
               e.base, e.base + e.size - 1, e.owner);
    }
}
