/*
 * Synopsys DesignWare Ethernet QoS (dwmac4) media access controller
 *
 * Copyright (c) 2016-2018 Fabrice Bellard
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
#include "dwmac.h"

#include <string.h>

#include "cutils.h"
#include "fdt.h"

/* MAC block */
#define GMAC_CONFIG               0x0000
#define GMAC_PACKET_FILTER        0x0008
#define GMAC_INT_STATUS           0x00b0
#define GMAC_INT_EN               0x00b4
#define GMAC_PHYIF_CONTROL_STATUS 0x00f8
#define GMAC_VERSION              0x0110
#define GMAC_DEBUG                0x0114
#define GMAC_HW_FEATURE0          0x011c
#define GMAC_HW_FEATURE1          0x0120
#define GMAC_HW_FEATURE2          0x0124
#define GMAC_HW_FEATURE3          0x0128
#define GMAC_MDIO_ADDR            0x0200
#define GMAC_MDIO_DATA            0x0204
#define GMAC_ADDR_HIGH(n)         (0x0300 + (n) * 8)
#define GMAC_ADDR_LOW(n)          (0x0304 + (n) * 8)
#define GMAC_MMC_BASE             0x0700
#define GMAC_MMC_END              0x0900

#define GMAC_CONFIG_RE (1u << 0)
#define GMAC_CONFIG_TE (1u << 1)

#define GMAC_PACKET_FILTER_PR  (1u << 0)  /* promiscuous */
#define GMAC_PACKET_FILTER_PM  (1u << 4)  /* pass all multicast */
#define GMAC_PACKET_FILTER_DBF (1u << 5)  /* disable broadcast */
#define GMAC_PACKET_FILTER_RA  (1u << 31) /* receive all */

#define GMAC_ADDR_HIGH_AE (1u << 31)

/* Link status change on the media independent interface. Latched here and
   cleared when the status register below is read. */
#define GMAC_INT_RGSMIIIS (1u << 0)

#define GMAC_PHYIF_LNKMOD (1u << 16) /* full duplex */
#define GMAC_PHYIF_SPEED_SHIFT 17
#define GMAC_PHYIF_LNKSTS (1u << 19)

#define GMAC_MDIO_ADDR_GB   (1u << 0)
#define GMAC_MDIO_ADDR_C45E (1u << 1)
#define GMAC_MDIO_GOC_WRITE 1
#define GMAC_MDIO_GOC_READ  3

/* MTL block: common registers, then one 0x40 block per queue. */
#define MTL_CHAN_BASE 0x0d00
#define MTL_CHAN_SIZE 0x0040
#define MTL_CHAN_TX_DEBUG      0x08
#define MTL_CHAN_RX_DEBUG      0x38
#define MTL_CHAN_RX_MISSED_PKT 0x3c

/* DMA block */
#define DMA_BUS_MODE     0x1000
#define DMA_SYS_BUS_MODE 0x1004
#define DMA_STATUS       0x1008

#define DMA_BUS_MODE_SWR      (1u << 0)
#define DMA_SYS_BUS_MODE_EAME (1u << 11)

#define DMA_CHAN_BASE 0x1100
#define DMA_CHAN_SIZE 0x0080

#define DMA_CHAN_CONTROL          0x00
#define DMA_CHAN_TX_CONTROL       0x04
#define DMA_CHAN_RX_CONTROL       0x08
#define DMA_CHAN_TXDESC_HADDR     0x10
#define DMA_CHAN_TXDESC_LADDR     0x14
#define DMA_CHAN_RXDESC_HADDR     0x18
#define DMA_CHAN_RXDESC_LADDR     0x1c
#define DMA_CHAN_TXDESC_TAIL      0x20
#define DMA_CHAN_RXDESC_TAIL      0x28
#define DMA_CHAN_TXDESC_RING_LEN  0x2c
#define DMA_CHAN_RXDESC_RING_LEN  0x30
#define DMA_CHAN_INTR_ENA         0x34
#define DMA_CHAN_CUR_TXDESC       0x44
#define DMA_CHAN_CUR_RXDESC       0x4c
#define DMA_CHAN_CUR_TXBUF        0x54
#define DMA_CHAN_CUR_RXBUF        0x5c
#define DMA_CHAN_STATUS           0x60

#define DMA_CHAN_CONTROL_DSL_SHIFT 18
#define DMA_CHAN_CONTROL_DSL_MASK  0x7

