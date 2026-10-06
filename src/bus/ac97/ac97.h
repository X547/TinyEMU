/*
 * AC'97 link
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

#include "device.h"

/* The primary codec and one secondary. */
#define AC97_MAX_CODECS 2

/* Codec registers are 16 bits wide, at even offsets up to here. */
#define AC97_REG_COUNT 0x40

/* Every stream carries 16 bit samples. */
#define AC97_SAMPLE_BYTES 2


/* The PCM streams of the link that a controller moves. */
typedef enum {
    AC97_PCM_OUT,   /* front left and right */
    AC97_PCM_IN,    /* left and right */
    AC97_MIC_IN,    /* one channel */
    AC97_STREAM_COUNT,
} AC97StreamEnum;

int ac97_stream_channels(AC97StreamEnum stream);


/* One codec on the link. Everything here is called with the device lock
   held. */
class AC97Codec {
public:
    virtual ~AC97Codec() = default;

    /* 'reg' is the even register offset. */
    virtual uint16_t ReadRegister(int reg) = 0;
    virtual void WriteRegister(int reg, uint16_t val) = 0;

    /* The link went through a cold reset, and the codec with it. */
    virtual void LinkReset() = 0;

    /* The link went up or down, or a stream started or stopped. */
    virtual void StreamsChanged() = 0;

    /* The guest is reading where the streams are: move them up to now, as
       hardware does all the time. */
    virtual void Sync() = 0;
};


/* The controller's half of the link, which the codecs call. The primary
   codec's clock moves every stream: it asks for frames as they are due. */
class AC97Link {
public:
    virtual ~AC97Link() = default;

    /* Whether the link is out of reset, so that the codecs on it run. */
    virtual bool LinkUp() const = 0;

    /* Whether the guest set the stream running, whether or not it has run
       out of buffers. */
    virtual bool StreamRunning(AC97StreamEnum stream) const = 0;

    /* The next 'frames' of PCM out, silence where the controller has
       none. */
    virtual void PullOutput(uint8_t *buf, size_t frames) = 0;

    /* Frames for an input stream, dropped where the controller takes
       none. */
    virtual void PushInput(AC97StreamEnum stream, const uint8_t *buf,
                           size_t frames) = 0;
};


/* The link a controller provides. Codecs take an address on it rather than
   any address space, so it assigns no resources. */
class AC97Bus final: public Bus {
private:
    AC97Link *fLink;
    AC97Codec *fCodecs[AC97_MAX_CODECS] {};

public:
    AC97Bus(Device *owner, AC97Link *link): Bus(owner), fLink(link) {}

    const char *Type() const override {return "ac97";}
    AC97Link *Link() const {return fLink;}

    bool AssignResources(Device *dev) override;

    /* 'addr' < 0 takes the first free address. Returns the address, or -1
       after reporting why not. */
    int AttachCodec(AC97Codec *codec, int addr, const char *name);

    AC97Codec *Codec(int addr) const
        {return addr >= 0 && addr < AC97_MAX_CODECS ? fCodecs[addr] : nullptr;}

    void Reset();
    void StreamsChanged();
    void Sync();
};
