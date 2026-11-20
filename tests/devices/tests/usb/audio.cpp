/*
 * USB audio devices
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
#include "format.h"
#include "heap.h"
#include "test.h"
#include "test_control.h"
#include "testctl.h"
#include "usb.h"

/* USB audio functions by the Universal Serial Bus Device Class Definition
   for Audio Devices, release 1.0 (UAC1) and its Audio Data Formats: the
   class specific descriptors (4), the feature unit and endpoint requests
   (5.2) and isochronous streaming. The host ends are temu's "test" audio
   driver, whose clock the test moves, so what a stream plays does not
   depend on how fast anything runs. The machines tag the streams: 1 and 2
   are the first device's outputs, 3 the composite device's. */

#define USB_CLASS_AUDIO 0x01
#define UAC_SUBCLASS_CONTROL   0x01
#define UAC_SUBCLASS_STREAMING 0x02
#define USB_DT_IAD 0x0b
#define UAC_CS_INTERFACE 0x24
#define UAC_CS_ENDPOINT  0x25
#define UAC_HEADER          0x01
#define UAC_INPUT_TERMINAL  0x02
#define UAC_OUTPUT_TERMINAL 0x03
#define UAC_FEATURE_UNIT    0x06
#define UAC_AS_GENERAL      0x01
#define UAC_FORMAT_TYPE     0x02

#define UAC_SET_CUR 0x01
#define UAC_GET_CUR 0x81
#define UAC_GET_MIN 0x82
#define UAC_GET_MAX 0x83
#define UAC_GET_RES 0x84
#define UAC_FU_MUTE   0x01
#define UAC_FU_VOLUME 0x02
#define UAC_FU_BASS   0x03
#define UAC_SAMPLING_FREQ 0x01

#define UAC_IF_OUT (USB_TYPE_CLASS | USB_RECIP_INTERFACE)
#define UAC_IF_IN  (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE)
#define UAC_EP_OUT (USB_TYPE_CLASS | USB_RECIP_ENDPOINT)
#define UAC_EP_IN  (USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_ENDPOINT)

#define MAX_PORTS 4
#define MAX_RATES 8
/* TDs queued at once, well within a ring of 32 TRBs */
#define BATCH 16

