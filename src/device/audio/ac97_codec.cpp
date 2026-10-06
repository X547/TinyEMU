/*
 * AC'97 audio codec
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
#include <math.h>
#include <string.h>

#include <memory>
#include <vector>

#include "ac97.h"
#include "audio_gain.h"
#include "config_props.h"
#include "cutils.h"
#include "device_class.h"
#include "host_audio.h"
#include "machine.h"

/* registers */
#define REG_RESET          0x00
#define REG_MASTER         0x02
#define REG_HEADPHONE      0x04
#define REG_MASTER_MONO    0x06
#define REG_PC_BEEP        0x0a
#define REG_PHONE          0x0c
#define REG_MIC            0x0e
#define REG_LINE_IN        0x10
#define REG_CD             0x12
#define REG_VIDEO          0x14
#define REG_AUX            0x16
#define REG_PCM_OUT        0x18
#define REG_RECORD_SELECT  0x1a
#define REG_RECORD_GAIN    0x1c
#define REG_GENERAL        0x20
#define REG_POWERDOWN      0x26
#define  PD_ADC_READY       bit_at(0)
#define  PD_DAC_READY       bit_at(1)
#define  PD_ANALOG_READY    bit_at(2)
#define  PD_VREF_READY      bit_at(3)
#define  PD_PR0             bit_at(8)  /* ADCs off */
#define  PD_PR1             bit_at(9)  /* DACs off */
#define  PD_PR2             bit_at(10) /* analog mixer off */
#define  PD_PR3             bit_at(11) /* analog mixer and reference off */
#define  PD_CONTROL_MASK    0xff00
#define REG_EXT_ID         0x28
#define  EXT_ID_VRA         bit_at(0)
#define  EXT_ID_REV_22      (1 << 10)
#define  EXT_ID_ADDR_SHIFT  14
#define REG_EXT_CTRL       0x2a
#define  EXT_CTRL_VRA       bit_at(0)
#define REG_PCM_FRONT_RATE 0x2c
#define REG_PCM_ADC_RATE   0x32
#define REG_VENDOR_ID1     0x7c
#define REG_VENDOR_ID2     0x7e

#define VOLUME_MUTE        bit_at(15)
/* A PCM gain of this many 1.5 dB steps below +12 dB is unity. */
#define PCM_UNITY          8

/* The link's own rate, and what variable rate audio may run at. */
#define AC97_LINK_RATE     48000
#define AC97_MIN_RATE      8000
#define AC97_MAX_RATE      48000

/* SigmaTel STAC9700, which every driver of the time knows. */
#define AC97_DEFAULT_VENDOR_ID 0x83847600


/* The mixer registers that hold what the guest writes, with their value
   after a reset and the bits a write keeps. Those not listed read 0. */
static const struct {
    uint8_t reg;
    uint16_t reset;
    uint16_t mask;
} kMixerRegs[] = {
    {REG_MASTER,        0x8000, 0xbf3f},
    {REG_HEADPHONE,     0x8000, 0xbf3f},
    {REG_MASTER_MONO,   0x8000, 0x803f},
    {REG_PC_BEEP,       0x0000, 0x801e},
    {REG_PHONE,         0x8008, 0x801f},
    {REG_MIC,           0x8008, 0x805f},
    {REG_LINE_IN,       0x8808, 0x9f1f},
    {REG_CD,            0x8808, 0x9f1f},
    {REG_VIDEO,         0x8808, 0x9f1f},
    {REG_AUX,           0x8808, 0x9f1f},
    {REG_PCM_OUT,       0x8808, 0x9f1f},
    {REG_RECORD_SELECT, 0x0000, 0x0707},
    {REG_RECORD_GAIN,   0x8000, 0x8f0f},
    {REG_GENERAL,       0x0000, 0x9380},
};


/* The frames due since 'moved', which moves up to 'due'. After a long stall
   the lost time is let go rather than made up for in one burst. */
static size_t frames_due(uint64_t due, uint64_t *moved, uint32_t rate)
{
    if (due <= *moved) {
        return 0;
    }
    uint64_t max_frames = rate / 10;
    if (due - *moved > max_frames) {
        *moved = due - max_frames;
    }
    size_t frames = (size_t)(due - *moved);
    *moved = due;
    return frames;
}


/* A codec with five bits of attenuation reads a sixth one set as all
   five. */
static uint16_t saturate_attenuation(uint16_t val)
{
    for (int shift = 0; shift <= 8; shift += 8) {
        if ((val & (0x20 << shift)) != 0) {
            val = (val & ~(0x3f << shift)) | (0x1f << shift);
        }
    }
    return val;
}


/* The rate a converter set to 'val' runs at: the host end's own when it
   has one. */
