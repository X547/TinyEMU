/*
 * Intel High Definition Audio controller
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
#include "intel_hda.h"

#include <string.h>

#include <vector>

#include "cutils.h"
#include "device_class.h"
#include "hda.h"
#include "host_time.h"
#include "machine.h"
#include "pci.h"

//#define DEBUG_HDA

#define HDA_REG_SIZE 0x4000

/* global registers */
#define HDA_GCAP        0x00
#define HDA_OUTPAY      0x04
#define HDA_GCTL        0x08
#define  HDA_GCTL_CRST   bit_at(0)
#define  HDA_GCTL_UNSOL  bit_at(8)
#define HDA_WAKEEN      0x0c
#define HDA_STATESTS    0x0e
#define HDA_GSTS        0x10
#define HDA_OUTSTRMPAY  0x18
#define HDA_INTCTL      0x20
#define  HDA_INT_GLOBAL  bit_at(31)
#define  HDA_INT_CTRL    bit_at(30)
#define HDA_INTSTS      0x24
#define HDA_WALCLK      0x30
/* Where the first controllers had the stream synchronization register, and
   where the specification moved it. */
#define HDA_SSYNC_OLD   0x34
#define HDA_SSYNC       0x38

/* the command and response rings */
#define HDA_CORBLBASE   0x40
#define HDA_CORBUBASE   0x44
#define HDA_CORBWP      0x48
#define HDA_CORBRP      0x4a
#define  HDA_CORBRP_RST  bit_at(15)
#define HDA_CORBCTL     0x4c
#define  HDA_CORBCTL_CMEIE bit_at(0)
#define  HDA_CORBCTL_RUN   bit_at(1)
#define HDA_CORBSTS     0x4d
#define  HDA_CORBSTS_CMEI  bit_at(0)
#define HDA_CORBSIZE    0x4e
#define HDA_RIRBLBASE   0x50
#define HDA_RIRBUBASE   0x54
#define HDA_RIRBWP      0x58
#define  HDA_RIRBWP_RST  bit_at(15)
#define HDA_RINTCNT     0x5a
#define HDA_RIRBCTL     0x5c
#define  HDA_RIRBCTL_RINTCTL bit_at(0)
#define  HDA_RIRBCTL_DMAEN   bit_at(1)
#define  HDA_RIRBCTL_OIC     bit_at(2)
#define HDA_RIRBSTS     0x5d
#define  HDA_RIRBSTS_RINTFL  bit_at(0)
#define  HDA_RIRBSTS_OIS     bit_at(2)
#define HDA_RIRBSIZE    0x5e
/* Only 256 entry rings are offered, which every driver takes. */
#define  HDA_RING_SIZE_CAP_256 0x40
#define  HDA_RING_SIZE_256     0x02

/* the immediate command interface */
#define HDA_ICOI        0x60
#define HDA_ICII        0x64
#define HDA_ICIS        0x68
#define  HDA_ICIS_ICB    bit_at(0)
#define  HDA_ICIS_IRV    bit_at(1)

#define HDA_DPLBASE     0x70
#define HDA_DPUBASE     0x74
#define  HDA_DPLBASE_ENABLE bit_at(0)

/* stream descriptors: the input ones, then the output ones */
#define HDA_SD_BASE     0x80
#define HDA_SD_SIZE     0x20
#define HDA_SD_CTL      0x00
#define  HDA_SD_CTL_SRST  bit_at(0)
#define  HDA_SD_CTL_RUN   bit_at(1)
#define  HDA_SD_CTL_IOCE  bit_at(2)
#define  HDA_SD_CTL_FEIE  bit_at(3)
#define  HDA_SD_CTL_DEIE  bit_at(4)
#define  HDA_SD_CTL_TAG_SHIFT 20
#define HDA_SD_STS      0x03
#define  HDA_SD_STS_BCIS    bit_at(2)
#define  HDA_SD_STS_FIFOE   bit_at(3)
#define  HDA_SD_STS_DESE    bit_at(4)
#define  HDA_SD_STS_FIFORDY bit_at(5)
#define HDA_SD_LPIB     0x04
#define HDA_SD_CBL      0x08
#define HDA_SD_LVI      0x0c
#define HDA_SD_FIFOS    0x10
#define HDA_SD_FMT      0x12
#define HDA_SD_BDPL     0x18
#define HDA_SD_BDPU     0x1c
/* A second view of the stream registers in which only the link position is
   readable. */
#define HDA_SD_ALIAS    0x2000

/* What the FIFO of a stream holds, in bytes less one. */
#define HDA_FIFO_SIZE   0xff

/* A buffer descriptor list entry. */
#define HDA_BDLE_SIZE   16
#define HDA_BDLE_IOC    bit_at(0)

/* The wall clock runs at the link's 24 MHz. */
#define HDA_WALCLK_MHZ  24

/* A converter that has not asked for its stream in this long has stopped
   pacing it, and another may. */
