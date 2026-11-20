/*
 * USB Audio Class 1.0 device
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
#include <stdio.h>
#include <string.h>

#include <memory>
#include <string>
#include <vector>

#include "bits.h"
#include "config_props.h"
#include "cutils.h"
#include "device_class.h"
#include "host_audio.h"
#include "machine.h"
#include "usb.h"
#include "usb_desc.h"
#include "usb_function.h"

//#define DEBUG_USB_AUDIO

#ifdef DEBUG_USB_AUDIO
#define usb_audio_debug(...) fprintf(stderr, "usb-audio: " __VA_ARGS__)
#else
#define usb_audio_debug(...) do {} while (0)
#endif


#define USB_CLASS_AUDIO          0x01
#define UAC_SUBCLASS_CONTROL     0x01
#define UAC_SUBCLASS_STREAMING   0x02

/* Class specific descriptors. */
#define UAC_DT_CS_INTERFACE      0x24
#define UAC_DT_CS_ENDPOINT       0x25
#define UAC_AC_HEADER            0x01
#define UAC_AC_INPUT_TERMINAL    0x02
#define UAC_AC_OUTPUT_TERMINAL   0x03
#define UAC_AC_FEATURE_UNIT      0x06
#define UAC_AS_GENERAL           0x01
#define UAC_AS_FORMAT_TYPE       0x02
#define UAC_EP_GENERAL           0x01

#define UAC_VERSION              0x0100
#define UAC_FORMAT_TYPE_I        0x01
#define UAC_FORMAT_PCM           0x0001

/* Terminal types. */
#define UAC_TERMINAL_STREAMING   0x0101
#define UAC_TERMINAL_MICROPHONE  0x0201
#define UAC_TERMINAL_SPEAKER     0x0301
#define UAC_TERMINAL_HEADPHONE   0x0302
#define UAC_TERMINAL_LINE        0x0603

/* Class specific requests. */
#define UAC_SET_CUR              0x01
#define UAC_GET_CUR              0x81
#define UAC_GET_MIN              0x82
#define UAC_GET_MAX              0x83
#define UAC_GET_RES              0x84

/* Control selectors. */
#define UAC_FU_MUTE              0x01
#define UAC_FU_VOLUME            0x02
#define UAC_EP_SAMPLING_FREQ     0x01

#define UAC_FU_CONTROL_MUTE      bit_at(0)
#define UAC_FU_CONTROL_VOLUME    bit_at(1)
#define UAC_EP_ATTR_SAMPLING_FREQ bit_at(0)

/* Volume, in 1/256 dB. */
#define UAC_VOLUME_MIN           (-60 * 256)
#define UAC_VOLUME_MAX           0
#define UAC_VOLUME_RES           128

/* Each input or output takes an interface and an endpoint number. */
#define USB_AUDIO_MAX_PORTS      4
#define USB_AUDIO_BITS           16
/* A full speed isochronous packet. */
#define USB_AUDIO_MAX_PACKET     1023

/* What the FIFO holds while it plays, and must reach before it starts. */
#define USB_AUDIO_FIFO_TARGET_MS 10
#define USB_AUDIO_FIFO_MS        100
/* The feedback endpoint reports every 2^n frames. */
#define USB_AUDIO_REFRESH        3
/* The sample clock is measured against the bus over this long, from points
   this far apart. */
#define USB_AUDIO_RATE_WINDOW_MS 8000
#define USB_AUDIO_RATE_STEP_MS   250
#define USB_AUDIO_RATE_MIN_MS    1000
/* How fast the reported rate pulls the FIFO back to its target. */
#define USB_AUDIO_SERVO_MS       500

#define GAIN_RAMP_MS             5


//#pragma mark - USBAudioPort

class USBAudio;

struct USBAudioPortConfig {
    uint16_t terminal_type = UAC_TERMINAL_SPEAKER;
    int channels = 2;
    std::vector<uint32_t> rates;
};


/* One streaming interface and the chain of terminals behind it, with a
   FIFO between the bus and the host end. The host end's clock is the
   sample clock; it is measured against the bus so that the rate the bus
   carries can follow it. */
class USBAudioPort: public Device, public AudioTarget {
protected:
    struct RateSample {
        uint64_t uframe;
        uint64_t frames;
    };

    USBAudioPortConfig fConfig;
    std::unique_ptr<HostAudio> fAudio;
    int fIndex = -1;

    uint32_t fRate = 0;       /* the sampling frequency the host chose */
    bool fStreaming = false;  /* alternate setting 1 */
    bool fStarted = false;    /* the host end runs */
    AudioFormat fFormat;
    uint64_t fMoved = 0;      /* frames moved to or from the host end */
    bool fClockRunning = false; /* the host end's clock has started */

    /* the FIFO, in frames */
    std::vector<uint8_t> fFifo;
    size_t fFifoCap = 0;
    size_t fFifoRead = 0;
    size_t fFifoLevel = 0;
    size_t fFifoTarget = 0;
    bool fPlaying = false;    /* the FIFO has filled and is being drained */
    double fLevelAvg = 0.0;

    std::vector<RateSample> fRateSamples;

    bool fMute = false;
    int16_t fVolume = UAC_VOLUME_MAX;
    /* the gain now, and the ramp it is on: from where to where, and how
       many of its frames have gone */
    float fGain = 1.0f;
    float fRampFrom = 1.0f;
    float fRampTo = 1.0f;
    size_t fRampDone = SIZE_MAX;
    std::vector<uint8_t> fBuf;

    uint32_t fUnderruns = 0;
    uint32_t fOverruns = 0;

