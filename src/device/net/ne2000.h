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
#pragma once

#include "devices.h"
#include "pci.h"

/* The card decodes thirty two ports: the DP8390's sixteen paged registers,
   then the window the remote DMA moves data through, then the reset port. */
#define NE2000_PORT_SIZE  0x20
#define NE2000_DATA_PORT  0x10
#define NE2000_RESET_PORT 0x18

/* The PCI part asks for a whole 256 byte block, because that is the smallest
   an I/O base address register is given; only the first thirty two of them
   decode. */
#define NE2000_PCI_BAR_SIZE 0x100

/* Where an NE2000 has conventionally been jumpered on a PC. Nothing in the
   machine finds the card for the guest, so a configuration that moves it has
   to tell the guest's driver as well. */
#define NE2000_PC_PORT 0x300
#define NE2000_PC_IRQ  9

/* The card's own memory, in the address space the remote DMA reaches: the
   address PROM at the bottom, a hole, then the packet buffer an NE2000 puts
   at 16 KB. Nothing else answers. */
#define NE2000_PROM_SIZE  32
#define NE2000_PMEM_START (16 * 1024)
#define NE2000_PMEM_SIZE  (32 * 1024)
#define NE2000_MEM_SIZE   (NE2000_PMEM_START + NE2000_PMEM_SIZE)

/* The receive buffer is a ring of these, and a packet always starts on one. */
#define NE2000_PAGE_SIZE 256

/* What arrives from the back end, and what the wire would have padded a
   runt to. */
#define NE2000_MAX_FRAME 1514
#define NE2000_MIN_FRAME 60

/* The most one frame occupies in the receive ring: its four byte header, the
   frame, the four byte checksum the card would have stored, rounded up to
   whole pages. A ring with less than this free is one no packet fits in. */
#define NE2000_RING_FRAME_SIZE \
    (((NE2000_MAX_FRAME + 8 + NE2000_PAGE_SIZE - 1) / NE2000_PAGE_SIZE) * \
     NE2000_PAGE_SIZE)


/* Command register, which is at offset zero of every page. */
#define NE_CR 0x00
#define  NE_CR_STOP     bit_at(0)
#define  NE_CR_START    bit_at(1)
#define  NE_CR_TXP      bit_at(2) /* transmit the buffer at TPSR */
#define  NE_CR_RD_SHIFT 3         /* three bits: what the remote DMA is doing */
#define  NE_CR_RD_READ  bit_at(3)
#define  NE_CR_RD_WRITE bit_at(4)
#define  NE_CR_RD_ABORT (4 << NE_CR_RD_SHIFT)
#define  NE_CR_PS_SHIFT 6         /* two bits: which register page is visible */

/* The registers behind the command register, named by the page they live on
   and their offset in it. Reading and writing one address usually reaches two
   different registers, so the two directions are listed apart. */
#define EN0_CLDALO     0x01 /* r: current local DMA address, low */
#define EN0_STARTPG    0x01 /* w: first page of the receive ring */
#define EN0_CLDAHI     0x02
#define EN0_STOPPG     0x02 /* w: last page of the receive ring, plus one */
#define EN0_BOUNDARY   0x03 /* rw: the page the guest has read up to */
#define EN0_TSR        0x04 /* r: transmit status */
#define EN0_TPSR       0x04 /* w: first page of the transmit buffer */
#define EN0_NCR        0x05 /* r: collisions */
#define EN0_TCNTLO     0x05 /* w: bytes to transmit */
#define EN0_FIFO       0x06
#define EN0_TCNTHI     0x06
#define EN0_ISR        0x07 /* rw: interrupt status, write one to clear */
#define EN0_CRDALO     0x08 /* r: where the remote DMA has got to */
#define EN0_RSARLO     0x08 /* w: remote DMA start address */
#define EN0_CRDAHI     0x09
#define EN0_RSARHI     0x09
#define EN0_RTL8029ID0 0x0a /* r: what marks the PCI part as an RTL8029AS */
#define EN0_RCNTLO     0x0a /* w: bytes the remote DMA is to move */
#define EN0_RTL8029ID1 0x0b
#define EN0_RCNTHI     0x0b
#define EN0_RSR        0x0c /* r: receive status of the last packet */
#define EN0_RXCR       0x0c /* w: receive configuration */
#define EN0_COUNTER0   0x0d /* r: the three error counters */
#define EN0_TXCR       0x0d /* w: transmit configuration */
#define EN0_COUNTER1   0x0e
#define EN0_DCFG       0x0e /* w: data configuration */
#define EN0_COUNTER2   0x0f
#define EN0_IMR        0x0f /* w: interrupt mask */

#define EN1_PHYS       0x11 /* rw: the station address, six bytes */
#define EN1_CURPAG     0x17 /* rw: the page the next packet is stored at */
#define EN1_MULT       0x18 /* rw: the multicast hash filter, eight bytes */

#define EN2_STARTPG    0x21 /* r: PSTART and PSTOP read back */
#define EN2_STOPPG     0x22

#define EN3_CONFIG0    0x33 /* r: what media the RTL8029AS reports */
#define EN3_CONFIG2    0x35
#define EN3_CONFIG3    0x36

