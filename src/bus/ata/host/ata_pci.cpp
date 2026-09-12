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
#include "ata_pci.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cutils.h"
#include "machine.h"

//#define DEBUG_ATA_DMA


//#pragma mark - ATAPCIWindow

uint32_t ATAPCIWindow::DeviceRead(uint32_t offset, int size_log2)
{
    return owner->WindowRead(channel, kind, offset, size_log2);
}


void ATAPCIWindow::DeviceWrite(uint32_t offset, uint32_t val, int size_log2)
{
    owner->WindowWrite(channel, kind, offset, val, size_log2);
}


//#pragma mark - construction

ATAPCIController::ATAPCIController(const char *name):
    Device(name)
{
    for (int i = 0; i < ATA_PCI_CHANNELS; i++) {
        ChannelState &c = fChannels[i];
        c.owner = this;
        c.ata.Init(this, i);
        /* Both drives are reported as capable of DMA; a drive that is not
           there simply never starts a transfer. */
        c.bm_status = ATA_BM_STAT_DMA0 | ATA_BM_STAT_DMA1;

        c.cmd_io.owner = this;
        c.cmd_io.channel = i;
        c.cmd_io.kind = ATAPCIWindow::kCommand;
        c.ctrl_io.owner = this;
        c.ctrl_io.channel = i;
        c.ctrl_io.kind = ATAPCIWindow::kControl;
    }
    fBmIo.owner = this;
    fBmIo.channel = 0;
    fBmIo.kind = ATAPCIWindow::kBusMaster;
}


ATAPCIController::~ATAPCIController()
{
    delete fChildBus;
}


bool ATAPCIController::AttachDevice(ATADevice *dev)
{
    for (int i = 0; i < ATA_PCI_CHANNELS; i++) {
        for (int unit = 0; unit < ATA_MAX_DEVICES; unit++) {
            if (fChannels[i].ata.DeviceAt(unit) == nullptr) {
                return fChannels[i].ata.AttachDevice(dev, unit);
            }
        }
    }
    vm_error("%s: no free place for another drive (%d channels of %d)\n",
             Name(), ATA_PCI_CHANNELS, ATA_MAX_DEVICES);
    return false;
}


bool ATAPCIController::RegisterPCI(PCIBus *pci_bus, int devfn)
{
    fPciDev = pci_register_device(pci_bus, Name(), devfn, ATA_PCI_VENDOR_ID,
                                  ATA_PCI_DEVICE_ID, 0x00, 0x0101);
    if (fPciDev == nullptr) {
        vm_error("%s: could not register the PCI function\n", Name());
        return false;
    }
    pci_device_set_config8(fPciDev, PCI_CLASS_PROG,
                           fLegacy ? ATA_PCI_PROGIF_LEGACY
                                   : ATA_PCI_PROGIF_NATIVE);
    pci_device_set_config8(fPciDev, PCI_INTERRUPT_PIN, 1);

    fPortMap = pci_device_get_port_map(fPciDev);
    if (fPortMap == nullptr) {
        vm_error("%s: this PCI bus has no I/O port space, which is where an "
                 "ATA controller's registers live\n", Name());
        return false;
    }

    if (fLegacy) {
        /* Compatibility mode: the channels are where they have always been,
           and the base address registers for them read as zero. */
        for (int i = 0; i < ATA_PCI_CHANNELS; i++) {
            fPortMap->RegisterDevice(fLegacyCmdRes[i]->base,
                                     fLegacyCmdRes[i]->size,
                                     &fChannels[i].cmd_io,
                                     DEVIO_SIZE8 | DEVIO_SIZE16 |
                                     DEVIO_SIZE32);
            /* Only the one register of the control block is decoded, which
               is all a compatibility mode controller answers. */
            fPortMap->RegisterDevice(fLegacyCtrlRes[i]->base,
                                     fLegacyCtrlRes[i]->size,
                                     &fChannels[i].ctrl_io, DEVIO_SIZE8);
        }
    } else {
        for (int i = 0; i < ATA_PCI_CHANNELS; i++) {
            fChannels[i].cmd_io.range =
                fPortMap->RegisterDevice(0, ATA_PCI_CMD_SIZE,
                                         &fChannels[i].cmd_io,
                                         DEVIO_SIZE8 | DEVIO_SIZE16 |
                                         DEVIO_SIZE32 | DEVIO_DISABLED);
            fChannels[i].ctrl_io.range =
                fPortMap->RegisterDevice(0, ATA_PCI_CTRL_SIZE,
                                         &fChannels[i].ctrl_io,
                                         DEVIO_SIZE8 | DEVIO_DISABLED);
            pci_register_bar(fPciDev, i * 2, ATA_PCI_CMD_SIZE,
                             PCI_ADDRESS_SPACE_IO, this);
            pci_register_bar(fPciDev, i * 2 + 1, ATA_PCI_CTRL_SIZE,
                             PCI_ADDRESS_SPACE_IO, this);
        }
    }

    fBmIo.range = fPortMap->RegisterDevice(0, ATA_PCI_BM_SIZE, &fBmIo,
                                           DEVIO_SIZE8 | DEVIO_SIZE16 |
                                           DEVIO_SIZE32 | DEVIO_DISABLED);
    pci_register_bar(fPciDev, 4, ATA_PCI_BM_SIZE, PCI_ADDRESS_SPACE_IO, this);
    return true;
}