#define HDA_PACER_TIMEOUT_US 50000


/* One stream descriptor and the DMA engine behind it. */
struct HDAStream {
    bool input = false;
    uint32_t ctl = 0;
    uint8_t sts = 0;
    uint32_t lpib = 0;
    uint32_t cbl = 0;
    uint16_t lvi = 0;
    uint16_t fmt = 0;
    uint64_t bdl = 0;

    /* where the engine is in the buffer list */
    int bd_index = 0;
    uint32_t bd_offset = 0;

    const void *pacer = nullptr;
    uint64_t pacer_us = 0;

    int Tag() const {return get_bits(ctl, HDA_SD_CTL_TAG_SHIFT, 4);}
};


class IntelHDADevice final: public Device, public PCIBarTarget,
                            public DeviceIO, public HDALink {
private:
    int fInputStreams;
    int fOutputStreams;

    PCIDevice *fPciDev = nullptr;
    PCIMsiState fMsi {};
    IRQSignal *fIrq = nullptr;
    bool fIrqLevel = false;
    PhysMemoryRange *fMemRange = nullptr;
    HDABus *fChildBus = nullptr;

    /* global */
    uint32_t fGctl = 0;
    uint16_t fWakeEn = 0;
    uint16_t fStateSts = 0;
    uint16_t fGsts = 0;
    uint32_t fIntCtl = 0;
    uint32_t fSsync = 0;
    uint64_t fWallBaseUs = 0;

    /* rings */
    uint64_t fCorbBase = 0;
    uint16_t fCorbWp = 0;
    uint16_t fCorbRp = 0;
    bool fCorbRpReset = false;
    uint8_t fCorbCtl = 0;
    uint8_t fCorbSts = 0;
    uint8_t fCorbSize = HDA_RING_SIZE_256;
    uint64_t fRirbBase = 0;
    uint16_t fRirbWp = 0;
    uint16_t fRintCnt = 0;
    uint8_t fRirbCtl = 0;
    uint8_t fRirbSts = 0;
    uint8_t fRirbSize = HDA_RING_SIZE_256;
    int fResponses = 0; /* since the last response interrupt */

    uint32_t fIcoi = 0;
    uint32_t fIcii = 0;
    uint16_t fIcis = 0;

    uint64_t fDpBase = 0;

    HDAStream fStreams[INTEL_HDA_MAX_STREAMS];
    std::vector<uint8_t> fBuffer;

    int StreamCount() const {return fInputStreams + fOutputStreams;}

    /* guest memory */
    bool Dma(uint64_t addr, uint8_t *buf, size_t len, bool to_guest);

    /* registers */
    uint32_t ReadDword(uint32_t offset);
    void WriteByte(uint32_t offset, uint8_t val, bool *corb_kick,
                   bool *immediate);
    void WriteStreamByte(HDAStream &s, uint32_t rel, uint8_t val);
    void EnterReset();
    void LeaveReset();

    /* interrupts */
    uint32_t StreamInterrupts() const;
    bool ControllerInterrupt() const;
    uint32_t IntSts() const;
    void UpdateIrq();

    /* commands */
    static int RingEntries(uint8_t size);
    bool Execute(uint32_t verb, uint32_t *response);
    void PutResponse(uint32_t response, int addr, bool unsolicited);
    void ProcessCorb();
    void RunImmediate();

    /* streams */
    void StreamReset(HDAStream &s);
    bool StreamActive(int index) const;
    HDAStream *FindStream(int tag, bool input, int *index);
    size_t Transfer(HDAStream &s, uint8_t *buf, size_t len);
    void UpdatePosition(int index);
    bool TakePace(HDAStream &s, const void *pacer);

public:
    IntelHDADevice(const char *name, int input_streams, int output_streams);
    ~IntelHDADevice() override {delete fChildBus;}

    bool Prepare() override;
    bool Realize() override;
    Bus *ChildBus() override {return fChildBus;}

    void SetBar(int bar_num, uint64_t addr, bool enabled) override;
    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;

    HDAStreamResultEnum PullOutput(int tag, size_t frames,
                                   const void *pacer) override;
    HDAStreamResultEnum PushInput(int tag, const uint8_t *data, size_t frames,
                                  const AudioFormat &fmt,
                                  const void *pacer) override;
    void Unsolicited(int addr, uint32_t response) override;
};


IntelHDADevice::IntelHDADevice(const char *name, int input_streams,
                               int output_streams):
    Device(name),
    fInputStreams(input_streams),
    fOutputStreams(output_streams)
{
    for (int i = 0; i < fInputStreams; i++) {
        fStreams[i].input = true;
    }
}


//#pragma mark - guest memory