    void StartStream();
    void StopStream();
    void FifoPush(const uint8_t *data, size_t frames);
    void FifoPop(uint8_t *out, size_t frames);
    float TargetGain() const;
    void ApplyGain(uint8_t *data, size_t frames);
    void RecordRate(uint64_t uframe);
    double MeasuredRate() const;

public:
    USBAudioPort(const char *name, const USBAudioPortConfig &config,
                 std::unique_ptr<HostAudio> audio);

    bool Realize() override;

    /* Recording: the device sends to the host. */
    virtual bool IsInput() const = 0;

    const USBAudioPortConfig &Config() const {return fConfig;}
    int FrameBytes() const {return fConfig.channels * USB_AUDIO_BITS / 8;}
    uint16_t MaxPacket() const;

    void SetIndex(int index) {fIndex = index;}
    int Index() const {return fIndex;}
    /* The chain, from where the sound comes in to where it goes out. */
    uint8_t InputTerminalId() const {return 3 * fIndex + 1;}
    uint8_t FeatureUnitId() const {return 3 * fIndex + 2;}
    uint8_t OutputTerminalId() const {return 3 * fIndex + 3;}
    /* The terminal on the USB side, which the streaming interface links
       to. */
    uint8_t StreamingTerminalId() const
        {return IsInput() ? OutputTerminalId() : InputTerminalId();}

    /* also what a reset of the USB device does */
    void Reset() override;
    void SetStreaming(bool on);
    bool SetRate(uint32_t rate);
    uint32_t Rate() const {return fRate;}

    bool Mute() const {return fMute;}
    void SetMute(bool mute) {fMute = mute;}
    int16_t Volume() const {return fVolume;}
    void SetVolume(int16_t volume);

    /* The data endpoint's packets, and the feedback endpoint's for a port
       that has one. */
    virtual USBStatusEnum DataPacket(URB *urb) = 0;
    virtual USBStatusEnum FeedbackPacket(URB *urb)
        {(void)urb; return USB_STATUS_STALL;}
};


USBAudioPort::USBAudioPort(const char *name, const USBAudioPortConfig &config,
                           std::unique_ptr<HostAudio> audio):
    Device(name), fConfig(config), fAudio(std::move(audio))
{
    fRate = fConfig.rates[0];
}


uint16_t USBAudioPort::MaxPacket() const
{
    uint32_t max_rate = 0;
    for (uint32_t rate : fConfig.rates) {
        if (rate > max_rate) {
            max_rate = rate;
        }
    }
    /* A packet may carry one frame more than the nominal rate, which is how
       the host follows a feedback value above it, and how a device sending
       on its own clock catches up. */
    return ((max_rate + 999) / 1000 + 1) * FrameBytes();
}


void USBAudioPort::Reset()
{
    SetStreaming(false);
    fRate = fConfig.rates[0];
    fMute = false;
    fVolume = UAC_VOLUME_MAX;
}


bool USBAudioPort::SetRate(uint32_t rate)
{
    bool known = false;
    for (uint32_t r : fConfig.rates) {
        known = known || r == rate;
    }
    if (!known) {
        return false;
    }
    if (rate != fRate) {
        fRate = rate;
        if (fStarted) {
            StartStream();
        }
    }
    return true;
}


void USBAudioPort::SetVolume(int16_t volume)
{
    if (volume < UAC_VOLUME_MIN) {
        volume = UAC_VOLUME_MIN;
    } else if (volume > UAC_VOLUME_MAX) {
        volume = UAC_VOLUME_MAX;
    }
    fVolume = volume;
}


void USBAudioPort::SetStreaming(bool on)
{
    /* The host end starts with the first packet, once the host has also set
       the rate, which it does after selecting the setting. */
    fStreaming = on;
    if (!on && fStarted) {
        StopStream();
    }
}


void USBAudioPort::StartStream()
{
    fFormat.rate = fRate;
    fFormat.channels = fConfig.channels;
    fFormat.bits = USB_AUDIO_BITS;

    fFifoCap = (size_t)fRate * USB_AUDIO_FIFO_MS / 1000;
    fFifoTarget = (size_t)fRate * USB_AUDIO_FIFO_TARGET_MS / 1000;
    fFifo.assign(fFifoCap * FrameBytes(), 0);
    fFifoRead = 0;
    fFifoLevel = 0;
    fPlaying = false;
    fLevelAvg = (double)fFifoTarget;
    fRateSamples.clear();
    fMoved = 0;
    fClockRunning = false;
    fUnderruns = 0;
    fOverruns = 0;
    /* A new stream starts at the volume already set, not ramping to it. */
    fGain = TargetGain();
    fRampFrom = fRampTo = fGain;
    fRampDone = SIZE_MAX;

    usb_audio_debug("%s: start %u Hz, %d channels\n", Name(), fRate,
                    fConfig.channels);
    if (!fAudio->Start(fFormat)) {
        vm_error("%s: the host end did not start\n", Name());
    }
    fStarted = true;
}


void USBAudioPort::StopStream()
{
    usb_audio_debug("%s: stop, %u underruns, %u overruns\n", Name(),
                    fUnderruns, fOverruns);
    fAudio->Stop();
    fStarted = false;
}


void USBAudioPort::FifoPush(const uint8_t *data, size_t frames)
{
    size_t fb = FrameBytes();

    if (frames > fFifoCap - fFifoLevel) {
        fOverruns++;
        frames = fFifoCap - fFifoLevel;
    }
    size_t write = (fFifoRead + fFifoLevel) % fFifoCap;
    while (frames > 0) {
        size_t n = fFifoCap - write;
        if (n > frames) {
            n = frames;
        }
        memcpy(&fFifo[write * fb], data, n * fb);
        data += n * fb;
        fFifoLevel += n;
        frames -= n;
        write = (write + n) % fFifoCap;
    }
}


