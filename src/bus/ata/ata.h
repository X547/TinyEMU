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
#pragma once

#include <stdint.h>

#include "device.h"

class HostBlockDevice;

#define ATA_SECTOR_SIZE 512

/* The largest READ/WRITE MULTIPLE this drive will accept, and so the size of
   the buffer one transfer needs. */
#define ATA_MAX_MULT_SECTORS 128

/* A channel carries a master and a slave. */
#define ATA_MAX_DEVICES 2

/* Task file register numbers, as the offsets from the command block base
   that they are. */
#define ATA_REG_DATA     0
#define ATA_REG_ERROR    1 /* reading; writing it is the feature register */
#define ATA_REG_FEATURE  1
#define ATA_REG_NSECTOR  2
#define ATA_REG_SECTOR   3 /* LBA 7:0 */
#define ATA_REG_LCYL     4 /* LBA 15:8 */
#define ATA_REG_HCYL     5 /* LBA 23:16 */
#define ATA_REG_SELECT   6 /* device and LBA 27:24 */
#define ATA_REG_STATUS   7 /* reading; writing it is the command register */
#define ATA_REG_COMMAND  7
#define ATA_REG_COUNT    8

/* Status register */
#define ATA_STAT_ERR   0x01
#define ATA_STAT_INDEX 0x02
#define ATA_STAT_ECC   0x04
#define ATA_STAT_DRQ   0x08
#define ATA_STAT_SEEK  0x10
#define ATA_STAT_WRERR 0x20
#define ATA_STAT_READY 0x40
#define ATA_STAT_BUSY  0x80

/* Error register */
#define ATA_ERR_MARK 0x01
#define ATA_ERR_TRK0 0x02
#define ATA_ERR_ABRT 0x04
#define ATA_ERR_MCR  0x08
#define ATA_ERR_ID   0x10
#define ATA_ERR_MC   0x20
#define ATA_ERR_ECC  0x40
#define ATA_ERR_BBD  0x80

/* Device control register, which is the write side of the control block. */
#define ATA_CTRL_NIEN  0x02 /* interrupts disabled */
#define ATA_CTRL_SRST  0x04 /* software reset */

/* The commands this bus understands. The list is longer than what any one
   device implements; a device answers what it can and aborts the rest. */
#define ATA_CMD_NOP              0x00
#define ATA_CMD_DEVICE_RESET     0x08
#define ATA_CMD_RECALIBRATE      0x10
#define ATA_CMD_READ             0x20
#define ATA_CMD_READ_ONCE        0x21
#define ATA_CMD_WRITE            0x30
#define ATA_CMD_WRITE_ONCE       0x31
#define ATA_CMD_VERIFY           0x40
#define ATA_CMD_VERIFY_ONCE      0x41
#define ATA_CMD_SEEK             0x70
#define ATA_CMD_DIAGNOSE         0x90
#define ATA_CMD_SPECIFY          0x91
#define ATA_CMD_PACKET           0xa0
#define ATA_CMD_IDENTIFY_PACKET  0xa1
#define ATA_CMD_SMART            0xb0
#define ATA_CMD_MULTREAD         0xc4
#define ATA_CMD_MULTWRITE        0xc5
#define ATA_CMD_SETMULT          0xc6
#define ATA_CMD_READDMA          0xc8
#define ATA_CMD_READDMA_ONCE     0xc9
#define ATA_CMD_WRITEDMA         0xca
#define ATA_CMD_WRITEDMA_ONCE    0xcb
#define ATA_CMD_STANDBYNOW       0xe0
#define ATA_CMD_IDLEIMMEDIATE    0xe1
#define ATA_CMD_STANDBY          0xe2
#define ATA_CMD_IDLE             0xe3
#define ATA_CMD_CHECKPOWERMODE   0xe5
#define ATA_CMD_SLEEP            0xe6
#define ATA_CMD_FLUSH_CACHE      0xe7
#define ATA_CMD_IDENTIFY         0xec
#define ATA_CMD_SETFEATURES      0xef
#define ATA_CMD_READ_NATIVE_MAX  0xf8

/* SET FEATURES subcommands that are acted on rather than merely accepted. */
#define ATA_FEATURE_SET_TRANSFER 0x03

/* Transfer mode values the SET FEATURES subcommand above takes: the mode
   number is the low three bits and the class is what is left. */
#define ATA_XFER_PIO_SLOW  0x00
#define ATA_XFER_PIO       0x08
#define ATA_XFER_MWDMA     0x20
#define ATA_XFER_UDMA      0x40

/* What the model advertises: multiword DMA modes 0 to 2, and Ultra DMA modes
   0 to 5. Nothing here is timed, so the mode a guest selects only decides
   what it reports; every one of them moves the data the same way. */
#define ATA_MWDMA_MODES 0x07
#define ATA_UDMA_MODES  0x3f


class ATAChannel;

/* One device on a channel: a disk, or one day something that answers packet
   commands. The task file is here rather than on the channel because every
   device has one of its own and the selected one is what answers, which is
   also what lets a channel with one device report the other as absent.

   A device implements ExecCommand() and, if it moves data, the PIO and DMA
   hooks the channel and the bus master drive it through. */
class ATADevice {
private:
    ATAChannel *fChannel = nullptr;
    int fUnit = 0;

protected:
    /* Task file. 'nsector' is wider than the register so that the 0 a guest
       writes for 256 sectors can be counted down. */
    uint8_t fFeature = 0;
    uint8_t fError = 0;
    uint16_t fNsector = 0;
    uint8_t fSector = 0;
    uint8_t fLcyl = 0;
    uint8_t fHcyl = 0;
    uint8_t fSelect = 0xa0;
    uint8_t fStatus = ATA_STAT_READY | ATA_STAT_SEEK;

