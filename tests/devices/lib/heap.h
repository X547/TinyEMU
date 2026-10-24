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
#pragma once

#include "klib.h"

#define HEAP_MAX_RANGES 32

/* Free RAM as the firmware tables describe it, handed out from the bottom
   of each range up. Nothing is freed one block at a time: the runner takes
   a mark before each test and releases back to it afterwards, so a test
   can allocate as it likes. Memory is identity mapped, so an allocation's
   address is also what a device is given for DMA. */

/* While the machine is discovered: RAM, then what lies in it already. */
void heap_add(uint64_t base, uint64_t size);
void heap_reserve(uint64_t base, uint64_t size);

/* Zeroed, at an address below 'limit' (exclusive); nullptr when nothing
   fits. */
void *heap_alloc(size_t size, size_t align = 16, uint64_t limit = UINT64_MAX);

struct HeapMark {
    uint64_t cursor[HEAP_MAX_RANGES];
};
HeapMark heap_mark();
void heap_release(const HeapMark &mark);

/* For what discovery builds before the heap exists: the parsed tables and
   the device registry. Zeroed, never released; panics when exhausted. */
void *boot_alloc(size_t size, size_t align = 16);

uint64_t heap_free_bytes();
int heap_range_count();
void heap_range(int index, uint64_t *base, uint64_t *size);
