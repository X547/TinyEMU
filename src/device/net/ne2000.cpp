/*
 * NE2000 (DP8390) Ethernet adapter
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
#include "ne2000.h"

#include <string.h>

#include "bits.h"
#include "cutils.h"

/* What the RTL8029AS answers in the two registers a driver identifies it by,
   and what it reports about its media. */
#define NE_RTL8029_ID0 0x50 /* 'P' */
#define NE_RTL8029_ID1 0x43 /* 'C' */
#define NE_RTL8029_CONFIG0 0x00 /* 10baseT */
#define NE_RTL8029_CONFIG2 0x40 /* the link is up */
#define NE_RTL8029_CONFIG3 0x40 /* full duplex */

/* Identifiers of the PCI part, and the class code that names it an Ethernet
   controller. */
#define NE2000_PCI_VENDOR_ID 0x10ec
#define NE2000_PCI_DEVICE_ID 0x8029
#define NE2000_PCI_CLASS     0x0200


//#pragma mark - construction

NE2000Device::NE2000Device(DeviceContext *ctx,
                           std::unique_ptr<HostEthernet> net,
                           const char *name, int port, int irq):
    Device(name),
    fCtx(ctx),
    fNet(std::move(net)),
    fPort(port),
    fIrqLine(irq)
{
}


NE2000Device::~NE2000Device() = default;


bool NE2000Device::Prepare()
{
    if (ParentBus() == nullptr) {
        return false;
    }

    if (ParentBus()->AsPCIBus() != nullptr) {
        if (fPort >= 0 || fIrqLine >= 0) {
            vm_error("%s: 'reg' and 'irq' are for a card jumpered onto a "
                     "machine that addresses its devices by port number; on "
                     "PCI the guest places both\n", Name());
            return false;
        }
        return true;
    }

    /* Everywhere else the card has to be found at a fixed address, and a
       machine whose devices are not addressed by port number has none to
       offer: there the card belongs on a PCI bus. */
    SystemBus *sys = dynamic_cast<SystemBus *>(ParentBus());
    if (sys == nullptr || !sys->IsPortBased()) {
        vm_error("%s: must be attached to a PCI bus, or to the bus of a "
                 "machine that addresses its devices by port number\n",
                 Name());
        return false;
    }

    fIoRes = AddFixedResource(RES_IO, fPort >= 0 ? fPort : NE2000_PC_PORT,
                              NE2000_PORT_SIZE);
    fIrqRes = AddFixedResource(RES_IRQ,
                               fIrqLine >= 0 ? fIrqLine : NE2000_PC_IRQ, 1);
    return fIoRes != nullptr && fIrqRes != nullptr;
}


bool NE2000Device::Realize()
{
    int devio_flags = DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32;

    if (fNet == nullptr) {
        vm_error("%s: no network back end\n", Name());
        return false;
    }

    PCIBus *pci_bus = ParentBus()->AsPCIBus();
    if (pci_bus != nullptr) {
        fPciDev = pci_register_device(pci_bus, Name(), -1,
                                      NE2000_PCI_VENDOR_ID,
                                      NE2000_PCI_DEVICE_ID, 0x00,
                                      NE2000_PCI_CLASS);
        if (fPciDev == nullptr) {
            vm_error("%s: could not register the PCI function\n", Name());
            return false;
        }
        pci_device_set_config8(fPciDev, PCI_INTERRUPT_PIN, 1);

        PhysMemoryMap *port_map = pci_device_get_port_map(fPciDev);
        if (port_map == nullptr) {
            vm_error("%s: this PCI bus has no I/O port space, which is where "
                     "an NE2000's registers live\n", Name());
            return false;
        }
        fIoRange = port_map->RegisterDevice(0, NE2000_PCI_BAR_SIZE, &fIo,
                                            devio_flags | DEVIO_DISABLED);
        pci_register_bar(fPciDev, 0, NE2000_PCI_BAR_SIZE,
                         PCI_ADDRESS_SPACE_IO, this);
        fIrq = pci_device_get_irq(fPciDev, 0);
    } else {
        SystemBus *sys = static_cast<SystemBus *>(ParentBus());
        fIrq = sys->IrqSignalFor(fIrqRes->base);
        if (fIrq == nullptr) {
            vm_error("%s: bad interrupt line %d\n", Name(),
                     (int)fIrqRes->base);
            return false;
        }
        sys->PortMap()->RegisterDevice(fIoRes->base, fIoRes->size, &fIo,
                                       devio_flags);
    }

    Reset();
    fNet->target = this;
    fCtx->ethernet.push_back(fNet.get());
    return true;
}