#define DMA_CHAN_TX_CONTROL_ST (1u << 0)
#define DMA_CHAN_RX_CONTROL_SR (1u << 0)
#define DMA_CHAN_RX_CONTROL_RBSZ_SHIFT 1
#define DMA_CHAN_RX_CONTROL_RBSZ_MASK  0x3fff

#define DMA_CHAN_STATUS_TI  (1u << 0)
#define DMA_CHAN_STATUS_TBU (1u << 2)
#define DMA_CHAN_STATUS_RI  (1u << 6)
#define DMA_CHAN_STATUS_RBU (1u << 7)
#define DMA_CHAN_STATUS_FBE (1u << 12)
#define DMA_CHAN_STATUS_AIS (1u << 14)
#define DMA_CHAN_STATUS_NIS (1u << 15)

/* Which summary bit a condition rolls up into. */
#define DMA_CHAN_STATUS_NORMAL \
    (DMA_CHAN_STATUS_TI | DMA_CHAN_STATUS_TBU | DMA_CHAN_STATUS_RI)
#define DMA_CHAN_STATUS_ABNORMAL \
    (DMA_CHAN_STATUS_RBU | DMA_CHAN_STATUS_FBE)

/* Descriptors: four little endian words, read format on the way in and write
   back format on the way out. */
#define TDES2_BUFFER1_SIZE_MASK 0x00003fff
#define TDES3_PACKET_SIZE_MASK  0x00007fff
#define TDES3_ERROR_SUMMARY     (1u << 15)
#define TDES3_LAST_DESCRIPTOR   (1u << 28)
#define TDES3_FIRST_DESCRIPTOR  (1u << 29)
#define TDES3_CONTEXT_TYPE      (1u << 30)
#define TDES3_OWN               (1u << 31)

#define RDES3_PACKET_SIZE_MASK  0x00007fff
#define RDES3_LAST_DESCRIPTOR   (1u << 28)
#define RDES3_FIRST_DESCRIPTOR  (1u << 29)
#define RDES3_OWN               (1u << 31)

/* The transmit and receive FIFO sizes are reported as log2(bytes / 128), and
   the drivers derive their queue sizes from them as (bytes / 256) - 1 without
   clamping. Anything below 2 makes that underflow, so 4 KB it is. */
#define DWMAC_FIFO_SIZE_LOG2 5

/* One queue, one channel, MDIO present, no checksum offload, no TSO, no
   timestamping, no counters and no filters beyond the perfect ones. Each
   count field is encoded as the count minus one, so a zero field means one. */
#define DWMAC_HW_FEATURE0 \
    ((1u << 0) |  /* 10/100 supported */ \
     (1u << 1) |  /* 1000 supported */ \
     (1u << 2) |  /* half duplex supported */ \
     (1u << 5) |  /* MDIO present */ \
     ((uint32_t)(DWMAC_ADDR_COUNT - 1) << 18))
#define DWMAC_HW_FEATURE1 \
    ((DWMAC_FIFO_SIZE_LOG2 << 6) | DWMAC_FIFO_SIZE_LOG2)
#define DWMAC_HW_FEATURE2 0
#define DWMAC_HW_FEATURE3 0

static const uint8_t kBroadcastAddr[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};


/* The core revision a driver reads back. It has to agree with what the
   configured "compatible" claims, because a driver picks its register
   accessors from one and sanity checks them against the other: the name in
   the device tree is what makes Linux choose the Ethernet QoS layout this
   models rather than the older GMAC one. A name that selects neither returns
   0 and is refused when the device is prepared. */
static uint32_t version_for_compatible(const char *compatible)
{
    static const struct {
        const char *suffix;
        uint32_t id;
    } kVersions[] = {
        {"4.00",  0x40},
        {"4.10a", 0x41},
        {"4.20a", 0x42},
        {"5.00a", 0x50},
        {"5.10a", 0x51},
        {"5.20",  0x52},
    };

    for (size_t i = 0; i < countof(kVersions); i++) {
        if (strstr(compatible, kVersions[i].suffix) != nullptr) {
            return kVersions[i].id;
        }
    }
    return 0;
}


//#pragma mark - construction

DwmacDevice::DwmacDevice(DeviceContext *ctx, VMDeviceNode *node,
                         const char *compatible, const char *phy_mode,
                         uint32_t quirks):
    Device("dwmac"),
    fCtx(ctx),
    fNode(node),
    fCompatible(compatible),
    fPhyMode(phy_mode),
    fQuirks(quirks)
{
}