namespace {

/* One streaming interface and the entities behind it. */
struct Port {
    uint8_t iface;        /* the streaming interface's number */
    uint8_t terminal;     /* the USB streaming terminal it links to */
    uint8_t unit;         /* the feature unit on that terminal */
    uint8_t data_ep, sync_ep;
    uint16_t max_packet;
    int channels;
    int subframe;         /* bytes a sample takes */
    uint32_t rates[MAX_RATES];
    int nrates;
    uint32_t tag;         /* of its test audio stream */
};

struct Audio {
    UsbDevice d;
    uint8_t control;      /* the audio control interface's number */
    Port ports[MAX_PORTS];
    int nports;
};

uint32_t le24(const uint8_t *p)
{
    return p[0] | (uint32_t)p[1] << 8 | (uint32_t)p[2] << 16;
}

/* The class specific descriptors after interface descriptor 'id', up to the
   next interface: the 'n'th with this subtype, or nullptr. */
const uint8_t *cs_desc(const UsbDevice &d, const uint8_t *id, uint8_t subtype,
                       int n = 0)
{
    const uint8_t *end = d.config + d.config_len;
    for (const uint8_t *p = id + id[0]; p + 3 <= end && p[0] >= 3;
         p += p[0]) {
        if (p[1] == USB_DT_INTERFACE)
            break;
        if (p[1] == UAC_CS_INTERFACE && p[2] == subtype && n-- == 0)
            return p;
    }
    return nullptr;
}

/* The 'index'th device with an audio control interface, addressed and
   configured, its streaming interfaces read from the descriptors and
   tagged from 'first_tag' on; or the test is skipped. */
void open_audio(Audio *a, int index, uint32_t first_tag)
{
    const uint8_t *ac = nullptr;
    for (int i = 0; usb_find_device(0, i, &a->d); i++) {
        for (int k = 0; (ac = usb_find_interface(&a->d, k)) != nullptr; k++) {
            if (ac[5] == USB_CLASS_AUDIO && ac[6] == UAC_SUBCLASS_CONTROL)
                break;
        }
        if (ac != nullptr && index-- == 0)
            break;
        ac = nullptr;
    }
    if (ac == nullptr)
        t_skip("no USB audio device");
    a->control = ac[2];

    const uint8_t *hdr = cs_desc(a->d, ac, UAC_HEADER);
    ASSERT_TRUE(hdr != nullptr);
    a->nports = min<int>(hdr[7], MAX_PORTS);
    for (int i = 0; i < a->nports; i++) {
        Port &p = a->ports[i];
        p.iface = hdr[8 + i];
        p.tag = first_tag + i;
        const uint8_t *as = usb_find_alt(&a->d, p.iface, 1);
        ASSERT_TRUE(as != nullptr);
        const uint8_t *gen = cs_desc(a->d, as, UAC_AS_GENERAL);
        const uint8_t *fmt = cs_desc(a->d, as, UAC_FORMAT_TYPE);
        ASSERT_TRUE(gen != nullptr && fmt != nullptr);
        p.terminal = gen[3];
        p.channels = fmt[4];
        p.subframe = fmt[5];
        p.nrates = min<int>(fmt[7], MAX_RATES);
        for (int r = 0; r < p.nrates; r++)
            p.rates[r] = le24(fmt + 8 + 3 * r);
        UsbEndpoint ep;
        ASSERT_TRUE(a->d.FindAltEndpoint(p.iface, 1, 0, &ep));
        p.data_ep = ep.address;
        p.max_packet = ep.max_packet;
        p.sync_ep = a->d.FindAltEndpoint(p.iface, 1, 1, &ep) ? ep.address : 0;
        p.unit = 0;
        for (int n = 0;; n++) {
            const uint8_t *fu = cs_desc(a->d, ac, UAC_FEATURE_UNIT, n);
            if (fu == nullptr)
                break;
            if (fu[4] == p.terminal)
                p.unit = fu[3];
        }
    }
    ASSERT_EQ(a->d.Configure(), CC_SUCCESS);
}

/* A request, with a stall cleared for the next one. */
uint32_t req(Audio &a, uint8_t type, uint8_t r, uint16_t value,
             uint16_t index, uint16_t len = 0, void *buf = nullptr,
             uint32_t *got = nullptr)
{
    uint32_t cc = a.d.Control(type, r, value, index, len, buf, got);
    if (cc == CC_STALL)
        a.d.Recover(1);
    return cc;
}

uint16_t unit_index(const Audio &a, const Port &p)
{
    return (uint16_t)(p.unit << 8 | a.control);
}

/* A two byte control of the port's feature unit, master channel. */
uint32_t fu_get(Audio &a, const Port &p, uint8_t r, uint8_t sel,
                int16_t *value)
{
    uint8_t *buf = (uint8_t *)usb_alloc(2);
    buf[0] = buf[1] = 0;
    uint32_t len = sel == UAC_FU_MUTE ? 1 : 2;
    uint32_t cc = req(a, UAC_IF_IN, r, sel << 8, unit_index(a, p), len, buf);
    *value = len == 1 ? buf[0] : (int16_t)le16(buf);
    return cc;
}

uint32_t fu_set(Audio &a, const Port &p, uint8_t sel, int16_t value)
{
    uint8_t *buf = (uint8_t *)usb_alloc(2);
    put_le16(buf, (uint16_t)value);
    return req(a, UAC_IF_OUT, UAC_SET_CUR, sel << 8, unit_index(a, p),
               sel == UAC_FU_MUTE ? 1 : 2, buf);
}

uint32_t rate_get(Audio &a, const Port &p, uint32_t *rate)
{
    uint8_t *buf = (uint8_t *)usb_alloc(3);
    memset(buf, 0, 3);
    uint32_t cc = req(a, UAC_EP_IN, UAC_GET_CUR, UAC_SAMPLING_FREQ << 8,
                      p.data_ep, 3, buf);
    *rate = le24(buf);
    return cc;
}

uint32_t rate_set(Audio &a, const Port &p, uint32_t rate)
{
    uint8_t *buf = (uint8_t *)usb_alloc(3);
    buf[0] = (uint8_t)rate;
    buf[1] = (uint8_t)(rate >> 8);
    buf[2] = (uint8_t)(rate >> 16);
    return req(a, UAC_EP_OUT, UAC_SET_CUR, UAC_SAMPLING_FREQ << 8, p.data_ep,
               3, buf);
}

/* The port's test stream, selected for the audio registers. */
void select_stream(const Port &p)
{
    ASSERT_GE(tc_audio_find(p.tag), 0);
}

int frame_bytes(const Port &p)
{
    return p.channels * p.subframe;
}

/* Frame 'f' of what the tests play, which is what the test driver records:
   each sample the top bytes of TEST_AUDIO_SAMPLE. */
void make_frames(const Port &p, uint32_t first, uint32_t frames, uint8_t *out)
{
    for (uint32_t f = 0; f < frames; f++) {
        for (int ch = 0; ch < p.channels; ch++) {
            uint32_t s = TEST_AUDIO_SAMPLE(first + f, ch);
            uint8_t *q = out + (f * p.channels + ch) * p.subframe;
            for (int i = 0; i < p.subframe; i++)
                q[i] = (uint8_t)(s >> (8 * (4 - p.subframe + i)));
        }
    }
}

/* Plays frames 'first' to 'first + frames' of the pattern, 'per_packet' to
   a packet, each in a frame of its own; false when a TD did not go
   through. */
bool play(Audio &a, const Port &p, uint32_t first, uint32_t frames,
          uint32_t per_packet)
{
    int dci = usb_dci(p.data_ep);
    uint32_t fb = frame_bytes(p);
    /* a page, so that no packet's buffer crosses a 64 KB boundary (xHCI
       6.4.1) */
    ASSERT_LE(BATCH * per_packet * fb, 4096u);
    uint8_t *buf = (uint8_t *)usb_alloc(4096, 4096);
    uint32_t done = 0;

    while (done < frames) {
        uint32_t sizes[BATCH];
        int n = 0;
        while (n < BATCH && done < frames) {
            uint32_t k = min(per_packet, frames - done);
            make_frames(p, first + done, k, buf + n * per_packet * fb);
            sizes[n] = k * fb;
            done += k;
            n++;
        }
        for (int i = 0; i < n; i++)
            a.d.QueueIsoch(dci, buf + i * per_packet * fb, sizes[i], -1,
                           i == n - 1);
        for (int i = 0; i < n; i++) {
            uint32_t actual = 0;
            uint32_t cc = a.d.WaitIsoch(dci, sizes[i], &actual);
            EXPECT_EQ(cc, CC_SUCCESS);
            EXPECT_EQ(actual, sizes[i]);
            if (cc != CC_SUCCESS)
                return false;
        }
    }
    return true;
}

uint32_t nominal(const Port &p, uint32_t rate)
{
    (void)p;
    return rate / 1000;
}

/* What the host end was given, from byte 'pos' of the stream on, compared
   with frames 'first' on of the pattern scaled by 'gain_num / gain_den'
   (within one step, for the rounding); how many frames differ. */
uint32_t compare(const Port &p, uint32_t pos, uint32_t first, uint32_t frames,
                 int64_t gain_num = 1, int64_t gain_den = 1)
{
    uint32_t fb = frame_bytes(p);
    uint8_t *got = (uint8_t *)heap_alloc(frames * fb);
    uint8_t *want = (uint8_t *)heap_alloc(frames * fb);
    tc_audio_data(pos, got, frames * fb);
    make_frames(p, first, frames, want);
    uint32_t bad = 0;
    for (uint32_t f = 0; f < frames; f++) {
        bool ok = true;
        for (int ch = 0; ch < p.channels; ch++) {
            uint32_t o = (f * p.channels + ch) * p.subframe;
            int64_t g = (int16_t)le16(got + o);
            int64_t w = (int16_t)le16(want + o) * gain_num / gain_den;
            if (g < w - 1 || g > w + 1)
                ok = false;
        }
        if (!ok) {
            if (bad == 0)
                t_log("frame %u differs", first + f);
            bad++;
        }
    }
    return bad;
}

/* Every byte from 'pos' on for 'len' is 0. */
bool silent(uint32_t pos, uint32_t len)
{
    uint8_t *got = (uint8_t *)heap_alloc(len);
    tc_audio_data(pos, got, len);
    bool zero = true;
    for (uint32_t i = 0; i < len; i++)
        zero = zero && got[i] == 0;
    return zero;
}

/* The feedback endpoint's value (UAC1 3.7.2.2; USB 2.0 5.12.4.2), 10.14
   frames per frame at full speed, or 0 when the TD failed. */
uint32_t feedback(Audio &a, const Port &p, uint32_t *cc_out = nullptr)
{
    int dci = usb_dci(p.sync_ep);
    uint8_t *buf = (uint8_t *)usb_alloc(4);
    memset(buf, 0, 4);
    a.d.QueueIsoch(dci, buf, 3);
    uint32_t actual = 0;
    uint32_t cc = a.d.WaitIsoch(dci, 3, &actual);
    if (cc_out)
        *cc_out = cc;
    return cc == CC_SUCCESS && actual == 3 ? le24(buf) : 0;
}

void snap_hex(const char *key, const uint8_t *p, uint32_t len)
{
    char hex[2 * 128 + 1];
    len = min(len, 128u);
    for (uint32_t i = 0; i < len; i++)
        snprintf(hex + 2 * i, 3, "%02x", p[i]);
    hex[2 * len] = 0;
    t_snap(key, "%s", hex);
}

/* The port, ready to stream: alternate setting 1 and its stream
   selected. */
void start(Audio &a, const Port &p)
{
    ASSERT_EQ(a.d.SetInterface(p.iface, 1), CC_SUCCESS);
    select_stream(p);
}

}


