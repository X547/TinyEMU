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
#include "ac97.h"

#include "machine.h"


int ac97_stream_channels(AC97StreamEnum stream)
{
    return stream == AC97_MIC_IN ? 1 : 2;
}


bool AC97Bus::AssignResources(Device *dev)
{
    for (int i = 0; i < dev->ResourceCount(); i++) {
        if (dev->ResourceAt(i)->type != RES_NONE) {
            vm_error("ac97 bus: device '%s' declared a resource, but a codec "
                     "has none of its own\n", dev->Name());
            return false;
        }
    }
    return true;
}


int AC97Bus::AttachCodec(AC97Codec *codec, int addr, const char *name)
{
    if (addr < 0) {
        for (addr = 0; addr < AC97_MAX_CODECS; addr++) {
            if (fCodecs[addr] == nullptr) {
                break;
            }
        }
        if (addr == AC97_MAX_CODECS) {
            vm_error("%s: the link has no free codec address\n", name);
            return -1;
        }
    }
    if (addr >= AC97_MAX_CODECS) {
        vm_error("%s: 'address' must be between 0 and %d\n", name,
                 AC97_MAX_CODECS - 1);
        return -1;
    }
    if (fCodecs[addr] != nullptr) {
        vm_error("%s: codec address %d is taken\n", name, addr);
        return -1;
    }
    fCodecs[addr] = codec;
    return addr;
}


void AC97Bus::Reset()
{
    for (AC97Codec *codec : fCodecs) {
        if (codec != nullptr) {
            codec->LinkReset();
        }
    }
}


void AC97Bus::StreamsChanged()
{
    for (AC97Codec *codec : fCodecs) {
        if (codec != nullptr) {
            codec->StreamsChanged();
        }
    }
}


void AC97Bus::Sync()
{
    for (AC97Codec *codec : fCodecs) {
        if (codec != nullptr) {
            codec->Sync();
        }
    }
}