void ATAPCIController::SetBar(int bar_num, uint64_t addr, bool enabled)
{
    if (bar_num == 4) {
        if (fBmIo.range != nullptr) {
            fBmIo.range->SetAddr(addr, enabled);
        }
        return;
    }
    if (fLegacy || bar_num < 0 || bar_num >= ATA_PCI_CHANNELS * 2) {
        return;
    }
    ChannelState &c = fChannels[bar_num / 2];
    PhysMemoryRange *range = (bar_num & 1) == 0 ? c.cmd_io.range
                                                : c.ctrl_io.range;
    if (range != nullptr) {
        range->SetAddr(addr, enabled);
    }
}


//#pragma mark - the configuration node

bool ATAPCIController::Prepare()
{
    if (ParentBus() == nullptr || ParentBus()->AsPCIBus() == nullptr) {
        vm_error("%s: must be attached to a PCI bus\n", Name());
        return false;
    }

    /* A machine that addresses its devices by port number is a PC, whose
       firmware and guests expect this function on the addresses the AT put
       it on. Anywhere else there is nothing legacy to be compatible with, so
       the base address registers place everything. */
    SystemBus *sys = dynamic_cast<SystemBus *>(ParentBus()->Root());
    fLegacy = sys != nullptr && sys->IsPortBased();

    if (fLegacy) {
        static const uint32_t cmd_base[ATA_PCI_CHANNELS] = {
            ATA_LEGACY_CMD0, ATA_LEGACY_CMD1,
        };
        static const uint32_t ctrl_base[ATA_PCI_CHANNELS] = {
            ATA_LEGACY_CTRL0, ATA_LEGACY_CTRL1,
        };
        static const uint32_t irq_line[ATA_PCI_CHANNELS] = {
            ATA_LEGACY_IRQ0, ATA_LEGACY_IRQ1,
        };
        for (int i = 0; i < ATA_PCI_CHANNELS; i++) {
            fLegacyCmdRes[i] = AddFixedResource(RES_IO, cmd_base[i],
                                                ATA_PCI_CMD_SIZE);
            fLegacyCtrlRes[i] = AddFixedResource(RES_IO, ctrl_base[i], 1);
            fLegacyIrqRes[i] = AddFixedResource(RES_IRQ, irq_line[i], 1);
            if (fLegacyCmdRes[i] == nullptr || fLegacyCtrlRes[i] == nullptr ||
                fLegacyIrqRes[i] == nullptr) {
                return false;
            }
        }
    }

    fChildBus = new ATABus(this, this);
    return true;
}


bool ATAPCIController::Realize()
{
    PCIBus *pci_bus = ParentBus()->AsPCIBus();

    if (!RegisterPCI(pci_bus, -1)) {
        return false;
    }
    if (fLegacy) {
        SystemBus *sys = static_cast<SystemBus *>(ParentBus()->Root());
        for (int i = 0; i < ATA_PCI_CHANNELS; i++) {
            fLegacyIrq[i] = sys->IrqSignalFor(fLegacyIrqRes[i]->base);
            if (fLegacyIrq[i] == nullptr) {
                vm_error("%s: bad interrupt line %d\n", Name(),
                         (int)fLegacyIrqRes[i]->base);
                return false;
            }
        }
    } else {
        fPciIrq = pci_device_get_irq(fPciDev, 0);
    }
    return true;
}


//#pragma mark - interrupts

void ATAPCIController::ATASetIrq(int channel, bool level)
{
    fChannels[channel].irq_level = level;
    if (level) {
        /* The bus master's interrupt bit latches whatever the channel
           raised, because a driver reads it to tell a finished transfer
           from a spurious line. */
        fChannels[channel].bm_status |= ATA_BM_STAT_IRQ;
    }
    UpdateIrq();
}