/* A null 'out' drops the frames. */
void USBAudioPort::FifoPop(uint8_t *out, size_t frames)
{
    size_t fb = FrameBytes();

    while (frames > 0) {
        size_t n = fFifoCap - fFifoRead;
        if (n > frames) {
            n = frames;
        }
        if (out != nullptr) {
            memcpy(out, &fFifo[fFifoRead * fb], n * fb);
            out += n * fb;
        }
        fFifoLevel -= n;
        frames -= n;
        fFifoRead = (fFifoRead + n) % fFifoCap;
    }
}


/* The feature unit's mute and volume, ramped over a few milliseconds so that
   a change does not click. */
float USBAudioPort::TargetGain() const
{
    return fMute ? 0.0f : powf(10.0f, (float)fVolume / 256.0f / 20.0f);
}


/* A new target starts a ramp from wherever the gain is, which runs on
   across calls, however few frames each brings, to reach it in
   GAIN_RAMP_MS. */
void USBAudioPort::ApplyGain(uint8_t *data, size_t frames)
{
    float target = TargetGain();
    size_t ramp = (size_t)fRate * GAIN_RAMP_MS / 1000 + 1;
    int channels = fConfig.channels;

    if (target != fRampTo) {
        fRampFrom = fGain;
        fRampTo = target;
        fRampDone = 0;
    }
    if (fRampDone >= ramp && fGain == 1.0f) {
        return;
    }
    for (size_t i = 0; i < frames; i++) {
        if (fRampDone < ramp) {
            fRampDone++;
            fGain = fRampFrom + (fRampTo - fRampFrom) * (float)fRampDone /
                                    (float)ramp;
        } else {
            fGain = fRampTo;
        }
        for (int c = 0; c < channels; c++) {
            uint8_t *p = data + (i * channels + c) * 2;
            int16_t s = (int16_t)get_le16(p);
            put_le16(p, (uint16_t)(int16_t)lrintf((float)s * fGain));
        }
    }
}


/* A point relating the bus clock to the sample clock, every so often. */
void USBAudioPort::RecordRate(uint64_t uframe)
{
    uint64_t step = USB_AUDIO_RATE_STEP_MS * 8;
    uint64_t window = USB_AUDIO_RATE_WINDOW_MS * 8;

    if (!fClockRunning) {
        return;
    }
    if (!fRateSamples.empty() && uframe < fRateSamples.back().uframe + step) {
        return;
    }
    fRateSamples.push_back({uframe, fAudio->FramesDue()});
    while (fRateSamples.size() > 2 &&
           uframe - fRateSamples[1].uframe >= window) {
        fRateSamples.erase(fRateSamples.begin());
    }
}


/* Frames of the sample clock per 1 ms frame of the bus, or the nominal rate
   until enough of both clocks has passed to tell. */
double USBAudioPort::MeasuredRate() const
{
    double nominal = fRate / 1000.0;

    if (fRateSamples.size() < 2) {
        return nominal;
    }
    const RateSample &a = fRateSamples.front();
    const RateSample &b = fRateSamples.back();
    if (b.uframe - a.uframe < USB_AUDIO_RATE_MIN_MS * 8 || b.frames < a.frames) {
        return nominal;
    }
    return (double)(b.frames - a.frames) * 8.0 / (double)(b.uframe - a.uframe);
}


//#pragma mark - USBAudioOutput

/* A playback path: a streaming interface with an asynchronous OUT endpoint
   and its feedback endpoint, the USB streaming input terminal, the feature
   unit and the output terminal its "kind" names. Packets arrive on the bus
   clock into the FIFO, the host clock drains it, and the feedback endpoint
   tells the host how many frames per USB frame that drain comes to. */
class USBAudioOutput final: public USBAudioPort {
public:
    using USBAudioPort::USBAudioPort;

    bool IsInput() const override {return false;}

    USBStatusEnum DataPacket(URB *urb) override;
    USBStatusEnum FeedbackPacket(URB *urb) override;

    /* AudioTarget */
    void AudioTick() override;
};


USBStatusEnum USBAudioOutput::DataPacket(URB *urb)
{
    uint8_t buf[USB_AUDIO_MAX_PACKET];

    if (!fStreaming || urb->is_in) {
        return USB_STATUS_STALL;
    }
    uint32_t len = urb->buffer != nullptr ? urb->buffer->Length() : 0;
    if (len > sizeof(buf)) {
        return USB_STATUS_BABBLE;
    }
    if (!fStarted) {
        StartStream();
    }
    if (len > 0) {
        len = urb->buffer->Read(0, buf, len);
    }
    size_t frames = len / FrameBytes();
    /* Until the host end's clock runs nothing drains the FIFO, so only the
       newest data is kept, as a device does while its DAC comes up. */
    if (!fClockRunning && fFifoLevel + frames > fFifoTarget) {
        size_t drop = fFifoLevel + frames - fFifoTarget;
        FifoPop(nullptr, drop < fFifoLevel ? drop : fFifoLevel);
    }
    FifoPush(buf, frames);
    RecordRate(urb->microframe);
    urb->actual_length = len;
    return USB_STATUS_OK;
}


/* The rate the host should send at, in 10.14 fixed point frames per frame:
   the measured sample clock, nudged so that the FIFO settles at its
   target. */
