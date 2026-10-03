/*
 * High Definition Audio codec
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
#include "hda_codec.h"

#include <math.h>
#include <string.h>

#include "config_props.h"
#include "hda.h"
#include "machine.h"

/* Verbs with a 12 bit identifier and 8 bits of payload. */
#define VERB_GET_PARAMETER     0xf00
#define VERB_GET_CONN_SELECT   0xf01
#define VERB_SET_CONN_SELECT   0x701
#define VERB_GET_CONN_LIST     0xf02
#define VERB_GET_PROC_STATE    0xf03
#define VERB_SET_PROC_STATE    0x703
#define VERB_GET_POWER_STATE   0xf05
#define VERB_SET_POWER_STATE   0x705
#define VERB_GET_STREAM        0xf06
#define VERB_SET_STREAM        0x706
#define VERB_GET_PIN_CTL       0xf07
#define VERB_SET_PIN_CTL       0x707
#define VERB_GET_UNSOL         0xf08
#define VERB_SET_UNSOL         0x708
#define VERB_GET_PIN_SENSE     0xf09
#define VERB_GET_EAPD          0xf0c
#define VERB_SET_EAPD          0x70c
#define VERB_GET_CONFIG        0xf1c
#define VERB_SET_CONFIG0       0x71c
#define VERB_SET_CONFIG3       0x71f
#define VERB_GET_SUBSYSTEM     0xf20
#define VERB_SET_SUBSYSTEM0    0x720
#define VERB_SET_SUBSYSTEM3    0x723
#define VERB_FUNCTION_RESET    0x7ff

/* Verbs with a 4 bit identifier and 16 bits of payload. */
#define VERB4_SET_FORMAT       0x2
#define VERB4_SET_AMP          0x3
#define VERB4_GET_FORMAT       0xa
#define VERB4_GET_AMP          0xb

#define  AMP_SET_OUTPUT        bit_at(15)
#define  AMP_SET_LEFT          bit_at(13)
#define  AMP_SET_RIGHT         bit_at(12)
#define  AMP_GET_OUTPUT        bit_at(15)
#define  AMP_GET_LEFT          bit_at(13)
#define  AMP_MUTE              bit_at(7)
#define  AMP_GAIN_MASK         0x7f

/* parameters */
#define PARAM_VENDOR_ID        0x00
#define PARAM_SUBSYSTEM_ID     0x01
#define PARAM_REVISION_ID      0x02
#define PARAM_NODE_COUNT       0x04
#define PARAM_FG_TYPE          0x05
#define PARAM_PCM              0x0a
#define PARAM_STREAM_FORMATS   0x0b
#define PARAM_WIDGET_CAPS      0x09
#define PARAM_PIN_CAPS         0x0c
#define PARAM_CONN_LIST_LEN    0x0e
#define PARAM_POWER_STATES     0x0f
#define PARAM_OUT_AMP_CAPS     0x12

#define FG_TYPE_AUDIO          0x01
#define STREAM_FORMAT_PCM      bit_at(0)
#define POWER_STATES_D0_D3     0x0f
#define POWER_D3               3

/* widget capabilities */
#define WIDGET_OUTPUT          0x0
#define WIDGET_PIN             0x4
#define WCAP_TYPE_SHIFT        20
#define WCAP_CHAN_EXT_SHIFT    13
#define WCAP_STEREO            bit_at(0)
#define WCAP_OUT_AMP           bit_at(2)
#define WCAP_AMP_OVERRIDE      bit_at(3)
#define WCAP_FORMAT_OVERRIDE   bit_at(4)
#define WCAP_CONN_LIST         bit_at(8)

#define PINCAP_PRESENCE        bit_at(2)
#define PINCAP_HEADPHONE       bit_at(3)
#define PINCAP_OUTPUT          bit_at(4)
#define PINCTL_OUT_ENABLE      bit_at(6)
#define PIN_SENSE_PRESENT      bit_at(31)

/* The pin configuration default fields. */
#define CONFIG_JACK            0x0
#define CONFIG_FIXED           0x2
#define CONFIG_CONN_SHIFT      30
#define CONFIG_LOCATION_SHIFT  24
#define CONFIG_DEVICE_SHIFT    20
#define CONFIG_TYPE_SHIFT      16
#define CONFIG_COLOR_SHIFT     12
#define CONFIG_MISC_SHIFT      8
#define CONFIG_ASSOC_SHIFT     4
#define CONFIG_TYPE_MINIJACK   0x1
#define CONFIG_MISC_NO_DETECT  0x1
#define LOCATION_INTERNAL      0x10

/* An output amplifier from -95.25 dB to 0 dB in 0.75 dB steps, with a mute. */
#define AMP_STEPS              0x7f
#define AMP_STEP_QUARTER_DB    3
#define AMP_CAPS               (bit_at(31) | (AMP_STEP_QUARTER_DB - 1) << 16 | \
                                AMP_STEPS << 8 | AMP_STEPS)

