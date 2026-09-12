/*
 * ATA (PATA) bus and devices
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
#include "ata.h"

#include <string.h>

#include "machine.h"


//#pragma mark - ATADevice

void ATADevice::Attach(ATAChannel *channel, int unit)
{
    fChannel = channel;
    fUnit = unit;
}


void ATADevice::RaiseIrq()
{
    if (fChannel != nullptr) {
        fChannel->RaiseIrq();
    }
}


void ATADevice::RequestDma(bool to_memory)
{
    fDmaToMemory = to_memory;
    fDmaPending = true;
    if (fChannel != nullptr) {
        fChannel->DmaRequested();
    }
}


void ATADevice::AbortCommand()
{
    fStatus = ATA_STAT_READY | ATA_STAT_ERR;
    fError = ATA_ERR_ABRT;
    fBufferPos = 0;
    fBufferEnd = 0;
    fDmaPending = false;
}


void ATADevice::SetSignature()
{
    /* The signature of a plain ATA device. One that answers packet commands
       puts 0xeb14 in the cylinder registers instead, which is how a guest
       tells the two apart without issuing a command. */
    fSelect &= 0xf0;
    fNsector = 1;
    fSector = 1;
    fLcyl = 0;
    fHcyl = 0;
}


void ATADevice::Reset()
{
    fStatus = ATA_STAT_READY | ATA_STAT_SEEK;
    fError = 0x01; /* diagnostics passed */
    fBufferPos = 0;
    fBufferEnd = 0;
    fDmaPending = false;
    SetSignature();
}


uint8_t ATADevice::ReadReg(int reg)
{
    switch (reg) {
    case ATA_REG_ERROR:   return fError;
    case ATA_REG_NSECTOR: return fNsector & 0xff;
    case ATA_REG_SECTOR:  return fSector;
    case ATA_REG_LCYL:    return fLcyl;
    case ATA_REG_HCYL:    return fHcyl;
    case ATA_REG_SELECT:  return fSelect;
    case ATA_REG_STATUS:  return fStatus;
    default:              return 0;
    }
}


void ATADevice::WriteReg(int reg, uint8_t val)
{
    switch (reg) {
    case ATA_REG_FEATURE: fFeature = val; break;
    case ATA_REG_NSECTOR: fNsector = val; break;
    case ATA_REG_SECTOR:  fSector = val; break;
    case ATA_REG_LCYL:    fLcyl = val; break;
    case ATA_REG_HCYL:    fHcyl = val; break;
    case ATA_REG_SELECT:  fSelect = val; break;
    default: break;
    }
}


uint16_t ATADevice::ReadData()
{
    if (fBufferPos + 2 > fBufferEnd) {
        /* Nothing is being transferred, so there is nothing to hand over.
           A real drive leaves the bus floating; all ones is what that reads
           as, and it keeps stale sector data out of the guest's hands. */
        return 0xffff;
    }
    uint16_t val = fBuffer[fBufferPos] | (fBuffer[fBufferPos + 1] << 8);
    fBufferPos += 2;
    if (fBufferPos >= fBufferEnd) {
        BufferComplete();
    }
    return val;
}


void ATADevice::WriteData(uint16_t val)
{
    if (fBufferPos + 2 > fBufferEnd) {
        return;
    }
    fBuffer[fBufferPos] = val & 0xff;
    fBuffer[fBufferPos + 1] = (val >> 8) & 0xff;
    fBufferPos += 2;
    if (fBufferPos >= fBufferEnd) {
        BufferComplete();
    }
}


//#pragma mark - ATAChannel

void ATAChannel::Init(ATAChannelTarget *target, int index)
{
    fTarget = target;
    fIndex = index;
}


bool ATAChannel::AttachDevice(ATADevice *dev, int unit)
{
    if (unit < 0 || unit >= ATA_MAX_DEVICES) {
        return false;
    }
    if (fDevices[unit] != nullptr) {
        vm_error("ata: channel %d already carries a %s device\n", fIndex,
                 unit == 0 ? "master" : "slave");
        return false;
    }
    fDevices[unit] = dev;
    dev->Attach(this, unit);
    return true;
}


ATADevice *ATAChannel::DeviceAt(int unit) const
{
    if (unit < 0 || unit >= ATA_MAX_DEVICES) {
        return nullptr;
    }
    return fDevices[unit];
}


ATADevice *ATAChannel::Selected() const
{
    return fDevices[fSelected];
}


void ATAChannel::RaiseIrq()
{
    /* A driver that means to poll sets nIEN, and then the line must stay
       down however much the device has to say. */
    if ((fControl & ATA_CTRL_NIEN) != 0 || fIrqLevel) {
        return;
    }
    fIrqLevel = true;
    if (fTarget != nullptr) {
        fTarget->ATASetIrq(fIndex, true);
    }
}