bool IntelHDADevice::Dma(uint64_t addr, uint8_t *buf, size_t len,
                         bool to_guest)
{
    while (len > 0) {
        size_t page_left = DEVRAM_PAGE_SIZE - (addr & (DEVRAM_PAGE_SIZE - 1));
        size_t l = len < page_left ? len : page_left;
        uint8_t *ptr = pci_device_get_dma_ptr(fPciDev, addr, to_guest);
        if (ptr == nullptr) {
            return false;
        }
        if (to_guest) {
            memcpy(ptr, buf, l);
        } else {
            memcpy(buf, ptr, l);
        }
        addr += l;
        buf += l;
        len -= l;
    }
    return true;
}


//#pragma mark - interrupts

uint32_t IntelHDADevice::StreamInterrupts() const
{
    uint32_t bits = 0;

    for (int i = 0; i < StreamCount(); i++) {
        const HDAStream &s = fStreams[i];
        if (((s.sts & HDA_SD_STS_BCIS) != 0 &&
             (s.ctl & HDA_SD_CTL_IOCE) != 0) ||
            ((s.sts & HDA_SD_STS_FIFOE) != 0 &&
             (s.ctl & HDA_SD_CTL_FEIE) != 0) ||
            ((s.sts & HDA_SD_STS_DESE) != 0 &&
             (s.ctl & HDA_SD_CTL_DEIE) != 0)) {
            bits |= bit_at(i);
        }
    }
    return bits;
}


bool IntelHDADevice::ControllerInterrupt() const
{
    return ((fRirbSts & HDA_RIRBSTS_RINTFL) != 0 &&
            (fRirbCtl & HDA_RIRBCTL_RINTCTL) != 0) ||
           ((fRirbSts & HDA_RIRBSTS_OIS) != 0 &&
            (fRirbCtl & HDA_RIRBCTL_OIC) != 0) ||
           ((fCorbSts & HDA_CORBSTS_CMEI) != 0 &&
            (fCorbCtl & HDA_CORBCTL_CMEIE) != 0) ||
           (fStateSts & fWakeEn) != 0;
}


uint32_t IntelHDADevice::IntSts() const
{
    uint32_t val = StreamInterrupts();

    if (ControllerInterrupt()) {
        val |= HDA_INT_CTRL;
    }
    if (val != 0) {
        val |= HDA_INT_GLOBAL;
    }
    return val;
}


void IntelHDADevice::UpdateIrq()
{
    if (fIrq == nullptr) {
        return;
    }

    bool level = (fIntCtl & HDA_INT_GLOBAL) != 0 &&
                 ((StreamInterrupts() & fIntCtl & bit_mask(30)) != 0 ||
                  ((fIntCtl & HDA_INT_CTRL) != 0 && ControllerInterrupt()));

    if (fMsi.Enabled()) {
        /* A message is an edge, sent as the condition appears; the pin must
           stay low while it is in use. */
        fIrq->Set(0);
        if (level && !fIrqLevel) {
            fMsi.Send(0);
        }
        fIrqLevel = level;
        return;
    }
    if ((pci_device_get_config(fPciDev, PCI_COMMAND, 1) &
         PCI_COMMAND_INTX_DISABLE) != 0) {
        level = false;
    }
    if (level != fIrqLevel) {
        fIrqLevel = level;
        fIrq->Set(level ? 1 : 0);
    }
}


//#pragma mark - commands

int IntelHDADevice::RingEntries(uint8_t size)
{
    switch (size & 3) {
    case 0:
        return 2;
    case 1:
        return 16;
    default:
        return 256;
    }
}


/* Whether a codec answered: a verb for an address nobody holds goes
   unanswered, and the driver times out as it would on hardware. */
bool IntelHDADevice::Execute(uint32_t verb, uint32_t *response)
{
    HDACodec *codec = fChildBus->Codec(verb >> 28);

    if (codec == nullptr) {
        return false;
    }
    *response = codec->Command(verb & 0x0fffffff);
#ifdef DEBUG_HDA
    printf("hda: verb %08x -> %08x\n", verb, *response);
#endif
    return true;
}


void IntelHDADevice::PutResponse(uint32_t response, int addr,
                                 bool unsolicited)
{
    uint8_t entry[8];

    if ((fRirbCtl & HDA_RIRBCTL_DMAEN) == 0) {
        return;
    }
    fRirbWp = (fRirbWp + 1) % RingEntries(fRirbSize);
    put_le32(entry, response);
    put_le32(entry + 4, addr | (unsolicited ? 0x10 : 0));
    Dma(fRirbBase + fRirbWp * 8, entry, sizeof(entry), true);

    /* The interrupt comes after as many responses as the count says, or
       once the commands run out, whichever is first. */
    int count = fRintCnt & 0xff;
    if (++fResponses >= (count == 0 ? 256 : count)) {
        fRirbSts |= HDA_RIRBSTS_RINTFL;
        fResponses = 0;
    }
}