/* The widths a converter offers: 16, 24 and 32 bits. */
#define PCM_BITS               (bit_at(HDA_PCM_BITS_SHIFT + 1) | \
                                bit_at(HDA_PCM_BITS_SHIFT + 3) | \
                                bit_at(HDA_PCM_BITS_SHIFT + 4))

/* A converter's format after a reset: 48 kHz, 16 bit, stereo. */
#define FORMAT_DEFAULT         0x0011

/* How long a change of gain takes, so that it is not heard as a click. */
#define GAIN_RAMP_MS           5


class HDACodecDevice;
class HDAAudioGroupDevice;
class HDAPort;


/* One node of a function group: what the widget is, and what the guest set
   it to. */
struct HDAWidget {
    int nid = 0;
    HDAPort *port = nullptr;
    uint32_t caps = 0;
    uint32_t pcm = 0;
    uint32_t pin_caps = 0;
    uint32_t config = 0;
    uint32_t out_amp_caps = 0;
    std::vector<int> conns;
    bool present = false;

    uint8_t conn_select = 0;
    uint8_t power = 0;
    uint8_t proc_state = 0;
    uint8_t pin_ctl = 0;
    uint8_t unsol = 0;
    uint8_t eapd = 0;
    uint8_t stream = 0; /* tag and channel */
    uint16_t format = FORMAT_DEFAULT;
    uint8_t gain[2] = {AMP_STEPS, AMP_STEPS};
    bool mute[2] = {false, false};

    int Tag() const {return stream >> 4;}
    int Channel() const {return stream & 0xf;}

    void Reset()
    {
        conn_select = 0;
        power = 0;
        proc_state = 0;
        pin_ctl = 0;
        unsol = 0;
        eapd = 0;
        stream = 0;
        format = FORMAT_DEFAULT;
        gain[0] = gain[1] = AMP_STEPS;
        mute[0] = mute[1] = false;
    }
};


static bool is_output_kind(HDAPinDeviceEnum kind)
{
    return kind == HDA_PIN_LINE_OUT || kind == HDA_PIN_SPEAKER ||
           kind == HDA_PIN_HEADPHONE;
}


bool hda_pin_kind_from_name(const char *name, HDAPinDeviceEnum *out)
{
    static const struct {
        const char *name;
        HDAPinDeviceEnum kind;
    } kKinds[] = {
        {"line-out", HDA_PIN_LINE_OUT},
        {"speaker", HDA_PIN_SPEAKER},
        {"headphone", HDA_PIN_HEADPHONE},
        {"line-in", HDA_PIN_LINE_IN},
        {"mic-in", HDA_PIN_MIC_IN},
    };
    for (const auto &k : kKinds) {
        if (strcmp(name, k.name) == 0) {
            *out = k.kind;
            return true;
        }
    }
    return false;
}


bool hda_pin_location_from_name(const char *name, int *out)
{
    static const struct {
        const char *name;
        int location;
    } kLocations[] = {
        {"rear", 0x01},
        {"front", 0x02},
        {"left", 0x03},
        {"right", 0x04},
        {"top", 0x05},
        {"bottom", 0x06},
        {"internal", LOCATION_INTERNAL},
    };
    for (const auto &l : kLocations) {
        if (strcmp(name, l.name) == 0) {
            *out = l.location;
            return true;
        }
    }
    return false;
}


//#pragma mark - HDAPort

/* A port of an audio function group: the widgets one configuration node
   contributes, and the host stream behind them. */
class HDAPort: public Device {
public:
    explicit HDAPort(const char *name): Device(name) {}

    virtual int WidgetCount() const = 0;
    /* Describe the widgets at 'widgets', whose node ids are already set. */
    virtual void BuildWidgets(HDAAudioGroupDevice *group,
                              HDAWidget *widgets) = 0;
    /* The guest changed one of the port's widgets, or the group. */
    virtual void WidgetChanged() = 0;
    virtual void StreamOutput(int tag, const uint8_t *data, size_t frames,
                              const AudioFormat &fmt)
        {(void)tag; (void)data; (void)frames; (void)fmt;}
};


//#pragma mark - HDAAudioGroupDevice

/* The bus an audio function group provides. Its ports are nodes of the
   group, not addressable parts, so it assigns no resources. */
class HDAAudioGroupBus final: public Bus {
public:
    explicit HDAAudioGroupBus(Device *owner): Bus(owner) {}

    const char *Type() const override {return "hda-audio-group";}
    bool AssignResources(Device *dev) override;
};


class HDAAudioGroupDevice final: public Device {
private:
    HDAAudioGroupBus *fChildBus = nullptr;
    HDACodecDevice *fCodec = nullptr;
    int fNid = 0;
    int fFirstWidget = 0;
    uint8_t fPower = 0;
    uint8_t fUnsol = 0;
    std::vector<HDAWidget> fWidgets;
    std::vector<HDAPort *> fPorts;