TEST(usb_audio, descriptors)
{
    Audio a;
    open_audio(&a, 0, 1);
    UsbDevice &d = a.d;

    /* every descriptor, in order */
    char key[16];
    int n = 0;
    for (uint32_t off = 0; off + 2 <= d.config_len && d.config[off] >= 2;
         off += d.config[off], n++) {
        snprintf(key, sizeof(key), "d%02d", n);
        snap_hex(key, d.config + off, d.config[off]);
    }

    /* The header's wTotalLength covers itself and the units and terminals
       (4.3.2), which are everything class specific up to the first
       streaming interface. */
    const uint8_t *ac = usb_find_alt(&d, a.control, 0);
    ASSERT_TRUE(ac != nullptr);
    EXPECT_EQ(ac[4], 0);     /* no endpoints of its own here */
    const uint8_t *hdr = cs_desc(d, ac, UAC_HEADER);
    EXPECT_EQ(le16(hdr + 3), 0x0100u);
    uint32_t total = 0;
    for (const uint8_t *p = hdr; p < d.config + d.config_len &&
         p[1] == UAC_CS_INTERFACE; p += p[0])
        total += p[0];
    EXPECT_EQ(le16(hdr + 5), total);
    EXPECT_EQ(hdr[0], 8 + hdr[7]);
    EXPECT_EQ(a.nports, 2);

    for (int i = 0; i < a.nports; i++) {
        Port &p = a.ports[i];
        /* alternate setting 0 takes no bandwidth (3.7.2.1) */
        const uint8_t *zero = usb_find_alt(&d, p.iface, 0);
        ASSERT_TRUE(zero != nullptr);
        EXPECT_EQ(zero[4], 0);
        EXPECT_EQ(zero[5], USB_CLASS_AUDIO);
        EXPECT_EQ(zero[6], UAC_SUBCLASS_STREAMING);
        const uint8_t *one = usb_find_alt(&d, p.iface, 1);
        EXPECT_EQ(one[4], 2);
        /* the terminal it links to is a USB streaming input terminal */
        bool linked = false;
        for (int k = 0;; k++) {
            const uint8_t *it = cs_desc(d, ac, UAC_INPUT_TERMINAL, k);
            if (it == nullptr)
                break;
            if (it[3] == p.terminal) {
                EXPECT_EQ(le16(it + 4), 0x0101u);
                EXPECT_EQ(it[7], p.channels);
                linked = true;
            }
        }
        EXPECT_TRUE(linked);
        EXPECT_NE(p.unit, 0);
        /* Type I PCM (Audio Data Formats 2.2.5) */
        EXPECT_EQ(p.subframe, 2);
        EXPECT_EQ(cs_desc(d, one, UAC_FORMAT_TYPE)[6], 16);

        /* the data endpoint: OUT, isochronous and asynchronous, with the
           audio class's two extra bytes and its feedback endpoint named
           (4.6.1.1) */
        UsbEndpoint ep;
        ASSERT_TRUE(d.FindAltEndpoint(p.iface, 1, 0, &ep));
        EXPECT_EQ(ep.address & USB_DIR_IN, 0);
        EXPECT_EQ(ep.attributes & 0xf, 0x05);
        EXPECT_EQ(ep.interval, 1);
        const uint8_t *e = one;
        while (e[1] != USB_DT_ENDPOINT)
            e += e[0];
        EXPECT_EQ(e[0], 9);
        EXPECT_EQ(e[8], p.sync_ep);
        /* a packet holds one frame more than the highest rate gives */
        uint32_t max_rate = 0;
        for (int r = 0; r < p.nrates; r++)
            max_rate = max(max_rate, p.rates[r]);
        EXPECT_GE(p.max_packet,
                  ((max_rate + 999) / 1000) * frame_bytes(p));
        EXPECT_LE(p.max_packet, 1023);
        const uint8_t *cs = e + e[0];
        EXPECT_EQ(cs[1], UAC_CS_ENDPOINT);
        EXPECT_EQ(cs[2], 0x01);     /* EP_GENERAL */
        EXPECT_TRUE(cs[3] & 1);     /* sampling frequency control */

        /* the feedback endpoint: IN, isochronous, three bytes, with
           bRefresh giving how often it reports (4.6.2.1) */
        ASSERT_NE(p.sync_ep, 0);
        ASSERT_TRUE(d.FindAltEndpoint(p.iface, 1, 1, &ep));
        EXPECT_TRUE(ep.address & USB_DIR_IN);
        EXPECT_EQ(ep.address & 0xf, p.data_ep & 0xf);
        EXPECT_EQ(ep.attributes & 3, 1);
        EXPECT_EQ(ep.max_packet, 3);
        const uint8_t *f = cs + cs[0];
        EXPECT_EQ(f[0], 9);
        EXPECT_GE(f[7], 1);
        EXPECT_LE(f[7], 9);
        EXPECT_EQ(f[8], 0);
    }
    SNAP("rates0", "%u %u", a.ports[0].rates[0],
         a.ports[0].nrates > 1 ? a.ports[0].rates[1] : 0);
    SNAP("rates1", "%u", a.ports[1].rates[0]);
}

