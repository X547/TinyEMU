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
#include "paced_audio.h"

#include <string.h>

#include "host_time.h"
#include "platform_backends.h"
#include "wait_set.h"


//#pragma mark - PacedAudio

PacedAudio::PacedAudio(EventLoop &loop, AudioDirectionEnum direction):
    fLoop(loop),
    fDirection(direction)
{
    fLoop.Add(this);
}


PacedAudio::~PacedAudio()
{
    fLoop.Remove(this);
}


uint64_t PacedAudio::ClockFrames()
{
    return (host_monotonic_us() - fStartUs) * fFormat.rate / 1000000;
}


bool PacedAudio::Start(const AudioFormat &fmt)
{
    Stop();
    fFormat = fmt;
    fStartUs = host_monotonic_us();
    fLastDue = 0;
    if (!StartStream()) {
        return false;
    }
    fRunning = true;
    /* The loop may be in a long wait that knows nothing of this stream. */
    fLoop.Wake();
    return true;
}


void PacedAudio::Stop()
{
    if (!fRunning) {
        return;
    }
    fRunning = false;
    StopStream();
}


uint64_t PacedAudio::FramesDue()
{
    if (fRunning) {
        uint64_t frames = ClockFrames();
        if (frames > fLastDue) {
            fLastDue = frames;
        }
    }
    return fLastDue;
}


void PacedAudio::Prepare(WaitSet &ws)
{
    if (fRunning) {
        ws.LimitTimeout(AUDIO_TICK_MS);
    }
}


void PacedAudio::Dispatch(WaitSet &ws)
{
    (void)ws;
    if (fRunning && target != nullptr) {
        target->AudioTick();
    }
}


//#pragma mark - AudioRing

void AudioRing::Reset(size_t frame_bytes, size_t capacity)
{
    fFrameBytes = frame_bytes;
    fCapacity = capacity;
    fBuf.assign(frame_bytes * capacity, 0);
    fWritten.store(0);
    fRead.store(0);
}


size_t AudioRing::Available() const
{
    return (size_t)(fWritten.load(std::memory_order_acquire) -
                    fRead.load(std::memory_order_acquire));
}


size_t AudioRing::Write(const uint8_t *buf, size_t frames)
{
    uint64_t written = fWritten.load(std::memory_order_relaxed);
    size_t room = fCapacity - (size_t)(written -
                                       fRead.load(std::memory_order_acquire));
    if (frames > room) {
        frames = room;
    }
    for (size_t done = 0; done < frames;) {
        size_t pos = (size_t)((written + done) % fCapacity);
        size_t n = fCapacity - pos;
        if (n > frames - done) {
            n = frames - done;
        }
        uint8_t *dst = &fBuf[pos * fFrameBytes];
        if (buf != nullptr) {
            memcpy(dst, buf + done * fFrameBytes, n * fFrameBytes);
        } else {
            memset(dst, 0, n * fFrameBytes);
        }
        done += n;
    }
    fWritten.store(written + frames, std::memory_order_release);
    return frames;
}


size_t AudioRing::Read(uint8_t *buf, size_t frames)
{
    uint64_t read = fRead.load(std::memory_order_relaxed);
    size_t avail = (size_t)(fWritten.load(std::memory_order_acquire) - read);
    if (frames > avail) {
        frames = avail;
    }
    if (buf != nullptr) {
        for (size_t done = 0; done < frames;) {
            size_t pos = (size_t)((read + done) % fCapacity);
            size_t n = fCapacity - pos;
            if (n > frames - done) {
                n = frames - done;
            }
            memcpy(buf + done * fFrameBytes, &fBuf[pos * fFrameBytes],
                   n * fFrameBytes);
            done += n;
        }
    }
    fRead.store(read + frames, std::memory_order_release);
    return frames;
}


//#pragma mark - samples

int32_t audio_sample_get(const uint8_t *p, int container_bytes)
{
    switch (container_bytes) {
    case 1:
        return (int32_t)((uint32_t)p[0] << 24);
    case 2:
        return (int32_t)(((uint32_t)p[0] | (uint32_t)p[1] << 8) << 16);
    default:
        return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 |
                         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
    }
}


//#pragma mark - NullAudio

/* Moves nothing, but keeps time, so that the guest sees a device that plays
   and records at the rate it asked for. */
class NullAudio final: public PacedAudio {
protected:
    bool StartStream() override {return true;}
    void StopStream() override {}

public:
    NullAudio(EventLoop &loop, AudioDirectionEnum direction):
        PacedAudio(loop, direction) {}

    AudioCaps Caps() override
    {
        AudioCaps caps;
        caps.any_rate = true;
        return caps;
    }

    void Write(const uint8_t *buf, size_t frames) override
        {(void)buf; (void)frames;}

    void Read(uint8_t *buf, size_t frames) override
        {memset(buf, 0, frames * fFormat.FrameBytes());}
};


std::unique_ptr<HostAudio> null_audio_open(EventLoop &loop,
                                           const AudioSettings &settings)
{
    return std::make_unique<NullAudio>(loop, settings.direction);
}