    uint32_t Parameter(const HDAWidget *w, int param);
    uint32_t GroupVerb(uint32_t verb);
    uint32_t WidgetVerb(HDAWidget &w, uint32_t verb);
    void NotifyPorts();

public:
    explicit HDAAudioGroupDevice(const char *name): Device(name) {}
    ~HDAAudioGroupDevice() override {delete fChildBus;}

    bool Prepare() override;
    bool Realize() override;
    Bus *ChildBus() override {return fChildBus;}

    /* Lay the widgets out from 'first_widget' on; returns how many. */
    int Layout(HDACodecDevice *codec, int nid, int first_widget);
    int Nid() const {return fNid;}
    bool Owns(int nid) const
    {
        return nid == fNid || (nid >= fFirstWidget &&
                               nid < fFirstWidget + (int)fWidgets.size());
    }
    bool Powered() const {return fPower != POWER_D3;}
    HDACodecDevice *Codec() const {return fCodec;}

    uint32_t Command(int nid, uint32_t verb);
    void Reset();
    void StreamOutput(int tag, const uint8_t *data, size_t frames,
                      const AudioFormat &fmt);
};


//#pragma mark - HDACodecDevice

/* The bus a codec provides, carrying its function groups. */
class HDACodecBus final: public Bus {
public:
    explicit HDACodecBus(Device *owner): Bus(owner) {}

    const char *Type() const override {return "hda-codec";}
    bool AssignResources(Device *dev) override;
};


class HDACodecDevice final: public Device, public HDACodec {
private:
    int fAddress;
    uint32_t fVendorId;
    uint32_t fSubsystemId;
    uint32_t fRevisionId;
    HDACodecBus *fChildBus = nullptr;
    HDABus *fLinkBus = nullptr;
    std::vector<HDAAudioGroupDevice *> fGroups;
    bool fLaidOut = false;

    void Layout();

public:
    HDACodecDevice(const char *name, int address, uint32_t vendor_id,
                   uint32_t subsystem_id, uint32_t revision_id):
        Device(name), fAddress(address), fVendorId(vendor_id),
        fSubsystemId(subsystem_id), fRevisionId(revision_id) {}
    ~HDACodecDevice() override {delete fChildBus;}

    bool Prepare() override;
    bool Realize() override;
    Bus *ChildBus() override {return fChildBus;}

    HDALink *Link() const {return fLinkBus->Link();}
    uint32_t VendorId() const {return fVendorId;}
    uint32_t SubsystemId() const {return fSubsystemId;}
    void SetSubsystemByte(int byte, uint8_t val)
    {
        fSubsystemId = set_bits(fSubsystemId, byte * 8, 8, val);
    }

    /* HDACodec */
    uint32_t Command(uint32_t verb) override;
    void LinkReset() override;
    void StreamOutput(int tag, const uint8_t *data, size_t frames,
                      const AudioFormat &fmt) override;
};


bool HDACodecBus::AssignResources(Device *dev)
{
    if (dynamic_cast<HDAAudioGroupDevice *>(dev) == nullptr) {
        vm_error("hda-codec bus: '%s' is not a function group\n", dev->Name());
        return false;
    }
    return true;
}


bool HDACodecDevice::Prepare()
{
    fChildBus = new HDACodecBus(this);
    return true;
}


bool HDACodecDevice::Realize()
{
    fLinkBus = dynamic_cast<HDABus *>(ParentBus());
    if (fLinkBus == nullptr) {
        vm_error("%s: must be attached to an HDA link\n", Name());
        return false;
    }
    fAddress = fLinkBus->AttachCodec(this, fAddress, Name());
    return fAddress >= 0;
}


/* Node ids go to the function groups first, which the root node counts,
   then to each group's widgets in turn. Done once the whole tree exists,
   which is when the guest first talks to the codec. */
void HDACodecDevice::Layout()
{
    if (fLaidOut) {
        return;
    }
    fLaidOut = true;
    for (int i = 0; i < fChildBus->DeviceCount(); i++) {
        fGroups.push_back(
            static_cast<HDAAudioGroupDevice *>(fChildBus->DeviceAt(i)));
    }
    int nid = 1 + (int)fGroups.size();
    for (size_t i = 0; i < fGroups.size(); i++) {
        nid += fGroups[i]->Layout(this, 1 + (int)i, nid);
    }
}