uint32_t dwmac_quirks_from_name(const char *name)
{
    if (strcmp(name, "clocks") == 0) {
        return DWMAC_QUIRK_CLOCKS;
    }
    if (strcmp(name, "link-on-reset") == 0) {
        return DWMAC_QUIRK_LINK_ON_RESET;
    }
    if (strcmp(name, "haiku") == 0) {
        /* Both, which is what Haiku's driver needs together. */
        return DWMAC_QUIRK_CLOCKS | DWMAC_QUIRK_LINK_ON_RESET;
    }
    return 0;
}


DwmacDevice::~DwmacDevice()
{
    delete fMdioBus;
}


bool DwmacDevice::Prepare()
{
    SystemBus *sys = static_cast<SystemBus *>(ParentBus());
    if (sys == nullptr || ParentBus()->AsPCIBus() != nullptr) {
        vm_error("%s: must be attached to a system bus\n", Name());
        return false;
    }

    /* Only the Ethernet QoS register layout is modelled, and which layout a
       driver uses is decided by the name in the device tree, so a name that
       would send it to the older GMAC one is refused rather than left to
       fail confusingly inside the guest. */
    fVersion = version_for_compatible(fCompatible);
    if (fVersion == 0) {
        vm_error("%s: 'compatible' is '%s', which does not name a DesignWare "
                 "Ethernet QoS core; it must contain one of 4.00, 4.10a, "
                 "4.20a, 5.00a, 5.10a or 5.20\n", Name(), fCompatible);
        return false;
    }

    fMmioRes = AddResource(RES_MMIO, DWMAC_REG_SIZE, DWMAC_REG_SIZE);
    fIrqRes = AddResource(RES_IRQ, 1);
    if (fMmioRes == nullptr || fIrqRes == nullptr) {
        return false;
    }

    fMdioBus = new MDIOBus(this);
    return true;
}


bool DwmacDevice::Realize()
{
    SystemBus *sys = static_cast<SystemBus *>(ParentBus());

    if (fNode->net == nullptr) {
        vm_error("%s: no network back end\n", Name());
        return false;
    }
    /* The emulator polls a single back end from its main loop, so a second
       network device would simply never receive anything. */
    if (fCtx->net != nullptr) {
        vm_error("%s: only one network device is supported\n", Name());
        return false;
    }
    if (Phy() == nullptr) {
        vm_error("%s: no PHY declared on the mdio bus\n", Name());
        return false;
    }

    fIrq = sys->IrqSignalFor(fIrqRes->base);
    if (fIrq == nullptr) {
        vm_error("%s: bad interrupt line %d\n", Name(), (int)fIrqRes->base);
        return false;
    }

    fNet = fNode->net;
    fMemMap = sys->MemMap();
    Reset();

    fMemMap->RegisterDevice(fMmioRes->base, fMmioRes->size, &fIo,
                            DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32);

    fNet->target = this;
    fCtx->net = fNet;
    return true;
}


void DwmacDevice::Reset()
{
    memset(fRegs, 0, sizeof(fRegs));
    fMacIntStatus = 0;
    fTxCur = 0;
    fRxCur = 0;
    if (fIrq != nullptr && fIrqLevel) {
        fIrqLevel = false;
        fIrq->Set(0);
    }

    /* The reset threw away the MAC's record of the link, so the next time it
       samples the in band status it finds one it has not reported. A driver
       that learns the link only from that announcement, and resets the MAC
       before enabling it, would otherwise never hear about a carrier that
       came up before the guest was running. */
    if ((fQuirks & DWMAC_QUIRK_LINK_ON_RESET) != 0) {
        MDIODevice *phy = Phy();
        if (phy != nullptr && phy->LinkUp()) {
            fMacIntStatus |= GMAC_INT_RGSMIIIS;
        }
    }
}


//#pragma mark - register helpers

uint32_t &DwmacDevice::ChanReg(uint32_t offset)
{
    return Reg(DMA_CHAN_BASE + offset);
}


uint32_t DwmacDevice::ChanReg(uint32_t offset) const
{
    return Reg(DMA_CHAN_BASE + offset);
}


MDIODevice *DwmacDevice::Phy() const
{
    if (fMdioBus == nullptr) {
        return nullptr;
    }
    return fMdioBus->MDIODeviceAt(0);
}


/* The in band link status a media independent interface reports back to the
   MAC. Some drivers read only this and never look at the PHY at all. */