void IntelHDADevice::ProcessCorb()
{
    int entries = RingEntries(fCorbSize);
    uint8_t buf[4];

    if ((fCorbCtl & HDA_CORBCTL_RUN) == 0 || (fGctl & HDA_GCTL_CRST) == 0) {
        return;
    }
    while (fCorbRp != (fCorbWp & (entries - 1))) {
        uint32_t response;
        fCorbRp = (fCorbRp + 1) & (entries - 1);
        if (!Dma(fCorbBase + fCorbRp * 4, buf, 4, false)) {
            fCorbSts |= HDA_CORBSTS_CMEI;
            break;
        }
        uint32_t verb = get_le32(buf);
        if (Execute(verb, &response)) {
            PutResponse(response, verb >> 28, false);
        }
    }
    if (fResponses > 0) {
        fRirbSts |= HDA_RIRBSTS_RINTFL;
        fResponses = 0;
    }
    UpdateIrq();
}


void IntelHDADevice::RunImmediate()
{
    uint32_t response;

    if (Execute(fIcoi, &response)) {
        fIcii = response;
        fIcis |= HDA_ICIS_IRV;
    }
    fIcis &= ~HDA_ICIS_ICB;
}


void IntelHDADevice::Unsolicited(int addr, uint32_t response)
{
    if ((fGctl & HDA_GCTL_UNSOL) == 0) {
        return;
    }
    PutResponse(response, addr, true);
    fRirbSts |= HDA_RIRBSTS_RINTFL;
    fResponses = 0;
    UpdateIrq();
}


//#pragma mark - streams

void IntelHDADevice::StreamReset(HDAStream &s)
{
    bool input = s.input;
    s = HDAStream();
    s.input = input;
}


bool IntelHDADevice::StreamActive(int index) const
{
    const HDAStream &s = fStreams[index];

    return (fGctl & HDA_GCTL_CRST) != 0 && (s.ctl & HDA_SD_CTL_RUN) != 0 &&
           (s.ctl & HDA_SD_CTL_SRST) == 0 && (fSsync & bit_at(index)) == 0;
}


HDAStream *IntelHDADevice::FindStream(int tag, bool input, int *index)
{
    if (tag == 0) {
        return nullptr;
    }
    for (int i = 0; i < StreamCount(); i++) {
        if (fStreams[i].input == input && fStreams[i].Tag() == tag &&
            StreamActive(i)) {
            *index = i;
            return &fStreams[i];
        }
    }
    return nullptr;
}


/* Move 'len' bytes between 'buf' and the stream's buffers, following the
   buffer descriptor list round and raising the completion of each buffer
   that asks for one. */
size_t IntelHDADevice::Transfer(HDAStream &s, uint8_t *buf, size_t len)
{
    size_t done = 0;
    int empty = 0;

    while (done < len) {
        uint8_t e[HDA_BDLE_SIZE];
        if (!Dma(s.bdl + (uint64_t)s.bd_index * HDA_BDLE_SIZE, e, sizeof(e),
                 false)) {
            s.sts |= HDA_SD_STS_DESE;
            break;
        }
        uint64_t addr = get_le64(e);
        uint32_t blen = get_le32(e + 8);
        uint32_t flags = get_le32(e + 12);

        if (s.bd_offset < blen) {
            empty = 0;
            size_t n = blen - s.bd_offset;
            if (n > len - done) {
                n = len - done;
            }
            if (!Dma(addr + s.bd_offset, buf + done, n, s.input) &&
                !s.input) {
                memset(buf + done, 0, n);
            }
            s.bd_offset += n;
            done += n;
            s.lpib += n;
            if (s.cbl != 0 && s.lpib >= s.cbl) {
                s.lpib %= s.cbl;
            }
            if (s.bd_offset < blen) {
                continue;
            }
            if ((flags & HDA_BDLE_IOC) != 0) {
                s.sts |= HDA_SD_STS_BCIS;
            }
        } else if (++empty > s.lvi + 1) {
            /* a list of nothing but empty buffers */
            s.sts |= HDA_SD_STS_DESE;
            break;
        }
        s.bd_offset = 0;
        if (s.bd_index >= s.lvi) {
            s.bd_index = 0;
            s.lpib = 0;
        } else {
            s.bd_index++;
        }
    }
    if (done < len && !s.input) {
        memset(buf + done, 0, len - done);
    }
    return done;
}


void IntelHDADevice::UpdatePosition(int index)
{
    uint8_t b[4];

    if ((fDpBase & HDA_DPLBASE_ENABLE) == 0) {
        return;
    }
    put_le32(b, fStreams[index].lpib);
    Dma((fDpBase & ~(uint64_t)0x7f) + index * 8, b, 4, true);
}


bool IntelHDADevice::TakePace(HDAStream &s, const void *pacer)
{
    uint64_t now = host_monotonic_us();

    if (s.pacer != pacer && s.pacer != nullptr &&
        now - s.pacer_us < HDA_PACER_TIMEOUT_US) {
        return false;
    }
    s.pacer = pacer;
    s.pacer_us = now;
    return true;
}