uint32_t HDACodecDevice::Command(uint32_t verb)
{
    int nid = get_bits(verb, 20, 8);
    uint32_t payload = get_bits(verb, 0, 20);

    Layout();
    if (nid != 0) {
        for (HDAAudioGroupDevice *group : fGroups) {
            if (group->Owns(nid)) {
                return group->Command(nid, payload);
            }
        }
        return 0;
    }

    /* The root node answers parameters and nothing else. */
    if ((payload >> 8) != VERB_GET_PARAMETER) {
        return 0;
    }
    switch (payload & 0xff) {
    case PARAM_VENDOR_ID:
        return fVendorId;
    case PARAM_SUBSYSTEM_ID:
        return fSubsystemId;
    case PARAM_REVISION_ID:
        return fRevisionId;
    case PARAM_NODE_COUNT:
        return (1 << 16) | (uint32_t)fGroups.size();
    }
    return 0;
}


void HDACodecDevice::LinkReset()
{
    Layout();
    for (HDAAudioGroupDevice *group : fGroups) {
        group->Reset();
    }
}


void HDACodecDevice::StreamOutput(int tag, const uint8_t *data, size_t frames,
                                  const AudioFormat &fmt)
{
    for (HDAAudioGroupDevice *group : fGroups) {
        group->StreamOutput(tag, data, frames, fmt);
    }
}


//#pragma mark - HDAAudioGroupDevice

bool HDAAudioGroupBus::AssignResources(Device *dev)
{
    if (dynamic_cast<HDAPort *>(dev) == nullptr) {
        vm_error("hda-audio-group bus: '%s' is not a port\n", dev->Name());
        return false;
    }
    return true;
}


bool HDAAudioGroupDevice::Prepare()
{
    fChildBus = new HDAAudioGroupBus(this);
    return true;
}


bool HDAAudioGroupDevice::Realize()
{
    if (dynamic_cast<HDACodecBus *>(ParentBus()) == nullptr) {
        vm_error("%s: must be attached to a codec\n", Name());
        return false;
    }
    return true;
}


int HDAAudioGroupDevice::Layout(HDACodecDevice *codec, int nid,
                                int first_widget)
{
    size_t count = 0;

    fCodec = codec;
    fNid = nid;
    fFirstWidget = first_widget;
    for (int i = 0; i < fChildBus->DeviceCount(); i++) {
        HDAPort *port = static_cast<HDAPort *>(fChildBus->DeviceAt(i));
        fPorts.push_back(port);
        count += port->WidgetCount();
    }
    /* Sized once, so that the ports may keep pointers into it. */
    fWidgets.resize(count);
    int index = 0;
    for (HDAPort *port : fPorts) {
        HDAWidget *w = &fWidgets[index];
        for (int i = 0; i < port->WidgetCount(); i++) {
            w[i].nid = first_widget + index + i;
            w[i].port = port;
        }
        port->BuildWidgets(this, w);
        index += port->WidgetCount();
    }
    return (int)fWidgets.size();
}


void HDAAudioGroupDevice::Reset()
{
    fPower = 0;
    fUnsol = 0;
    for (HDAWidget &w : fWidgets) {
        w.Reset();
    }
    NotifyPorts();
}


void HDAAudioGroupDevice::NotifyPorts()
{
    for (HDAPort *port : fPorts) {
        port->WidgetChanged();
    }
}


uint32_t HDAAudioGroupDevice::Parameter(const HDAWidget *w, int param)
{
    if (w == nullptr) {
        switch (param) {
        case PARAM_VENDOR_ID:
            return fCodec->VendorId();
        case PARAM_SUBSYSTEM_ID:
            return fCodec->SubsystemId();
        case PARAM_NODE_COUNT:
            return (uint32_t)fFirstWidget << 16 | (uint32_t)fWidgets.size();
        case PARAM_FG_TYPE:
            return FG_TYPE_AUDIO;
        case PARAM_PCM:
            /* what a converter without formats of its own would take; every
               one here has its own */
            for (const HDAWidget &cw : fWidgets) {
                if (cw.pcm != 0) {
                    return cw.pcm;
                }
            }
            return bit_at(hda_rate_index(48000)) |
                   bit_at(HDA_PCM_BITS_SHIFT + 1);
        case PARAM_STREAM_FORMATS:
            return STREAM_FORMAT_PCM;
        case PARAM_POWER_STATES:
            return POWER_STATES_D0_D3;
        }
        return 0;
    }

    switch (param) {
    case PARAM_WIDGET_CAPS:
        return w->caps;
    case PARAM_PCM:
        return w->pcm;
    case PARAM_STREAM_FORMATS:
        return w->pcm != 0 ? STREAM_FORMAT_PCM : 0;
    case PARAM_PIN_CAPS:
        return w->pin_caps;
    case PARAM_CONN_LIST_LEN:
        return (uint32_t)w->conns.size();
    case PARAM_OUT_AMP_CAPS:
        return w->out_amp_caps;
    }
    return 0;
}


