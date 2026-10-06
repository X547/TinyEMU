/*
 * Intel ICH AC'97 audio controller
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
#include <string.h>

#include <vector>

#include "ac97.h"
#include "cutils.h"
#include "device_class.h"
#include "machine.h"
#include "pci.h"

//#define DEBUG_ICH_AC97

/* The mixer: the codec registers, the primary's then the secondary's. */
#define ICH_NAM_SIZE      0x100
#define ICH_NAM_CODEC     0x80

/* The bus master: a box of registers per stream, then the global ones. */
#define ICH_NABM_SIZE     0x40
#define ICH_BOX_SIZE      0x10
/* The registers of a box end here; the last box's stride holds
   GLOB_CNT. */
#define ICH_BOX_REGS      0x0c

#define ICH_BDBAR         0x00
#define ICH_CIV           0x04
#define ICH_LVI           0x05
#define ICH_SR            0x06
#define  ICH_SR_DCH        bit_at(0)
#define  ICH_SR_CELV       bit_at(1)
#define  ICH_SR_LVBCI      bit_at(2)
#define  ICH_SR_BCIS       bit_at(3)
#define  ICH_SR_FIFOE      bit_at(4)
#define  ICH_SR_W1C        (ICH_SR_LVBCI | ICH_SR_BCIS | ICH_SR_FIFOE)
#define ICH_PICB          0x08
#define ICH_PIV           0x0a
#define ICH_CR            0x0b
#define  ICH_CR_RPBM       bit_at(0)
#define  ICH_CR_RR         bit_at(1)
#define  ICH_CR_LVBIE      bit_at(2)
#define  ICH_CR_FEIE       bit_at(3)
#define  ICH_CR_IOCE       bit_at(4)
#define  ICH_CR_IE         (ICH_CR_LVBIE | ICH_CR_FEIE | ICH_CR_IOCE)

#define ICH_GLOB_CNT      0x2c
#define  ICH_CNT_GIE       bit_at(0)
#define  ICH_CNT_COLD      bit_at(1) /* clear holds the link in reset */
#define  ICH_CNT_WARM      bit_at(2)
#define  ICH_CNT_MASK      0x3f
#define ICH_GLOB_STA      0x30
#define  ICH_STA_GSCI      bit_at(0)
#define  ICH_STA_PIINT     bit_at(5)
#define  ICH_STA_POINT     bit_at(6)
#define  ICH_STA_MINT      bit_at(7)
#define  ICH_STA_PCR       bit_at(8)
#define  ICH_STA_SCR       bit_at(9)
#define  ICH_STA_PRI       bit_at(10)
#define  ICH_STA_SRI       bit_at(11)
#define  ICH_STA_RCS       bit_at(15)
#define  ICH_STA_AD3       bit_at(16)
#define  ICH_STA_MD3       bit_at(17)
#define  ICH_STA_W1C       (ICH_STA_GSCI | ICH_STA_PRI | ICH_STA_SRI | \
                            ICH_STA_RCS)
#define  ICH_STA_RW        (ICH_STA_AD3 | ICH_STA_MD3)
#define ICH_CAS           0x34

/* A buffer descriptor: an address and a length in samples. */
#define ICH_BD_COUNT      32
#define ICH_BD_SIZE       8
#define ICH_BD_IOC        bit_at(31)
#define ICH_BD_LEN_MASK   0xffff

/* The boxes, in register order. */
static const AC97StreamEnum kBoxStream[AC97_STREAM_COUNT] = {
    AC97_PCM_IN, AC97_PCM_OUT, AC97_MIC_IN,
};
static const uint32_t kBoxInt[AC97_STREAM_COUNT] = {
    ICH_STA_PIINT, ICH_STA_POINT, ICH_STA_MINT,
};


/* The DMA engine of one stream. */
struct ICHBox {
    uint32_t bdbar = 0;
    uint8_t civ = 0;
    uint8_t lvi = 0;
    uint16_t sr = ICH_SR_DCH;
    uint16_t picb = 0;
    uint8_t cr = 0;