uint32_t DwmacDevice::PhyIfStatus() const
{
    MDIODevice *phy = Phy();
    if (phy == nullptr || !phy->LinkUp()) {
        return 0;
    }

    uint32_t speed_code;
    switch (phy->Speed()) {
    case 10:
        speed_code = 0; /* 2.5 MHz */
        break;
    case 100:
        speed_code = 1; /* 25 MHz */
        break;
    default:
        speed_code = 2; /* 125 MHz */
        break;
    }

    uint32_t val = GMAC_PHYIF_LNKSTS | (speed_code << GMAC_PHYIF_SPEED_SHIFT);
    if (phy->FullDuplex()) {
        val |= GMAC_PHYIF_LNKMOD;
    }
    return val;
}


void DwmacDevice::MdioTransfer(uint32_t val)
{
    int phy_addr = (val >> 21) & 0x1f;
    int reg = (val >> 16) & 0x1f;
    int op = (val >> 2) & 3;
    MDIODevice *dev = fMdioBus->DeviceAtAddress(phy_addr);

    /* Clause 45 is not modelled, and neither is an empty address: both have
       to read as all ones so that a driver scanning the bus moves on. */
    if (dev == nullptr || (val & GMAC_MDIO_ADDR_C45E) != 0) {
        if (op == GMAC_MDIO_GOC_READ) {
            Reg(GMAC_MDIO_DATA) = MDIO_NO_DEVICE;
        }
    } else if (op == GMAC_MDIO_GOC_READ) {
        Reg(GMAC_MDIO_DATA) = dev->MdioRead(reg);
    } else if (op == GMAC_MDIO_GOC_WRITE) {
        dev->MdioWrite(reg, Reg(GMAC_MDIO_DATA) & 0xffff);
    }

    /* The transfer completes within the write, so the busy bit a driver
       polls is never observed set. */
    Reg(GMAC_MDIO_ADDR) = val & ~GMAC_MDIO_ADDR_GB;
}


void DwmacDevice::RaiseDma(uint32_t bits)
{
    if ((bits & DMA_CHAN_STATUS_NORMAL) != 0) {
        bits |= DMA_CHAN_STATUS_NIS;
    }
    if ((bits & DMA_CHAN_STATUS_ABNORMAL) != 0) {
        bits |= DMA_CHAN_STATUS_AIS;
    }
    ChanReg(DMA_CHAN_STATUS) |= bits;
    UpdateIrq();
}


void DwmacDevice::UpdateIrq()
{
    if (fIrq == nullptr) {
        return;
    }
    bool level =
        (ChanReg(DMA_CHAN_STATUS) & ChanReg(DMA_CHAN_INTR_ENA)) != 0 ||
        (fMacIntStatus & Reg(GMAC_INT_EN) & GMAC_INT_RGSMIIIS) != 0;
    if (level != fIrqLevel) {
        fIrqLevel = level;
        fIrq->Set(level);
    }
}


//#pragma mark - guest memory

/* GetRamPtr() is only valid inside one page, so both directions walk the
   transfer a page at a time, exactly as the virtio queue helpers do. */
bool DwmacDevice::DmaRead(uint64_t addr, uint8_t *buf, int len)
{
    while (len > 0) {
        int page_left =
            DEVRAM_PAGE_SIZE - (int)(addr & (DEVRAM_PAGE_SIZE - 1));
        int chunk = min_int(len, page_left);
        uint8_t *ptr = fMemMap->GetRamPtr(addr, false);
        if (ptr == nullptr) {
            return false;
        }
        memcpy(buf, ptr, chunk);
        addr += chunk;
        buf += chunk;
        len -= chunk;
    }
    return true;
}


bool DwmacDevice::DmaWrite(uint64_t addr, const uint8_t *buf, int len)
{
    while (len > 0) {
        int page_left =
            DEVRAM_PAGE_SIZE - (int)(addr & (DEVRAM_PAGE_SIZE - 1));
        int chunk = min_int(len, page_left);
        uint8_t *ptr = fMemMap->GetRamPtr(addr, true);
        if (ptr == nullptr) {
            return false;
        }
        memcpy(ptr, buf, chunk);
        addr += chunk;
        buf += chunk;
        len -= chunk;
    }
    return true;
}


/* The high half of an address is only part of it when extended addressing is
   enabled; without it a driver may leave anything in the upper word. */
uint64_t DwmacDevice::BufferAddress(uint32_t lo, uint32_t hi) const
{
    if ((Reg(DMA_SYS_BUS_MODE) & DMA_SYS_BUS_MODE_EAME) == 0) {
        return lo;
    }
    return lo | ((uint64_t)hi << 32);
}


/* Descriptors are 16 bytes but the ring may be sparse: the skip length says
   how many bus words to step over between them. */