void ATAChannel::LowerIrq()
{
    if (!fIrqLevel) {
        return;
    }
    fIrqLevel = false;
    if (fTarget != nullptr) {
        fTarget->ATASetIrq(fIndex, false);
    }
}


void ATAChannel::DmaRequested()
{
    if (fTarget != nullptr) {
        fTarget->ATADmaRequested(fIndex);
    }
}


uint32_t ATAChannel::CommandRead(uint32_t offset, int size_log2)
{
    ATADevice *dev = Selected();

    if (offset == ATA_REG_DATA) {
        /* The data port is the one register that is not a byte wide, and a
           guest may read it a word or a doubleword at a time. */
        if (dev == nullptr) {
            return 0xffffffff;
        }
        uint32_t val = dev->ReadData();
        if (size_log2 >= 2) {
            val |= (uint32_t)dev->ReadData() << 16;
        }
        return val;
    }

    if (dev == nullptr) {
        /* Nothing is plugged in here. A status of zero is how an empty unit
           reads, and is what a guest probes for. */
        return 0x00;
    }
    if (offset == ATA_REG_STATUS) {
        /* Reading the status register is what acknowledges the interrupt. */
        LowerIrq();
    }
    return dev->ReadReg(offset);
}


void ATAChannel::CommandWrite(uint32_t offset, uint32_t val, int size_log2)
{
    ATADevice *dev = Selected();

    if (offset == ATA_REG_DATA) {
        if (dev == nullptr) {
            return;
        }
        dev->WriteData(val & 0xffff);
        if (size_log2 >= 2) {
            dev->WriteData((val >> 16) & 0xffff);
        }
        return;
    }

    if (offset == ATA_REG_SELECT) {
        /* Bit 4 chooses the device, and both of them latch the write so
           that the one not selected still knows which it is. */
        fSelected = (val >> 4) & 1;
        for (int i = 0; i < ATA_MAX_DEVICES; i++) {
            if (fDevices[i] != nullptr) {
                fDevices[i]->WriteReg(ATA_REG_SELECT, val);
            }
        }
        return;
    }

    if (dev == nullptr) {
        return;
    }
    if (offset == ATA_REG_COMMAND) {
        /* Issuing a command clears the previous one's interrupt. */
        LowerIrq();
        dev->ExecCommand(val & 0xff);
        return;
    }
    dev->WriteReg(offset, val & 0xff);
}


uint32_t ATAChannel::ControlRead(uint32_t offset, int size_log2)
{
    (void)offset;
    (void)size_log2;
    ATADevice *dev = Selected();

    /* The alternate status reads exactly what the status register reads,
       except that it leaves the interrupt alone. */
    if (dev == nullptr) {
        return 0x00;
    }
    return dev->Status();
}


void ATAChannel::ControlWrite(uint32_t offset, uint32_t val, int size_log2)
{
    (void)offset;
    (void)size_log2;
    uint8_t old = fControl;

    fControl = val & 0xff;

    if ((old & ATA_CTRL_SRST) == 0 && (fControl & ATA_CTRL_SRST) != 0) {
        /* Reset asserted: every device goes busy until it is released. */
        for (int i = 0; i < ATA_MAX_DEVICES; i++) {
            if (fDevices[i] != nullptr) {
                fDevices[i]->Reset();
                fDevices[i]->WriteReg(ATA_REG_SELECT, 0);
            }
        }
        LowerIrq();
    } else if ((old & ATA_CTRL_SRST) != 0 && (fControl & ATA_CTRL_SRST) == 0) {
        /* Released: the devices come back ready, with their signatures in
           the task file. */
        for (int i = 0; i < ATA_MAX_DEVICES; i++) {
            if (fDevices[i] != nullptr) {
                fDevices[i]->Reset();
            }
        }
        fSelected = 0;
    }

    if ((fControl & ATA_CTRL_NIEN) != 0) {
        LowerIrq();
    }
}


//#pragma mark - ATABus

bool ATABus::AssignResources(Device *dev)
{
    /* A drive is reached through its channel's registers, so it holds no
       host address space and no interrupt line of its own. */
    for (int i = 0; i < dev->ResourceCount(); i++) {
        if (dev->ResourceAt(i)->type != RES_NONE) {
            vm_error("ata bus: device '%s' declared a resource, but a drive "
                     "has none of its own\n", dev->Name());
            return false;
        }
    }
    return true;
}


//#pragma mark - ATADeviceNode

ATADeviceNode::ATADeviceNode(const char *name, ATADevice *dev):
    Device(name), fDev(dev)
{
}


ATADeviceNode::~ATADeviceNode()
{
    delete fDev;
}


bool ATADeviceNode::Realize()
{
    ATABus *bus = dynamic_cast<ATABus *>(ParentBus());
    if (bus == nullptr) {
        vm_error("%s: must be attached to an ATA bus\n", Name());
        return false;
    }
    return bus->Target()->AttachDevice(fDev);
}