TEST(usb_audio, composite)
{
    /* The second audio device is one function of several: an interface
       association covers its control and streaming interfaces (Interface
       Association Descriptor ECN), and the device says so. */
    Audio a;
    open_audio(&a, 1, 3);
    UsbDevice &d = a.d;
    EXPECT_EQ(d.desc[4], 0xef);
    EXPECT_EQ(d.desc[5], 0x02);
    EXPECT_EQ(d.desc[6], 0x01);
    const uint8_t *iad = nullptr;
    for (uint32_t off = 0; off + 2 <= d.config_len && d.config[off] >= 2;
         off += d.config[off]) {
        const uint8_t *p = d.config + off;
        if (p[1] == USB_DT_IAD && p[2] == a.control)
            iad = p;
    }
    ASSERT_TRUE(iad != nullptr);
    EXPECT_EQ(iad[3], 1 + a.nports);
    EXPECT_EQ(iad[4], USB_CLASS_AUDIO);
    EXPECT_EQ(a.nports, 1);
    EXPECT_EQ(a.ports[0].iface, a.control + 1);
    SNAP("control", "%u", a.control);

    /* and it plays like any other */
    Port &p = a.ports[0];
    start(a, p);
    uint32_t per = nominal(p, p.rates[0]);
    ASSERT_TRUE(play(a, p, 0, per * 10, per));
    tc_audio_advance(per * 10);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_FRAMES), per * 10);
    EXPECT_EQ(compare(p, 0, 0, per * 10), 0u);
}