uint32_t DwmacDevice::DescStride() const
{
    uint32_t dsl = (ChanReg(DMA_CHAN_CONTROL) >> DMA_CHAN_CONTROL_DSL_SHIFT) &
        DMA_CHAN_CONTROL_DSL_MASK;
    return 16 + dsl * 8;
}


uint32_t DwmacDevice::RingLength(bool tx) const
{
    uint32_t reg = tx ? DMA_CHAN_TXDESC_RING_LEN : DMA_CHAN_RXDESC_RING_LEN;
    return (ChanReg(reg) & 0x3ff) + 1;
}


uint64_t DwmacDevice::DescAddress(bool tx, uint32_t index) const
{
    uint32_t lo = tx ? DMA_CHAN_TXDESC_LADDR : DMA_CHAN_RXDESC_LADDR;
    uint32_t hi = tx ? DMA_CHAN_TXDESC_HADDR : DMA_CHAN_RXDESC_HADDR;
    return BufferAddress(ChanReg(lo), ChanReg(hi)) +
        (uint64_t)index * DescStride();
}


bool DwmacDevice::ReadDesc(uint64_t addr, uint32_t desc[4])
{
    uint8_t buf[16];
    if (!DmaRead(addr, buf, sizeof(buf))) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        desc[i] = get_le32(buf + i * 4);
    }
    return true;
}


bool DwmacDevice::WriteDesc(uint64_t addr, const uint32_t desc[4])
{
    uint8_t buf[16];
    for (int i = 0; i < 4; i++) {
        put_le32(buf + i * 4, desc[i]);
    }
    return DmaWrite(addr, buf, sizeof(buf));
}


//#pragma mark - transmit and receive

bool DwmacDevice::TxEnabled() const
{
    return (Reg(GMAC_CONFIG) & GMAC_CONFIG_TE) != 0 &&
        (ChanReg(DMA_CHAN_TX_CONTROL) & DMA_CHAN_TX_CONTROL_ST) != 0 &&
        ChanReg(DMA_CHAN_TXDESC_LADDR) != 0;
}


bool DwmacDevice::RxEnabled() const
{
    return (Reg(GMAC_CONFIG) & GMAC_CONFIG_RE) != 0 &&
        (ChanReg(DMA_CHAN_RX_CONTROL) & DMA_CHAN_RX_CONTROL_SR) != 0 &&
        ChanReg(DMA_CHAN_RXDESC_LADDR) != 0;
}


/* Drains the ring from the descriptor the engine stopped at. Ownership alone
   decides what may be sent: the tail pointer only says when to look, and the
   two reference drivers place it differently. */
void DwmacDevice::TxPoll()
{
    if (!TxEnabled() || fTxRunning) {
        return;
    }
    fTxRunning = true;

    uint8_t frame[DWMAC_MAX_FRAME];
    int frame_len = 0;
    bool in_frame = false;
    bool sent = false;
    uint32_t ring_len = RingLength(true);

    for (uint32_t i = 0; i < ring_len; i++) {
        uint64_t addr = DescAddress(true, fTxCur);
        uint32_t desc[4];

        if (!ReadDesc(addr, desc)) {
            RaiseDma(DMA_CHAN_STATUS_FBE);
            break;
        }
        if ((desc[3] & TDES3_OWN) == 0) {
            RaiseDma(DMA_CHAN_STATUS_TBU);
            break;
        }

        if ((desc[3] & TDES3_CONTEXT_TYPE) == 0) {
            if ((desc[3] & TDES3_FIRST_DESCRIPTOR) != 0) {
                frame_len = 0;
                in_frame = true;
            }
            uint32_t buf_len = desc[2] & TDES2_BUFFER1_SIZE_MASK;
            if (in_frame && buf_len > 0) {
                if (frame_len + (int)buf_len > (int)sizeof(frame) ||
                    !DmaRead(BufferAddress(desc[0], desc[1]),
                             frame + frame_len, buf_len)) {
                    /* Give the descriptor back rather than stalling the
                       ring; an oversized or unbacked frame is dropped. */
                    in_frame = false;
                } else {
                    frame_len += buf_len;
                }
            }
        }

        /* Write back: ownership returns to the driver and the error summary
           says the frame went out cleanly. */
        desc[3] &= ~(TDES3_OWN | TDES3_ERROR_SUMMARY);
        WriteDesc(addr, desc);
        fTxCur = (fTxCur + 1) % ring_len;

        if ((desc[3] & TDES3_LAST_DESCRIPTOR) != 0) {
            if (in_frame && frame_len > 0) {
                fNet->WritePacket(frame, frame_len);
            }
            in_frame = false;
            sent = true;
        }
    }

    fTxRunning = false;
    if (sent) {
        /* Raised for every completed frame rather than only for those asking
           for it: one of the drivers this models never sets the interrupt on
           completion bit yet still waits for this. */
        RaiseDma(DMA_CHAN_STATUS_TI);
    }
}


