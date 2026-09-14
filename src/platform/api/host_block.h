/*
 * Host block storage
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

/* Notified when an asynchronous block request finishes. */
class BlockCompletion {
public:
    virtual ~BlockCompletion() = default;

    virtual void Complete(int ret) = 0;
};


/* A disk image in 512 byte sectors. A request either finishes at once and
   returns 0, or returns 1 and notifies its completion later, from the event
   loop; a negative return is an error. The completion may be null when the
   caller does not care. */
class HostBlockDevice {
public:
    virtual ~HostBlockDevice() = default;

    virtual int64_t SectorCount() = 0;
    virtual int ReadAsync(uint64_t sector_num, uint8_t *buf, int n,
                          BlockCompletion *completion) = 0;
    virtual int WriteAsync(uint64_t sector_num, const uint8_t *buf, int n,
                           BlockCompletion *completion) = 0;
};