void ATAPCIController::ATADmaRequested(int channel)
{
    /* A guest that armed the engine before issuing the command transfers
       here rather than from the register write. */
    if ((fChannels[channel].bm_command & ATA_BM_CMD_START) != 0) {
        BusMasterRun(channel);
    }
}


void ATAPCIController::UpdateIrq()
{
    if (fLegacy) {
        for (int i = 0; i < ATA_PCI_CHANNELS; i++) {
            if (fLegacyIrq[i] != nullptr) {
                fLegacyIrq[i]->Set(fChannels[i].irq_level);
            }
        }
        return;
    }
    if (fPciIrq == nullptr) {
        return;
    }
    /* In native mode the two channels share the one pin. */
    bool level = false;
    for (int i = 0; i < ATA_PCI_CHANNELS; i++) {
        level = level || fChannels[i].irq_level;
    }
    fPciIrq->Set(level);
}


//#pragma mark - guest memory

uint8_t *ATAPCIController::GuestPtr(uint64_t addr, bool is_rw)
{
    return pci_device_get_dma_ptr(fPciDev, addr, is_rw);
}


/* One scatter list entry. The specification aligns the table and forbids it
   from crossing a 64 KB boundary, so an entry never straddles a page and one
   pointer covers it. */
bool ATAPCIController::ReadPrd(uint64_t addr, uint8_t *buf)
{
    uint8_t *ptr = GuestPtr(addr, false);
    if (ptr == nullptr) {
        return false;
    }
    memcpy(buf, ptr, ATA_PRD_ENTRY_SIZE);
    return true;
}


/* Move one scatter list entry's worth between the drive and guest memory,
   a page at a time because that is as far as one host pointer reaches. */
uint32_t ATAPCIController::MoveData(ATADevice *dev, uint64_t addr,
                                    uint32_t len, bool to_memory)
{
    uint32_t done = 0;

    while (done < len) {
        uint64_t a = addr + done;
        uint32_t chunk = DEVRAM_PAGE_SIZE -
                         (uint32_t)(a & (DEVRAM_PAGE_SIZE - 1));
        if (chunk > len - done) {
            chunk = len - done;
        }
        uint8_t *ptr = GuestPtr(a, to_memory);
        if (ptr == nullptr) {
            break;
        }
        uint32_t moved = dev->DmaMove(ptr, chunk);
        done += moved;
        if (moved < chunk) {
            /* The drive has no more to give or take, which is the ordinary
               way a transfer ends: the list may describe more than the
               command asked for. */
            break;
        }
    }
    return done;
}


void ATAPCIController::BusMasterRun(int index)
{
    ChannelState &c = fChannels[index];
    ATADevice *dev = c.ata.Selected();

    if (dev == nullptr || !dev->DmaPending()) {
        /* Armed before the command was issued, which is allowed: the engine
           stays active, and ATADmaRequested() runs it once a drive has
           something to move. */
        c.bm_status |= ATA_BM_STAT_ACTIVE;
        return;
    }

    bool to_memory = (c.bm_command & ATA_BM_CMD_WRITE) != 0;
    if (to_memory != dev->DmaToMemory()) {
        /* The engine was pointed the other way from the command. */
        c.bm_status |= ATA_BM_STAT_ERROR;
        c.bm_status &= ~ATA_BM_STAT_ACTIVE;
        dev->DmaComplete(false);
        return;
    }

    c.bm_status |= ATA_BM_STAT_ACTIVE;
    c.bm_status &= ~ATA_BM_STAT_ERROR;

    bool ok = true;
    uint64_t prd = c.bm_prd;
    for (;;) {
        uint8_t entry[ATA_PRD_ENTRY_SIZE];
        if (!ReadPrd(prd, entry)) {
            ok = false;
            break;
        }
        uint64_t addr = get_le32(entry);
        uint32_t count = get_le16(entry + 4);
        if (count == 0) {
            count = 0x10000;
        }
        bool eot = (get_le16(entry + 6) & ATA_PRD_EOT) != 0;
#ifdef DEBUG_ATA_DMA
        printf("ata: prd %08" PRIx64 " -> addr=%08" PRIx64 " count=%u%s\n",
               prd, addr, count, eot ? " eot" : "");
#endif
        uint32_t moved = MoveData(dev, addr, count, to_memory);
        prd += ATA_PRD_ENTRY_SIZE;
        if (moved < count) {
            /* Either the drive finished or guest memory gave out. Which of
               the two is decided by whether the drive has sectors left,
               which is what DmaComplete() looks at. */
            break;
        }
        if (eot) {
            break;
        }
    }

    c.bm_status &= ~ATA_BM_STAT_ACTIVE;
    if (!ok) {
        c.bm_status |= ATA_BM_STAT_ERROR;
    }
    /* DmaComplete() raises the channel's interrupt, which also latches the
       bus master's interrupt bit. */
    dev->DmaComplete(ok);
}