bool DwmacDevice::RxDescAvailable(uint32_t desc[4], uint64_t *addr_out)
{
    if (!RxEnabled()) {
        return false;
    }
    uint64_t addr = DescAddress(false, fRxCur);
    if (!ReadDesc(addr, desc)) {
        return false;
    }
    if ((desc[3] & RDES3_OWN) == 0) {
        return false;
    }
    *addr_out = addr;
    return true;
}


bool DwmacDevice::AddressMatches(const uint8_t *buf, int len) const
{
    if (len < 6) {
        return false;
    }
    uint32_t filter = Reg(GMAC_PACKET_FILTER);
    if ((filter & (GMAC_PACKET_FILTER_RA | GMAC_PACKET_FILTER_PR)) != 0) {
        return true;
    }

    if (memcmp(buf, kBroadcastAddr, 6) == 0) {
        return (filter & GMAC_PACKET_FILTER_DBF) == 0;
    }
    if ((buf[0] & 1) != 0) {
        /* No hash filter is modelled and none is advertised, so multicast is
           passed up for the guest's own filtering. A driver that wants it
           narrowed asks for it by setting the pass all multicast bit, which
           this already satisfies. */
        return true;
    }

    for (int i = 0; i < DWMAC_ADDR_COUNT; i++) {
        uint32_t high = Reg(GMAC_ADDR_HIGH(i));
        uint32_t low = Reg(GMAC_ADDR_LOW(i));
        /* Slot 0 holds the station address and is always active; the drivers
           modelled here do not bother to set its enable bit. */
        if (i != 0 && (high & GMAC_ADDR_HIGH_AE) == 0) {
            continue;
        }
        uint8_t addr[6];
        addr[0] = low;
        addr[1] = low >> 8;
        addr[2] = low >> 16;
        addr[3] = low >> 24;
        addr[4] = high;
        addr[5] = high >> 8;
        if (memcmp(buf, addr, 6) == 0) {
            return true;
        }
    }
    return false;
}


//#pragma mark - EthernetTarget

bool DwmacDevice::CanWritePacket()
{
    uint32_t desc[4];
    uint64_t addr;
    return RxDescAvailable(desc, &addr);
}


void DwmacDevice::WritePacket(const uint8_t *buf, int len)
{
    uint32_t desc[4];
    uint64_t addr;

    if (!RxEnabled() || !AddressMatches(buf, len)) {
        return;
    }
    if (!RxDescAvailable(desc, &addr)) {
        RaiseDma(DMA_CHAN_STATUS_RBU);
        return;
    }

    uint32_t buf_size = (ChanReg(DMA_CHAN_RX_CONTROL) >>
                         DMA_CHAN_RX_CONTROL_RBSZ_SHIFT) &
        DMA_CHAN_RX_CONTROL_RBSZ_MASK;
    if (buf_size == 0 || len > (int)buf_size) {
        /* A frame is never split over descriptors here, so one that does not
           fit is dropped rather than delivered in pieces. */
        return;
    }
    if (!DmaWrite(BufferAddress(desc[0], desc[1]), buf, len)) {
        RaiseDma(DMA_CHAN_STATUS_FBE);
        return;
    }

    /* Write back format: the status words replace the addresses the driver
       put there, and the whole frame sits in this one descriptor. */
    desc[0] = 0;
    desc[1] = 0;
    desc[2] = 0;
    desc[3] = ((uint32_t)len & RDES3_PACKET_SIZE_MASK) |
        RDES3_FIRST_DESCRIPTOR | RDES3_LAST_DESCRIPTOR;
    WriteDesc(addr, desc);

    fRxCur = (fRxCur + 1) % RingLength(false);
    RaiseDma(DMA_CHAN_STATUS_RI);
}


void DwmacDevice::SetCarrier(bool carrier_state)
{
    MDIODevice *phy = Phy();
    if (phy == nullptr) {
        return;
    }
    phy->SetCarrier(carrier_state);
    /* Latched until the driver reads the status register the change is
       reported in. */
    fMacIntStatus |= GMAC_INT_RGSMIIIS;
    UpdateIrq();
}


//#pragma mark - register access