void NE2000Device::Reset()
{
    /* What the command register holds after a reset: stopped, with the
       remote DMA aborted. */
    fCmd = NE_CR_STOP | NE_CR_RD_ABORT;
    fIsr = NE_ISR_RESET;
    fImr = 0;
    fDcr = 0;
    fRcr = 0;
    fTcr = 0;
    fTsr = 0;
    fRsr = 0;
    fTpsr = 0;
    fTcnt = 0;
    fRsar = 0;
    fRcnt = 0;
    fStart = 0;
    fStop = 0;
    fBoundary = 0;
    fCurPage = 0;
    memset(fPar, 0, sizeof(fPar));
    memset(fMar, 0, sizeof(fMar));
    LoadProm();
    UpdateIrq();
}


/* The address PROM, as the remote DMA reads it: the station address, then a
   signature that tells a driver it is looking at an NE2000 rather than at an
   NE1000. Every byte is stored twice because the card presents the PROM to a
   16 bit bus a byte at a time. */
void NE2000Device::LoadProm()
{
    uint8_t prom[NE2000_PROM_SIZE / 2] = {};

    memcpy(prom, fNet->mac_addr, 6);
    prom[14] = 0x57;
    prom[15] = 0x57;
    for (int i = 0; i < (int)sizeof(prom); i++) {
        fMem[i * 2] = prom[i];
        fMem[i * 2 + 1] = prom[i];
    }
}


void NE2000Device::UpdateIrq()
{
    /* The reset bit is not maskable, and is not an interrupt either: a driver
       polls it. */
    bool level = (fIsr & fImr & ~NE_ISR_RESET) != 0;

    /* A driver that means to poll turns the pin off in configuration space
       rather than in the card. */
    if (fPciDev != nullptr &&
        (pci_device_get_config(fPciDev, PCI_COMMAND, 1) &
         PCI_COMMAND_INTX_DISABLE) != 0) {
        level = false;
    }
    if (fIrq != nullptr && level != fIrqLevel) {
        fIrqLevel = level;
        fIrq->Set(level ? 1 : 0);
    }
}


//#pragma mark - the receive ring

/* The ring as the guest has left it. Only the packet buffer is memory, so a
   ring the guest placed anywhere else is one that stores nothing. */
bool NE2000Device::RingValid() const
{
    return fStart >= NE2000_PMEM_START && fStop <= NE2000_MEM_SIZE &&
        fStart + NE2000_PAGE_SIZE <= fStop;
}


/* A page pointer the guest left outside the ring -- which is every one of
   them until it has programmed the ring at all -- starts again at the
   beginning, so that nothing below ever addresses past the buffer. */
uint32_t NE2000Device::RingClamp(uint32_t addr) const
{
    return addr >= fStart && addr < fStop ? addr : fStart;
}


bool NE2000Device::BufferFull() const
{
    if (!RingValid()) {
        return true;
    }

    uint32_t index = RingClamp((uint32_t)fCurPage << 8);
    uint32_t boundary = RingClamp((uint32_t)fBoundary << 8);
    uint32_t avail;

    /* The guest reads up to the boundary page, so what is free is whatever
       lies between where the next packet would go and there. */
    if (index < boundary) {
        avail = boundary - index;
    } else {
        avail = (fStop - fStart) - (index - boundary);
    }
    return avail < NE2000_RING_FRAME_SIZE;
}


/* Which multicast hash bit an address falls in: the top six bits of the
   Ethernet checksum of the address, which is what the filter registers are
   indexed by. */
static int multicast_index(const uint8_t *addr)
{
    uint32_t crc = 0xffffffff;

    for (int i = 0; i < 6; i++) {
        uint8_t byte = addr[i];
        for (int bit = 0; bit < 8; bit++, byte >>= 1) {
            bool carry = get_bit(crc, 31) != ((byte & 1) != 0);
            crc <<= 1;
            if (carry) {
                crc ^= 0x04c11db7;
            }
        }
    }
    return crc >> 26;
}