static uint32_t lock_rate(uint32_t val, uint32_t locked)
{
    if (locked != 0) {
        return locked;
    }
    return val < AC97_MIN_RATE ? AC97_MIN_RATE
           : val > AC97_MAX_RATE ? AC97_MAX_RATE : val;
}


/* The primary codec plays PCM out to its "host" end and records PCM in from
   its "input" end. The output end's clock moves every stream on the link,
   input included unless the input end has a clock of its own; streams
   nothing feeds record silence. A secondary codec only has registers. */
class AC97CodecDevice final: public Device, public AC97Codec,
                             public AudioTarget {
private:
    /* An input stream that moves at the output's pace. */
    struct Follower {
        uint32_t rate = 0;
        uint64_t moved = 0;
    };

    class InputClock final: public AudioTarget {
    private:
        AC97CodecDevice &fOwner;

    public:
        explicit InputClock(AC97CodecDevice &owner): fOwner(owner) {}
        void AudioTick() override {fOwner.InputTick();}
    };

    int fAddress;
    uint32_t fVendorId;
    std::unique_ptr<HostAudio> fOut;
    std::unique_ptr<HostAudio> fIn;
    InputClock fInClock {*this};
    AC97Bus *fLinkBus = nullptr;

    /* The rates the host ends run at; 0 where any rate will do. Variable
       rate audio is offered only when the output is not at the link's own
       rate, so that the guest picks what the host plays without
       conversion. */
    uint32_t fDacLock = 0;
    uint32_t fAdcLock = 0;
    bool fVra = false;

    uint16_t fRegs[AC97_REG_COUNT] {};
    uint32_t fDacRate = AC97_LINK_RATE;
    uint32_t fAdcRate = AC97_LINK_RATE;

    bool fOutRunning = false;
    AudioFormat fOutFormat;
    uint64_t fOutMoved = 0;
    bool fInRunning = false;
    AudioFormat fInFormat;
    uint64_t fInMoved = 0;
    Follower fPcmIn;
    Follower fMicIn;
    GainRamp fGain;
    std::vector<uint8_t> fBuf;

    bool VraEnabled() const
        {return fVra && (fRegs[REG_EXT_CTRL / 2] & EXT_CTRL_VRA) != 0;}
    uint32_t DacRate() const
        {return VraEnabled() ? fDacRate : AC97_LINK_RATE;}
    uint32_t AdcRate() const
        {return VraEnabled() ? fAdcRate : AC97_LINK_RATE;}

    void RegisterReset();
    float OutputGain(int ch) const;
    void Play(size_t frames);
    void Follow(AC97StreamEnum stream, Follower &f, uint32_t rate);
    void UpdateStreams();
    void InputTick();

public:
    AC97CodecDevice(const char *name, int address, uint32_t vendor_id,
                    std::unique_ptr<HostAudio> out,
                    std::unique_ptr<HostAudio> in):
        Device(name), fAddress(address), fVendorId(vendor_id),
        fOut(std::move(out)), fIn(std::move(in)) {}

    bool Realize() override;

    /* AC97Codec */
    uint16_t ReadRegister(int reg) override;
    void WriteRegister(int reg, uint16_t val) override;
    void LinkReset() override;
    void StreamsChanged() override {UpdateStreams();}
    void Sync() override;

    /* AudioTarget: the output's clock */
    void AudioTick() override;
};


/* The rate a host end runs at, 0 if any, as far as the link can carry
   it. */
static uint32_t host_rate(HostAudio *audio, const char *name)
{
    AudioCaps caps = audio->Caps();

    if (caps.any_rate) {
        return 0;
    }
    if (caps.rate < AC97_MIN_RATE || caps.rate > AC97_MAX_RATE) {
        fprintf(stderr, "%s: the host end runs at %u Hz, which AC'97 cannot "
                "carry; the guest gets %u Hz, converted on the host\n", name,
                caps.rate, AC97_LINK_RATE);
        return AC97_LINK_RATE;
    }
    return caps.rate;
}


bool AC97CodecDevice::Realize()
{
    fLinkBus = dynamic_cast<AC97Bus *>(ParentBus());
    if (fLinkBus == nullptr) {
        vm_error("%s: must be attached to an AC'97 link\n", Name());
        return false;
    }
    fAddress = fLinkBus->AttachCodec(this, fAddress, Name());
    if (fAddress < 0) {
        return false;
    }
    fOut->target = this;
    fDacLock = host_rate(fOut.get(), Name());
    if (fIn != nullptr) {
        fIn->target = &fInClock;
        fAdcLock = host_rate(fIn.get(), Name());
    }
    fVra = fDacLock != AC97_LINK_RATE;
    RegisterReset();
    return true;
}


//#pragma mark - registers