TEST(usb_audio, feature_unit)
{
    Audio a;
    open_audio(&a, 0, 1);
    Port &p = a.ports[0];
    int16_t v;

    /* the range is the device's to choose, in 1/256 dB (5.2.2.4.3.2) */
    EXPECT_EQ(fu_get(a, p, UAC_GET_MIN, UAC_FU_VOLUME, &v), CC_SUCCESS);
    int16_t vmin = v;
    EXPECT_EQ(fu_get(a, p, UAC_GET_MAX, UAC_FU_VOLUME, &v), CC_SUCCESS);
    int16_t vmax = v;
    EXPECT_EQ(fu_get(a, p, UAC_GET_RES, UAC_FU_VOLUME, &v), CC_SUCCESS);
    int16_t vres = v;
    EXPECT_LT(vmin, vmax);
    EXPECT_GT(vres, 0);
    SNAP("volume_range", "%d %d %d", vmin, vmax, vres);
    EXPECT_EQ(fu_get(a, p, UAC_GET_CUR, UAC_FU_VOLUME, &v), CC_SUCCESS);
    SNAP("volume_reset", "%d", v);

    EXPECT_EQ(fu_set(a, p, UAC_FU_VOLUME, -3000), CC_SUCCESS);
    EXPECT_EQ(fu_get(a, p, UAC_GET_CUR, UAC_FU_VOLUME, &v), CC_SUCCESS);
    EXPECT_EQ(v, -3000);
    /* out of range, it is held at the end of it */
    EXPECT_EQ(fu_set(a, p, UAC_FU_VOLUME, (int16_t)(vmin - 256)), CC_SUCCESS);
    EXPECT_EQ(fu_get(a, p, UAC_GET_CUR, UAC_FU_VOLUME, &v), CC_SUCCESS);
    EXPECT_EQ(v, vmin);
    EXPECT_EQ(fu_set(a, p, UAC_FU_VOLUME, (int16_t)(vmax + 256)), CC_SUCCESS);
    EXPECT_EQ(fu_get(a, p, UAC_GET_CUR, UAC_FU_VOLUME, &v), CC_SUCCESS);
    EXPECT_EQ(v, vmax);

    /* mute is a byte, and has only a current value (5.2.2.4.3.1) */
    EXPECT_EQ(fu_get(a, p, UAC_GET_CUR, UAC_FU_MUTE, &v), CC_SUCCESS);
    EXPECT_EQ(v, 0);
    EXPECT_EQ(fu_set(a, p, UAC_FU_MUTE, 1), CC_SUCCESS);
    EXPECT_EQ(fu_get(a, p, UAC_GET_CUR, UAC_FU_MUTE, &v), CC_SUCCESS);
    EXPECT_EQ(v, 1);
    EXPECT_EQ(fu_get(a, p, UAC_GET_MIN, UAC_FU_MUTE, &v), CC_STALL);

    /* the other output's unit is a unit of its own */
    Port &q = a.ports[1];
    EXPECT_EQ(fu_get(a, q, UAC_GET_CUR, UAC_FU_MUTE, &v), CC_SUCCESS);
    EXPECT_EQ(v, 0);

    /* what the unit does not have stalls (5.2.2.4.3) */
    uint8_t *buf = (uint8_t *)usb_alloc(2);
    EXPECT_EQ(req(a, UAC_IF_IN, UAC_GET_CUR, UAC_FU_VOLUME << 8 | 1,
                  unit_index(a, p), 2, buf), CC_STALL);
    EXPECT_EQ(req(a, UAC_IF_IN, UAC_GET_CUR, UAC_FU_BASS << 8,
                  unit_index(a, p), 1, buf), CC_STALL);
    EXPECT_EQ(req(a, UAC_IF_IN, UAC_GET_CUR, UAC_FU_VOLUME << 8,
                  (uint16_t)(p.terminal << 8 | a.control), 2, buf), CC_STALL);
    EXPECT_EQ(req(a, UAC_IF_IN, UAC_GET_CUR, UAC_FU_VOLUME << 8,
                  (uint16_t)(p.unit << 8 | p.iface), 2, buf), CC_STALL);
    EXPECT_EQ(req(a, UAC_IF_IN, 0x85 /* GET_MEM */, 0, unit_index(a, p), 2,
                  buf), CC_STALL);
}