/* Every register here is a word, but a driver need not reach one with a word
   access: a compiler narrows a store to a bit field to the half or the byte
   that field lives in, and Haiku's driver writes several registers that way.
   A narrow access is therefore served out of the word it lands in. */

uint32_t DwmacDevice::Read(uint32_t offset, int size_log2)
{
    uint32_t size = 1u << size_log2;

    if (size >= 4) {
        return ReadWord(offset);
    }
    uint32_t base = offset & ~3u;
    uint32_t shift = (offset - base) * 8;
    return (ReadWord(base) >> shift) & ((1u << (size * 8)) - 1);
}


void DwmacDevice::Write(uint32_t offset, uint32_t val, int size_log2)
{
    uint32_t size = 1u << size_log2;

    if (size >= 4) {
        WriteWord(offset, val);
        return;
    }
    if (offset >= DWMAC_SHADOW_SIZE) {
        return;
    }
    /* Fold the bytes into what the register holds and write the whole of it,
       so that a register with a side effect still sees one whole write. The
       merge takes the shadow rather than Read(), because reading the link
       status register is what acknowledges a change and a write to it must
       not do that. */
    uint32_t base = offset & ~3u;
    uint32_t shift = (offset - base) * 8;
    uint32_t mask = ((1u << (size * 8)) - 1) << shift;
    WriteWord(base, (Reg(base) & ~mask) | ((val << shift) & mask));
}


uint32_t DwmacDevice::ReadWord(uint32_t offset)
{
    if (offset >= DWMAC_SHADOW_SIZE) {
        return 0;
    }

    switch (offset) {
    case GMAC_VERSION:
        return fVersion;
    case GMAC_DEBUG:
        return 0; /* neither the transmitter nor the receiver is ever busy */
    case GMAC_HW_FEATURE0:
        return DWMAC_HW_FEATURE0;
    case GMAC_HW_FEATURE1:
        return DWMAC_HW_FEATURE1;
    case GMAC_HW_FEATURE2:
        return DWMAC_HW_FEATURE2;
    case GMAC_HW_FEATURE3:
        return DWMAC_HW_FEATURE3;
    case GMAC_INT_STATUS:
        return fMacIntStatus;
    case GMAC_PHYIF_CONTROL_STATUS:
        /* Reading the resolved link state is what acknowledges the change
           that reported it. Clearing it anywhere else leaves a driver that
           only reads this register spinning in its handler. */
        if (fMacIntStatus != 0) {
            fMacIntStatus = 0;
            UpdateIrq();
        }
        return PhyIfStatus();
    case DMA_STATUS:
        return (ChanReg(DMA_CHAN_STATUS) & ChanReg(DMA_CHAN_INTR_ENA)) != 0
            ? 1 : 0;
    case DMA_CHAN_BASE + DMA_CHAN_CUR_TXDESC:
        return (uint32_t)DescAddress(true, fTxCur);
    case DMA_CHAN_BASE + DMA_CHAN_CUR_RXDESC:
        return (uint32_t)DescAddress(false, fRxCur);
    case DMA_CHAN_BASE + DMA_CHAN_CUR_TXBUF:
    case DMA_CHAN_BASE + DMA_CHAN_CUR_RXBUF:
        return 0;
    case MTL_CHAN_BASE + MTL_CHAN_TX_DEBUG:
    case MTL_CHAN_BASE + MTL_CHAN_RX_DEBUG:
    case MTL_CHAN_BASE + MTL_CHAN_RX_MISSED_PKT:
        return 0; /* the queues are never occupied and nothing is missed */
    default:
        break;
    }

    /* No counters are modelled, and none are advertised. */
    if (offset >= GMAC_MMC_BASE && offset < GMAC_MMC_END) {
        return 0;
    }

    return Reg(offset);
}