uint32_t HDAAudioGroupDevice::GroupVerb(uint32_t verb)
{
    uint32_t id = verb >> 8;
    uint8_t data = verb & 0xff;

    switch (id) {
    case VERB_GET_PARAMETER:
        return Parameter(nullptr, data);
    case VERB_GET_POWER_STATE:
        return fPower << 4 | fPower;
    case VERB_SET_POWER_STATE:
        fPower = data & 3;
        NotifyPorts();
        return 0;
    case VERB_GET_UNSOL:
        return fUnsol;
    case VERB_SET_UNSOL:
        fUnsol = data;
        return 0;
    case VERB_GET_SUBSYSTEM:
        return fCodec->SubsystemId();
    case VERB_SET_SUBSYSTEM0 ... VERB_SET_SUBSYSTEM3:
        fCodec->SetSubsystemByte(id - VERB_SET_SUBSYSTEM0, data);
        return 0;
    case VERB_FUNCTION_RESET:
        Reset();
        return 0;
    }
    return 0;
}


uint32_t HDAAudioGroupDevice::WidgetVerb(HDAWidget &w, uint32_t verb)
{
    uint32_t id4 = verb >> 16;

    if (id4 != 0x7 && id4 != 0xf) {
        uint16_t data = verb & 0xffff;
        bool has_amp = (w.caps & WCAP_OUT_AMP) != 0;
        switch (id4) {
        case VERB4_GET_FORMAT:
            return w.pcm != 0 ? w.format : 0;
        case VERB4_SET_FORMAT:
            if (w.pcm != 0) {
                w.format = data & 0x7f7f;
            }
            return 0;
        case VERB4_GET_AMP:
            if (!has_amp || (data & AMP_GET_OUTPUT) == 0) {
                return 0;
            } else {
                int ch = (data & AMP_GET_LEFT) != 0 ? 0 : 1;
                return (w.mute[ch] ? AMP_MUTE : 0) | w.gain[ch];
            }
        case VERB4_SET_AMP:
            if (has_amp && (data & AMP_SET_OUTPUT) != 0) {
                uint8_t gain = data & AMP_GAIN_MASK;
                if (gain > AMP_STEPS) {
                    gain = AMP_STEPS;
                }
                for (int ch = 0; ch < 2; ch++) {
                    if ((data & (ch == 0 ? AMP_SET_LEFT : AMP_SET_RIGHT)) != 0) {
                        w.gain[ch] = gain;
                        w.mute[ch] = (data & AMP_MUTE) != 0;
                    }
                }
            }
            return 0;
        }
        /* processing coefficients: there are none */
        return 0;
    }

    uint32_t id = verb >> 8;
    uint8_t data = verb & 0xff;
    switch (id) {
    case VERB_GET_PARAMETER:
        return Parameter(&w, data);
    case VERB_GET_CONN_SELECT:
        return w.conn_select;
    case VERB_SET_CONN_SELECT:
        if (data < w.conns.size()) {
            w.conn_select = data;
        }
        return 0;
    case VERB_GET_CONN_LIST: {
        /* four short form entries from the one asked for */
        uint32_t val = 0;
        for (int i = 0; i < 4 && data + i < (int)w.conns.size(); i++) {
            val |= (uint32_t)w.conns[data + i] << (8 * i);
        }
        return val;
    }
    case VERB_GET_PROC_STATE:
        return w.proc_state;
    case VERB_SET_PROC_STATE:
        w.proc_state = data;
        return 0;
    case VERB_GET_POWER_STATE:
        return w.power << 4 | w.power;
    case VERB_SET_POWER_STATE:
        w.power = data & 3;
        return 0;
    case VERB_GET_STREAM:
        return w.pcm != 0 ? w.stream : 0;
    case VERB_SET_STREAM:
        if (w.pcm != 0) {
            w.stream = data;
        }
        return 0;
    case VERB_GET_PIN_CTL:
        return w.pin_caps != 0 ? w.pin_ctl : 0;
    case VERB_SET_PIN_CTL:
        if (w.pin_caps != 0) {
            w.pin_ctl = data;
        }
        return 0;
    case VERB_GET_UNSOL:
        return w.unsol;
    case VERB_SET_UNSOL:
        w.unsol = data;
        return 0;
    case VERB_GET_PIN_SENSE:
        return w.pin_caps != 0 && w.present ? PIN_SENSE_PRESENT : 0;
    case VERB_GET_EAPD:
        return w.eapd;
    case VERB_SET_EAPD:
        w.eapd = data;
        return 0;
    case VERB_GET_CONFIG:
        return w.config;
    case VERB_SET_CONFIG0 ... VERB_SET_CONFIG3:
        if (w.pin_caps != 0) {
            w.config = set_bits(w.config, (id - VERB_SET_CONFIG0) * 8, 8,
                                data);
        }
        return 0;
    case VERB_GET_SUBSYSTEM:
        return fCodec->SubsystemId();
    }
    return 0;
}


