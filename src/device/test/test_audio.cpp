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
#include "test_audio.h"

#include <string.h>

#include <algorithm>
#include <memory>
#include <vector>

#include "config_props.h"
#include "devices.h"
#include "machine.h"
#include "test_control.h"

namespace {

class TestAudio;

std::vector<TestAudio *> sStreams;


uint32_t crc32_update(uint32_t crc, const uint8_t *p, size_t len)
{
    crc = ~crc;
    while (len-- > 0) {
        crc ^= *p++;
        for (int k = 0; k < 8; k++)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}


/* A host end with a clock only the guest moves: what is due is what
   test-control advanced it by, and the device is ticked then, under the
   device lock the access to test-control holds. A render stream keeps what
   it is given; a capture stream records TEST_AUDIO_SAMPLE. */
class TestAudio final: public HostAudio, public TestAudioControl {
private:
    uint32_t fTag;
    AudioDirectionEnum fDirection;
    AudioCaps fCaps;
    AudioFormat fFormat;
    bool fRunning = false;
    uint32_t fStarts = 0;
    uint64_t fClock = 0;
    uint64_t fFrames = 0;
    uint64_t fBytes = 0;
    uint32_t fCrc = 0;
    std::vector<uint8_t> fHistory;

    void Keep(const uint8_t *buf, size_t frames)
    {
        size_t len = frames * fFormat.FrameBytes();
        fCrc = crc32_update(fCrc, buf, len);
        for (size_t i = 0; i < len; i++)
            fHistory[(fBytes + i) % TEST_AUDIO_HISTORY] = buf[i];
        fBytes += len;
        fFrames += frames;
    }

public:
    TestAudio(uint32_t tag, AudioDirectionEnum direction,
              const AudioCaps &caps):
        fTag(tag),
        fDirection(direction),
        fCaps(caps),
        fHistory(TEST_AUDIO_HISTORY)
    {
        sStreams.push_back(this);
    }

    ~TestAudio() override
    {
        sStreams.erase(std::find(sStreams.begin(), sStreams.end(), this));
    }

    /* HostAudio */
    AudioCaps Caps() override {return fCaps;}

    bool Start(const AudioFormat &fmt) override
    {
        fFormat = fmt;
        fRunning = true;
        fStarts++;
        fClock = 0;
        fFrames = 0;
        fBytes = 0;
        fCrc = 0;
        std::fill(fHistory.begin(), fHistory.end(), 0);
        return true;
    }

    void Stop() override {fRunning = false;}
    bool Running() const override {return fRunning;}
    uint64_t FramesDue() override {return fClock;}

    void Write(const uint8_t *buf, size_t frames) override
    {
        if (fRunning && fDirection == AUDIO_RENDER)
            Keep(buf, frames);
    }

    void Read(uint8_t *buf, size_t frames) override
    {
        size_t fb = fFormat.FrameBytes();
        int cb = fFormat.ContainerBytes();

        if (!fRunning || fDirection != AUDIO_CAPTURE) {
            memset(buf, 0, frames * fb);
            return;
        }
        for (size_t f = 0; f < frames; f++) {
            for (int ch = 0; ch < fFormat.channels; ch++) {
                uint32_t s = TEST_AUDIO_SAMPLE(fFrames + f, ch);
                uint8_t *p = buf + f * fb + ch * cb;
                for (int i = 0; i < cb; i++)
                    p[i] = (uint8_t)(s >> (8 * (4 - cb + i)));
            }
        }
        Keep(buf, frames);
    }

    /* TestAudioControl */
    uint32_t Tag() const override {return fTag;}
    uint32_t Direction() const override
    {
        return fDirection == AUDIO_CAPTURE ? TEST_AUDIO_DIR_CAPTURE
                                           : TEST_AUDIO_DIR_RENDER;
    }
    bool IsRunning() const override {return fRunning;}
    uint32_t Starts() const override {return fStarts;}
    uint32_t FormatRate() const override {return fFormat.rate;}
    uint32_t FormatChannels() const override {return fFormat.channels;}
    uint32_t FormatBits() const override {return fFormat.bits;}

    void Advance(uint32_t frames) override
    {
        if (!fRunning)
            return;
        fClock += frames;
        if (target != nullptr)
            target->AudioTick();
    }

    uint64_t Clock() const override {return fClock;}
    uint64_t Frames() const override {return fFrames;}
    uint32_t Crc() const override {return fCrc;}

    uint32_t Data(uint32_t pos) const override
    {
        uint32_t v = 0;
        for (int i = 0; i < 4; i++) {
            uint64_t p = (uint64_t)pos + i;
            if (p < fBytes && p + TEST_AUDIO_HISTORY >= fBytes)
                v |= (uint32_t)fHistory[p % TEST_AUDIO_HISTORY] << (8 * i);
        }
        return v;
    }
};


/* "tag": what the guest tells the stream by, 0 unless given; "rate" and
   "channels": what the host end runs at, 48000 and 2 unless given;
   "any_rate": 1 takes whatever rate the device asks for. */
class TestAudioDriver final: public AudioDriverClass {
public:
    TestAudioDriver(): AudioDriverClass("test") {}

    std::unique_ptr<HostAudio> Open(const DeviceConfig &cfg, JSONValue host,
                                    const AudioSettings &settings,
                                    DeviceContext *ctx) const override
    {
        int tag, rate, channels, any_rate;
        AudioCaps caps;

        (void)ctx;
        if (vm_get_int_opt(host, "tag", &tag, 0) < 0 ||
            vm_get_int_opt(host, "rate", &rate, (int)caps.rate) < 0 ||
            vm_get_int_opt(host, "channels", &channels, caps.channels) < 0 ||
            vm_get_int_opt(host, "any_rate", &any_rate, 0) < 0)
            return nullptr;
        if (rate < 8000 || rate > 192000 || channels < 1 || channels > 8) {
            vm_error("%s: the test audio driver takes rates of 8000 to "
                     "192000 and 1 to 8 channels\n", cfg.Type());
            return nullptr;
        }
        caps.rate = rate;
        caps.channels = channels;
        caps.any_rate = any_rate != 0;
        return std::make_unique<TestAudio>((uint32_t)tag, settings.direction,
                                           caps);
    }
};

const TestAudioDriver sTestAudioDriver;

}


int test_audio_count()
{
    return sStreams.size();
}


TestAudioControl *test_audio_get(int index)
{
    if (index < 0 || index >= (int)sStreams.size())
        return nullptr;
    return sStreams[index];
}