void DwmacDevice::WriteWord(uint32_t offset, uint32_t val)
{
    if (offset >= DWMAC_SHADOW_SIZE) {
        return;
    }

    switch (offset) {
    /* Read only. */
    case GMAC_VERSION:
    case GMAC_DEBUG:
    case GMAC_HW_FEATURE0:
    case GMAC_HW_FEATURE1:
    case GMAC_HW_FEATURE2:
    case GMAC_HW_FEATURE3:
    case GMAC_INT_STATUS:
    case GMAC_PHYIF_CONTROL_STATUS:
    case DMA_STATUS:
        return;

    case GMAC_MDIO_ADDR:
        if ((val & GMAC_MDIO_ADDR_GB) != 0) {
            MdioTransfer(val);
        } else {
            Reg(offset) = val;
        }
        return;

    case GMAC_INT_EN:
        Reg(offset) = val;
        UpdateIrq();
        return;

    case GMAC_CONFIG:
        Reg(offset) = val;
        TxPoll();
        return;

    case DMA_BUS_MODE:
        if ((val & DMA_BUS_MODE_SWR) != 0) {
            /* A full reset, and the bit clears itself: a driver polls it and
               gives up if it is still set. */
            Reset();
            return;
        }
        Reg(offset) = val;
        return;

    case DMA_CHAN_BASE + DMA_CHAN_STATUS:
        /* Every bit is write one to clear, including the ones no condition
           ever sets: one of the drivers acknowledges the whole word. */
        ChanReg(DMA_CHAN_STATUS) &= ~val;
        UpdateIrq();
        return;

    case DMA_CHAN_BASE + DMA_CHAN_INTR_ENA:
        ChanReg(DMA_CHAN_INTR_ENA) = val;
        UpdateIrq();
        return;

    case DMA_CHAN_BASE + DMA_CHAN_TX_CONTROL:
    case DMA_CHAN_BASE + DMA_CHAN_TXDESC_TAIL:
        Reg(offset) = val;
        TxPoll();
        return;

    default:
        break;
    }

    if (offset >= GMAC_MMC_BASE && offset < GMAC_MMC_END) {
        return;
    }

    Reg(offset) = val;
}


//#pragma mark - device tree

void DwmacDevice::BuildFDT(FDTContext &ctx)
{
    FDTBuilder *fdt = ctx.fdt;
    MDIODevice *phy = Phy();

    /* Properties come before child nodes, so the phandle the MAC points at
       has to be handed to the PHY before its node is written. */
    uint32_t phy_phandle = fdt->AllocPhandle();
    phy->SetPhandle(phy_phandle);

    /* A driver written against a platform binding refuses to probe without
       the clocks that binding names. There is no clock controller in this
       machine to take them from, so one fixed clock stands in for all of
       them, named after the register window so that a machine with two MACs
       has two of them. */
    uint32_t clock_phandle = 0;
    if ((fQuirks & DWMAC_QUIRK_CLOCKS) != 0) {
        clock_phandle = fdt->AllocPhandle();
        fdt->BeginNodeNum("ethernet-clock", fMmioRes->base);
        fdt->PropStr("compatible", "fixed-clock");
        fdt->PropU32("#clock-cells", 0);
        fdt->PropU32("clock-frequency", DWMAC_CLOCK_HZ);
        fdt->PropU32("phandle", clock_phandle);
        fdt->EndNode();
    }

    fdt->BeginNodeNum("ethernet", fMmioRes->base);
    fdt->PropStrList("compatible", fCompatible, "snps,dwmac", nullptr);
    fdt->PropU64Range("reg", fMmioRes->base, fMmioRes->size);

    fdt_prop_plic_irq(ctx, fIrqRes->base);
    /* The line is looked up by name, not by index. */
    fdt->PropStr("interrupt-names", "macirq");

    if (clock_phandle != 0) {
        /* Every name the StarFive binding lists, so that a driver asking for
           any of them finds it. They are all the one fixed clock: nothing
           here is clocked, and what a driver does with the rate is divide it
           down for a bus it cannot observe either. */
        uint32_t clocks[7];
        for (int i = 0; i < 7; i++) {
            clocks[i] = clock_phandle;
        }
        fdt->PropTabU32("clocks", clocks, 7);
        fdt->PropStrList("clock-names", "gtx", "tx", "ptp_ref", "stmmaceth",
                         "pclk", "gtxc", "rmii_rtx", nullptr);
    }

    fdt->PropStr("phy-mode", fPhyMode);
    fdt->Prop("local-mac-address", fNet->mac_addr, 6);
    fdt->Prop("mac-address", fNet->mac_addr, 6);
    fdt->PropU32("snps,txpbl", 8);
    fdt->PropU32("snps,rxpbl", 8);
    fdt->PropU32("snps,perfect-filter-entries", DWMAC_ADDR_COUNT);
    fdt->PropU32("phy-handle", phy_phandle);

    fdt->BeginNode("mdio");
    fdt->PropStr("compatible", "snps,dwmac-mdio");
    fdt->PropU32("#address-cells", 1);
    fdt->PropU32("#size-cells", 0);
    fMdioBus->BuildFDTAll(ctx);
    fdt->EndNode(); /* mdio */

    fdt->EndNode(); /* ethernet */
}