USBStatusEnum USBAudioOutput::FeedbackPacket(URB *urb)
{
    uint8_t buf[3];

    if (!fStreaming || !urb->is_in) {
        return USB_STATUS_STALL;
    }

    double nominal = fRate / 1000.0;
    double rate = nominal;
    if (fStarted) {
        RecordRate(urb->microframe);
        fLevelAvg += ((double)fFifoLevel - fLevelAvg) / 8.0;
        double correction =
            ((double)fFifoTarget - fLevelAvg) / USB_AUDIO_SERVO_MS;
        if (correction > nominal / 100.0) {
            correction = nominal / 100.0;
        } else if (correction < -nominal / 100.0) {
            correction = -nominal / 100.0;
        }
        rate = MeasuredRate() + correction;
        if (rate > nominal * 1.02) {
            rate = nominal * 1.02;
        } else if (rate < nominal * 0.98) {
            rate = nominal * 0.98;
        }
    }

    uint32_t value = (uint32_t)lround(rate * 16384.0);
    buf[0] = get_bits(value, 0, 8);
    buf[1] = get_bits(value, 8, 8);
    buf[2] = get_bits(value, 16, 8);
    if ((urb->microframe & 0x1fff) < 8) {
        usb_audio_debug("%s: feedback %.4f (measured %.4f), fifo %zu\n",
                        Name(), rate, MeasuredRate(), fFifoLevel);
    }
    urb->actual_length =
        urb->buffer != nullptr ? urb->buffer->Write(0, buf, sizeof(buf)) : 0;
    return USB_STATUS_OK;
}


/* The host clock moved on: hand it as many frames. The FIFO plays once it
   has filled to its target, and fills again from silence when it runs dry. */
void USBAudioOutput::AudioTick()
{
    if (!fStarted) {
        return;
    }
    uint64_t due = fAudio->FramesDue();
    if (due <= fMoved) {
        return;
    }
    bool clock_started = !fClockRunning;
    fClockRunning = true;
    size_t max_frames = fRate / 10;
    if (due - fMoved > max_frames) {
        fMoved = due - max_frames;
    }
    size_t frames = due - fMoved;
    size_t fb = FrameBytes();

    if (!fPlaying && fFifoLevel >= fFifoTarget) {
        fPlaying = true;
    }
    size_t take = 0;
    size_t lead = 0;
    if (fPlaying) {
        take = frames < fFifoLevel ? frames : fFifoLevel;
        if (take < frames && clock_started) {
            /* A host end may start by taking more than the FIFO holds; that
               is its own lead, filled with silence ahead of the stream. */
            lead = frames - take;
        } else if (take < frames) {
            fUnderruns++;
            fPlaying = false;
            usb_audio_debug("%s: underrun, %zu of %zu frames\n", Name(), take,
                            frames);
        }
    }

    fBuf.assign(frames * fb, 0);
    FifoPop(fBuf.data() + lead * fb, take);
    ApplyGain(fBuf.data(), frames);
    fAudio->Write(fBuf.data(), frames);
    fMoved += frames;
}


//#pragma mark - USBAudioInput

/* A recording path: the input terminal its "kind" names, the feature unit,
   the USB streaming output terminal, and a streaming interface with an
   asynchronous IN endpoint. The host clock fills the FIFO, and each packet
   takes as many frames as that clock made in a frame of the bus, nudged so
   that the FIFO settles at its target; the host follows the packets' sizes
   (UAC 1.0 3.7.2.2). */
class USBAudioInput final: public USBAudioPort {
private:
    double fOwed = 0.0;       /* the fraction of a frame the packets owe */
    std::vector<uint8_t> fPacket;

public:
    using USBAudioPort::USBAudioPort;

    bool IsInput() const override {return true;}

    USBStatusEnum DataPacket(URB *urb) override;

    /* AudioTarget */
    void AudioTick() override;
};


/* Until the FIFO has filled, and again when it runs dry, a packet carries
   silence of the nominal size, so that the stream never stops. */
USBStatusEnum USBAudioInput::DataPacket(URB *urb)
{
    if (!fStreaming || !urb->is_in) {
        return USB_STATUS_STALL;
    }
    if (!fStarted) {
        StartStream();
        fOwed = 0.0;
    }

    double nominal = fRate / 1000.0;
    double rate = nominal;
    if (fClockRunning) {
        RecordRate(urb->microframe);
        fLevelAvg += ((double)fFifoLevel - fLevelAvg) / 8.0;
        double correction =
            (fLevelAvg - (double)fFifoTarget) / USB_AUDIO_SERVO_MS;
        if (correction > nominal / 100.0) {
            correction = nominal / 100.0;
        } else if (correction < -nominal / 100.0) {
            correction = -nominal / 100.0;
        }
        rate = MeasuredRate() + correction;
        if (rate > nominal * 1.02) {
            rate = nominal * 1.02;
        } else if (rate < nominal * 0.98) {
            rate = nominal * 0.98;
        }
    }

    size_t fb = FrameBytes();
    /* so that ten packets at 44.1 come to 441 frames, not 440.99999 */
    fOwed += rate;
    size_t frames = (size_t)(fOwed + 1e-6);
    fOwed -= (double)frames;
    size_t room = urb->buffer != nullptr ? urb->buffer->Length() / fb : 0;
    size_t max_frames = MaxPacket() / fb;
    if (frames > max_frames) {
        frames = max_frames;
    }
    if (frames > room) {
        frames = room;
    }

    if (!fPlaying && fFifoLevel >= fFifoTarget) {
        fPlaying = true;
    }
    fPacket.assign(frames * fb, 0);
    if (fPlaying) {
        size_t take = frames < fFifoLevel ? frames : fFifoLevel;
        FifoPop(fPacket.data(), take);
        if (take < frames) {
            fUnderruns++;
            fPlaying = false;
            usb_audio_debug("%s: underrun, %zu of %zu frames\n", Name(), take,
                            frames);
        }
    }
    ApplyGain(fPacket.data(), frames);
    if ((urb->microframe & 0x1fff) < 8) {
        usb_audio_debug("%s: %zu frames at %.4f (measured %.4f), fifo %zu\n",
                        Name(), frames, rate, MeasuredRate(), fFifoLevel);
    }
    urb->actual_length =
        frames > 0 ? urb->buffer->Write(0, fPacket.data(), frames * fb) : 0;
    return USB_STATUS_OK;
}