uint32_t HDAAudioGroupDevice::Command(int nid, uint32_t verb)
{
    if (nid == fNid) {
        return GroupVerb(verb);
    }
    HDAWidget &w = fWidgets[nid - fFirstWidget];
    uint32_t response = WidgetVerb(w, verb);
    w.port->WidgetChanged();
    return response;
}


void HDAAudioGroupDevice::StreamOutput(int tag, const uint8_t *data,
                                       size_t frames, const AudioFormat &fmt)
{
    for (HDAPort *port : fPorts) {
        port->StreamOutput(tag, data, frames, fmt);
    }
}


//#pragma mark - HDAOutputPort

/* Samples as left justified 32 bit integers, whatever their container. */
static int32_t sample_get(const uint8_t *p, int bytes)
{
    switch (bytes) {
    case 1:
        return (int32_t)((uint32_t)p[0] << 24);
    case 2:
        return (int32_t)(((uint32_t)p[0] | (uint32_t)p[1] << 8) << 16);
    default:
        return (int32_t)((uint32_t)p[0] | (uint32_t)p[1] << 8 |
                         (uint32_t)p[2] << 16 | (uint32_t)p[3] << 24);
    }
}


static void sample_put(uint8_t *p, int bytes, int32_t v)
{
    uint32_t u = (uint32_t)v >> (32 - bytes * 8);
    for (int i = 0; i < bytes; i++) {
        p[i] = (uint8_t)(u >> (8 * i));
    }
}


/* An output converter and the pin it drives. Its host stream runs while
   the converter is set to a stream, and the host's clock is what pulls the
   stream's frames from the controller. */
class HDAOutputPort final: public HDAPort, public AudioTarget {
private:
    HDAPortConfig fConfig;
    std::unique_ptr<HostAudio> fAudio;
    HDAAudioGroupDevice *fGroup = nullptr;
    HDAWidget *fConverter = nullptr;
    HDAWidget *fPin = nullptr;

    bool fRunning = false;
    AudioFormat fFormat; /* what the host stream runs at */
    uint64_t fMoved = 0;
    /* the gain applied last, per channel of a stereo pair */
    float fGain[2] = {0.0f, 0.0f};
    std::vector<uint8_t> fOut;

    float TargetGain(int ch) const;
    void Play(const uint8_t *data, size_t frames, const AudioFormat &fmt);

public:
    HDAOutputPort(const char *name, const HDAPortConfig &config,
                  std::unique_ptr<HostAudio> audio):
        HDAPort(name), fConfig(config), fAudio(std::move(audio)) {}

    bool Realize() override;

    /* HDAPort */
    int WidgetCount() const override {return 2;}
    void BuildWidgets(HDAAudioGroupDevice *group, HDAWidget *widgets) override;
    void WidgetChanged() override;
    void StreamOutput(int tag, const uint8_t *data, size_t frames,
                      const AudioFormat &fmt) override;

    /* AudioTarget */
    void AudioTick() override;
};


bool HDAOutputPort::Realize()
{
    if (dynamic_cast<HDAAudioGroupBus *>(ParentBus()) == nullptr) {
        vm_error("%s: must be attached to an audio function group\n", Name());
        return false;
    }
    fAudio->target = this;
    return true;
}


void HDAOutputPort::BuildWidgets(HDAAudioGroupDevice *group,
                                 HDAWidget *widgets)
{
    const HDAPortConfig &c = fConfig;
    AudioCaps caps = fAudio->Caps();
    int location = c.location;
    /* green for a line out, black for headphones */
    uint32_t color = c.kind == HDA_PIN_HEADPHONE ? 0x1 : 0x4;

    fGroup = group;
    fConverter = &widgets[0];
    fPin = &widgets[1];

    /* The converter offers the host end's own rate unless told otherwise, so
       that whatever the guest picks plays without conversion. */
    uint32_t rates = 0;
    for (uint32_t rate : c.rates) {
        rates |= bit_at(hda_rate_index(rate));
    }
    if (rates == 0) {
        int index = hda_rate_index(caps.rate);
        rates = bit_at(index >= 0 ? index : hda_rate_index(48000));
    }
    fConverter->caps = WIDGET_OUTPUT << WCAP_TYPE_SHIFT |
                       ((c.channels - 1) >> 1) << WCAP_CHAN_EXT_SHIFT |
                       WCAP_OUT_AMP | WCAP_AMP_OVERRIDE |
                       WCAP_FORMAT_OVERRIDE |
                       (c.channels > 1 ? WCAP_STEREO : 0);
    fConverter->pcm = rates | PCM_BITS;
    fConverter->out_amp_caps = AMP_CAPS;

    if (location < 0) {
        location = c.kind == HDA_PIN_SPEAKER ? LOCATION_INTERNAL
                   : c.kind == HDA_PIN_HEADPHONE ? 0x02 : 0x01;
    }
    bool fixed = (location & 0x30) == LOCATION_INTERNAL;
    fPin->caps = WIDGET_PIN << WCAP_TYPE_SHIFT | WCAP_CONN_LIST |
                 (c.channels > 1 ? WCAP_STEREO : 0);
    fPin->pin_caps = PINCAP_OUTPUT |
                     (c.kind == HDA_PIN_HEADPHONE ? PINCAP_HEADPHONE : 0) |
                     (fixed ? 0 : PINCAP_PRESENCE);
    fPin->config = (fixed ? CONFIG_FIXED : CONFIG_JACK) << CONFIG_CONN_SHIFT |
                   (uint32_t)location << CONFIG_LOCATION_SHIFT |
                   (uint32_t)c.kind << CONFIG_DEVICE_SHIFT |
                   (fixed ? 0 : CONFIG_TYPE_MINIJACK) << CONFIG_TYPE_SHIFT |
                   (fixed ? 0 : color) << CONFIG_COLOR_SHIFT |
                   (fixed ? CONFIG_MISC_NO_DETECT : 0) << CONFIG_MISC_SHIFT |
                   (uint32_t)c.association << CONFIG_ASSOC_SHIFT |
                   (uint32_t)c.sequence;
    fPin->conns.push_back(fConverter->nid);
    fPin->present = fixed || c.plugged;
}