void AC97CodecDevice::RegisterReset()
{
    memset(fRegs, 0, sizeof(fRegs));
    for (const auto &r : kMixerRegs) {
        fRegs[r.reg / 2] = r.reset;
    }
    fDacRate = AC97_LINK_RATE;
    fAdcRate = AC97_LINK_RATE;
}


uint16_t AC97CodecDevice::ReadRegister(int reg)
{
    switch (reg) {
    case REG_POWERDOWN: {
        uint16_t pd = fRegs[reg / 2];
        uint16_t ready = PD_ADC_READY | PD_DAC_READY | PD_ANALOG_READY |
                         PD_VREF_READY;
        if ((pd & PD_PR0) != 0) {
            ready &= ~PD_ADC_READY;
        }
        if ((pd & PD_PR1) != 0) {
            ready &= ~PD_DAC_READY;
        }
        if ((pd & (PD_PR2 | PD_PR3)) != 0) {
            ready &= ~PD_ANALOG_READY;
        }
        if ((pd & PD_PR3) != 0) {
            ready &= ~PD_VREF_READY;
        }
        return pd | ready;
    }
    case REG_EXT_ID:
        return (fVra ? EXT_ID_VRA : 0) | EXT_ID_REV_22 |
               fAddress << EXT_ID_ADDR_SHIFT;
    case REG_PCM_FRONT_RATE:
        return DacRate();
    case REG_PCM_ADC_RATE:
        return AdcRate();
    case REG_VENDOR_ID1:
        return fVendorId >> 16;
    case REG_VENDOR_ID2:
        return fVendorId & 0xffff;
    }
    return fRegs[reg / 2];
}


void AC97CodecDevice::WriteRegister(int reg, uint16_t val)
{
    switch (reg) {
    case REG_RESET:
        RegisterReset();
        UpdateStreams();
        return;
    case REG_POWERDOWN:
        fRegs[reg / 2] = val & PD_CONTROL_MASK;
        return;
    case REG_EXT_CTRL: {
        bool was = VraEnabled();
        fRegs[reg / 2] = val & (fVra ? EXT_CTRL_VRA : 0);
        if (VraEnabled() != was) {
            /* The converters start out at the rate the host ends run at,
               and go back to the link's when variable rates are off. */
            fDacRate = lock_rate(AC97_LINK_RATE, fDacLock);
            fAdcRate = lock_rate(AC97_LINK_RATE, fAdcLock);
            UpdateStreams();
        }
        return;
    }
    case REG_PCM_FRONT_RATE:
        if (VraEnabled()) {
            fDacRate = lock_rate(val, fDacLock);
            UpdateStreams();
        }
        return;
    case REG_PCM_ADC_RATE:
        if (VraEnabled()) {
            fAdcRate = lock_rate(val, fAdcLock);
            UpdateStreams();
        }
        return;
    case REG_MASTER:
    case REG_HEADPHONE:
    case REG_MASTER_MONO:
        val = saturate_attenuation(val);
        break;
    }
    for (const auto &r : kMixerRegs) {
        if (r.reg == reg) {
            fRegs[reg / 2] = val & r.mask;
            return;
        }
    }
}


void AC97CodecDevice::LinkReset()
{
    RegisterReset();
}


//#pragma mark - streams

/* What reaches the line out: master and PCM volume, with the DACs and
   mixer powered. */
float AC97CodecDevice::OutputGain(int ch) const
{
    uint16_t master = fRegs[REG_MASTER / 2];
    uint16_t pcm = fRegs[REG_PCM_OUT / 2];
    int shift = ch == 0 ? 8 : 0;

    if (((master | pcm) & VOLUME_MUTE) != 0 ||
        (fRegs[REG_POWERDOWN / 2] & (PD_PR1 | PD_PR2 | PD_PR3)) != 0) {
        return 0.0f;
    }
    float db = 1.5f * ((float)PCM_UNITY - (float)get_bits(pcm, shift, 5)) -
               1.5f * (float)get_bits(master, shift, 5);
    return powf(10.0f, db / 20.0f);
}


void AC97CodecDevice::Play(size_t frames)
{
    fGain.Begin(OutputGain(0), OutputGain(1), fOutFormat.rate);
    for (size_t i = 0; i < frames; i++) {
        for (int c = 0; c < 2; c++) {
            uint8_t *p = &fBuf[(i * 2 + c) * AC97_SAMPLE_BYTES];
            float gain = fGain.At(c, i);
            if (gain == 1.0f) {
                continue;
            }
            float s = (float)(int16_t)get_le16(p) * gain;
            s = s < -32768.0f ? -32768.0f : s > 32767.0f ? 32767.0f : s;
            put_le16(p, (uint16_t)(int16_t)lrintf(s));
        }
    }
    fGain.End(frames);
}