HDAStreamResultEnum IntelHDADevice::PullOutput(int tag, size_t frames,
                                               const void *pacer)
{
    AudioFormat fmt;
    int index;

    HDAStream *s = FindStream(tag, false, &index);
    if (s == nullptr || !hda_format_decode(s->fmt, &fmt)) {
        return HDA_STREAM_IDLE;
    }
    if (!TakePace(*s, pacer)) {
        return HDA_STREAM_PACED;
    }
    size_t len = frames * fmt.FrameBytes();
    fBuffer.resize(len);
    Transfer(*s, fBuffer.data(), len);
    UpdatePosition(index);
    UpdateIrq();
    fChildBus->StreamOutput(tag, fBuffer.data(), frames, fmt);
    return HDA_STREAM_MOVED;
}


HDAStreamResultEnum IntelHDADevice::PushInput(int tag, const uint8_t *data,
                                              size_t frames,
                                              const AudioFormat &fmt,
                                              const void *pacer)
{
    AudioFormat stream_fmt;
    int index;

    HDAStream *s = FindStream(tag, true, &index);
    if (s == nullptr || !hda_format_decode(s->fmt, &stream_fmt)) {
        return HDA_STREAM_IDLE;
    }
    if (!TakePace(*s, pacer)) {
        return HDA_STREAM_PACED;
    }
    /* A converter set to another layout than its stream sends noise on real
       hardware; silence here. */
    size_t len = frames * stream_fmt.FrameBytes();
    if (stream_fmt.FrameBytes() == fmt.FrameBytes()) {
        fBuffer.assign(data, data + len);
    } else {
        fBuffer.assign(len, 0);
    }
    Transfer(*s, fBuffer.data(), len);
    UpdatePosition(index);
    UpdateIrq();
    return HDA_STREAM_MOVED;
}


//#pragma mark - registers

void IntelHDADevice::EnterReset()
{
    fGctl = 0;
    fWakeEn = 0;
    fStateSts = 0;
    fGsts = 0;
    fIntCtl = 0;
    fSsync = 0;
    fCorbBase = 0;
    fCorbWp = 0;
    fCorbRp = 0;
    fCorbRpReset = false;
    fCorbCtl = 0;
    fCorbSts = 0;
    fCorbSize = HDA_RING_SIZE_256;
    fRirbBase = 0;
    fRirbWp = 0;
    fRintCnt = 0;
    fRirbCtl = 0;
    fRirbSts = 0;
    fRirbSize = HDA_RING_SIZE_256;
    fResponses = 0;
    fIcoi = 0;
    fIcii = 0;
    fIcis = 0;
    fDpBase = 0;
    for (int i = 0; i < StreamCount(); i++) {
        StreamReset(fStreams[i]);
    }
    UpdateIrq();
}


void IntelHDADevice::LeaveReset()
{
    fWallBaseUs = host_monotonic_us();
    fChildBus->Reset();
    /* Each codec signals its presence as the link comes up. */
    fStateSts = fChildBus->PresentMask();
    UpdateIrq();
}