/* Interrupt status, and the mask that has the same bits. */
#define NE_ISR_RX       bit_at(0) /* a packet was received */
#define NE_ISR_TX       bit_at(1) /* a packet was transmitted */
#define NE_ISR_RX_ERR   bit_at(2)
#define NE_ISR_TX_ERR   bit_at(3)
#define NE_ISR_OVER     bit_at(4)
#define NE_ISR_COUNTERS bit_at(5)
#define NE_ISR_RDC      bit_at(6) /* the remote DMA finished */
#define NE_ISR_RESET    bit_at(7) /* read only, and not maskable */

/* Receive configuration. */
#define NE_RCR_SEP bit_at(0) /* keep packets with a bad checksum */
#define NE_RCR_AR  bit_at(1) /* keep runts */
#define NE_RCR_AB  bit_at(2) /* accept broadcast */
#define NE_RCR_AM  bit_at(3) /* accept the multicast the filter passes */
#define NE_RCR_PRO bit_at(4) /* promiscuous */
#define NE_RCR_MON bit_at(5) /* monitor only: nothing is buffered */

/* Data configuration: the only bit that changes anything here is the width
   the remote DMA moves. */
#define NE_DCR_WTS bit_at(0)

#define NE_TSR_PTX  bit_at(0) /* transmitted without error */
#define NE_RSR_RXOK bit_at(0) /* received without error */
#define NE_RSR_PHY  bit_at(5) /* the address it matched was a group one */


/* An NE2000 card: on PCI as an RTL8029AS, or on the ports a PC's ISA bus has
 * always carried one on. Both are the same DP8390 with the same buffer memory
 * behind it, and only how the guest finds the thirty two ports and the
 * interrupt differs.
 *
 * The card has no bus master of its own: everything the guest sends or
 * receives passes through the one data port, which it points at the card's
 * memory with a start address and a byte count. Frames are moved
 * synchronously -- a transmit command hands the buffer straight to the back
 * end, and a frame arriving from the back end is placed straight into the
 * receive ring.
 */
class NE2000Device final: public Device, public EthernetTarget,
                          public PCIBarTarget {
private:
    DeviceContext *fCtx;
    std::unique_ptr<HostEthernet> fNet;

    /* The port and line the configuration asked for, or -1 for the ones an
       NE2000 is conventionally jumpered to. Unused on PCI. */
    int fPort;
    int fIrqLine;

    Resource *fIoRes = nullptr;
    Resource *fIrqRes = nullptr;

    PCIDevice *fPciDev = nullptr;
    PhysMemoryRange *fIoRange = nullptr;
    IRQSignal *fIrq = nullptr;
    bool fIrqLevel = false;

    uint8_t fCmd = NE_CR_STOP;
    uint8_t fIsr = 0;
    uint8_t fImr = 0;
    uint8_t fDcr = 0;
    uint8_t fRcr = 0;
    uint8_t fTcr = 0;
    uint8_t fTsr = 0;
    uint8_t fRsr = 0;

    /* The transmit buffer, as a page number and a byte count. */
    uint8_t fTpsr = 0;
    uint16_t fTcnt = 0;

    /* The remote DMA: where it is and how much is left to move. */
    uint16_t fRsar = 0;
    uint16_t fRcnt = 0;

    /* The receive ring. The guest programs page numbers; the two ends are
       kept as byte addresses because everything below uses them that way. */
    uint32_t fStart = 0;
    uint32_t fStop = 0;
    uint8_t fBoundary = 0; /* the page the guest has read up to */
    uint8_t fCurPage = 0;  /* the page the next packet goes at */

    uint8_t fPar[6] {}; /* station address */
    uint8_t fMar[8] {}; /* multicast hash filter */

    uint8_t fMem[NE2000_MEM_SIZE] {};

    void Reset();
    void LoadProm();
    void UpdateIrq();

    /* The ring as the guest has left it: a packet is only stored while it
       describes real packet memory. */
    bool RingValid() const;
    uint32_t RingClamp(uint32_t addr) const;
    bool BufferFull() const;
    bool AddressMatches(const uint8_t *buf) const;

    void Transmit();

    /* The card's memory as the remote DMA sees it. An access outside what
       the card answers reads as all ones and is dropped on a write. */
    uint32_t MemRead(uint32_t addr, int len) const;
    void MemWrite(uint32_t addr, uint32_t val, int len);
    void DmaUpdate(int len);
    int DmaWidth(int size_log2) const;

    uint32_t RegRead(uint32_t offset);
    void RegWrite(uint32_t offset, uint32_t val);

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t val, int size_log2);

public:
    NE2000Device(DeviceContext *ctx, std::unique_ptr<HostEthernet> net,
                 const char *name, int port, int irq);
    ~NE2000Device() override;

    bool Prepare() override;
    bool Realize() override;

    /* EthernetTarget */
    bool CanWritePacket() override;
    void WritePacket(const uint8_t *buf, int len) override;
    void SetCarrier(bool carrier_state) override;

    /* PCIBarTarget */
    void SetBar(int bar_num, uint64_t addr, bool enabled) override;

    DeviceIOAdapter<NE2000Device, &NE2000Device::Read,
                    &NE2000Device::Write> fIo {*this};
};
