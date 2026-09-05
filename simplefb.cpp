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
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include "cutils.h"
#include "iomem.h"
#include "virtio.h"
#include "machine.h"

//#define DEBUG_VBE

#define FB_ALLOC_ALIGN 65536

class SimpleFBState final: public FBDevice {
public:
    int fb_page_count = 0;
    PhysMemoryRange *mem_range = nullptr;

    void Refresh(SimpleFBDraw *draw) override;
};

#define MAX_MERGE_DISTANCE 3

void simplefb_refresh(FBDevice *fb_dev, SimpleFBDraw *draw,
                      PhysMemoryRange *mem_range, int fb_page_count)
{
    const uint32_t *dirty_bits;
    uint32_t dirty_val;
    int y0, y1, page_y0, page_y1, byte_pos, page_index, bit_pos;

    dirty_bits = mem_range->DirtyBits();
    
    page_index = 0;
    y0 = y1 = 0;
    while (page_index < fb_page_count) {
        dirty_val = dirty_bits[page_index >> 5];
        if (dirty_val != 0) {
            bit_pos = 0;
            while (dirty_val != 0) {
                while (((dirty_val >> bit_pos) & 1) == 0)
                    bit_pos++;
                dirty_val &= ~(1 << bit_pos);

                byte_pos = (page_index + bit_pos) * DEVRAM_PAGE_SIZE;
                page_y0 = byte_pos / fb_dev->stride;
                page_y1 = ((byte_pos + DEVRAM_PAGE_SIZE - 1) / fb_dev->stride) + 1;
                page_y1 = min_int(page_y1, fb_dev->height);
                if (y0 == y1) {
                    y0 = page_y0;
                    y1 = page_y1;
                } else if (page_y0 <= (y1 + MAX_MERGE_DISTANCE)) {
                    /* union with current region */
                    y1 = page_y1;
                } else {
                    /* flush */
                    draw->Draw(fb_dev, 0, y0, fb_dev->width, y1 - y0);
                    y0 = page_y0;
                    y1 = page_y1;
                }
            }
        }
        page_index += 32;
    }

    if (y0 != y1) {
        draw->Draw(fb_dev, 0, y0, fb_dev->width, y1 - y0);
    }
}

void SimpleFBState::Refresh(SimpleFBDraw *draw)
{
    simplefb_refresh(this, draw, mem_range, fb_page_count);
}

FBDevice *simplefb_init(PhysMemoryMap *map, uint64_t phys_addr,
                        int width, int height)
{
    SimpleFBState *s;

    s = new SimpleFBState();
    FBDevice *fb_dev = s;

    fb_dev->width = width;
    fb_dev->height = height;
    fb_dev->stride = width * 4;
    fb_dev->fb_size = (height * fb_dev->stride + FB_ALLOC_ALIGN - 1) & ~(FB_ALLOC_ALIGN - 1);
    s->fb_page_count = fb_dev->fb_size >> DEVRAM_PAGE_SIZE_LOG2;

    s->mem_range = map->RegisterRam(phys_addr, fb_dev->fb_size,
                                    DEVRAM_FLAG_DIRTY_BITS);
    
    fb_dev->fb_data = s->mem_range->phys_mem;
    return s;
}