/* The host clock moved on: take as many frames from it. A FIFO that is full
   makes room by dropping its oldest, so that what the host gets is never
   late by more than the FIFO holds. */
void USBAudioInput::AudioTick()
{
    if (!fStarted) {
        return;
    }
    uint64_t due = fAudio->FramesDue();
    if (due <= fMoved) {
        return;
    }
    fClockRunning = true;
    size_t max_frames = fFifoCap;
    if (due - fMoved > max_frames) {
        fMoved = due - max_frames;
    }
    size_t frames = due - fMoved;

    fBuf.resize(frames * FrameBytes());
    fAudio->Read(fBuf.data(), frames);
    if (frames > fFifoCap - fFifoLevel) {
        size_t drop = frames - (fFifoCap - fFifoLevel);
        FifoPop(nullptr, drop);
        fOverruns++;
    }
    FifoPush(fBuf.data(), frames);
    fMoved += frames;
}


//#pragma mark - USBAudio

/* The spatial positions of the first channels, as a cluster describes them;
   a single channel has none. */
static uint16_t audio_channel_config(int channels)
{
    switch (channels) {
    case 1: return 0x0000;
    case 2: return 0x0003; /* left and right front */
    case 4: return 0x0033; /* and left and right surround */
    case 6: return 0x003f; /* 5.1 */
    case 8: return 0x063f; /* 7.1, with the sides */
    default: return (uint16_t)(bit_at(channels) - 1);
    }
}


/* A device with one audio control interface and a streaming interface per
   port. Each port is a chain of three entities: a terminal, a feature unit
   with the master mute and volume, and a terminal, one of the two the USB
   streaming terminal. */
class USBAudio final: public USBFunction {
private:
    std::vector<USBAudioPort *> fPorts;

    /* The control interface comes first, then a streaming interface per
       port, each with an endpoint number of its own: the data endpoint's,
       and for an output its feedback endpoint's. */
    uint8_t StreamingInterface(const USBAudioPort *port) const
        {return FirstInterface() + 1 + port->Index();}
    uint8_t EndpointNumber(const USBAudioPort *port) const
        {return FirstEndpoint() + port->Index();}
    uint8_t DataEndpoint(const USBAudioPort *port) const
        {return (port->IsInput() ? USB_DIR_IN : 0) | EndpointNumber(port);}
    uint8_t FeedbackEndpoint(const USBAudioPort *port) const
        {return USB_DIR_IN | EndpointNumber(port);}

    USBAudioPort *PortForInterface(int iface);
    USBAudioPort *PortForEntity(int id);
    USBAudioPort *PortForEndpoint(int address);

    void BuildControlDescriptors(USBDescBuilder &b, USBAudioPort *port);
    void BuildStreamingDescriptors(USBDescBuilder &b, USBAudioPort *port);

    USBStatusEnum Reply(URB *urb, const void *data, uint32_t len);
    USBStatusEnum FeatureUnitRequest(URB *urb, USBAudioPort *port);
    USBStatusEnum EndpointRequest(URB *urb, USBAudioPort *port);

public:
    USBAudio(): USBFunction("usb-audio") {}

    bool AttachPort(USBAudioPort *port);

    USBFunctionInfo Info() const override;
    int InterfaceCount() const override {return 1 + fPorts.size();}
    int EndpointCount() const override {return fPorts.size();}
    void BuildDescriptors(USBDescBuilder &b) override;

    bool SetInterface(int iface, int alt) override;
    void Reset() override;
    USBStatusEnum HandleControl(URB *urb) override;
    USBStatusEnum Submit(URB *urb) override;
};


USBFunctionInfo USBAudio::Info() const
{
    return {
        .speed = USB_SPEED_FULL,
        .product_id = 0x0004,
        .product = "USB Audio",
    };
}


bool USBAudio::AttachPort(USBAudioPort *port)
{
    if (fPorts.size() >= USB_AUDIO_MAX_PORTS) {
        vm_error("usb-audio: at most %d inputs and outputs\n",
                 USB_AUDIO_MAX_PORTS);
        return false;
    }
    port->SetIndex(fPorts.size());
    fPorts.push_back(port);
    return DescriptorsChanged();
}


/* The port's terminals and unit, in the order the sound goes through
   them. */
void USBAudio::BuildControlDescriptors(USBDescBuilder &b, USBAudioPort *port)
{
    const USBAudioPortConfig &c = port->Config();
    bool input = port->IsInput();

    b.BeginDescriptor(UAC_DT_CS_INTERFACE);
    b.Byte(UAC_AC_INPUT_TERMINAL);
    b.Byte(port->InputTerminalId());
    b.Word(input ? c.terminal_type : UAC_TERMINAL_STREAMING);
    b.Byte(0); /* no associated terminal */
    b.Byte(c.channels);
    b.Word(audio_channel_config(c.channels));
    b.Byte(0); /* no channel names */
    b.Byte(0);
    b.EndDescriptor();

    b.BeginDescriptor(UAC_DT_CS_INTERFACE);
    b.Byte(UAC_AC_FEATURE_UNIT);
    b.Byte(port->FeatureUnitId());
    b.Byte(port->InputTerminalId());
    b.Byte(1); /* one byte of controls per channel */
    b.Byte(UAC_FU_CONTROL_MUTE | UAC_FU_CONTROL_VOLUME); /* master */
    for (int ch = 0; ch < c.channels; ch++) {
        b.Byte(0);
    }
    b.Byte(0);
    b.EndDescriptor();

    b.BeginDescriptor(UAC_DT_CS_INTERFACE);
    b.Byte(UAC_AC_OUTPUT_TERMINAL);
    b.Byte(port->OutputTerminalId());
    b.Word(input ? UAC_TERMINAL_STREAMING : c.terminal_type);
    b.Byte(0);
    b.Byte(port->FeatureUnitId());
    b.Byte(0);
    b.EndDescriptor();
}