TEST(usb_audio, sampling_freq)
{
    Audio a;
    open_audio(&a, 0, 1);
    Port &p = a.ports[0];
    ASSERT_EQ(p.nrates, 2);
    uint32_t rate;

    EXPECT_EQ(rate_get(a, p, &rate), CC_SUCCESS);
    EXPECT_EQ(rate, p.rates[0]);
    EXPECT_EQ(rate_set(a, p, p.rates[1]), CC_SUCCESS);
    EXPECT_EQ(rate_get(a, p, &rate), CC_SUCCESS);
    EXPECT_EQ(rate, p.rates[1]);
    /* a rate the format descriptor does not list is refused */
    EXPECT_EQ(rate_set(a, p, 32000), CC_STALL);
    EXPECT_EQ(rate_get(a, p, &rate), CC_SUCCESS);
    EXPECT_EQ(rate, p.rates[1]);
    /* the control has only a current value here */
    uint8_t *buf = (uint8_t *)usb_alloc(3);
    EXPECT_EQ(req(a, UAC_EP_IN, UAC_GET_MAX, UAC_SAMPLING_FREQ << 8,
                  p.data_ep, 3, buf), CC_STALL);
    EXPECT_EQ(req(a, UAC_EP_IN, UAC_GET_CUR, 0x02 << 8 /* PITCH */,
                  p.data_ep, 1, buf), CC_STALL);

    /* the stream starts at the rate chosen, and starts again at a new
       one */
    start(a, p);
    uint32_t starts = tc_audio_read(TEST_CONTROL_AUDIO_STARTS);
    ASSERT_TRUE(play(a, p, 0, 44, 44));
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_RUNNING), 1u);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_STARTS), starts + 1);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_RATE), p.rates[1]);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_CHANNELS), (uint32_t)p.channels);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_BITS), 16u);
    EXPECT_EQ(rate_set(a, p, p.rates[0]), CC_SUCCESS);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_STARTS), starts + 2);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_RATE), p.rates[0]);
    /* the same rate again changes nothing */
    EXPECT_EQ(rate_set(a, p, p.rates[0]), CC_SUCCESS);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_STARTS), starts + 2);
}

TEST(usb_audio, alt_settings)
{
    Audio a;
    open_audio(&a, 0, 1);
    Port &p = a.ports[0];

    /* nothing streams until the host takes setting 1 and sends */
    select_stream(p);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_RUNNING), 0u);
    EXPECT_EQ(a.d.SetInterface(p.iface, 2), CC_STALL);
    a.d.Recover(1);
    EXPECT_EQ(req(a, USB_RECIP_INTERFACE, USB_REQ_SET_INTERFACE, 1, a.control),
              CC_STALL);
    start(a, p);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_RUNNING), 0u);
    ASSERT_TRUE(play(a, p, 0, 48, 48));
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_RUNNING), 1u);
    /* setting 0 stops it */
    EXPECT_EQ(a.d.SetInterface(p.iface, 0), CC_SUCCESS);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_RUNNING), 0u);
    EXPECT_EQ(a.d.EpState(usb_dci(p.data_ep)), (uint32_t)EP_DISABLED);
    /* and the other output never started */
    select_stream(a.ports[1]);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_RUNNING), 0u);
}

