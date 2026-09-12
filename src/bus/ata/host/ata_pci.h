/*
 * PCI IDE controller with bus master DMA
 *
 * Copyright (c) 2003-2016 Fabrice Bellard
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

#include "ata.h"
#include "pci.h"

/* Two channels, each a master and a slave, so four drives. */
#define ATA_PCI_CHANNELS 2
#define ATA_PCI_MAX_DEVICES (ATA_PCI_CHANNELS * ATA_MAX_DEVICES)

/* The addresses a controller in compatibility mode decodes, which are the
   ones every PC has had since the AT. */
#define ATA_LEGACY_CMD0  0x1f0
#define ATA_LEGACY_CTRL0 0x3f6
#define ATA_LEGACY_IRQ0  14
#define ATA_LEGACY_CMD1  0x170
#define ATA_LEGACY_CTRL1 0x376
#define ATA_LEGACY_IRQ1  15

/* Base address register windows. The command block is the eight task file
   registers; the control block is four ports with the device control
   register at offset two, which is what puts the alternate status at the
   0x3f6 a compatibility mode controller answers. */
#define ATA_PCI_CMD_SIZE   8
#define ATA_PCI_CTRL_SIZE  4
#define ATA_PCI_CTRL_OFFSET 2
/* Eight bus master registers per channel. */
#define ATA_PCI_BM_SIZE    (8 * ATA_PCI_CHANNELS)

/* Bus master registers, relative to a channel's eight byte block. */
#define ATA_BM_COMMAND 0x00
#define ATA_BM_STATUS  0x02
#define ATA_BM_PRD     0x04

#define ATA_BM_CMD_START 0x01
/* Direction. Set means the engine writes to memory, which is a disk read;
   Linux spells the same bit ATA_DMA_WR and sets it for exactly that. */
#define ATA_BM_CMD_WRITE 0x08

#define ATA_BM_STAT_ACTIVE  0x01
#define ATA_BM_STAT_ERROR   0x02
#define ATA_BM_STAT_IRQ     0x04
#define ATA_BM_STAT_DMA0    0x20 /* master is DMA capable */
#define ATA_BM_STAT_DMA1    0x40 /* slave is */
#define ATA_BM_STAT_SIMPLEX 0x80

/* One scatter list entry: a 32 bit address, a byte count where zero means
   64 KB, and a flag word whose top bit ends the list. */
#define ATA_PRD_ENTRY_SIZE 8
#define ATA_PRD_EOT 0x8000

/* What a PIIX3 reports. A guest binds a driver by these, and they are the
   most widely recognised PCI IDE identifiers there are. */
#define ATA_PCI_VENDOR_ID 0x8086
#define ATA_PCI_DEVICE_ID 0x7010

/* Programming interface byte. Compatibility mode puts both channels on the
   legacy addresses and interrupts; native mode puts them on base address
   registers and INTx. The top bit says the bus master is there either
   way. */
#define ATA_PCI_PROGIF_LEGACY 0x80
#define ATA_PCI_PROGIF_NATIVE 0x8f


class ATAPCIController;

/* One window of registers. A controller has up to five of them and they all
   behave differently, so each carries which channel and which block it is
   rather than there being an adapter type per window. */
class ATAPCIWindow final: public DeviceIO {
public:
    enum KindEnum {
        kCommand,
        kControl,
        kBusMaster,
    };

    ATAPCIController *owner = nullptr;
    int channel = 0;
    KindEnum kind = kCommand;
    PhysMemoryRange *range = nullptr;

    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;
};


/* A PCI IDE controller: two ATA channels and the bus master that moves their
   data.

   It runs in one of two modes, chosen from the machine it lands in. On a
   machine that addresses its devices by port number -- a PC -- it is the
   southbridge's IDE function in compatibility mode: the channels answer the
   fixed AT addresses and drive the fixed interrupt lines, and only the bus
   master block is placed by a base address register. Anywhere else -- a
   device tree machine reaching PCI through a host bridge with an I/O
   aperture, for one -- it runs in native mode, where all five windows are
   base address registers and the interrupt goes out over INTx. Nothing below
   the registers differs between the two. */
class ATAPCIController final: public Device, public ATAChannelTarget,
                              public ATABusTarget, public PCIBarTarget {
private:
    /* Each channel is its own bus target so that a drive lands where it was
       declared rather than where it happened to be added. */
    struct ChannelState {
        ATAPCIController *owner = nullptr;
        ATAChannel ata;
        bool irq_level = false;

        /* bus master */
        uint8_t bm_command = 0;
        uint8_t bm_status = 0;
        uint32_t bm_prd = 0;

        ATAPCIWindow cmd_io;
        ATAPCIWindow ctrl_io;
    };

    ChannelState fChannels[ATA_PCI_CHANNELS];
    ATAPCIWindow fBmIo;

    Bus *fChildBus = nullptr;
    PCIDevice *fPciDev = nullptr;
    PhysMemoryMap *fPortMap = nullptr;

    /* Compatibility mode: the fixed ports and lines, and no base address
       registers for the channels. Unused in native mode. */
    bool fLegacy = false;
    Resource *fLegacyCmdRes[ATA_PCI_CHANNELS] {};
    Resource *fLegacyCtrlRes[ATA_PCI_CHANNELS] {};
    Resource *fLegacyIrqRes[ATA_PCI_CHANNELS] {};
    IRQSignal *fLegacyIrq[ATA_PCI_CHANNELS] {};
    IRQSignal *fPciIrq = nullptr;

    bool RegisterPCI(PCIBus *pci_bus, int devfn);
    void UpdateIrq();

    uint8_t *GuestPtr(uint64_t addr, bool is_rw);
    bool ReadPrd(uint64_t addr, uint8_t *buf);
    uint32_t MoveData(ATADevice *dev, uint64_t addr, uint32_t len,
                      bool to_memory);
    void BusMasterRun(int index);

public:
    ATAPCIController(const char *name);
    ~ATAPCIController() override;

    bool Prepare() override;
    bool Realize() override;
    Bus *ChildBus() override {return fChildBus;}

    /* ATAChannelTarget */
    void ATASetIrq(int channel, bool level) override;
    void ATADmaRequested(int channel) override;

    /* ATABusTarget: every drive declared fills the next free place -- the
       master and the slave of the first channel, then of the second. */
    bool AttachDevice(ATADevice *dev) override;

    /* PCIBarTarget */
    void SetBar(int bar_num, uint64_t addr, bool enabled) override;

    uint32_t WindowRead(int channel, ATAPCIWindow::KindEnum kind,
                        uint32_t offset, int size_log2);
    void WindowWrite(int channel, ATAPCIWindow::KindEnum kind,
                     uint32_t offset, uint32_t val, int size_log2);
};


/* The "pci-ide" configuration node. The drives nested inside fill the
   channels in the order they appear: the first two are the master and slave
   of the first channel, the next two of the second. */
Device *ata_pci_node_create(const char *name);
