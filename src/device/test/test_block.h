/*
 * Test disk
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

/* What the test-control device needs of the "test" block driver's disks:
   how they finish requests, which ones fail, and what they were asked. */
class TestBlockControl {
public:
    virtual ~TestBlockControl() = default;

    virtual uint32_t Sectors() const = 0;
    virtual uint32_t Mode() const = 0;
    virtual void SetMode(uint32_t mode) = 0;
    virtual uint32_t FailCount() const = 0;
    virtual void SetFailCount(uint32_t count) = 0;
    virtual uint32_t Held() const = 0;
    /* 'count' held requests, oldest first or newest first, finish soon
       after from the event loop. */
    virtual void Release(uint32_t count, bool newest_first) = 0;
    virtual uint32_t Reads() const = 0;
    virtual uint32_t Writes() const = 0;
    virtual uint32_t Cancels() const = 0;
};

/* The disks open now, in the order they were opened. */
int test_block_count();
TestBlockControl *test_block_get(int index);
