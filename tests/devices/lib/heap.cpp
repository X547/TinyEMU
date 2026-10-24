/*
 * Memory allocation
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
#include "heap.h"

#include "arch.h"

namespace {

struct Range {
    uint64_t base;
    uint64_t end;
    uint64_t cursor;
};

Range sRanges[HEAP_MAX_RANGES];
int sCount;

/* in the BSS, so cleared at entry */
alignas(16) uint8_t sBootArena[512 * 1024];
size_t sBootUsed;

void insert(uint64_t base, uint64_t end)
{
    if (base >= end)
        return;
    if (sCount == HEAP_MAX_RANGES)
        panic("heap: more than %d free ranges", HEAP_MAX_RANGES);
    int i = sCount++;
    /* kept sorted, so allocations go to the lowest address that fits */
    while (i > 0 && sRanges[i - 1].base > base) {
        sRanges[i] = sRanges[i - 1];
        i--;
    }
    sRanges[i] = {base, end, base};
}

}


void heap_add(uint64_t base, uint64_t size)
{
    /* only what the processor can address with paging off or the boot
       identity map */
    uint64_t end = base + size;
    if (end < base || end > ARCH_ADDRESS_LIMIT)
        end = ARCH_ADDRESS_LIMIT;
    /* page 0 stays unused so a null pointer is never a valid block */
    if (base < 0x1000)
        base = 0x1000;
    insert(align_up(base, 16), align_down(end, 16));
}


void heap_reserve(uint64_t base, uint64_t size)
{
    uint64_t end = base + size;
    if (end < base)
        end = UINT64_MAX;
    for (int i = 0; i < sCount; i++) {
        Range r = sRanges[i];
        if (end <= r.base || base >= r.end)
            continue;
        /* drop it and put back what is left on either side */
        for (int j = i; j + 1 < sCount; j++)
            sRanges[j] = sRanges[j + 1];
        sCount--;
        i--;
        insert(r.base, min(base, r.end) & ~(uint64_t)15);
        insert(align_up(max(end, r.base), 16), r.end);
    }
}


void *heap_alloc(size_t size, size_t align, uint64_t limit)
{
    if (align < 16)
        align = 16;
    for (int i = 0; i < sCount; i++) {
        Range &r = sRanges[i];
        uint64_t p = align_up(r.cursor, align);
        if (p < r.cursor || p + size < p || p + size > r.end ||
            p + size > limit)
            continue;
        r.cursor = align_up(p + size, 16);
        memset((void *)(uintptr_t)p, 0, size);
        return (void *)(uintptr_t)p;
    }
    return nullptr;
}


HeapMark heap_mark()
{
    HeapMark m {};
    for (int i = 0; i < sCount; i++)
        m.cursor[i] = sRanges[i].cursor;
    return m;
}


void heap_release(const HeapMark &mark)
{
    for (int i = 0; i < sCount; i++)
        sRanges[i].cursor = mark.cursor[i];
}


void *boot_alloc(size_t size, size_t align)
{
    size_t p = align_up(sBootUsed, align);
    if (p + size > sizeof(sBootArena))
        panic("boot arena exhausted allocating %zu bytes", size);
    sBootUsed = p + size;
    return sBootArena + p;
}


uint64_t heap_free_bytes()
{
    uint64_t n = 0;
    for (int i = 0; i < sCount; i++)
        n += sRanges[i].end - sRanges[i].cursor;
    return n;
}


int heap_range_count()
{
    return sCount;
}


void heap_range(int index, uint64_t *base, uint64_t *size)
{
    *base = sRanges[index].base;
    *size = sRanges[index].end - sRanges[index].base;
}
