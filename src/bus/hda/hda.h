/*
 * High Definition Audio link
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
#include "host_audio.h"

/* Codec addresses 0 to 14; 15 is a broadcast no controller sends. */
#define HDA_MAX_CODECS 15

/* Stream tags 1 to 15 name a stream on the link; 0 is none. */
#define HDA_MAX_STREAM_TAG 15

/* The stream format word, as a stream descriptor and a converter hold it. */
#define HDA_FMT_BASE_44K     bit_at(14)
#define HDA_FMT_MULT_SHIFT   11
#define HDA_FMT_DIV_SHIFT    8
#define HDA_FMT_BITS_SHIFT   4
#define HDA_FMT_CHAN_MASK    0xf

/* The PCM size and rate parameter: one bit per rate from 8 kHz up, and one
   per sample width from 8 bits up. */
#define HDA_PCM_RATE_COUNT   12
#define HDA_PCM_BITS_SHIFT   16

/* 'fmt' as a frame layout; false if it names no format. */
bool hda_format_decode(uint16_t fmt, AudioFormat *out);

/* Which bit of the PCM parameter a rate is, or -1 if the link cannot carry
   it. */
int hda_rate_index(uint32_t rate);
uint32_t hda_rate_at(int index);


/* One codec on the link. Everything here is called with the device lock
   held. */
class HDACodec {
public:
    virtual ~HDACodec() = default;

    /* Run one verb, the codec address already stripped, and answer it. */
    virtual uint32_t Command(uint32_t verb) = 0;

    /* The link went through a reset, and every codec on it with it. */
    virtual void LinkReset() = 0;

    /* The next frames of output stream 'tag', for every converter set to
       it. */
    virtual void StreamOutput(int tag, const uint8_t *data, size_t frames,
                              const AudioFormat &fmt) = 0;
};


typedef enum {
    HDA_STREAM_MOVED,   /* the frames moved */
    HDA_STREAM_IDLE,    /* no stream runs with that tag */
    HDA_STREAM_PACED,   /* another converter sets the stream's pace */
} HDAStreamResultEnum;


/* The controller's half of the link, which the codecs call.

   A stream moves at the pace of one converter's host clock: the first to ask
   after the stream starts keeps it until it stops asking, and the frames it
   pulls reach every converter set to the same tag. */
class HDALink {
public:
    virtual ~HDALink() = default;

    /* The next 'frames' of output stream 'tag', which reach the codecs
       through StreamOutput(). */
    virtual HDAStreamResultEnum PullOutput(int tag, size_t frames,
                                           const void *pacer) = 0;

    /* Frames for input stream 'tag'. */
    virtual HDAStreamResultEnum PushInput(int tag, const uint8_t *data,
                                          size_t frames,
                                          const AudioFormat &fmt,
                                          const void *pacer) = 0;

    /* A response the codec at 'addr' sends on its own, such as a jack
       being plugged. */
    virtual void Unsolicited(int addr, uint32_t response) = 0;
};


/* The link a controller provides. Codecs take an address on it rather than
   any address space, so it assigns no resources. */
class HDABus final: public Bus {
private:
    HDALink *fLink;
    HDACodec *fCodecs[HDA_MAX_CODECS] {};

public:
    HDABus(Device *owner, HDALink *link): Bus(owner), fLink(link) {}

    const char *Type() const override {return "hda";}
    HDALink *Link() const {return fLink;}

    bool AssignResources(Device *dev) override;

    /* 'addr' < 0 takes the first free address. Returns the address, or -1
       after reporting why not. */
    int AttachCodec(HDACodec *codec, int addr, const char *name);

    HDACodec *Codec(int addr) const
        {return addr >= 0 && addr < HDA_MAX_CODECS ? fCodecs[addr] : nullptr;}
    /* One bit per address a codec answers on. */
    uint16_t PresentMask() const;

    void Reset();
    void StreamOutput(int tag, const uint8_t *data, size_t frames,
                      const AudioFormat &fmt);
};