void USBAudio::BuildStreamingDescriptors(USBDescBuilder &b,
                                         USBAudioPort *port)
{
    const USBAudioPortConfig &c = port->Config();
    uint8_t number = StreamingInterface(port);

    /* Setting 0 takes no bandwidth; setting 1 streams. */
    b.BeginInterface({
        .number = number,
        .iface_class = USB_CLASS_AUDIO,
        .iface_subclass = UAC_SUBCLASS_STREAMING,
    });
    b.BeginInterface({
        .number = number,
        .alternate = 1,
        .iface_class = USB_CLASS_AUDIO,
        .iface_subclass = UAC_SUBCLASS_STREAMING,
    });

    b.BeginDescriptor(UAC_DT_CS_INTERFACE);
    b.Byte(UAC_AS_GENERAL);
    b.Byte(port->StreamingTerminalId());
    b.Byte(1); /* a frame of delay */
    b.Word(UAC_FORMAT_PCM);
    b.EndDescriptor();

    b.BeginDescriptor(UAC_DT_CS_INTERFACE);
    b.Byte(UAC_AS_FORMAT_TYPE);
    b.Byte(UAC_FORMAT_TYPE_I);
    b.Byte(c.channels);
    b.Byte(USB_AUDIO_BITS / 8);
    b.Byte(USB_AUDIO_BITS);
    b.Byte(c.rates.size());
    for (uint32_t rate : c.rates) {
        b.Byte(get_bits(rate, 0, 8));
        b.Byte(get_bits(rate, 8, 8));
        b.Byte(get_bits(rate, 16, 8));
    }
    b.EndDescriptor();

    /* An input sends on its own clock and needs no feedback: the host
       follows the sizes of its packets. */
    b.AudioEndpoint(DataEndpoint(port),
                    USB_ENDPOINT_ATTR_ISOCH | USB_ENDPOINT_SYNC_ASYNC,
                    port->MaxPacket(), 1, 0,
                    port->IsInput() ? 0 : FeedbackEndpoint(port));

    b.BeginDescriptor(UAC_DT_CS_ENDPOINT);
    b.Byte(UAC_EP_GENERAL);
    b.Byte(UAC_EP_ATTR_SAMPLING_FREQ);
    b.Byte(0); /* lock delay units */
    b.Word(0);
    b.EndDescriptor();

    if (!port->IsInput()) {
        /* The feedback value is three bytes at full speed. */
        b.AudioEndpoint(FeedbackEndpoint(port), USB_ENDPOINT_ATTR_ISOCH, 3,
                        1, USB_AUDIO_REFRESH, 0);
    }
}


void USBAudio::BuildDescriptors(USBDescBuilder &b)
{
    int n = fPorts.size();

    b.BeginInterface({
        .number = FirstInterface(),
        .iface_class = USB_CLASS_AUDIO,
        .iface_subclass = UAC_SUBCLASS_CONTROL,
    });

    /* The header's total covers itself and every unit and terminal. */
    int total = 8 + n;
    for (USBAudioPort *port : fPorts) {
        total += 12 + (8 + port->Config().channels) + 9;
    }
    b.BeginDescriptor(UAC_DT_CS_INTERFACE);
    b.Byte(UAC_AC_HEADER);
    b.Word(UAC_VERSION);
    b.Word(total);
    b.Byte(n);
    for (USBAudioPort *port : fPorts) {
        b.Byte(StreamingInterface(port));
    }
    b.EndDescriptor();

    for (USBAudioPort *port : fPorts) {
        BuildControlDescriptors(b, port);
    }
    for (USBAudioPort *port : fPorts) {
        BuildStreamingDescriptors(b, port);
    }
}


/* 'iface' counts from the control interface. */
USBAudioPort *USBAudio::PortForInterface(int iface)
{
    if (iface < 1 || iface > (int)fPorts.size()) {
        return nullptr;
    }
    return fPorts[iface - 1];
}


USBAudioPort *USBAudio::PortForEntity(int id)
{
    for (USBAudioPort *port : fPorts) {
        if (port->FeatureUnitId() == id) {
            return port;
        }
    }
    return nullptr;
}


USBAudioPort *USBAudio::PortForEndpoint(int address)
{
    for (USBAudioPort *port : fPorts) {
        if (DataEndpoint(port) == address ||
            (!port->IsInput() && FeedbackEndpoint(port) == address)) {
            return port;
        }
    }
    return nullptr;
}


bool USBAudio::SetInterface(int iface, int alt)
{
    if (iface == 0) {
        return alt == 0;
    }
    USBAudioPort *port = PortForInterface(iface);
    if (port == nullptr || alt > 1) {
        return false;
    }
    port->SetStreaming(alt == 1);
    return true;
}


void USBAudio::Reset()
{
    for (USBAudioPort *port : fPorts) {
        port->Reset();
    }
}


USBStatusEnum USBAudio::Reply(URB *urb, const void *data, uint32_t len)
{
    if (len > urb->setup.length) {
        len = urb->setup.length;
    }
    urb->actual_length =
        urb->buffer != nullptr ? urb->buffer->Write(0, data, len) : 0;
    return USB_STATUS_OK;
}


