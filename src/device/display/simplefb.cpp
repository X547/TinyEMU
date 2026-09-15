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
#include "simplefb.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include "cutils.h"
#include "devices.h"
#include "fdt.h"
#include "iomem.h"
#include "virtio.h"
#include "machine.h"

//#define DEBUG_VBE

/* The frame buffer is mapped as RAM with dirty tracking, so its size is
   rounded up to something the page granularity divides. The device wrapper
   below reserves its window the same way, so that the reservation and the
   mapping can never disagree. */
#define FB_ALLOC_ALIGN 65536

class SimpleFBState final: public FBDevice {
public:
    int fb_page_count = 0;
    PhysMemoryRange *mem_range = nullptr;

    void Refresh(HostScreen *screen) override;
};

#define MAX_MERGE_DISTANCE 3

void simplefb_refresh(FBDevice *fb_dev, HostScreen *screen,
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
                    screen->Update(0, y0, fb_dev->width, y1 - y0);
                    y0 = page_y0;
                    y1 = page_y1;
                }
            }
        }
        page_index += 32;
    }

    if (y0 != y1) {
        screen->Update(0, y0, fb_dev->width, y1 - y0);
    }
}

void SimpleFBState::Refresh(HostScreen *screen)
{
    screen->SetFramebuffer(fb_data, width, height, stride);
    simplefb_refresh(this, screen, mem_range, fb_page_count);
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


//#pragma mark - SimpleFBDevice

class SimpleFBDevice final: public Device {
private:
    DeviceContext *fCtx;
    int fWidth;
    int fHeight;
    Resource *fMmio = nullptr;
    std::unique_ptr<FBDevice> fFb;

public:
    SimpleFBDevice(DeviceContext *ctx, int width, int height):
        Device("framebuffer"), fCtx(ctx), fWidth(width), fHeight(height) {}

    bool Prepare() override
    {
        /* simplefb_init() rounds the allocation the same way; computing it
           here keeps the reservation and the mapping identical. */
        uint64_t size = (uint64_t)fHeight * fWidth * 4;
        size = (size + FB_ALLOC_ALIGN - 1) & ~(uint64_t)(FB_ALLOC_ALIGN - 1);
        fMmio = AddResource(RES_MMIO, size, FB_ALLOC_ALIGN);
        return fMmio != nullptr;
    }

    bool Realize() override
    {
        SystemBus *sys = static_cast<SystemBus *>(ParentBus());
        fFb.reset(simplefb_init(sys->MemMap(), fMmio->base, fWidth, fHeight));
        fCtx->fb_dev = fFb.get();
        fCtx->fb_base = fMmio->base;
        fCtx->screen = fFb.get();
        fCtx->screen_width = fFb->width;
        fCtx->screen_height = fFb->height;
        return fFb != nullptr;
    }

    void BuildFDT(FDTContext &ctx) override
    {
        ctx.fdt->BeginNodeNum("framebuffer", fMmio->base);
        ctx.fdt->PropStr("compatible", "simple-framebuffer");
        ctx.fdt->PropU64Range("reg", fMmio->base, fFb->fb_size);
        ctx.fdt->PropU32("width", fFb->width);
        ctx.fdt->PropU32("height", fFb->height);
        ctx.fdt->PropU32("stride", fFb->stride);
        ctx.fdt->PropStr("format", "a8r8g8b8");
        ctx.fdt->EndNode();
    }
};


//#pragma mark - factory

Device *simplefb_node_create(DeviceContext *ctx, int width, int height)
{
    return new SimpleFBDevice(ctx, width, height);
}
