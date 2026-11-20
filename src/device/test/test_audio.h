/*
 * The "test" audio driver
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

/* What the test-control device needs of the "test" audio driver's streams:
   what the device made of them, and their clock. */
class TestAudioControl {
public:
    virtual ~TestAudioControl() = default;

    virtual uint32_t Tag() const = 0;
    virtual uint32_t Direction() const = 0;
    virtual bool IsRunning() const = 0;
    virtual uint32_t Starts() const = 0;
    virtual uint32_t FormatRate() const = 0;
    virtual uint32_t FormatChannels() const = 0;
    virtual uint32_t FormatBits() const = 0;
    /* The clock moves on and the device moves what became due; nothing
       while the stream is stopped. */
    virtual void Advance(uint32_t frames) = 0;
    virtual uint64_t Clock() const = 0;
    virtual uint64_t Frames() const = 0;
    virtual uint32_t Crc() const = 0;
    /* What was played to it or recorded from it: the CRC-32 of every byte,
       and 4 bytes from 'pos' on, little endian, 0 where there are none or
       they are no longer kept. */
    virtual uint32_t Data(uint32_t pos) const = 0;
};

/* The streams open now, in the order they were opened. */
int test_audio_count();
TestAudioControl *test_audio_get(int index);