bool NE2000Device::AddressMatches(const uint8_t *buf) const
{
    static const uint8_t broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};

    if ((fRcr & NE_RCR_PRO) != 0) {
        return true;
    }
    if (memcmp(buf, broadcast, sizeof(broadcast)) == 0) {
        return (fRcr & NE_RCR_AB) != 0;
    }
    if ((buf[0] & 1) != 0) {
        if ((fRcr & NE_RCR_AM) == 0) {
            return false;
        }
        int index = multicast_index(buf);
        return get_bit(fMar[index >> 3], index & 7);
    }
    return memcmp(buf, fPar, sizeof(fPar)) == 0;
}


//#pragma mark - EthernetTarget

bool NE2000Device::CanWritePacket()
{
    return (fCmd & NE_CR_STOP) == 0 && (fRcr & NE_RCR_MON) == 0 &&
        !BufferFull();
}


void NE2000Device::WritePacket(const uint8_t *buf, int len)
{
    uint8_t frame[NE2000_MIN_FRAME];

    /* Anything shorter than a destination address is not a frame at all, and
       the filter would read past it. */
    if (!CanWritePacket() || len < 6 || len > NE2000_MAX_FRAME ||
        !AddressMatches(buf)) {
        return;
    }

    /* The wire pads a short frame out; nothing on the way here does, so it is
       padded before it is stored. */
    if (len < NE2000_MIN_FRAME) {
        memcpy(frame, buf, len);
        memset(frame + len, 0, NE2000_MIN_FRAME - len);
        buf = frame;
        len = NE2000_MIN_FRAME;
    }

    uint32_t index = RingClamp((uint32_t)fCurPage << 8);
    /* The header counts itself; the next packet starts a whole number of
       pages further on, with room left for the checksum the card would have
       stored after the frame. */
    uint32_t total_len = len + 4;
    uint32_t next = index + ((total_len + 4 + NE2000_PAGE_SIZE - 1) &
                             ~(uint32_t)(NE2000_PAGE_SIZE - 1));
    while (next >= fStop) {
        next -= fStop - fStart;
    }

    fRsr = NE_RSR_RXOK;
    if ((buf[0] & 1) != 0) {
        fRsr |= NE_RSR_PHY;
    }
    fMem[index + 0] = fRsr;
    fMem[index + 1] = next >> 8;
    fMem[index + 2] = total_len;
    fMem[index + 3] = total_len >> 8;
    index += 4;

    while (len > 0) {
        uint32_t chunk = fStop - index;
        if (chunk > (uint32_t)len) {
            chunk = len;
        }
        memcpy(fMem + index, buf, chunk);
        buf += chunk;
        len -= chunk;
        index += chunk;
        if (index >= fStop) {
            index = fStart;
        }
    }

    fCurPage = next >> 8;
    fIsr |= NE_ISR_RX;
    UpdateIrq();
}


void NE2000Device::SetCarrier(bool carrier_state)
{
    /* The part has no link status register a driver reads, and no interrupt
       to announce a change in one. */
    (void)carrier_state;
}


//#pragma mark - transmit

void NE2000Device::Transmit()
{
    uint32_t index = (uint32_t)fTpsr << 8;

    /* Only the packet buffer is memory; a transmit from anywhere else sends
       nothing, and still completes so that a driver is not left waiting. */
    if (fTcnt > 0 && index >= NE2000_PMEM_START &&
        index + fTcnt <= NE2000_MEM_SIZE) {
        fNet->WritePacket(fMem + index, fTcnt);
    }
    fTsr = NE_TSR_PTX;
    fIsr |= NE_ISR_TX;
    fCmd &= ~NE_CR_TXP;
    UpdateIrq();
}


//#pragma mark - the card's memory

/* Whether an access of 'len' bytes at 'addr' reaches something. The remote
   DMA address is sixteen bits and the card is smaller than that, so the
   bound matters. */
static bool mem_decodes(uint32_t addr, int len)
{
    if (addr + len > NE2000_MEM_SIZE) {
        return false;
    }
    return addr + len <= NE2000_PROM_SIZE || addr >= NE2000_PMEM_START;
}