uint32_t IntelHDADevice::ReadDword(uint32_t offset)
{
    uint32_t streams_end = HDA_SD_BASE + StreamCount() * HDA_SD_SIZE;

    if (offset >= HDA_SD_ALIAS) {
        uint32_t o = offset - HDA_SD_ALIAS;
        if (o >= HDA_SD_BASE && o < streams_end &&
            (o - HDA_SD_BASE) % HDA_SD_SIZE == HDA_SD_LPIB) {
            return fStreams[(o - HDA_SD_BASE) / HDA_SD_SIZE].lpib;
        }
        return 0;
    }

    if (offset >= HDA_SD_BASE && offset < streams_end) {
        int index = (offset - HDA_SD_BASE) / HDA_SD_SIZE;
        const HDAStream &s = fStreams[index];
        uint8_t sts = s.sts;
        if (StreamActive(index)) {
            sts |= HDA_SD_STS_FIFORDY;
        }
        switch ((offset - HDA_SD_BASE) % HDA_SD_SIZE) {
        case HDA_SD_CTL:
            return s.ctl | (uint32_t)sts << 24;
        case HDA_SD_LPIB:
            return s.lpib;
        case HDA_SD_CBL:
            return s.cbl;
        case HDA_SD_LVI:
            return s.lvi;
        case HDA_SD_FIFOS:
            return HDA_FIFO_SIZE | (uint32_t)s.fmt << 16;
        case HDA_SD_BDPL:
            return (uint32_t)s.bdl;
        case HDA_SD_BDPU:
            return (uint32_t)(s.bdl >> 32);
        }
        return 0;
    }

    switch (offset) {
    case HDA_GCAP:
        /* the stream counts, one SDO line, 64 bit addressing; version 1.0 */
        return (fOutputStreams << 12) | (fInputStreams << 8) | 1 |
               (1u << 24);
    case HDA_OUTPAY:
        return 0x3c | (0x1d << 16);
    case HDA_GCTL:
        return fGctl;
    case HDA_WAKEEN:
        return fWakeEn | (uint32_t)fStateSts << 16;
    case HDA_GSTS:
        return fGsts;
    case HDA_OUTSTRMPAY:
        return 0x30 | (0x18 << 16);
    case HDA_INTCTL:
        return fIntCtl;
    case HDA_INTSTS:
        return IntSts();
    case HDA_WALCLK:
        return (uint32_t)((host_monotonic_us() - fWallBaseUs) *
                          HDA_WALCLK_MHZ);
    case HDA_SSYNC_OLD:
    case HDA_SSYNC:
        return fSsync;
    case HDA_CORBLBASE:
        return (uint32_t)fCorbBase;
    case HDA_CORBUBASE:
        return (uint32_t)(fCorbBase >> 32);
    case HDA_CORBWP:
        return fCorbWp |
               (uint32_t)(fCorbRp | (fCorbRpReset ? HDA_CORBRP_RST : 0)) << 16;
    case HDA_CORBCTL:
        return fCorbCtl | (uint32_t)fCorbSts << 8 |
               (uint32_t)(HDA_RING_SIZE_CAP_256 | fCorbSize) << 16;
    case HDA_RIRBLBASE:
        return (uint32_t)fRirbBase;
    case HDA_RIRBUBASE:
        return (uint32_t)(fRirbBase >> 32);
    case HDA_RIRBWP:
        return fRirbWp | (uint32_t)fRintCnt << 16;
    case HDA_RIRBCTL:
        return fRirbCtl | (uint32_t)fRirbSts << 8 |
               (uint32_t)(HDA_RING_SIZE_CAP_256 | fRirbSize) << 16;
    case HDA_ICOI:
        return fIcoi;
    case HDA_ICII:
        return fIcii;
    case HDA_ICIS:
        return fIcis;
    case HDA_DPLBASE:
        return (uint32_t)fDpBase;
    case HDA_DPUBASE:
        return (uint32_t)(fDpBase >> 32);
    }
    return 0;
}


static void set_byte32(uint32_t *reg, int byte, uint8_t val, uint32_t mask)
{
    uint32_t shift = byte * 8;
    *reg = (*reg & ~(0xffu << shift)) | (((uint32_t)val << shift) & mask);
}


static void set_byte64(uint64_t *reg, int byte, uint8_t val, uint64_t mask)
{
    uint32_t shift = byte * 8;
    *reg = (*reg & ~((uint64_t)0xff << shift)) |
           (((uint64_t)val << shift) & mask);
}


void IntelHDADevice::WriteStreamByte(HDAStream &s, uint32_t rel, uint8_t val)
{
    switch (rel) {
    case HDA_SD_CTL:
        set_byte32(&s.ctl, 0, val, 0x1f);
        break;
    case HDA_SD_CTL + 2:
        /* the stream tag; the direction and striping of a bidirectional
           stream are not offered */
        set_byte32(&s.ctl, 2, val, 0xf00000);
        break;
    case HDA_SD_STS:
        s.sts &= ~(val & (HDA_SD_STS_BCIS | HDA_SD_STS_FIFOE |
                          HDA_SD_STS_DESE));
        break;
    case HDA_SD_CBL ... HDA_SD_CBL + 3:
        set_byte32(&s.cbl, rel - HDA_SD_CBL, val, ~0u);
        break;
    case HDA_SD_LVI:
        s.lvi = val;
        break;
    case HDA_SD_FMT:
    case HDA_SD_FMT + 1: {
        uint32_t fmt = s.fmt;
        set_byte32(&fmt, rel - HDA_SD_FMT, val, 0x7f7f);
        s.fmt = fmt;
        break;
    }
    case HDA_SD_BDPL ... HDA_SD_BDPL + 3:
        set_byte64(&s.bdl, rel - HDA_SD_BDPL, val, ~(uint64_t)0x7f);
        break;
    case HDA_SD_BDPU ... HDA_SD_BDPU + 3:
        set_byte64(&s.bdl, rel - HDA_SD_BDPL, val, ~(uint64_t)0x7f);
        break;
    }
}