    /* The PIO buffer, and how much of it the guest has moved. */
    uint8_t fBuffer[ATA_MAX_MULT_SECTORS * ATA_SECTOR_SIZE] {};
    int fBufferPos = 0;
    int fBufferEnd = 0;

    /* Set while a DMA command waits for the bus master to move its data. */
    bool fDmaPending = false;
    bool fDmaToMemory = false;

    void RaiseIrq();
    void AbortCommand();
    /* Ask the bus master for the data a DMA command needs. A running engine
       moves it here; one started later moves it then. */
    void RequestDma(bool to_memory);

public:
    virtual ~ATADevice() = default;

    void Attach(ATAChannel *channel, int unit);
    ATAChannel *Channel() const {return fChannel;}
    int Unit() const {return fUnit;}

    /* The task file, by register number. Register 0 is the data port and is
       reached through ReadData()/WriteData() instead. */
    uint8_t ReadReg(int reg);
    void WriteReg(int reg, uint8_t val);

    uint8_t Status() const {return fStatus;}

    /* The 16 bit data port, which is how PIO moves its bytes. */
    uint16_t ReadData();
    void WriteData(uint16_t val);

    /* A software reset from the control register, and the signature it
       leaves in the task file so that a guest can tell what kind of device
       answered. */
    virtual void Reset();
    virtual void SetSignature();

    /* Decode one command. A device aborts what it does not implement. */
    virtual void ExecCommand(uint8_t cmd) = 0;

    /* Bus master interface. A DMA command leaves the device pending until
       the guest starts the engine, which then moves the data in whatever
       chunks its scatter list describes. */
    bool DmaPending() const {return fDmaPending;}
    bool DmaToMemory() const {return fDmaToMemory;}
    /* Move at most 'len' bytes between the drive and 'mem'. Returns how many
       were moved; short means the drive has no more to give or take. */
    virtual uint32_t DmaMove(uint8_t *mem, uint32_t len) {(void)mem; (void)len;
                                                          return 0;}
    /* The engine stopped, either because the scatter list ended or because
       something failed. */
    virtual void DmaComplete(bool ok) {(void)ok;}

    /* What the device does when the buffer the guest was filling or draining
       runs out. */
    virtual void BufferComplete() {}
};


/* Implemented by whatever owns a channel, so that the channel can raise the
   line its interrupts go to without knowing how it is wired. */
class ATAChannelTarget {
public:
    virtual ~ATAChannelTarget() = default;

    virtual void ATASetIrq(int channel, bool level) = 0;

    /* A device on the channel has a DMA command waiting for data. */
    virtual void ATADmaRequested(int channel) = 0;
};


/* One channel: the two devices on it, which of them is selected, and the
   registers the guest reaches them through.

   The command block is the eight task file registers and the control block
   is the device control register and the alternate status that shares its
   address. Which addresses those land on is the controller's business. */
class ATAChannel {
private:
    ATAChannelTarget *fTarget = nullptr;
    int fIndex = 0;
    ATADevice *fDevices[ATA_MAX_DEVICES] {};
    int fSelected = 0;
    uint8_t fControl = 0;
    bool fIrqLevel = false;

public:
    void Init(ATAChannelTarget *target, int index);

    int Index() const {return fIndex;}
    bool AttachDevice(ATADevice *dev, int unit);
    ATADevice *DeviceAt(int unit) const;
    /* Null when the guest selected a unit nothing is plugged into. */
    ATADevice *Selected() const;

    /* Raised by a device when it wants attention, and dropped when the guest
       reads the status register or the reset line is pulled. */
    void RaiseIrq();
    void LowerIrq();
    bool IrqLevel() const {return fIrqLevel;}

    /* Raised by a device whose DMA command is waiting for data. */
    void DmaRequested();

    uint32_t CommandRead(uint32_t offset, int size_log2);
    void CommandWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t ControlRead(uint32_t offset, int size_log2);
    void ControlWrite(uint32_t offset, uint32_t val, int size_log2);
};


/* Implemented by a controller so that a device declared in the configuration
   can be plugged into one of its channels. */
class ATABusTarget {
public:
    virtual ~ATABusTarget() = default;

    /* A channel carries a master and a slave; a third device is reported
       rather than silently ignored. */
    virtual bool AttachDevice(ATADevice *dev) = 0;
};


/* The bus one channel provides. Like the SD and USB buses what it hands out
   is a place on a bus rather than host address space, so it assigns no
   resource records. */
class ATABus final: public Bus {
private:
    ATABusTarget *fTarget;

public:
    ATABus(Device *owner, ATABusTarget *target):
        Bus(owner), fTarget(target) {}

    const char *Type() const override {return "ata";}
    ATABusTarget *Target() const {return fTarget;}

    bool AssignResources(Device *dev) override;
};


/* Attaches one device to the channel it was declared on, the way
   SDDeviceNode attaches a card. */
class ATADeviceNode final: public Device {
private:
    std::unique_ptr<ATADevice> fDev;

public:
    ATADeviceNode(const char *name, std::unique_ptr<ATADevice> dev);
    ~ATADeviceNode() override;

    ATADevice *Dev() const {return fDev.get();}

    bool Realize() override;
};


/* A disk. 'read_only' refuses writes the way a jumpered drive would, rather
   than letting them fail one at a time in the back end. */
std::unique_ptr<ATADevice> ata_disk_create(std::unique_ptr<HostBlockDevice> bs,
                                           bool read_only);
Device *ata_disk_node_create(std::unique_ptr<HostBlockDevice> bs, bool read_only);