void HDAOutputPort::WidgetChanged()
{
    AudioFormat fmt;
    bool run = fConverter->Tag() != 0 &&
               hda_format_decode(fConverter->format, &fmt);

    if (run) {
        if (fmt.channels > fConfig.channels) {
            fmt.channels = fConfig.channels;
        }
        if (fRunning && fmt == fFormat) {
            return;
        }
        fFormat = fmt;
        fMoved = 0;
        fRunning = fAudio->Start(fmt);
    } else if (fRunning) {
        fAudio->Stop();
        fRunning = false;
    }
}


float HDAOutputPort::TargetGain(int ch) const
{
    if (!fGroup->Powered() || (fPin->pin_ctl & PINCTL_OUT_ENABLE) == 0 ||
        fConverter->mute[ch]) {
        return 0.0f;
    }
    float db = (float)(fConverter->gain[ch] - AMP_STEPS) *
               AMP_STEP_QUARTER_DB / 4.0f;
    return powf(10.0f, db / 20.0f);
}


/* The channels this converter takes from the stream, at the gain the guest
   set, in the width the host stream runs at. */
void HDAOutputPort::Play(const uint8_t *data, size_t frames,
                         const AudioFormat &fmt)
{
    int in_bytes = fmt.ContainerBytes();
    int out_bytes = fFormat.ContainerBytes();
    int first = fConverter->Channel();
    size_t ramp = fFormat.rate * GAIN_RAMP_MS / 1000 + 1;
    float target[2] = {TargetGain(0), TargetGain(1)};

    fOut.assign(frames * fFormat.FrameBytes(), 0);
    for (size_t i = 0; i < frames; i++) {
        const uint8_t *src = data + i * fmt.FrameBytes();
        uint8_t *dst = &fOut[i * fFormat.FrameBytes()];
        float gain[2];
        for (int g = 0; g < 2; g++) {
            gain[g] = i < ramp ? fGain[g] + (target[g] - fGain[g]) *
                                                (float)(i + 1) / (float)ramp
                               : target[g];
        }
        for (int c = 0; c < fFormat.channels; c++) {
            if (first + c >= fmt.channels) {
                break;
            }
            int32_t s = sample_get(src + (first + c) * in_bytes, in_bytes);
            if (gain[c & 1] != 1.0f) {
                s = (int32_t)((double)s * gain[c & 1]);
            }
            sample_put(dst + c * out_bytes, out_bytes, s);
        }
    }
    for (int g = 0; g < 2; g++) {
        fGain[g] = frames >= ramp ? target[g]
                   : fGain[g] + (target[g] - fGain[g]) * (float)frames /
                                    (float)ramp;
    }
    fAudio->Write(fOut.data(), frames);
    fMoved += frames;
}


void HDAOutputPort::StreamOutput(int tag, const uint8_t *data, size_t frames,
                                 const AudioFormat &fmt)
{
    if (fRunning && tag == fConverter->Tag()) {
        Play(data, frames, fmt);
    }
}


void HDAOutputPort::AudioTick()
{
    if (!fRunning) {
        return;
    }
    uint64_t due = fAudio->FramesDue();
    if (due <= fMoved) {
        return;
    }
    /* After a long stall the lost time is let go rather than made up for
       in one burst. */
    size_t max_frames = fFormat.rate / 10;
    if (due - fMoved > max_frames) {
        fMoved = due - max_frames;
    }
    size_t frames = due - fMoved;

    switch (fGroup->Codec()->Link()->PullOutput(fConverter->Tag(), frames,
                                                this)) {
    case HDA_STREAM_MOVED:
        /* played as the frames went past */
        break;
    case HDA_STREAM_IDLE: {
        /* No stream is running: the converter plays silence. */
        std::vector<uint8_t> silence(frames * fFormat.FrameBytes(), 0);
        fAudio->Write(silence.data(), frames);
        fMoved += frames;
        break;
    }
    case HDA_STREAM_PACED:
        /* another converter's clock moves the stream */
        break;
    }
}


