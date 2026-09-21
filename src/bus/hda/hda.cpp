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
#include "hda.h"

#include "machine.h"

/* In the order the PCM parameter numbers them. */
static const uint32_t kRates[HDA_PCM_RATE_COUNT] = {
    8000, 11025, 16000, 22050, 32000, 44100, 48000, 88200, 96000, 176400,
    192000, 384000,
};


bool hda_format_decode(uint16_t fmt, AudioFormat *out)
{
    static const int kBits[8] = {8, 16, 20, 24, 32, 0, 0, 0};
    uint32_t base = (fmt & HDA_FMT_BASE_44K) != 0 ? 44100 : 48000;
    uint32_t mult = get_bits(fmt, HDA_FMT_MULT_SHIFT, 3) + 1;
    uint32_t div = get_bits(fmt, HDA_FMT_DIV_SHIFT, 3) + 1;
    int bits = kBits[get_bits(fmt, HDA_FMT_BITS_SHIFT, 3)];

    /* Multipliers of 3 and above 4 are reserved. */
    if (bits == 0 || mult == 3 || mult > 4) {
        return false;
    }
    out->rate = base * mult / div;
    out->bits = bits;
    out->channels = (fmt & HDA_FMT_CHAN_MASK) + 1;
    return true;
}


int hda_rate_index(uint32_t rate)
{
    for (int i = 0; i < HDA_PCM_RATE_COUNT; i++) {
        if (kRates[i] == rate) {
            return i;
        }
    }
    return -1;
}


uint32_t hda_rate_at(int index)
{
    return kRates[index];
}


//#pragma mark - HDABus

bool HDABus::AssignResources(Device *dev)
{
    for (int i = 0; i < dev->ResourceCount(); i++) {
        if (dev->ResourceAt(i)->type != RES_NONE) {
            vm_error("hda bus: device '%s' declared a resource, but a codec "
                     "has none of its own\n", dev->Name());
            return false;
        }
    }
    return true;
}


int HDABus::AttachCodec(HDACodec *codec, int addr, const char *name)
{
    if (addr < 0) {
        for (addr = 0; addr < HDA_MAX_CODECS; addr++) {
            if (fCodecs[addr] == nullptr) {
                break;
            }
        }
        if (addr == HDA_MAX_CODECS) {
            vm_error("%s: the link has no free codec address\n", name);
            return -1;
        }
    }
    if (addr >= HDA_MAX_CODECS) {
        vm_error("%s: 'address' must be between 0 and %d\n", name,
                 HDA_MAX_CODECS - 1);
        return -1;
    }
    if (fCodecs[addr] != nullptr) {
        vm_error("%s: codec address %d is taken\n", name, addr);
        return -1;
    }
    fCodecs[addr] = codec;
    return addr;
}


uint16_t HDABus::PresentMask() const
{
    uint16_t mask = 0;

    for (int i = 0; i < HDA_MAX_CODECS; i++) {
        if (fCodecs[i] != nullptr) {
            mask |= bit_at(i);
        }
    }
    return mask;
}


void HDABus::Reset()
{
    for (HDACodec *codec : fCodecs) {
        if (codec != nullptr) {
            codec->LinkReset();
        }
    }
}


void HDABus::StreamOutput(int tag, const uint8_t *data, size_t frames,
                          const AudioFormat &fmt)
{
    for (HDACodec *codec : fCodecs) {
        if (codec != nullptr) {
            codec->StreamOutput(tag, data, frames, fmt);
        }
    }
}