void IntelHDADevice::WriteByte(uint32_t offset, uint8_t val, bool *corb_kick,
                               bool *immediate)
{
    /* While the controller is in reset only the reset bit is live. */
    if ((fGctl & HDA_GCTL_CRST) == 0 && offset != HDA_GCTL) {
        return;
    }

    if (offset >= HDA_SD_BASE &&
        offset < (uint32_t)(HDA_SD_BASE + StreamCount() * HDA_SD_SIZE)) {
        WriteStreamByte(fStreams[(offset - HDA_SD_BASE) / HDA_SD_SIZE],
                        (offset - HDA_SD_BASE) % HDA_SD_SIZE, val);
        return;
    }

    switch (offset) {
    case HDA_GCTL:
        set_byte32(&fGctl, 0, val, HDA_GCTL_CRST);
        break;
    case HDA_GCTL + 1:
        set_byte32(&fGctl, 1, val, HDA_GCTL_UNSOL);
        break;
    case HDA_WAKEEN:
    case HDA_WAKEEN + 1: {
        uint32_t v = fWakeEn;
        set_byte32(&v, offset - HDA_WAKEEN, val, 0x7fff);
        fWakeEn = v;
        break;
    }
    case HDA_STATESTS:
    case HDA_STATESTS + 1:
        fStateSts &= ~((uint16_t)val << ((offset - HDA_STATESTS) * 8));
        break;
    case HDA_GSTS:
        fGsts &= ~(val & 0x02);
        break;
    case HDA_INTCTL ... HDA_INTCTL + 3:
        set_byte32(&fIntCtl, offset - HDA_INTCTL, val,
                   HDA_INT_GLOBAL | HDA_INT_CTRL |
                   bit_mask(StreamCount()));
        break;
    case HDA_SSYNC_OLD ... HDA_SSYNC_OLD + 3:
        set_byte32(&fSsync, offset - HDA_SSYNC_OLD, val,
                   bit_mask(StreamCount()));
        break;
    case HDA_SSYNC ... HDA_SSYNC + 3:
        set_byte32(&fSsync, offset - HDA_SSYNC, val, bit_mask(StreamCount()));
        break;
    case HDA_CORBLBASE ... HDA_CORBLBASE + 3:
        set_byte64(&fCorbBase, offset - HDA_CORBLBASE, val, ~(uint64_t)0x7f);
        break;
    case HDA_CORBUBASE ... HDA_CORBUBASE + 3:
        set_byte64(&fCorbBase, offset - HDA_CORBLBASE, val, ~(uint64_t)0);
        break;
    case HDA_CORBWP:
        fCorbWp = val;
        *corb_kick = true;
        break;
    case HDA_CORBRP + 1:
        /* The read pointer goes back to 0 while the reset bit is held, and
           the bit reads back set so the driver can tell. */
        fCorbRpReset = (val & (HDA_CORBRP_RST >> 8)) != 0;
        if (fCorbRpReset) {
            fCorbRp = 0;
        }
        break;
    case HDA_CORBCTL:
        fCorbCtl = val & (HDA_CORBCTL_CMEIE | HDA_CORBCTL_RUN);
        *corb_kick = true;
        break;
    case HDA_CORBSTS:
        fCorbSts &= ~(val & HDA_CORBSTS_CMEI);
        break;
    case HDA_CORBSIZE:
        fCorbSize = val & 3;
        break;
    case HDA_RIRBLBASE ... HDA_RIRBLBASE + 3:
        set_byte64(&fRirbBase, offset - HDA_RIRBLBASE, val, ~(uint64_t)0x7f);
        break;
    case HDA_RIRBUBASE ... HDA_RIRBUBASE + 3:
        set_byte64(&fRirbBase, offset - HDA_RIRBLBASE, val, ~(uint64_t)0);
        break;
    case HDA_RIRBWP + 1:
        if ((val & (HDA_RIRBWP_RST >> 8)) != 0) {
            fRirbWp = 0;
        }
        break;
    case HDA_RINTCNT:
        fRintCnt = val;
        break;
    case HDA_RIRBCTL:
        fRirbCtl = val & (HDA_RIRBCTL_RINTCTL | HDA_RIRBCTL_DMAEN |
                          HDA_RIRBCTL_OIC);
        break;
    case HDA_RIRBSTS:
        fRirbSts &= ~(val & (HDA_RIRBSTS_RINTFL | HDA_RIRBSTS_OIS));
        break;
    case HDA_RIRBSIZE:
        fRirbSize = val & 3;
        break;
    case HDA_ICOI ... HDA_ICOI + 3:
        set_byte32(&fIcoi, offset - HDA_ICOI, val, ~0u);
        break;
    case HDA_ICIS:
        fIcis &= ~(val & HDA_ICIS_IRV);
        if ((val & HDA_ICIS_ICB) != 0) {
            fIcis |= HDA_ICIS_ICB;
            *immediate = true;
        }
        break;
    case HDA_DPLBASE ... HDA_DPLBASE + 3:
        set_byte64(&fDpBase, offset - HDA_DPLBASE, val,
                   ~(uint64_t)0x7e);
        break;
    case HDA_DPUBASE ... HDA_DPUBASE + 3:
        set_byte64(&fDpBase, offset - HDA_DPLBASE, val, ~(uint64_t)0);
        break;
    }
}


uint32_t IntelHDADevice::DeviceRead(uint32_t offset, int size_log2)
{
    uint32_t val = ReadDword(offset & ~3u) >> ((offset & 3) * 8);

    if (size_log2 < 2) {
        val &= bit_mask(8 << size_log2);
    }
#ifdef DEBUG_HDA
    printf("hda: read  %04x/%d -> %08x\n", offset, 1 << size_log2, val);
#endif
    return val;
}