//#pragma mark - classes

/* A port's pin and converter. Reports and returns false on anything it
   cannot read. */
static bool parse_port_config(const DeviceConfig &cfg, HDAPortConfig *out)
{
    const char *type = cfg.Type();
    const char *kind, *location;
    int plugged;

    if (!cfg.GetStrOpt("kind", &kind) ||
        !cfg.GetStrOpt("location", &location) ||
        !cfg.GetInt("association", &out->association, 1) ||
        !cfg.GetInt("sequence", &out->sequence, 0) ||
        !cfg.GetInt("channels", &out->channels, 2) ||
        !cfg.GetInt("plugged", &plugged, 1)) {
        return false;
    }
    if (kind != nullptr && !hda_pin_kind_from_name(kind, &out->kind)) {
        vm_error("%s: unknown kind '%s'\n", type, kind);
        return false;
    }
    if (location != nullptr &&
        !hda_pin_location_from_name(location, &out->location)) {
        vm_error("%s: unknown location '%s'\n", type, location);
        return false;
    }
    if (out->association < 1 || out->association > 15) {
        vm_error("%s: 'association' must be between 1 and 15\n", type);
        return false;
    }
    if (out->sequence < 0 || out->sequence > 15) {
        vm_error("%s: 'sequence' must be between 0 and 15\n", type);
        return false;
    }
    if (out->channels < 1 || out->channels > 16) {
        vm_error("%s: 'channels' must be between 1 and 16\n", type);
        return false;
    }
    out->plugged = plugged != 0;

    JSONValue list = cfg.Get("rates");
    if (!json_is_undefined(list)) {
        if (list.type != JSON_ARRAY) {
            vm_error("%s: 'rates' must be an array of sample rates\n", type);
            return false;
        }
        for (int i = 0; i < list.u.array->Length(); i++) {
            JSONValue item = json_array_get(list, i);
            if (item.type != JSON_INT || hda_rate_index(item.u.int32) < 0) {
                vm_error("%s: 'rates' may only hold rates the link carries, "
                         "8000 to 192000\n", type);
                return false;
            }
            out->rates.push_back(item.u.int32);
        }
    }
    return true;
}


/* A codec on an HDA link; without an "address" it takes the first free
   one. */
class HDACodecClass final: public DeviceClass {
public:
    HDACodecClass(): DeviceClass("hda-codec") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        int address, vendor_id, subsystem_id, revision_id;

        (void)ctx;
        if (!cfg.GetInt("address", &address, -1) ||
            !cfg.GetInt("vendor_id", &vendor_id,
                        HDA_CODEC_DEFAULT_VENDOR_ID) ||
            !cfg.GetInt("subsystem_id", &subsystem_id, 0) ||
            !cfg.GetInt("revision_id", &revision_id, 0x00100100)) {
            return nullptr;
        }
        if (!cfg.HasChildren()) {
            vm_error("hda-codec: needs a nested bus with a function group on "
                     "it\n");
            return nullptr;
        }
        return new HDACodecDevice(cfg.IdOr("hda-codec"), address, vendor_id,
                                  subsystem_id, revision_id);
    }
};

static const HDACodecClass sHDACodecClass;


/* The audio function group of a codec, which its ports hang from. */
class HDAAudioGroupClass final: public DeviceClass {
public:
    HDAAudioGroupClass(): DeviceClass("hda-audio-group") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        (void)ctx;
        return new HDAAudioGroupDevice(cfg.IdOr("hda-audio-group"));
    }
};

static const HDAAudioGroupClass sHDAAudioGroupClass;


/* A converter and the pin it drives, playing to the host. */
class HDAOutputClass final: public DeviceClass {
public:
    HDAOutputClass(): DeviceClass("hda-output") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        HDAPortConfig config;
        const char *name = cfg.IdOr("hda-output");

        if (!parse_port_config(cfg, &config)) {
            return nullptr;
        }
        auto audio = config_open_audio(cfg, ctx, AUDIO_RENDER);
        if (audio == nullptr) {
            return nullptr;
        }
        if (!is_output_kind(config.kind)) {
            vm_error("%s: 'kind' must be \"line-out\", \"speaker\" or "
                     "\"headphone\"\n", name);
            return nullptr;
        }
        return new HDAOutputPort(name, config, std::move(audio));
    }
};

static const HDAOutputClass sHDAOutputClass;