    /* the buffer at CIV, once fetched */
    bool fetched = false;
    uint32_t addr = 0;
    uint32_t ctl = 0;

    uint8_t Piv() const {return (civ + 1) % ICH_BD_COUNT;}
    uint32_t Length() const {return ctl & ICH_BD_LEN_MASK;}
    bool Moving() const
        {return (cr & ICH_CR_RPBM) != 0 && (sr & ICH_SR_DCH) == 0;}
    bool Interrupt() const
    {
        return ((sr & ICH_SR_BCIS) != 0 && (cr & ICH_CR_IOCE) != 0) ||
               ((sr & ICH_SR_LVBCI) != 0 && (cr & ICH_CR_LVBIE) != 0) ||
               ((sr & ICH_SR_FIFOE) != 0 && (cr & ICH_CR_FEIE) != 0);
    }
};


class ICHAC97Device final: public Device, public PCIBarTarget,
                           public AC97Link {
private:
    PCIDevice *fPciDev = nullptr;
    IRQSignal *fIrq = nullptr;
    bool fIrqLevel = false;
    PhysMemoryRange *fNamRange = nullptr;
    PhysMemoryRange *fNabmRange = nullptr;
    AC97Bus *fChildBus = nullptr;

    ICHBox fBoxes[AC97_STREAM_COUNT];
    /* The link stays in reset until a driver takes it out. */
    uint32_t fGlobCnt = 0;
    uint32_t fGlobSta = 0; /* the bits that are not derived */
    uint8_t fCas = 0;
    std::vector<uint8_t> fBuffer;

    ICHBox &Box(AC97StreamEnum stream);

    /* guest memory */
    bool Dma(uint32_t addr, uint8_t *buf, size_t len, bool to_guest);

    /* streams */
    void BoxReset(ICHBox &b);
    void Fetch(ICHBox &b);
    void Complete(ICHBox &b);
    void Transfer(ICHBox &b, uint8_t *buf, size_t len, bool to_guest);

    /* interrupts */
    uint32_t GlobSta() const;
    void UpdateIrq();

    /* registers */
    uint8_t BoxReadByte(const ICHBox &b, uint32_t rel) const;
    void BoxWriteByte(ICHBox &b, uint32_t rel, uint8_t val);
    void BoxWritten(ICHBox &b, uint8_t old_cr, uint8_t old_lvi);
    uint8_t ReadByte(uint32_t offset);
    void GlobCntWritten(uint32_t old_cnt);

public:
    explicit ICHAC97Device(const char *name): Device(name) {}
    ~ICHAC97Device() override {delete fChildBus;}

    bool Prepare() override;
    bool Realize() override;
    Bus *ChildBus() override {return fChildBus;}

    void SetBar(int bar_num, uint64_t addr, bool enabled) override;
    uint32_t NamRead(uint32_t offset, int size_log2);
    void NamWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t NabmRead(uint32_t offset, int size_log2);
    void NabmWrite(uint32_t offset, uint32_t val, int size_log2);

    /* AC97Link */
    bool LinkUp() const override {return (fGlobCnt & ICH_CNT_COLD) != 0;}
    bool StreamRunning(AC97StreamEnum stream) const override;
    void PullOutput(uint8_t *buf, size_t frames) override;
    void PushInput(AC97StreamEnum stream, const uint8_t *buf,
                   size_t frames) override;

private:
    DeviceIOAdapter<ICHAC97Device, &ICHAC97Device::NamRead,
                    &ICHAC97Device::NamWrite> fNamIo {*this};
    DeviceIOAdapter<ICHAC97Device, &ICHAC97Device::NabmRead,
                    &ICHAC97Device::NabmWrite> fNabmIo {*this};
};


/* The box whose registers 'offset' falls in, or -1. */
static int box_at(uint32_t offset)
{
    if (offset >= AC97_STREAM_COUNT * ICH_BOX_SIZE ||
        offset % ICH_BOX_SIZE >= ICH_BOX_REGS) {
        return -1;
    }
    return offset / ICH_BOX_SIZE;
}


