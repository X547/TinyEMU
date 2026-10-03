/*
 * Simple frame buffer
 *
 * Copyright (c) 2017 Fabrice Bellard
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

#include "cutils.h"
#include "device.h"
#include "machine.h"

FBDevice *simplefb_init(PhysMemoryMap *map, uint64_t phys_addr,
                        int width, int height);

/* Walk the dirty pages of a frame buffer mapping and hand each run of rows
   the guest has written to 'draw', as the first row and the row past the
   last. The displayed rows start 'offset' bytes into the mapping and are
   'stride' bytes apart, which is how the VGA device follows a mode that
   does not start at the beginning of its video memory. Runs a few rows
   apart are drawn as one, and the dirty bits are reset. */
template <typename Draw>
void fb_walk_dirty(PhysMemoryRange *mem_range, int page_count,
                   uint32_t offset, int stride, int height, Draw draw)
{
    const int kMaxMergeDistance = 3;
    const uint32_t *dirty_bits = mem_range->DirtyBits();
    int y0 = 0, y1 = 0;

    for (int page_index = 0; page_index < page_count; page_index += 32) {
        uint32_t dirty_val = dirty_bits[page_index >> 5];
        while (dirty_val != 0) {
            int bit_pos = 0;
            while (!get_bit(dirty_val, bit_pos))
                bit_pos++;
            dirty_val = set_bit(dirty_val, bit_pos, false);

            int64_t first = (int64_t)(page_index + bit_pos) *
                DEVRAM_PAGE_SIZE - offset;
            int64_t last = first + DEVRAM_PAGE_SIZE - 1;
            if (last < 0)
                continue; /* before the first displayed row */
            if (first < 0)
                first = 0;
            int page_y0 = (int)(first / stride);
            int page_y1 = min_int((int)(last / stride) + 1, height);
            if (page_y0 >= page_y1)
                continue; /* past the last displayed row */

            if (y0 == y1) {
                y0 = page_y0;
                y1 = page_y1;
            } else if (page_y0 <= y1 + kMaxMergeDistance) {
                /* union with the current run */
                y1 = max_int(y1, page_y1);
            } else {
                draw(y0, y1);
                y0 = page_y0;
                y1 = page_y1;
            }
        }
    }

    if (y0 != y1)
        draw(y0, y1);
}
