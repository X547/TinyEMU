/*
 * Host audio stream
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

#include <stddef.h>
#include <stdint.h>

typedef enum {
    AUDIO_RENDER,  /* guest to host */
    AUDIO_CAPTURE, /* host to guest */
} AudioDirectionEnum;


/* Linear PCM: signed little endian samples, each in the smallest container of
   1, 2 or 4 bytes that holds it, with the valid bits at the top. Channels are
   interleaved. */
struct AudioFormat {
    uint32_t rate = 48000;
    int channels = 2;
    int bits = 16;

    int ContainerBytes() const {return bits <= 8 ? 1 : bits <= 16 ? 2 : 4;}
    int FrameBytes() const {return channels * ContainerBytes();}

    bool operator==(const AudioFormat &o) const
        {return rate == o.rate && channels == o.channels && bits == o.bits;}
    bool operator!=(const AudioFormat &o) const {return !(*this == o);}
};


/* How a stream is set up, as the configuration says it. */
struct AudioSettings {
    AudioDirectionEnum direction = AUDIO_RENDER;
    /* "host" is the host's own audio system, whichever that is; "wav" is a
       file; "none" moves nothing but keeps time. */
    const char *driver = "host";
    /* Part of the name of the host device; null for the default one, which
       is then followed when the user changes it. */
    const char *device = nullptr;
    /* for "wav" */
    const char *file = nullptr;
    bool loop = false;
    /* How far ahead of the listener the guest may run, in milliseconds. */
    int latency_ms = 20;
};


/* What the host end prefers. A device that lets the guest choose offers this
   and nothing else, so that nobody has to convert. */
struct AudioCaps {
    uint32_t rate = 48000;
    int channels = 2;
    /* The rate is merely a preference: any rate is taken as it is. */
    bool any_rate = false;
};


/* The device half of a stream. */
class AudioTarget {
public:
    virtual ~AudioTarget() = default;

    /* The stream's clock moved on: FramesDue() says how far. Called from the
       event loop with the device lock held, a few times a millisecond at
       most, while the stream runs. */
    virtual void AudioTick() = 0;
};


/* One direction of one host audio endpoint, paced by the endpoint's own clock
   when it has one and by the host's otherwise. The device moves frames as
   the clock says they are due, so what it hands over and what the host
   plays never drift apart and nothing needs resampling. */
class HostAudio {
public:
    AudioTarget *target = nullptr; /* set by the device */

    virtual ~HostAudio() = default;

    virtual AudioCaps Caps() = 0;

    /* A running stream is restarted in the new format. */
    virtual bool Start(const AudioFormat &fmt) = 0;
    virtual void Stop() = 0;
    virtual bool Running() const = 0;

    /* Frames the device should have moved since Start(). Never goes
       backwards. */
    virtual uint64_t FramesDue() = 0;

    /* Render: frames for the host. Those that do not fit are dropped. */
    virtual void Write(const uint8_t *buf, size_t frames) = 0;
    /* Capture: frames from the host, silence where there are none. */
    virtual void Read(uint8_t *buf, size_t frames) = 0;
};