//#pragma mark - registers

uint32_t ATAPCIController::WindowRead(int channel,
                                      ATAPCIWindow::KindEnum kind,
                                      uint32_t offset, int size_log2)
{
    switch (kind) {
    case ATAPCIWindow::kCommand:
        return fChannels[channel].ata.CommandRead(offset, size_log2);

    case ATAPCIWindow::kControl:
        /* Only the one register of the block answers; in compatibility mode
           the window is that register alone and the offset is zero. */
        if (offset != 0 && offset != ATA_PCI_CTRL_OFFSET) {
            return 0xff;
        }
        return fChannels[channel].ata.ControlRead(0, size_log2);

    case ATAPCIWindow::kBusMaster: {
        ChannelState &c = fChannels[offset / 8];
        switch (offset % 8) {
        case ATA_BM_COMMAND: return c.bm_command;
        case ATA_BM_STATUS:  return c.bm_status;
        case ATA_BM_PRD:     return c.bm_prd;
        case ATA_BM_PRD + 1: return c.bm_prd >> 8;
        case ATA_BM_PRD + 2: return c.bm_prd >> 16;
        case ATA_BM_PRD + 3: return c.bm_prd >> 24;
        default:             return 0;
        }
    }
    }
    return 0;
}


void ATAPCIController::WindowWrite(int channel, ATAPCIWindow::KindEnum kind,
                                   uint32_t offset, uint32_t val,
                                   int size_log2)
{
    switch (kind) {
    case ATAPCIWindow::kCommand:
        fChannels[channel].ata.CommandWrite(offset, val, size_log2);
        return;

    case ATAPCIWindow::kControl:
        if (offset != 0 && offset != ATA_PCI_CTRL_OFFSET) {
            return;
        }
        fChannels[channel].ata.ControlWrite(0, val, size_log2);
        return;

    case ATAPCIWindow::kBusMaster: {
        int index = offset / 8;
        ChannelState &c = fChannels[index];
        switch (offset % 8) {
        case ATA_BM_COMMAND: {
            uint8_t old = c.bm_command;
            c.bm_command = val & (ATA_BM_CMD_START | ATA_BM_CMD_WRITE);
            if ((old & ATA_BM_CMD_START) == 0 &&
                (c.bm_command & ATA_BM_CMD_START) != 0) {
                BusMasterRun(index);
            } else if ((c.bm_command & ATA_BM_CMD_START) == 0) {
                c.bm_status &= ~ATA_BM_STAT_ACTIVE;
            }
            return;
        }
        case ATA_BM_STATUS:
            /* The error and interrupt bits are cleared by writing them back;
               the two capability bits are a driver's to set. */
            c.bm_status &= ~(val & (ATA_BM_STAT_ERROR | ATA_BM_STAT_IRQ));
            c.bm_status = (c.bm_status & ~(ATA_BM_STAT_DMA0 |
                                           ATA_BM_STAT_DMA1)) |
                          (val & (ATA_BM_STAT_DMA0 | ATA_BM_STAT_DMA1));
            return;
        case ATA_BM_PRD:
            if (size_log2 >= 2) {
                /* The whole pointer at once, which is how a driver writes
                   it. The low two bits are always zero. */
                c.bm_prd = val & ~3u;
            } else {
                c.bm_prd = (c.bm_prd & 0xffffff00) | (val & 0xfc);
            }
            return;
        case ATA_BM_PRD + 1:
            c.bm_prd = (c.bm_prd & 0xffff00ff) | ((val & 0xff) << 8);
            return;
        case ATA_BM_PRD + 2:
            c.bm_prd = (c.bm_prd & 0xff00ffff) | ((val & 0xff) << 16);
            return;
        case ATA_BM_PRD + 3:
            c.bm_prd = (c.bm_prd & 0x00ffffff) | ((val & 0xff) << 24);
            return;
        default:
            return;
        }
    }
    }
}


//#pragma mark - factory

Device *ata_pci_node_create(const char *name)
{
    return new ATAPCIController(name);
}