/* Silence for an input stream, as much as the output's clock says is due
   at the stream's own rate. */
void AC97CodecDevice::Follow(AC97StreamEnum stream, Follower &f,
                             uint32_t rate)
{
    uint64_t due = fOutMoved * rate / fOutFormat.rate;

    if (f.rate != rate) {
        f.rate = rate;
        f.moved = due;
        return;
    }
    size_t frames = frames_due(due, &f.moved, rate);
    if (frames == 0) {
        return;
    }
    fBuf.assign(frames * ac97_stream_channels(stream) * AC97_SAMPLE_BYTES, 0);
    fLinkBus->Link()->PushInput(stream, fBuf.data(), frames);
}


void AC97CodecDevice::AudioTick()
{
    if (!fOutRunning) {
        return;
    }
    size_t frames = frames_due(fOut->FramesDue(), &fOutMoved,
                               fOutFormat.rate);
    if (frames == 0) {
        return;
    }
    fBuf.resize(frames * fOutFormat.FrameBytes());
    fLinkBus->Link()->PullOutput(fBuf.data(), frames);
    Play(frames);
    fOut->Write(fBuf.data(), frames);

    if (fIn == nullptr) {
        Follow(AC97_PCM_IN, fPcmIn, AdcRate());
    }
    /* No variable rate for the microphone: it records at the link's. */
    Follow(AC97_MIC_IN, fMicIn, AC97_LINK_RATE);
}


void AC97CodecDevice::InputTick()
{
    if (!fInRunning) {
        return;
    }
    size_t frames = frames_due(fIn->FramesDue(), &fInMoved, fInFormat.rate);
    if (frames == 0) {
        return;
    }
    fBuf.resize(frames * fInFormat.FrameBytes());
    fIn->Read(fBuf.data(), frames);
    if ((fRegs[REG_RECORD_GAIN / 2] & VOLUME_MUTE) != 0) {
        memset(fBuf.data(), 0, fBuf.size());
    }
    fLinkBus->Link()->PushInput(AC97_PCM_IN, fBuf.data(), frames);
}


void AC97CodecDevice::Sync()
{
    AudioTick();
    InputTick();
}


/* The host ends run while the controller runs a stream they move: the
   output for every stream it paces, so that capture alone keeps its clock
   too. */
void AC97CodecDevice::UpdateStreams()
{
    AC97Link *link = fLinkBus->Link();
    bool up = fAddress == 0 && link->LinkUp();
    bool pcm_in = up && link->StreamRunning(AC97_PCM_IN);
    bool out = up && (link->StreamRunning(AC97_PCM_OUT) ||
                      link->StreamRunning(AC97_MIC_IN) ||
                      (pcm_in && fIn == nullptr));
    AudioFormat fmt;

    fmt.channels = 2;
    fmt.bits = 16;
    fmt.rate = DacRate();
    if (out) {
        if (!fOutRunning || fmt != fOutFormat) {
            fOutFormat = fmt;
            fOutMoved = 0;
            fPcmIn = Follower();
            fMicIn = Follower();
            fOutRunning = fOut->Start(fmt);
        }
    } else if (fOutRunning) {
        fOut->Stop();
        fOutRunning = false;
    }

    if (fIn == nullptr) {
        return;
    }
    fmt.rate = AdcRate();
    if (pcm_in) {
        if (!fInRunning || fmt != fInFormat) {
            fInFormat = fmt;
            fInMoved = 0;
            fInRunning = fIn->Start(fmt);
        }
    } else if (fInRunning) {
        fIn->Stop();
        fInRunning = false;
    }
}


//#pragma mark - class

/* An audio codec on an AC'97 link; without an "address" it takes the first
   free one. */
class AC97CodecClass final: public DeviceClass {
public:
    AC97CodecClass(): DeviceClass("ac97-codec") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        int address, vendor_id;
        const char *name = cfg.IdOr("ac97-codec");
        std::unique_ptr<HostAudio> in;

        if (!cfg.GetInt("address", &address, -1) ||
            !cfg.GetInt("vendor_id", &vendor_id,
                        (int)AC97_DEFAULT_VENDOR_ID)) {
            return nullptr;
        }
        auto out = config_open_audio(cfg, ctx, AUDIO_RENDER);
        if (out == nullptr) {
            return nullptr;
        }
        if (!json_is_undefined(cfg.Get("input"))) {
            in = config_open_audio(cfg, ctx, AUDIO_CAPTURE, "input");
            if (in == nullptr) {
                return nullptr;
            }
        }
        return new AC97CodecDevice(name, address, (uint32_t)vendor_id,
                                   std::move(out), std::move(in));
    }
};

static const AC97CodecClass sAC97CodecClass;