/* Mute and volume of the master channel; the per channel controls are not
   offered. */
USBStatusEnum USBAudio::FeatureUnitRequest(URB *urb, USBAudioPort *port)
{
    const USBSetup &setup = urb->setup;
    int selector = setup.value >> 8;
    int channel = get_bits(setup.value, 0, 8);
    uint8_t buf[2] = {0, 0};

    if (channel != 0) {
        return USB_STATUS_STALL;
    }

    if (setup.request == UAC_SET_CUR) {
        uint32_t len = urb->buffer != nullptr && setup.length > 0
                       ? urb->buffer->Read(0, buf, setup.length < 2
                                                       ? setup.length : 2)
                       : 0;
        if (selector == UAC_FU_MUTE && len >= 1) {
            port->SetMute(buf[0] != 0);
        } else if (selector == UAC_FU_VOLUME && len >= 2) {
            port->SetVolume((int16_t)get_le16(buf));
        } else {
            return USB_STATUS_STALL;
        }
        urb->actual_length = len;
        return USB_STATUS_OK;
    }

    if (selector == UAC_FU_MUTE) {
        if (setup.request != UAC_GET_CUR) {
            return USB_STATUS_STALL;
        }
        buf[0] = port->Mute() ? 1 : 0;
        return Reply(urb, buf, 1);
    }
    if (selector != UAC_FU_VOLUME) {
        return USB_STATUS_STALL;
    }

    int16_t value;
    switch (setup.request) {
    case UAC_GET_CUR: value = port->Volume(); break;
    case UAC_GET_MIN: value = UAC_VOLUME_MIN; break;
    case UAC_GET_MAX: value = UAC_VOLUME_MAX; break;
    case UAC_GET_RES: value = UAC_VOLUME_RES; break;
    default:
        return USB_STATUS_STALL;
    }
    put_le16(buf, (uint16_t)value);
    return Reply(urb, buf, 2);
}


/* The sampling frequency of the data endpoint, three bytes in hertz. */
USBStatusEnum USBAudio::EndpointRequest(URB *urb, USBAudioPort *port)
{
    const USBSetup &setup = urb->setup;
    uint8_t buf[3] = {0, 0, 0};

    if ((setup.value >> 8) != UAC_EP_SAMPLING_FREQ ||
        get_bits(setup.index, 0, 8) != DataEndpoint(port)) {
        return USB_STATUS_STALL;
    }

    if (setup.request == UAC_SET_CUR) {
        if (urb->buffer == nullptr || setup.length < 3 ||
            urb->buffer->Read(0, buf, 3) != 3) {
            return USB_STATUS_STALL;
        }
        uint32_t rate = buf[0] | (uint32_t)buf[1] << 8 |
                        (uint32_t)buf[2] << 16;
        usb_audio_debug("%s: rate %u\n", port->Name(), rate);
        if (!port->SetRate(rate)) {
            return USB_STATUS_STALL;
        }
        urb->actual_length = 3;
        return USB_STATUS_OK;
    }
    if (setup.request != UAC_GET_CUR) {
        return USB_STATUS_STALL;
    }
    uint32_t rate = port->Rate();
    buf[0] = get_bits(rate, 0, 8);
    buf[1] = get_bits(rate, 8, 8);
    buf[2] = get_bits(rate, 16, 8);
    return Reply(urb, buf, 3);
}


USBStatusEnum USBAudio::HandleControl(URB *urb)
{
    const USBSetup &setup = urb->setup;

    if ((setup.request_type & USB_TYPE_MASK) != USB_TYPE_CLASS) {
        return USB_STATUS_STALL;
    }

    switch (setup.request_type & USB_RECIP_MASK) {
    case USB_RECIP_INTERFACE: {
        /* The entity in the high byte, the control interface in the low. */
        if (get_bits(setup.index, 0, 8) != FirstInterface()) {
            return USB_STATUS_STALL;
        }
        USBAudioPort *port = PortForEntity(setup.index >> 8);
        if (port == nullptr) {
            return USB_STATUS_STALL;
        }
        return FeatureUnitRequest(urb, port);
    }

    case USB_RECIP_ENDPOINT: {
        USBAudioPort *port = PortForEndpoint(get_bits(setup.index, 0, 8));
        if (port == nullptr) {
            return USB_STATUS_STALL;
        }
        return EndpointRequest(urb, port);
    }
    }
    return USB_STATUS_STALL;
}


USBStatusEnum USBAudio::Submit(URB *urb)
{
    if (urb->type != USB_ENDPOINT_ISOCH) {
        return USB_STATUS_STALL;
    }
    int address = urb->endpoint | (urb->is_in ? USB_DIR_IN : 0);
    USBAudioPort *port = PortForEndpoint(address);
    if (port == nullptr) {
        return USB_STATUS_STALL;
    }
    return address == DataEndpoint(port) ? port->DataPacket(urb)
                                         : port->FeedbackPacket(urb);
}


//#pragma mark - bus

/* The bus a USB audio device provides: its outputs and inputs, numbered in
   the order they are declared. */
class USBAudioBus final: public Bus {
private:
    USBAudio *fAudio;

public:
    USBAudioBus(Device *owner, USBAudio *audio): Bus(owner), fAudio(audio) {}

    const char *Type() const override {return "usb-audio";}
    USBAudio *Audio() const {return fAudio;}

    bool AssignResources(Device *dev) override;
};


bool USBAudioBus::AssignResources(Device *dev)
{
    for (int i = 0; i < dev->ResourceCount(); i++) {
        if (dev->ResourceAt(i)->type != RES_NONE) {
            vm_error("usb-audio bus: device '%s' declared a resource, but an "
                     "audio port has none of its own\n", dev->Name());
            return false;
        }
    }
    return true;
}


