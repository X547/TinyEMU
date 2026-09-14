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

#include "device.h"
#include "machine.h"

struct DeviceContext;

FBDevice *simplefb_init(PhysMemoryMap *map, uint64_t phys_addr,
                        int width, int height);

/* Walk the dirty bits of a frame buffer mapping and hand the back end the
   rectangles that changed. Shared with the VGA device, which is a simple
   frame buffer once the guest has put it in a linear mode. */
void simplefb_refresh(FBDevice *fb_dev, HostScreen *screen,
                      PhysMemoryRange *mem_range, int fb_page_count);

Device *simplefb_node_create(DeviceContext *ctx, int width, int height);