TEST(usb_audio, reset)
{
    Audio a;
    open_audio(&a, 0, 1);
    Port &p = a.ports[0];
    EXPECT_EQ(fu_set(a, p, UAC_FU_VOLUME, -2560), CC_SUCCESS);
    EXPECT_EQ(fu_set(a, p, UAC_FU_MUTE, 1), CC_SUCCESS);
    EXPECT_EQ(rate_set(a, p, p.rates[1]), CC_SUCCESS);
    start(a, p);
    ASSERT_TRUE(play(a, p, 0, 44, 44));
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_RUNNING), 1u);

    /* a bus reset puts the controls back and stops the stream */
    Xhci *hc = a.d.hc;
    EXPECT_EQ(hc->DisableSlot(a.d.slot), CC_SUCCESS);
    ASSERT_TRUE(hc->PortReset(a.d.root_port));
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_RUNNING), 0u);
    open_audio(&a, 0, 1);
    int16_t v;
    uint32_t rate;
    EXPECT_EQ(fu_get(a, p, UAC_GET_CUR, UAC_FU_VOLUME, &v), CC_SUCCESS);
    SNAP("volume", "%d", v);
    EXPECT_EQ(fu_get(a, p, UAC_GET_CUR, UAC_FU_MUTE, &v), CC_SUCCESS);
    EXPECT_EQ(v, 0);
    EXPECT_EQ(rate_get(a, p, &rate), CC_SUCCESS);
    EXPECT_EQ(rate, p.rates[0]);
}

TEST(usb_audio, playback)
{
    Audio a;
    open_audio(&a, 0, 1);
    Port &p = a.ports[0];
    uint32_t fb = frame_bytes(p);
    start(a, p);

    /* What arrives before the host end's clock runs waits for it; the
       clock then takes it in order, and what comes after follows on. */
    ASSERT_TRUE(play(a, p, 0, 480, 48));
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_FRAMES), 0u);
    tc_audio_advance(480);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_CLOCK), 480u);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_FRAMES), 480u);
    ASSERT_TRUE(play(a, p, 480, 960, 48));
    tc_audio_advance(480);
    tc_audio_advance(480);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_FRAMES), 1440u);
    EXPECT_EQ(compare(p, 0, 0, 1440), 0u);

    uint8_t *want = (uint8_t *)heap_alloc(1440 * fb);
    make_frames(p, 0, 1440, want);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_CRC),
              tc_crc32(0, want, 1440 * fb));
}

TEST(usb_audio, playback_44k)
{
    /* the mono output at 44.1 kHz, 44 or 45 frames a packet as a host
       sends them */
    Audio a;
    open_audio(&a, 0, 1);
    Port &p = a.ports[1];
    ASSERT_EQ(p.channels, 1);
    start(a, p);
    /* ten packets make the 10 ms the device fills before it plays */
    uint32_t done = 0;
    for (int round = 0; round < 2; round++) {
        for (int i = 0; i < 10; i++) {
            uint32_t n = i == 9 ? 45 : 44;
            ASSERT_TRUE(play(a, p, done, n, n));
            done += n;
        }
        tc_audio_advance(441);
    }
    EXPECT_EQ(done, 882u);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_RATE), 44100u);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_CHANNELS), 1u);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_FRAMES), 882u);
    EXPECT_EQ(compare(p, 0, 0, 882), 0u);
}