bool USBAudioPort::Realize()
{
    USBAudioBus *bus = dynamic_cast<USBAudioBus *>(ParentBus());
    if (bus == nullptr) {
        vm_error("%s: must be attached to a USB audio device\n", Name());
        return false;
    }
    if (!bus->Audio()->AttachPort(this)) {
        return false;
    }
    fAudio->target = this;
    return true;
}


//#pragma mark - classes

/* "port" is the port asked for, or 0 for the first free one. */
class USBAudioClass final: public DeviceClass {
public:
    USBAudioClass(): DeviceClass("usb-audio") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        int port;

        (void)ctx;
        if (!cfg.GetInt("port", &port, 0)) {
            return nullptr;
        }
        if (!cfg.HasChildren()) {
            vm_error("usb-audio: needs a nested bus with at least one input "
                     "or output on it\n");
            return nullptr;
        }
        USBAudio *audio = new USBAudio();
        USBFunctionNode *node = new USBFunctionNode("usb-audio", audio, port);

        node->SetChildBus(new USBAudioBus(node, audio));
        return node;
    }
};

static const USBAudioClass sUSBAudioClass;


struct USBAudioKind {
    const char *name;
    uint16_t terminal_type;
};

static const USBAudioKind sOutputKinds[] = {
    {"speaker", UAC_TERMINAL_SPEAKER},
    {"headphone", UAC_TERMINAL_HEADPHONE},
    {"line-out", UAC_TERMINAL_LINE},
    {nullptr, 0},
};

static const USBAudioKind sInputKinds[] = {
    {"microphone", UAC_TERMINAL_MICROPHONE},
    {"line-in", UAC_TERMINAL_LINE},
    {nullptr, 0},
};


/* "kind" among 'kinds', the first of them by default; "channels"; "rates",
   only the host end's own by default. */
static bool parse_port_config(const DeviceConfig &cfg,
                              const USBAudioKind *kinds,
                              USBAudioPortConfig *out, uint32_t host_rate)
{
    const char *type = cfg.Type();
    const char *kind;

    if (!cfg.GetStrOpt("kind", &kind) ||
        !cfg.GetInt("channels", &out->channels, 2)) {
        return false;
    }
    out->terminal_type = kinds[0].terminal_type;
    if (kind != nullptr) {
        const USBAudioKind *k = kinds;
        while (k->name != nullptr && strcmp(k->name, kind) != 0) {
            k++;
        }
        if (k->name == nullptr) {
            std::string names;
            for (k = kinds; k->name != nullptr; k++) {
                names += names.empty() ? "" : k[1].name == nullptr ? " or "
                                                                   : ", ";
                names += std::string("\"") + k->name + "\"";
            }
            vm_error("%s: 'kind' must be %s\n", type, names.c_str());
            return false;
        }
        out->terminal_type = k->terminal_type;
    }
    if (out->channels < 1 || out->channels > 8) {
        vm_error("%s: 'channels' must be between 1 and 8\n", type);
        return false;
    }

    JSONValue list = cfg.Get("rates");
    if (!json_is_undefined(list)) {
        if (list.type != JSON_ARRAY || list.u.array->Length() == 0) {
            vm_error("%s: 'rates' must be an array of sample rates\n", type);
            return false;
        }
        for (int i = 0; i < list.u.array->Length(); i++) {
            JSONValue item = json_array_get(list, i);
            if (item.type != JSON_INT || item.u.int32 < 8000 ||
                item.u.int32 > 96000) {
                vm_error("%s: 'rates' may only hold rates from 8000 to "
                         "96000\n", type);
                return false;
            }
            out->rates.push_back(item.u.int32);
        }
    } else {
        /* Only the host end's own rate, so that nothing is converted. */
        out->rates.push_back(host_rate);
    }

    uint32_t max_rate = 0;
    for (uint32_t rate : out->rates) {
        if (rate > max_rate) {
            max_rate = rate;
        }
    }
    if (((max_rate + 999) / 1000 + 1) * out->channels * USB_AUDIO_BITS / 8 >
        USB_AUDIO_MAX_PACKET) {
        vm_error("%s: %d channels at %u Hz do not fit a full speed "
                 "isochronous packet\n", type, out->channels, max_rate);
        return false;
    }
    return true;
}


/* A playback path, playing to the host. */
class USBAudioOutputClass final: public DeviceClass {
public:
    USBAudioOutputClass(): DeviceClass("usb-audio-output") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        USBAudioPortConfig config;
        const char *name = cfg.IdOr("usb-audio-output");

        auto audio = config_open_audio(cfg, ctx, AUDIO_RENDER);
        if (audio == nullptr) {
            return nullptr;
        }
        AudioCaps caps = audio->Caps();
        if (!parse_port_config(cfg, sOutputKinds, &config, caps.rate)) {
            return nullptr;
        }
        return new USBAudioOutput(name, config, std::move(audio));
    }
};

static const USBAudioOutputClass sUSBAudioOutputClass;


/* A recording path, recording from the host. */
class USBAudioInputClass final: public DeviceClass {
public:
    USBAudioInputClass(): DeviceClass("usb-audio-input") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        USBAudioPortConfig config;
        const char *name = cfg.IdOr("usb-audio-input");

        auto audio = config_open_audio(cfg, ctx, AUDIO_CAPTURE);
        if (audio == nullptr) {
            return nullptr;
        }
        AudioCaps caps = audio->Caps();
        if (!parse_port_config(cfg, sInputKinds, &config, caps.rate)) {
            return nullptr;
        }
        return new USBAudioInput(name, config, std::move(audio));
    }
};

static const USBAudioInputClass sUSBAudioInputClass;