uint32_t NE2000Device::MemRead(uint32_t addr, int len) const
{
    addr &= ~(uint32_t)(len - 1);
    if (!mem_decodes(addr, len)) {
        return bit_mask(len * 8);
    }

    uint32_t val = 0;
    for (int i = 0; i < len; i++) {
        val |= (uint32_t)fMem[addr + i] << (i * 8);
    }
    return val;
}


void NE2000Device::MemWrite(uint32_t addr, uint32_t val, int len)
{
    addr &= ~(uint32_t)(len - 1);
    if (!mem_decodes(addr, len)) {
        return;
    }

    for (int i = 0; i < len; i++) {
        fMem[addr + i] = val >> (i * 8);
    }
}


/* How wide one access of the data port moves. A driver that reaches the port
   with a 32 bit access gets the transfer the RTL8029AS adds; otherwise the
   width is the one the data configuration register asked for, whatever the
   access itself was. */
int NE2000Device::DmaWidth(int size_log2) const
{
    if (size_log2 >= 2) {
        return 4;
    }
    return (fDcr & NE_DCR_WTS) != 0 ? 2 : 1;
}


void NE2000Device::DmaUpdate(int len)
{
    fRsar += len;
    /* The transfer wraps at the top of the receive ring, which is how a
       driver reads a packet that ran over the end of it in one go. */
    if (fRsar == fStop) {
        fRsar = fStart;
    }

    if (fRcnt <= len) {
        fRcnt = 0;
        fIsr |= NE_ISR_RDC;
        UpdateIrq();
    } else {
        fRcnt -= len;
    }
}


//#pragma mark - registers

uint32_t NE2000Device::RegRead(uint32_t offset)
{
    if (offset == NE_CR) {
        return fCmd;
    }

    uint32_t reg = offset | (get_bits(fCmd, NE_CR_PS_SHIFT, 2) << 4);

    if (reg >= EN1_PHYS && reg < EN1_PHYS + 6) {
        return fPar[reg - EN1_PHYS];
    }
    if (reg >= EN1_MULT && reg < EN1_MULT + 8) {
        return fMar[reg - EN1_MULT];
    }

    switch (reg) {
    case EN0_CLDALO: /* nothing local is ever in flight to report */
    case EN0_CLDAHI:
        return 0;
    case EN0_BOUNDARY:
        return fBoundary;
    case EN0_TSR:
        return fTsr;
    case EN0_NCR: /* no collision is ever reported */
    case EN0_FIFO:
        return 0;
    case EN0_ISR:
        return fIsr;
    /* Where the remote DMA has got to, which is the address it will move
       next. */
    case EN0_CRDALO:
        return fRsar & 0xff;
    case EN0_CRDAHI:
        return fRsar >> 8;
    /* Only the PCI part is an RTL8029AS; on the ports a plain DP8390 has
       nothing here. */
    case EN0_RTL8029ID0:
        return fPciDev != nullptr ? NE_RTL8029_ID0 : 0;
    case EN0_RTL8029ID1:
        return fPciDev != nullptr ? NE_RTL8029_ID1 : 0;
    case EN0_RSR:
        return fRsr;
    /* The error counters, which nothing here ever counts up. */
    case EN0_COUNTER0:
    case EN0_COUNTER1:
    case EN0_COUNTER2:
        return 0;
    case EN1_CURPAG:
        return fCurPage;
    case EN2_STARTPG:
        return fStart >> 8;
    case EN2_STOPPG:
        return fStop >> 8;
    case EN3_CONFIG0:
        return fPciDev != nullptr ? NE_RTL8029_CONFIG0 : 0;
    case EN3_CONFIG2:
        return fPciDev != nullptr ? NE_RTL8029_CONFIG2 : 0;
    case EN3_CONFIG3:
        return fPciDev != nullptr ? NE_RTL8029_CONFIG3 : 0;
    default:
        return 0;
    }
}


