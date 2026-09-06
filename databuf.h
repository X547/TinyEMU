/*
 * Transfer payload buffers
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
#pragma once

#include <stdint.h>


/* The payload of a transfer, described by whoever owns the memory behind it.
   A host controller implements one over the descriptor chain the guest built,
   so the device it hands the transfer to never sees a guest physical address,
   and the page-at-a-time chunking that DMA here needs happens in one place
   instead of in every device.

   The two methods are named from the point of view of the code holding the
   buffer: Read() takes bytes out of it, Write() puts bytes into it. Both
   return how many bytes were actually moved, which falls short of 'len' only
   when the request runs off the end of the buffer or a page behind it cannot
   be resolved. */
class DataBuffer {
public:
    virtual ~DataBuffer() = default;

    virtual uint32_t Length() const = 0;
    virtual uint32_t Read(uint32_t offset, void *dst, uint32_t len) = 0;
    virtual uint32_t Write(uint32_t offset, const void *src, uint32_t len) = 0;
};