TEST(usb_audio, playback_gain)
{
    Audio a;
    open_audio(&a, 0, 1);
    Port &p = a.ports[0];
    uint32_t fb = frame_bytes(p);

    /* -6 dB from the start: every sample at 10^(-6/20) of itself */
    EXPECT_EQ(fu_set(a, p, UAC_FU_VOLUME, -6 * 256), CC_SUCCESS);
    start(a, p);
    ASSERT_TRUE(play(a, p, 0, 480, 48));
    tc_audio_advance(480);
    EXPECT_EQ(compare(p, 0, 0, 480, 501187, 1000000), 0u);

    /* muted, the gain falls to nothing over a few milliseconds rather
       than at once, and stays there */
    EXPECT_EQ(fu_set(a, p, UAC_FU_MUTE, 1), CC_SUCCESS);
    ASSERT_TRUE(play(a, p, 480, 480, 48));
    tc_audio_advance(480);
    uint32_t pos = 480 * fb;
    EXPECT_FALSE(silent(pos, 8 * fb));
    EXPECT_TRUE(silent(pos + 240 * fb, 240 * fb));
    uint32_t ramp = 0;
    while (ramp < 480 && !silent(pos + ramp * fb, fb))
        ramp++;
    SNAP("mute_ramp", "%u", ramp);

    /* and comes back up the same way, to 0 dB, however little the host end
       takes at a time */
    EXPECT_EQ(fu_set(a, p, UAC_FU_MUTE, 0), CC_SUCCESS);
    EXPECT_EQ(fu_set(a, p, UAC_FU_VOLUME, 0), CC_SUCCESS);
    ASSERT_TRUE(play(a, p, 960, 960, 48));
    for (int i = 0; i < 20; i++)
        tc_audio_advance(48);
    EXPECT_EQ(compare(p, 1201 * fb, 1201, 719), 0u);
}

TEST(usb_audio, underrun)
{
    Audio a;
    open_audio(&a, 0, 1);
    Port &p = a.ports[0];
    uint32_t fb = frame_bytes(p);
    start(a, p);
    ASSERT_TRUE(play(a, p, 0, 480, 48));
    tc_audio_advance(480);

    /* With nothing to play the host end gets silence, and the device
       waits until it has its 10 ms again before it plays on. */
    tc_audio_advance(480);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_FRAMES), 960u);
    EXPECT_TRUE(silent(480 * fb, 480 * fb));
    ASSERT_TRUE(play(a, p, 480, 240, 48));
    tc_audio_advance(240);
    EXPECT_TRUE(silent(960 * fb, 240 * fb));
    ASSERT_TRUE(play(a, p, 720, 240, 48));
    tc_audio_advance(480);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_FRAMES), 1680u);
    EXPECT_EQ(compare(p, 1200 * fb, 480, 480), 0u);
}

TEST(usb_audio, overrun)
{
    Audio a;
    open_audio(&a, 0, 1);
    Port &p = a.ports[0];
    uint32_t fb = frame_bytes(p);
    start(a, p);
    ASSERT_TRUE(play(a, p, 0, 480, 48));
    tc_audio_advance(480);

    /* More than the device holds: what does not fit is lost, and what it
       held plays in order. */
    ASSERT_TRUE(play(a, p, 480, 9600, 48));
    tc_audio_advance(4800);
    tc_audio_advance(4800);
    EXPECT_EQ(tc_audio_read(TEST_CONTROL_AUDIO_FRAMES), 10080u);
    uint32_t held = 0;
    while (held < 9600 && compare(p, (480 + held) * fb, 480 + held, 1) == 0)
        held++;
    SNAP("held", "%u", held);
    EXPECT_GE(held, 4000u);
    EXPECT_LT(held, 9600u);
}

TEST(usb_audio, feedback)
{
    Audio a;
    open_audio(&a, 0, 1);
    Port &p = a.ports[0];
    uint32_t nom = p.rates[0] / 1000 << 14;
    uint32_t cc;

    /* Before the stream starts, the nominal rate. */
    start(a, p);
    EXPECT_EQ(feedback(a, p, &cc), nom);
    EXPECT_EQ(cc, CC_SUCCESS);
    ASSERT_TRUE(play(a, p, 0, 480, 48));
    tc_audio_advance(480);

    /* With more waiting than the device wants to hold, it asks for less;
       with less, for more; never far from the nominal rate. The measured
       sample clock takes a second of the bus to count, which this test
       does not wait for. */
    ASSERT_TRUE(play(a, p, 480, 1920, 48));
    uint32_t over = 0;
    for (int i = 0; i < 16; i++) {
        over = feedback(a, p);
        EXPECT_GE(over, nom * 98 / 100);
        EXPECT_LE(over, nom * 102 / 100);
    }
    EXPECT_LT(over, nom);
    tc_audio_advance(1920 + 480);
    uint32_t under = 0;
    for (int i = 0; i < 32; i++) {
        under = feedback(a, p);
        EXPECT_GE(under, nom * 98 / 100);
        EXPECT_LE(under, nom * 102 / 100);
    }
    EXPECT_GT(under, nom);

    /* in setting 0 the endpoint is gone */
    EXPECT_EQ(a.d.SetInterface(p.iface, 0), CC_SUCCESS);
    EXPECT_EQ(a.d.EpState(usb_dci(p.sync_ep)), (uint32_t)EP_DISABLED);
}