ICHBox &ICHAC97Device::Box(AC97StreamEnum stream)
{
    for (int i = 0; i < AC97_STREAM_COUNT; i++) {
        if (kBoxStream[i] == stream) {
            return fBoxes[i];
        }
    }
    return fBoxes[0];
}


//#pragma mark - guest memory

bool ICHAC97Device::Dma(uint32_t addr, uint8_t *buf, size_t len,
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


//#pragma mark - streams

/* Everything but the interrupt enables goes back to its default. */
void ICHAC97Device::BoxReset(ICHBox &b)
{
    uint8_t cr = b.cr & ICH_CR_IE;
    b = ICHBox();
    b.cr = cr;
}


void ICHAC97Device::Fetch(ICHBox &b)
{
    uint8_t e[ICH_BD_SIZE];

    if (!Dma(b.bdbar + b.civ * ICH_BD_SIZE, e, sizeof(e), false)) {
        memset(e, 0, sizeof(e));
    }
    /* Buffers hold whole samples. */
    b.addr = get_le32(e) & ~1u;
    b.ctl = get_le32(e + 4);
    b.picb = b.Length();
    b.fetched = true;
}


/* The buffer at CIV has been played or filled. The engine moves on to the
   next, or stops at the last valid one until the driver gives it more. */
void ICHAC97Device::Complete(ICHBox &b)
{
    if ((b.ctl & ICH_BD_IOC) != 0) {
        b.sr |= ICH_SR_BCIS;
    }
    if (b.civ == b.lvi) {
        b.sr |= ICH_SR_LVBCI | ICH_SR_CELV | ICH_SR_DCH;
        return;
    }
    b.civ = b.Piv();
    Fetch(b);
}


/* Move 'len' bytes between 'buf' and the stream's buffers. Output the engine
   cannot supply is silence. Running out of buffers is not a FIFO error; the
   FIFO never runs dry while there are buffers left. */
void ICHAC97Device::Transfer(ICHBox &b, uint8_t *buf, size_t len,
                             bool to_guest)
{
    size_t done = 0;
    int empty = 0;

    while (done < len && b.Moving()) {
        if (!b.fetched) {
            Fetch(b);
        }
        if (b.picb > 0) {
            empty = 0;
            uint32_t pos = (b.Length() - b.picb) * AC97_SAMPLE_BYTES;
            size_t n = (size_t)b.picb * AC97_SAMPLE_BYTES;
            if (n > len - done) {
                n = len - done;
            }
            if (!Dma(b.addr + pos, buf + done, n, to_guest) && !to_guest) {
                memset(buf + done, 0, n);
            }
            b.picb -= n / AC97_SAMPLE_BYTES;
            done += n;
            if (b.picb > 0) {
                continue;
            }
        } else if (++empty > ICH_BD_COUNT) {
            /* a list of nothing but empty buffers */
            break;
        }
        Complete(b);
    }
    if (done < len && !to_guest) {
        memset(buf + done, 0, len - done);
    }
}


bool ICHAC97Device::StreamRunning(AC97StreamEnum stream) const
{
    for (int i = 0; i < AC97_STREAM_COUNT; i++) {
        if (kBoxStream[i] == stream) {
            return (fBoxes[i].cr & ICH_CR_RPBM) != 0;
        }
    }
    return false;
}


void ICHAC97Device::PullOutput(uint8_t *buf, size_t frames)
{
    Transfer(Box(AC97_PCM_OUT), buf,
             frames * ac97_stream_channels(AC97_PCM_OUT) * AC97_SAMPLE_BYTES,
             false);
    UpdateIrq();
}


void ICHAC97Device::PushInput(AC97StreamEnum stream, const uint8_t *buf,
                              size_t frames)
{
    size_t len = frames * ac97_stream_channels(stream) * AC97_SAMPLE_BYTES;

    fBuffer.assign(buf, buf + len);
    Transfer(Box(stream), fBuffer.data(), len, true);
    UpdateIrq();
}


//#pragma mark - interrupts

uint32_t ICHAC97Device::GlobSta() const
{
    uint32_t sta = fGlobSta;

    for (int i = 0; i < AC97_STREAM_COUNT; i++) {
        if (fBoxes[i].Interrupt()) {
            sta |= kBoxInt[i];
        }
    }
    if (LinkUp()) {
        if (fChildBus->Codec(0) != nullptr) {
            sta |= ICH_STA_PCR;
        }
        if (fChildBus->Codec(1) != nullptr) {
            sta |= ICH_STA_SCR;
        }
    }
    return sta;
}


void ICHAC97Device::UpdateIrq()
{
    if (fIrq == nullptr) {
        return;
    }

    bool level = false;
    for (const ICHBox &b : fBoxes) {
        level |= b.Interrupt();
    }
    if ((fGlobSta & ICH_STA_GSCI) != 0 && (fGlobCnt & ICH_CNT_GIE) != 0) {
        level = true;
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


//#pragma mark - mixer registers

uint32_t ICHAC97Device::NamRead(uint32_t offset, int size_log2)
{
    int size = 1 << size_log2;
    uint32_t val = 0;

    fCas = 0;
    for (int i = 0; i < size; i += 2) {
        uint32_t o = offset + i;
        AC97Codec *codec = fChildBus->Codec(o / ICH_NAM_CODEC);
        uint16_t reg = 0;
        if (codec != nullptr && LinkUp()) {
            reg = codec->ReadRegister(o % ICH_NAM_CODEC & ~1u);
        } else {
            /* Nobody answers, and the read times out. */
            fGlobSta |= ICH_STA_RCS;
        }
        val |= (uint32_t)reg << (i * 8);
    }
    if (size == 1) {
        val = (val >> ((offset & 1) * 8)) & 0xff;
    }
#ifdef DEBUG_ICH_AC97
    printf("ac97: codec read  %02x/%d -> %04x\n", offset, size, val);
#endif
    return val;
}


void ICHAC97Device::NamWrite(uint32_t offset, uint32_t val, int size_log2)
{
    int size = 1 << size_log2;

#ifdef DEBUG_ICH_AC97
    printf("ac97: codec write %02x/%d <- %04x\n", offset, size, val);
#endif
    fCas = 0;
    for (int i = 0; i < (size < 2 ? 2 : size); i += 2) {
        uint32_t o = (offset & ~1u) + i;
        AC97Codec *codec = fChildBus->Codec(o / ICH_NAM_CODEC);
        if (codec == nullptr || !LinkUp()) {
            continue;
        }
        int reg = o % ICH_NAM_CODEC;
        uint16_t v = (uint16_t)(val >> (i * 8));
        if (size == 1) {
            /* the other half as it was */
            int shift = (offset & 1) * 8;
            v = (codec->ReadRegister(reg) & ~(0xff << shift)) |
                (val & 0xff) << shift;
        }
        codec->WriteRegister(reg, v);
    }
}


//#pragma mark - bus master registers

uint8_t ICHAC97Device::BoxReadByte(const ICHBox &b, uint32_t rel) const
{
    switch (rel) {
    case ICH_BDBAR ... ICH_BDBAR + 3:
        return (uint8_t)(b.bdbar >> ((rel - ICH_BDBAR) * 8));
    case ICH_CIV:
        return b.civ;
    case ICH_LVI:
        return b.lvi;
    case ICH_SR:
        return (uint8_t)b.sr;
    case ICH_SR + 1:
        return (uint8_t)(b.sr >> 8);
    case ICH_PICB:
        return (uint8_t)b.picb;
    case ICH_PICB + 1:
        return (uint8_t)(b.picb >> 8);
    case ICH_PIV:
        return b.Piv();
    case ICH_CR:
        return b.cr;
    }
    return 0;
}


uint8_t ICHAC97Device::ReadByte(uint32_t offset)
{
    int box = box_at(offset);

    if (box >= 0) {
        return BoxReadByte(fBoxes[box], offset % ICH_BOX_SIZE);
    }
    switch (offset) {
    case ICH_GLOB_CNT ... ICH_GLOB_CNT + 3:
        return (uint8_t)(fGlobCnt >> ((offset - ICH_GLOB_CNT) * 8));
    case ICH_GLOB_STA ... ICH_GLOB_STA + 3:
        return (uint8_t)(GlobSta() >> ((offset - ICH_GLOB_STA) * 8));
    case ICH_CAS: {
        /* Reading the semaphore takes it; the next codec access gives it
           back. */
        uint8_t val = fCas;
        fCas = 1;
        return val;
    }
    }
    return 0;
}


uint32_t ICHAC97Device::NabmRead(uint32_t offset, int size_log2)
{
    uint32_t val = 0;

    /* Positions and status are read as of now, not as of the last audio
       tick; a driver that measures the link's clock against its own
       needs that. */
    if (box_at(offset) >= 0 &&
        offset % ICH_BOX_SIZE + (1 << size_log2) > ICH_CIV &&
        offset % ICH_BOX_SIZE <= ICH_PIV) {
        fChildBus->Sync();
    }
    for (int i = 0; i < (1 << size_log2); i++) {
        val |= (uint32_t)ReadByte(offset + i) << (i * 8);
    }
#ifdef DEBUG_ICH_AC97
    printf("ac97: read  %02x/%d -> %08x\n", offset, 1 << size_log2, val);
#endif
    return val;
}


void ICHAC97Device::BoxWriteByte(ICHBox &b, uint32_t rel, uint8_t val)
{
    switch (rel) {
    case ICH_BDBAR ... ICH_BDBAR + 3: {
        int shift = (rel - ICH_BDBAR) * 8;
        b.bdbar = (b.bdbar & ~(0xffu << shift)) | (uint32_t)val << shift;
        b.bdbar &= ~(uint32_t)(ICH_BD_SIZE - 1);
        break;
    }
    case ICH_LVI:
        b.lvi = val % ICH_BD_COUNT;
        break;
    case ICH_SR:
        b.sr &= ~(val & ICH_SR_W1C);
        break;
    case ICH_CR:
        b.cr = val & (ICH_CR_RPBM | ICH_CR_RR | ICH_CR_IE);
        break;
    }
}


/* What a write to a box sets off, once the whole of it has landed. */
void ICHAC97Device::BoxWritten(ICHBox &b, uint8_t old_cr, uint8_t old_lvi)
{
    bool changed = false;

    if ((b.cr & ICH_CR_RR) != 0) {
        BoxReset(b);
        changed = (old_cr & ICH_CR_RPBM) != 0;
    } else if ((old_cr ^ b.cr) & ICH_CR_RPBM) {
        changed = true;
        if ((b.cr & ICH_CR_RPBM) == 0) {
            /* paused where it was */
            b.sr |= ICH_SR_DCH;
        } else if ((b.sr & ICH_SR_CELV) == 0) {
            b.sr &= ~ICH_SR_DCH;
            if (!b.fetched) {
                Fetch(b);
            }
        }
    }
    /* A new last valid buffer lets an engine that ran out go on. */
    if (b.lvi != old_lvi && (b.sr & ICH_SR_CELV) != 0 && b.lvi != b.civ) {
        b.sr &= ~ICH_SR_CELV;
        b.civ = b.Piv();
        Fetch(b);
        if ((b.cr & ICH_CR_RPBM) != 0) {
            b.sr &= ~ICH_SR_DCH;
        }
    }
    if (changed) {
        fChildBus->StreamsChanged();
    }
}


void ICHAC97Device::GlobCntWritten(uint32_t old_cnt)
{
    if ((old_cnt ^ fGlobCnt) & ICH_CNT_COLD) {
        if (LinkUp()) {
            fChildBus->Reset();
        }
        fChildBus->StreamsChanged();
    }
    /* A warm reset is over at once. */
    fGlobCnt &= ~ICH_CNT_WARM;
}


void ICHAC97Device::NabmWrite(uint32_t offset, uint32_t val, int size_log2)
{
    int size = 1 << size_log2;

#ifdef DEBUG_ICH_AC97
    printf("ac97: write %02x/%d <- %08x\n", offset, size, val);
#endif
    if (box_at(offset) >= 0) {
        ICHBox &b = fBoxes[box_at(offset)];
        uint8_t old_cr = b.cr, old_lvi = b.lvi;
        for (int i = 0; i < size; i++) {
            uint32_t rel = (offset + i) % ICH_BOX_SIZE;
            BoxWriteByte(b, rel, (uint8_t)(val >> (i * 8)));
        }
        BoxWritten(b, old_cr, old_lvi);
        UpdateIrq();
        return;
    }

    uint32_t old_cnt = fGlobCnt;
    for (int i = 0; i < size; i++) {
        uint32_t o = offset + i;
        uint32_t v = (val >> (i * 8)) & 0xff;
        switch (o) {
        case ICH_GLOB_CNT ... ICH_GLOB_CNT + 3: {
            int shift = (o - ICH_GLOB_CNT) * 8;
            fGlobCnt = ((fGlobCnt & ~(0xffu << shift)) | v << shift) &
                       ICH_CNT_MASK;
            break;
        }
        case ICH_GLOB_STA ... ICH_GLOB_STA + 3: {
            int shift = (o - ICH_GLOB_STA) * 8;
            fGlobSta &= ~((v << shift) & ICH_STA_W1C);
            fGlobSta = (fGlobSta & ~(ICH_STA_RW & 0xffu << shift)) |
                       ((v << shift) & ICH_STA_RW);
            break;
        }
        }
    }
    if (offset <= ICH_GLOB_CNT + 3 && offset + size > ICH_GLOB_CNT) {
        GlobCntWritten(old_cnt);
    }
    UpdateIrq();
}


//#pragma mark - lifecycle

void ICHAC97Device::SetBar(int bar_num, uint64_t addr, bool enabled)
{
    (bar_num == 0 ? fNamRange : fNabmRange)->SetAddr(addr, enabled);
}


bool ICHAC97Device::Prepare()
{
    if (ParentBus()->AsPCIBus() == nullptr) {
        vm_error("%s: must be attached to a PCI bus\n", Name());
        return false;
    }
    fChildBus = new AC97Bus(this, this);
    return true;
}


bool ICHAC97Device::Realize()
{
    PCIBus *pci_bus = ParentBus()->AsPCIBus();
    int devio_flags = DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32 |
                      DEVIO_DISABLED;

    /* The first ICH's AC'97 function, 82801AA. */
    fPciDev = pci_register_device(pci_bus, Name(), -1, 0x8086, 0x2415, 0x01,
                                  0x0401);
    if (fPciDev == nullptr) {
        vm_error("%s: could not register the PCI device\n", Name());
        return false;
    }
    pci_device_set_config8(fPciDev, PCI_INTERRUPT_PIN, 1);

    PhysMemoryMap *port_map = pci_device_get_port_map(fPciDev);
    if (port_map == nullptr) {
        vm_error("%s: this PCI bus has no I/O port space, which is where the "
                 "controller's registers live\n", Name());
        return false;
    }
    fNamRange = port_map->RegisterDevice(0, ICH_NAM_SIZE, &fNamIo,
                                         devio_flags);
    pci_register_bar(fPciDev, 0, ICH_NAM_SIZE, PCI_ADDRESS_SPACE_IO, this);
    fNabmRange = port_map->RegisterDevice(0, ICH_NABM_SIZE, &fNabmIo,
                                          devio_flags);
    pci_register_bar(fPciDev, 1, ICH_NABM_SIZE, PCI_ADDRESS_SPACE_IO, this);
    fIrq = pci_device_get_irq(fPciDev, 0);
    return true;
}


//#pragma mark - class

/* An ICH AC'97 controller on a PCI bus, and the link it provides. */
class ICHAC97Class final: public DeviceClass {
public:
    ICHAC97Class(): DeviceClass("ich-ac97") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        (void)ctx;
        return new ICHAC97Device(cfg.IdOr("ac97"));
    }
};

static const ICHAC97Class sICHAC97Class;
