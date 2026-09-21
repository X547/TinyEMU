/*
 * Audio streams paced from the event loop
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

#include <atomic>
#include <vector>

#include "event_loop.h"
#include "host_audio.h"

/* How often a running stream asks its device for frames. Small, so that the
   guest's buffer is consumed steadily rather than in lumps. */
#define AUDIO_TICK_MS 1


/* The part every back end shares: while a stream runs, its device is ticked
   from the event loop, and FramesDue() follows whatever clock the back end
   keeps. By default that is the host's, which suits a back end with no clock
   of its own. */
class PacedAudio: public HostAudio, public PollSource {
private:
    EventLoop &fLoop;
    bool fRunning = false;
    uint64_t fLastDue = 0;

protected:
    AudioDirectionEnum fDirection;
    AudioFormat fFormat;
    uint64_t fStartUs = 0;

    /* Frames the host end has moved since the stream started. */
    virtual uint64_t ClockFrames();
    virtual bool StartStream() = 0;
    virtual void StopStream() = 0;

public:
    PacedAudio(EventLoop &loop, AudioDirectionEnum direction);
    ~PacedAudio() override;

    /* HostAudio */
    bool Start(const AudioFormat &fmt) override;
    void Stop() override;
    bool Running() const override {return fRunning;}
    uint64_t FramesDue() override;

    /* PollSource */
    void Prepare(WaitSet &ws) override;
    void Dispatch(WaitSet &ws) override;
};


/* Whole frames between a device thread and a host thread, one writer and
   one reader, neither of which ever waits for the other. */
class AudioRing {
private:
    std::vector<uint8_t> fBuf;
    size_t fFrameBytes = 0;
    size_t fCapacity = 0; /* in frames */
    std::atomic<uint64_t> fWritten {0};
    std::atomic<uint64_t> fRead {0};

public:
    /* Only while neither side is running. */
    void Reset(size_t frame_bytes, size_t capacity);

    size_t Capacity() const {return fCapacity;}
    size_t Available() const;
    size_t Free() const {return fCapacity - Available();}

    /* Both take as many frames as there are room or data for, and return
       that count. A null buffer moves silence in, or drops frames out. */
    size_t Write(const uint8_t *buf, size_t frames);
    size_t Read(uint8_t *buf, size_t frames);
};


/* Samples as left justified 32 bit integers, whatever their container. */
int32_t audio_sample_get(const uint8_t *p, int container_bytes);