/* Written a byte at a time, the way the registers are laid out; what a write
   sets off happens once the whole of it has landed. */
void IntelHDADevice::DeviceWrite(uint32_t offset, uint32_t val, int size_log2)
{
    uint32_t old_gctl = fGctl;
    bool corb_kick = false, immediate = false;
    int index = -1;
    uint32_t old_ctl = 0;

#ifdef DEBUG_HDA
    printf("hda: write %04x/%d <- %08x\n", offset, 1 << size_log2, val);
#endif
    if (offset >= HDA_SD_BASE &&
        offset < (uint32_t)(HDA_SD_BASE + StreamCount() * HDA_SD_SIZE)) {
        index = (offset - HDA_SD_BASE) / HDA_SD_SIZE;
        old_ctl = fStreams[index].ctl;
    }

    for (int i = 0; i < (1 << size_log2); i++) {
        WriteByte(offset + i, (uint8_t)(val >> (i * 8)), &corb_kick,
                  &immediate);
    }

    if ((old_gctl & HDA_GCTL_CRST) != 0 && (fGctl & HDA_GCTL_CRST) == 0) {
        EnterReset();
        return;
    }
    if ((old_gctl & HDA_GCTL_CRST) == 0 && (fGctl & HDA_GCTL_CRST) != 0) {
        LeaveReset();
    }
    if (index >= 0) {
        HDAStream &s = fStreams[index];
        uint32_t changed = old_ctl ^ s.ctl;
        if ((changed & s.ctl & HDA_SD_CTL_SRST) != 0) {
            /* Everything but the reset bit itself goes back to its default,
               and stays there while the bit is held. */
            StreamReset(s);
            s.ctl = HDA_SD_CTL_SRST;
        }
        if ((changed & s.ctl & HDA_SD_CTL_RUN) != 0) {
            s.pacer = nullptr;
        }
    }
    if (immediate) {
        RunImmediate();
    }
    if (corb_kick) {
        ProcessCorb();
    }
    UpdateIrq();
}


//#pragma mark - lifecycle

void IntelHDADevice::SetBar(int bar_num, uint64_t addr, bool enabled)
{
    (void)bar_num;
    fMemRange->SetAddr(addr, enabled);
}


bool IntelHDADevice::Prepare()
{
    if (ParentBus()->AsPCIBus() == nullptr) {
        vm_error("%s: must be attached to a PCI bus\n", Name());
        return false;
    }
    fChildBus = new HDABus(this, this);
    return true;
}


bool IntelHDADevice::Realize()
{
    PCIBus *pci_bus = ParentBus()->AsPCIBus();

    /* The ICH6 controller, which every driver knows. Its capabilities start
       at 0x60, leaving the chipset registers below them where drivers
       expect to write. */
    fPciDev = pci_register_device(pci_bus, Name(), -1, 0x8086, 0x2668, 0x01,
                                  0x0403, 0x60);
    if (fPciDev == nullptr) {
        vm_error("%s: could not register the PCI device\n", Name());
        return false;
    }
    pci_device_set_config8(fPciDev, PCI_INTERRUPT_PIN, 1);
    fMsi.Init(fPciDev, 1);

    fIrq = pci_device_get_irq(fPciDev, 0);
    fMemRange = pci_device_get_mem_map(fPciDev)->RegisterDevice(
        0, HDA_REG_SIZE, this,
        DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32 | DEVIO_DISABLED);
    pci_register_bar(fPciDev, 0, HDA_REG_SIZE,
                     PCI_ADDRESS_SPACE_MEM | PCI_ADDRESS_SPACE_MEM_TYPE_64,
                     this);
    return true;
}


//#pragma mark - class

/* An ICH6 HD Audio controller on a PCI bus, and the link it provides. */
class IntelHDAClass final: public DeviceClass {
public:
    IntelHDAClass(): DeviceClass("intel-hda") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        int input_streams, output_streams;
        const char *name = cfg.IdOr("hda");

        (void)ctx;
        if (!cfg.GetInt("input_streams", &input_streams,
                        INTEL_HDA_DEFAULT_STREAMS) ||
            !cfg.GetInt("output_streams", &output_streams,
                        INTEL_HDA_DEFAULT_STREAMS)) {
            return nullptr;
        }
        if (input_streams < 0 || output_streams < 0 ||
            input_streams > 15 || output_streams > 15 ||
            input_streams + output_streams > INTEL_HDA_MAX_STREAMS) {
            vm_error("%s: 'input_streams' and 'output_streams' must each be "
                     "between 0 and 15, and %d together at most\n", name,
                     INTEL_HDA_MAX_STREAMS);
            return nullptr;
        }
        return new IntelHDADevice(name, input_streams, output_streams);
    }
};

static const IntelHDAClass sIntelHDAClass;
