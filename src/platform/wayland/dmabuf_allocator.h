/*
 * Wayland window buffers the compositor's GPU reads in place
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

#include <stdint.h>

#include <memory>

#include "window_buffer.h"

struct wl_display;
struct wl_registry;


/* Buffers in host memory that Vulkan allocates on the compositor's GPU and
   hands over as linear ARGB8888 dma-bufs, so that the compositor samples
   them rather than copying them into a texture with the CPU. 'name' and
   'version' are the registry's zwp_linux_dmabuf_v1 global. nullptr, with
   the reason printed, when they can't be used. */
std::unique_ptr<BufferAllocator> dmabuf_allocator_create(
    wl_display *display, wl_registry *registry, uint32_t name,
    uint32_t version);