void NE2000Device::RegWrite(uint32_t offset, uint32_t val)
{
    if (offset == NE_CR) {
        fCmd = val;
        if ((fCmd & NE_CR_STOP) != 0) {
            return;
        }
        fIsr &= ~NE_ISR_RESET;
        /* A remote DMA of nothing has already finished. */
        if ((fCmd & (NE_CR_RD_READ | NE_CR_RD_WRITE)) != 0 && fRcnt == 0) {
            fIsr |= NE_ISR_RDC;
            UpdateIrq();
        }
        if ((fCmd & NE_CR_TXP) != 0) {
            Transmit();
        }
        /* A card that has just been started can take what the back end is
           holding. */
        fNet->TargetReady();
        return;
    }

    uint32_t reg = offset | (get_bits(fCmd, NE_CR_PS_SHIFT, 2) << 4);

    if (reg >= EN1_PHYS && reg < EN1_PHYS + 6) {
        fPar[reg - EN1_PHYS] = val;
        return;
    }
    if (reg >= EN1_MULT && reg < EN1_MULT + 8) {
        fMar[reg - EN1_MULT] = val;
        return;
    }

    switch (reg) {
    case EN0_STARTPG:
        fStart = val << 8;
        break;
    case EN0_STOPPG:
        fStop = val << 8;
        break;
    case EN0_BOUNDARY:
        fBoundary = val;
        /* The guest has read a packet, so the ring has room again. */
        fNet->TargetReady();
        break;
    case EN0_TPSR:
        fTpsr = val;
        break;
    case EN0_TCNTLO:
        fTcnt = set_bits(fTcnt, 0, 8, val);
        break;
    case EN0_TCNTHI:
        fTcnt = set_bits(fTcnt, 8, 8, val);
        break;
    case EN0_ISR:
        /* Write one to clear, except for the reset bit, which a driver
           cannot clear this way. */
        fIsr &= ~(val & ~NE_ISR_RESET);
        UpdateIrq();
        break;
    case EN0_RSARLO:
        fRsar = set_bits(fRsar, 0, 8, val);
        break;
    case EN0_RSARHI:
        fRsar = set_bits(fRsar, 8, 8, val);
        break;
    case EN0_RCNTLO:
        fRcnt = set_bits(fRcnt, 0, 8, val);
        break;
    case EN0_RCNTHI:
        fRcnt = set_bits(fRcnt, 8, 8, val);
        break;
    case EN0_RXCR:
        fRcr = val;
        fNet->TargetReady();
        break;
    case EN0_TXCR:
        fTcr = val;
        break;
    case EN0_DCFG:
        fDcr = val;
        break;
    case EN0_IMR:
        fImr = val;
        UpdateIrq();
        break;
    case EN1_CURPAG:
        fCurPage = val;
        break;
    }
}


//#pragma mark - the port window

uint32_t NE2000Device::Read(uint32_t offset, int size_log2)
{
    if (offset < NE2000_DATA_PORT) {
        /* The paged registers are bytes, and a wider access decodes
           nothing. */
        if (size_log2 != 0) {
            return bit_mask(8 << size_log2);
        }
        return RegRead(offset);
    }
    if (offset < NE2000_RESET_PORT) {
        int len = DmaWidth(size_log2);
        uint32_t val = MemRead(fRsar, len);
        DmaUpdate(len);
        return val;
    }
    if (offset < NE2000_PORT_SIZE) {
        /* Reading the reset port is one of the two ways a driver resets the
           card; what it reads back is not a value. */
        Reset();
        return 0;
    }
    /* Only the PCI part has a window wider than the card decodes. */
    return bit_mask(8 << size_log2);
}


void NE2000Device::Write(uint32_t offset, uint32_t val, int size_log2)
{
    if (offset < NE2000_DATA_PORT) {
        if (size_log2 == 0) {
            RegWrite(offset, val);
        }
        return;
    }
    if (offset < NE2000_RESET_PORT) {
        /* Without a byte count there is nowhere for the data to go. */
        if (fRcnt == 0) {
            return;
        }
        int len = DmaWidth(size_log2);
        MemWrite(fRsar, val, len);
        DmaUpdate(len);
        return;
    }
    if (offset < NE2000_PORT_SIZE) {
        Reset();
    }
}


//#pragma mark - PCIBarTarget

void NE2000Device::SetBar(int bar_num, uint64_t addr, bool enabled)
{
    if (bar_num == 0 && fIoRange != nullptr) {
        fIoRange->SetAddr(addr, enabled);
    }
}
