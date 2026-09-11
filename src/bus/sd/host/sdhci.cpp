/*
 * SD Host Controller (SDHCI)
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
#include "sdhci.h"

#include <inttypes.h>
#include <stdio.h>
#include <string.h>

#include "cutils.h"
#include "fdt.h"
#include "machine.h"
#include "pci.h"
#include "sd.h"

/* The register file, as the SD Host Controller Standard Specification 3.00
   lays it out. The widths matter as much as the offsets: a guest addresses
   each of these with a cycle of the register's own width, and several of them
   act on being written. */
#define SDHCI_DMA_ADDRESS       0x00 /* also argument 2, for Auto CMD23 */
#define SDHCI_BLOCK_SIZE        0x04
#define SDHCI_BLOCK_COUNT       0x06
#define SDHCI_ARGUMENT          0x08
#define SDHCI_TRANSFER_MODE     0x0c
#define SDHCI_COMMAND           0x0e
#define SDHCI_RESPONSE          0x10 /* four registers */
#define SDHCI_BUFFER            0x20
#define SDHCI_PRESENT_STATE     0x24
#define SDHCI_HOST_CONTROL      0x28
#define SDHCI_POWER_CONTROL     0x29
#define SDHCI_BLOCK_GAP_CONTROL 0x2a
#define SDHCI_WAKEUP_CONTROL    0x2b
#define SDHCI_CLOCK_CONTROL     0x2c
#define SDHCI_TIMEOUT_CONTROL   0x2e
#define SDHCI_SOFTWARE_RESET    0x2f
#define SDHCI_INT_STATUS        0x30
#define SDHCI_ERR_INT_STATUS    0x32
#define SDHCI_INT_ENABLE        0x34
#define SDHCI_ERR_INT_ENABLE    0x36
#define SDHCI_SIGNAL_ENABLE     0x38
#define SDHCI_ERR_SIGNAL_ENABLE 0x3a
#define SDHCI_AUTO_CMD_STATUS   0x3c
#define SDHCI_HOST_CONTROL2     0x3e
#define SDHCI_CAPABILITIES      0x40
#define SDHCI_CAPABILITIES_HI   0x44
#define SDHCI_MAX_CURRENT       0x48
#define SDHCI_FORCE_AUTO_CMD    0x50
#define SDHCI_FORCE_ERR_INT     0x52
#define SDHCI_ADMA_ERROR        0x54
#define SDHCI_ADMA_ADDRESS      0x58
#define SDHCI_ADMA_ADDRESS_HI   0x5c
#define SDHCI_PRESET_VALUES     0x60
#define SDHCI_SLOT_INT_STATUS   0xfc
#define SDHCI_HOST_VERSION      0xfe

/* Everything above this is either not a register at all or, on the PCI
   binding, one of the two MSI-X windows placed further up the same BAR. */
#define SDHCI_REG_FILE_SIZE     0x100

/* Transfer mode. */
#define SDHCI_TRNS_DMA          (1 << 0)
#define SDHCI_TRNS_BLK_CNT_EN   (1 << 1)
#define SDHCI_TRNS_AUTO_MASK    (3 << 2)
#define SDHCI_TRNS_AUTO_CMD12   (1 << 2)
#define SDHCI_TRNS_AUTO_CMD23   (2 << 2)
#define SDHCI_TRNS_READ         (1 << 4)
#define SDHCI_TRNS_MULTI        (1 << 5)

/* Command. */
#define SDHCI_CMD_RESP_MASK     0x0003
#define  SDHCI_CMD_RESP_NONE    0x0000
#define  SDHCI_CMD_RESP_LONG    0x0001 /* 136 bits */
#define  SDHCI_CMD_RESP_SHORT   0x0002 /* 48 bits */
#define  SDHCI_CMD_RESP_BUSY    0x0003 /* 48 bits, and the card holds DAT0 */
#define SDHCI_CMD_CRC_CHECK     0x0008
#define SDHCI_CMD_INDEX_CHECK   0x0010
#define SDHCI_CMD_DATA          0x0020
#define SDHCI_CMD_TYPE_MASK     0x00c0
#define  SDHCI_CMD_TYPE_ABORT   0x00c0
#define SDHCI_CMD_INDEX_SHIFT   8

/* Present state. */
#define SDHCI_PRESENT_CMD_INHIBIT    (1u << 0)
#define SDHCI_PRESENT_DAT_INHIBIT    (1u << 1)
#define SDHCI_PRESENT_DAT_ACTIVE     (1u << 2)
#define SDHCI_PRESENT_WRITE_ACTIVE   (1u << 8)
#define SDHCI_PRESENT_READ_ACTIVE    (1u << 9)
#define SDHCI_PRESENT_BUF_WR_ENABLE  (1u << 10)
#define SDHCI_PRESENT_BUF_RD_ENABLE  (1u << 11)
#define SDHCI_PRESENT_CARD_INSERTED  (1u << 16)
#define SDHCI_PRESENT_CARD_STABLE    (1u << 17)
#define SDHCI_PRESENT_CARD_DETECT    (1u << 18)
#define SDHCI_PRESENT_WRITE_ENABLED  (1u << 19) /* the switch is not set */
#define SDHCI_PRESENT_DAT_LEVEL      (0xfu << 20)
#define SDHCI_PRESENT_CMD_LEVEL      (1u << 24)

/* Host control 1. */
#define SDHCI_CTRL_4BIT         (1 << 1)
#define SDHCI_CTRL_HIGH_SPEED   (1 << 2)
#define SDHCI_CTRL_DMA_MASK     0x18
#define  SDHCI_CTRL_SDMA        0x00
#define  SDHCI_CTRL_ADMA32      0x10
#define  SDHCI_CTRL_ADMA64      0x18
#define SDHCI_CTRL_8BIT         (1 << 5)

/* Clock control. */
#define SDHCI_CLOCK_INT_EN      (1 << 0)
#define SDHCI_CLOCK_INT_STABLE  (1 << 1)

/* Software reset. */
#define SDHCI_RESET_ALL         (1 << 0)
#define SDHCI_RESET_CMD         (1 << 1)
#define SDHCI_RESET_DATA        (1 << 2)

/* Normal interrupt status. */
#define SDHCI_INT_CMD_COMPLETE  (1 << 0)
#define SDHCI_INT_XFER_COMPLETE (1 << 1)
#define SDHCI_INT_BLOCK_GAP     (1 << 2)
#define SDHCI_INT_DMA_END       (1 << 3)
#define SDHCI_INT_BUF_WR_READY  (1 << 4)
#define SDHCI_INT_BUF_RD_READY  (1 << 5)
#define SDHCI_INT_CARD_INSERT   (1 << 6)
#define SDHCI_INT_CARD_REMOVE   (1 << 7)
#define SDHCI_INT_CARD_INT      (1 << 8)
#define SDHCI_INT_ERROR         (1 << 15)

/* Error interrupt status. */
#define SDHCI_ERR_CMD_TIMEOUT   (1 << 0)
#define SDHCI_ERR_CMD_CRC       (1 << 1)
#define SDHCI_ERR_CMD_END_BIT   (1 << 2)
#define SDHCI_ERR_CMD_INDEX     (1 << 3)
#define SDHCI_ERR_DATA_TIMEOUT  (1 << 4)
#define SDHCI_ERR_DATA_CRC      (1 << 5)
#define SDHCI_ERR_DATA_END_BIT  (1 << 6)
#define SDHCI_ERR_CURRENT_LIMIT (1 << 7)
#define SDHCI_ERR_AUTO_CMD      (1 << 8)
#define SDHCI_ERR_ADMA          (1 << 9)

/* Auto CMD error status. */
#define SDHCI_AUTOCMD_NOT_EXECUTED (1 << 0)
#define SDHCI_AUTOCMD_TIMEOUT      (1 << 1)

/* Capabilities. */
#define SDHCI_CAN_8BIT_EMBEDDED (1u << 18)
#define SDHCI_CAN_ADMA2         (1u << 19)
#define SDHCI_CAN_HIGH_SPEED    (1u << 21)
#define SDHCI_CAN_SDMA          (1u << 22)
#define SDHCI_CAN_VDD_330       (1u << 24)
#define SDHCI_CAN_64BIT         (1u << 28)
#define SDHCI_SLOT_TYPE_SHIFT   30
#define  SDHCI_SLOT_REMOVABLE   0
#define  SDHCI_SLOT_EMBEDDED    1

/* Host controller version: vendor version in the high byte, and 2 for
   specification 3.00 in the low one. */
#define SDHCI_SPEC_300          0x0002

/* Host control 2. Version 4 mode is a 4.00 feature, and reporting 3.00 above
   is what makes it unavailable; the bit is masked off so that a driver
   reading it back is told so. */
#define SDHCI_CTRL2_V4_MODE     (1 << 12)

/* An ADMA2 descriptor's attribute word. */
#define SDHCI_ADMA_VALID        (1 << 0)
#define SDHCI_ADMA_END          (1 << 1)
#define SDHCI_ADMA_INT          (1 << 2)
#define SDHCI_ADMA_ACT_SHIFT    4
#define  SDHCI_ADMA_ACT_NOP     0
#define  SDHCI_ADMA_ACT_TRAN    2
#define  SDHCI_ADMA_ACT_LINK    3

/* ADMA error status: the state the engine was in when it failed, plus the
   bit that says the length in the descriptor was wrong. */
#define SDHCI_ADMA_ERR_STATE_ST_FDS 0
#define SDHCI_ADMA_ERR_STATE_ST_TFR 3
#define SDHCI_ADMA_ERR_LENGTH   (1 << 2)

/* A descriptor list that is nothing but links and no-ops is a guest mistake
   rather than something to walk forever. */
#define SDHCI_ADMA_MAX_FETCHES  1024

/* Where the two MSI-X windows sit inside the register BAR, well above the
   256 byte register file. */
#define SDHCI_MSIX_TABLE_OFFSET 0x800
#define SDHCI_MSIX_PBA_OFFSET   0xc00

/* The PCI binding puts the slot description in configuration space: which
   base address register holds the first slot's registers, and how many slots
   follow it. That is exactly where a capability list would otherwise start,
   so this device's list is moved above it. */
#define SDHCI_PCI_SLOT_INFO     0x40
#define SDHCI_PCI_FIRST_CAP     0x44


/* Which natural register the byte at 'offset' belongs to, in bytes. A guest
   may use a cycle wider or narrower than the register really is, and knowing
   the register's own width is what lets such an access be turned back into
   the register writes it stands for. */
static uint32_t sdhci_reg_width(uint32_t offset)
{
    if (offset >= SDHCI_HOST_CONTROL && offset < SDHCI_CLOCK_CONTROL) {
        return 1;
    }
    if (offset == SDHCI_TIMEOUT_CONTROL || offset == SDHCI_SOFTWARE_RESET) {
        return 1;
    }
    if (offset >= SDHCI_ADMA_ERROR && offset < SDHCI_ADMA_ADDRESS) {
        return 1;
    }
    if (offset >= SDHCI_BLOCK_SIZE && offset < SDHCI_ARGUMENT) {
        return 2;
    }
    if (offset >= SDHCI_TRANSFER_MODE && offset < SDHCI_RESPONSE) {
        return 2;
    }
    if (offset >= SDHCI_CLOCK_CONTROL && offset < SDHCI_TIMEOUT_CONTROL) {
        return 2;
    }
    if (offset >= SDHCI_INT_STATUS && offset < SDHCI_CAPABILITIES) {
        return 2;
    }
    if (offset >= SDHCI_FORCE_AUTO_CMD && offset < SDHCI_ADMA_ERROR) {
        return 2;
    }
    if (offset >= SDHCI_PRESET_VALUES && offset < SDHCI_PRESET_VALUES + 0x10) {
        return 2;
    }
    if (offset >= SDHCI_SLOT_INT_STATUS) {
        return 2;
    }
    return 4;
}


static uint32_t sdhci_size_mask(uint32_t size)
{
    return size >= 4 ? 0xffffffffu : ((1u << (size * 8)) - 1);
}


//#pragma mark - SDHCIDevice

/* One SD host controller and the single slot it owns.

   Which transport it is on is decided by the bus it was attached to, exactly
   as it is for a virtio device: on PCI the guest enumerates it and places its
   base address register, and on the device tree bus it takes a register
   window and an interrupt line of its own and describes itself in the tree.
   Everything between the register file and the card is the same either way,
   which is the point of having one class rather than two. */
class SDHCIDevice final: public Device, public PCIBarTarget, public DeviceIO,
                         public SDBusTarget, public SDHost,
                         public SDDataCompletion {
private:
    const char *fCompatible;
    uint32_t fClockHz;

    /* transport */
    PCIDevice *fPciDev = nullptr;
    PCIMsixState fMsix {};
    PhysMemoryMap *fMemMap = nullptr;
    PhysMemoryRange *fMemRange = nullptr;
    IRQSignal *fIrq = nullptr;
    bool fIrqLevel = false;
    Resource *fMmioRes = nullptr;
    Resource *fIrqRes = nullptr;

    SDBus *fChildBus = nullptr;
    SDDevice *fCard = nullptr;

    /* register file */
    uint32_t fSdmaAddr = 0;
    uint16_t fBlockSize = 0;
    uint16_t fBlockCount = 0;
    uint32_t fArgument = 0;
    uint16_t fTransferMode = 0;
    uint16_t fCommand = 0;
    uint32_t fResponse[4] {};
    uint8_t fHostControl = 0;
    uint8_t fPowerControl = 0;
    uint8_t fBlockGapControl = 0;
    uint8_t fWakeupControl = 0;
    uint16_t fClockControl = 0;
    uint8_t fTimeoutControl = 0;
    uint16_t fNormalIntStatus = 0;
    uint16_t fErrorIntStatus = 0;
    uint16_t fNormalIntEnable = 0;
    uint16_t fErrorIntEnable = 0;
    uint16_t fNormalSignalEnable = 0;
    uint16_t fErrorSignalEnable = 0;
    uint16_t fAutoCmdErrStatus = 0;
    uint16_t fHostControl2 = 0;
    uint8_t fAdmaErrStatus = 0;
    uint64_t fAdmaAddr = 0;

    /* The interrupt an SDIO card raises on DAT[1], which is a level rather
       than an event and so is not held in the status register. */
    bool fCardIrq = false;

    /* The block buffer and the engine that moves it. One block is in flight
       at a time: the buffer is filled from the card and drained to the guest,
       or filled by the guest and handed to the card. */
    uint8_t fBuffer[SD_BLOCK_SIZE] {};
    uint32_t fBlockLen = SD_BLOCK_SIZE;
    uint32_t fBufferLen = 0;
    uint32_t fBufferPos = 0;
    bool fTransferActive = false;
    bool fTransferIsRead = false;
    bool fTransferDma = false;
    bool fBlockCountLimited = false;
    uint32_t fBlocksLeft = 0;
    /* A block is with the card, which will answer through DataComplete(). */
    bool fWaitingCard = false;
    /* The SDMA engine reached its buffer boundary and is waiting for the
       driver to program the next address. */
    bool fDmaPaused = false;
    bool fSdmaAtBoundary = false;

    /* The ADMA2 descriptor being consumed. */
    uint64_t fAdmaBufAddr = 0;
    uint32_t fAdmaBufLen = 0;
    bool fAdmaEnd = false;

    /* guest memory, through whichever transport this is on */
    uint8_t *GuestPtr(uint64_t addr, bool is_rw);
    bool DmaCopy(uint64_t addr, uint8_t *buf, uint32_t len, bool to_guest);

    /* register file */
    uint32_t ReadReg(uint32_t offset);
    void WriteReg(uint32_t offset, uint32_t val);
    uint32_t PresentState() const;
    uint32_t Capabilities() const;
    uint16_t NormalIntStatus() const;
    uint32_t BufferRead(uint32_t size);
    void BufferWrite(uint32_t val, uint32_t size);
    void SoftReset(uint32_t val);

    /* interrupts */
    void RaiseNormal(uint16_t bits);
    void RaiseError(uint16_t bits);
    void UpdateIrq();

    /* commands */
    void SendCommand();
    void StoreResponse(const uint8_t *resp, int len);
    void SendAutoCmd12();
    bool SendAutoCmd23();

    /* the data engine */
    void TransferStart();
    void TransferFinish();
    void TransferError();
    void TransferAbort(bool complete);
    void CountBlock();
    bool ReadNextBlock();
    void ReadBlockArrived();
    bool WriteCurrentBlock();
    bool WriteBlockAccepted();

    /* the DMA engines */
    bool AdmaSelected() const;
    uint32_t SdmaBoundary() const;
    void DmaRun();
    uint32_t DmaMove(uint8_t *buf, uint32_t len, bool to_guest);
    bool AdmaFetch();
    void AdmaError(uint8_t state);

public:
    SDHCIDevice(const char *name, const char *compatible, uint32_t clock_hz):
        Device(name), fCompatible(compatible), fClockHz(clock_hz) {}
    ~SDHCIDevice() override {delete fChildBus;}

    bool Prepare() override;
    bool Realize() override;
    void BuildFDT(FDTContext &ctx) override;
    Bus *ChildBus() override {return fChildBus;}

    void SetBar(int bar_num, uint64_t addr, bool enabled) override;
    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;

    bool AttachDevice(SDDevice *dev) override;
    void SetCardInterrupt(bool level) override;
    void DataComplete(bool ok) override;
};


//#pragma mark - guest memory

uint8_t *SDHCIDevice::GuestPtr(uint64_t addr, bool is_rw)
{
    if (fPciDev != nullptr) {
        return pci_device_get_dma_ptr(fPciDev, addr, is_rw);
    }
    return fMemMap->GetRamPtr(addr, is_rw);
}


bool SDHCIDevice::DmaCopy(uint64_t addr, uint8_t *buf, uint32_t len,
                          bool to_guest)
{
    /* Chunked a page at a time, which is all a pointer into guest memory is
       good for. */
    while (len > 0) {
        uint32_t page_left = DEVRAM_PAGE_SIZE - (addr & (DEVRAM_PAGE_SIZE - 1));
        uint32_t l = len < page_left ? len : page_left;
        uint8_t *ptr = GuestPtr(addr, to_guest);
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

uint16_t SDHCIDevice::NormalIntStatus() const
{
    uint16_t val = fNormalIntStatus;

    /* Both of these follow something rather than being written: the card
       interrupt follows the line the card drives, and the error bit is the
       or of everything the error status register holds. */
    if (fCardIrq && (fNormalIntEnable & SDHCI_INT_CARD_INT) != 0) {
        val |= SDHCI_INT_CARD_INT;
    }
    if (fErrorIntStatus != 0) {
        val |= SDHCI_INT_ERROR;
    }
    return val;
}


void SDHCIDevice::RaiseNormal(uint16_t bits)
{
    /* A status bit is only recorded while its status enable is set, which is
       how a driver stops an event it does not want from ever being seen. */
    fNormalIntStatus |= bits & fNormalIntEnable;
    UpdateIrq();
}


void SDHCIDevice::RaiseError(uint16_t bits)
{
    fErrorIntStatus |= bits & fErrorIntEnable;
    UpdateIrq();
}


void SDHCIDevice::UpdateIrq()
{
    if (fIrq == nullptr) {
        return;
    }

    bool level = (NormalIntStatus() & fNormalSignalEnable) != 0 ||
                 (fErrorIntStatus & fErrorSignalEnable) != 0;

    if (fMsix.Present() && fMsix.Enabled()) {
        /* A message is an edge, sent as the condition appears; the pin must
           stay low while MSI-X is in use. */
        fIrq->Set(0);
        if (level && !fIrqLevel) {
            fMsix.Send(0);
        }
        fIrqLevel = level;
        return;
    }

    /* A driver that means to poll turns the pin off in configuration space
       rather than in the controller. */
    if (fPciDev != nullptr &&
        (pci_device_get_config(fPciDev, PCI_COMMAND, 1) &
         PCI_COMMAND_INTX_DISABLE) != 0) {
        level = false;
    }
    if (level != fIrqLevel) {
        fIrqLevel = level;
        fIrq->Set(level ? 1 : 0);
    }
}


void SDHCIDevice::SetCardInterrupt(bool level)
{
    if (fCardIrq == level) {
        return;
    }
    fCardIrq = level;
    UpdateIrq();
}


//#pragma mark - register file

uint32_t SDHCIDevice::PresentState() const
{
    uint32_t state = SDHCI_PRESENT_CARD_STABLE;

    if (fTransferActive) {
        /* While the data line is busy the command line only takes an abort,
           which is what the inhibit bits tell the driver. */
        state |= SDHCI_PRESENT_DAT_INHIBIT | SDHCI_PRESENT_DAT_ACTIVE;
        state |= fTransferIsRead ? SDHCI_PRESENT_READ_ACTIVE
                                 : SDHCI_PRESENT_WRITE_ACTIVE;
        if (!fTransferDma && fBufferPos < fBufferLen) {
            state |= fTransferIsRead ? SDHCI_PRESENT_BUF_RD_ENABLE
                                     : SDHCI_PRESENT_BUF_WR_ENABLE;
        }
    }
    if (fCard != nullptr) {
        state |= SDHCI_PRESENT_CARD_INSERTED | SDHCI_PRESENT_CARD_DETECT;
        state |= SDHCI_PRESENT_DAT_LEVEL | SDHCI_PRESENT_CMD_LEVEL;
        /* The bit is the level of the write protect pin, and a card that is
           not write protected leaves it high. */
        if (!fCard->ReadOnly()) {
            state |= SDHCI_PRESENT_WRITE_ENABLED;
        }
    }
    return state;
}


uint32_t SDHCIDevice::Capabilities() const
{
    uint32_t mhz = fClockHz / 1000000;
    uint32_t caps;

    if (mhz > 255) {
        mhz = 255;
    }
    /* The timeout clock is the same clock, and its field is six bits. */
    caps = mhz > 63 ? 63 : mhz;
    caps |= 1u << 7;   /* the timeout clock is counted in MHz */
    caps |= mhz << 8;  /* base clock for the SD clock */
    /* Max block length 0 is 512 bytes, which is the only length served. */
    caps |= SDHCI_CAN_ADMA2 | SDHCI_CAN_SDMA | SDHCI_CAN_HIGH_SPEED;
    caps |= SDHCI_CAN_VDD_330;
    /* Descriptors and DMA addresses may name memory above 4 GB, which a
       machine with enough RAM will have. */
    caps |= SDHCI_CAN_64BIT;

    if (fCard != nullptr && fCard->CardType() == SD_CARD_MMC) {
        /* An embedded device is soldered down, and is the only thing that may
           have eight data lines. */
        caps |= SDHCI_CAN_8BIT_EMBEDDED;
        caps |= (uint32_t)SDHCI_SLOT_EMBEDDED << SDHCI_SLOT_TYPE_SHIFT;
    }
    return caps;
}


uint32_t SDHCIDevice::ReadReg(uint32_t offset)
{
    switch (offset) {
    case SDHCI_DMA_ADDRESS:
        return fSdmaAddr;
    case SDHCI_BLOCK_SIZE:
        return fBlockSize;
    case SDHCI_BLOCK_COUNT:
        return fBlockCount;
    case SDHCI_ARGUMENT:
        return fArgument;
    case SDHCI_TRANSFER_MODE:
        return fTransferMode;
    case SDHCI_COMMAND:
        return fCommand;
    case SDHCI_RESPONSE + 0:
        return fResponse[0];
    case SDHCI_RESPONSE + 4:
        return fResponse[1];
    case SDHCI_RESPONSE + 8:
        return fResponse[2];
    case SDHCI_RESPONSE + 12:
        return fResponse[3];
    case SDHCI_PRESENT_STATE:
        return PresentState();
    case SDHCI_HOST_CONTROL:
        return fHostControl;
    case SDHCI_POWER_CONTROL:
        return fPowerControl;
    case SDHCI_BLOCK_GAP_CONTROL:
        return fBlockGapControl;
    case SDHCI_WAKEUP_CONTROL:
        return fWakeupControl;
    case SDHCI_CLOCK_CONTROL:
        /* Nothing here has to settle, so the clock is stable as soon as it
           has been asked for. A driver spins on this bit. */
        if ((fClockControl & SDHCI_CLOCK_INT_EN) != 0) {
            return fClockControl | SDHCI_CLOCK_INT_STABLE;
        }
        return fClockControl & ~(uint16_t)SDHCI_CLOCK_INT_STABLE;
    case SDHCI_TIMEOUT_CONTROL:
        return fTimeoutControl;
    case SDHCI_SOFTWARE_RESET:
        return 0; /* the reset bits clear themselves */
    case SDHCI_INT_STATUS:
        return NormalIntStatus();
    case SDHCI_ERR_INT_STATUS:
        return fErrorIntStatus;
    case SDHCI_INT_ENABLE:
        return fNormalIntEnable;
    case SDHCI_ERR_INT_ENABLE:
        return fErrorIntEnable;
    case SDHCI_SIGNAL_ENABLE:
        return fNormalSignalEnable;
    case SDHCI_ERR_SIGNAL_ENABLE:
        return fErrorSignalEnable;
    case SDHCI_AUTO_CMD_STATUS:
        return fAutoCmdErrStatus;
    case SDHCI_HOST_CONTROL2:
        return fHostControl2;
    case SDHCI_CAPABILITIES:
        return Capabilities();
    case SDHCI_CAPABILITIES_HI:
        /* No UHS-I mode, no re-tuning and no clock multiplier: without 1.8 V
           signalling none of them can be reached anyway. */
        return 0;
    case SDHCI_MAX_CURRENT:
        /* The 3.3 V supply, in the 4 mA units the field counts. */
        return 0xff;
    case SDHCI_ADMA_ERROR:
        return fAdmaErrStatus;
    case SDHCI_ADMA_ADDRESS:
        return (uint32_t)fAdmaAddr;
    case SDHCI_ADMA_ADDRESS_HI:
        return (uint32_t)(fAdmaAddr >> 32);
    case SDHCI_SLOT_INT_STATUS:
        return fIrqLevel ? 1 : 0;
    case SDHCI_HOST_VERSION:
        return SDHCI_SPEC_300;
    default:
        return 0;
    }
}


void SDHCIDevice::WriteReg(uint32_t offset, uint32_t val)
{
    switch (offset) {
    case SDHCI_DMA_ADDRESS:
        fSdmaAddr = val;
        /* Programming the next address is what resumes a transfer that
           stopped at an SDMA buffer boundary. */
        if (fDmaPaused) {
            fDmaPaused = false;
            fSdmaAtBoundary = false;
            DmaRun();
        }
        break;
    case SDHCI_BLOCK_SIZE:
        fBlockSize = val;
        break;
    case SDHCI_BLOCK_COUNT:
        fBlockCount = val;
        break;
    case SDHCI_ARGUMENT:
        fArgument = val;
        break;
    case SDHCI_TRANSFER_MODE:
        fTransferMode = val;
        break;
    case SDHCI_COMMAND:
        fCommand = val;
        SendCommand();
        break;
    case SDHCI_HOST_CONTROL:
        fHostControl = val;
        break;
    case SDHCI_POWER_CONTROL:
        fPowerControl = val;
        break;
    case SDHCI_BLOCK_GAP_CONTROL:
        fBlockGapControl = val;
        break;
    case SDHCI_WAKEUP_CONTROL:
        fWakeupControl = val;
        break;
    case SDHCI_CLOCK_CONTROL:
        fClockControl = val;
        break;
    case SDHCI_TIMEOUT_CONTROL:
        fTimeoutControl = val;
        break;
    case SDHCI_SOFTWARE_RESET:
        SoftReset(val);
        break;
    case SDHCI_INT_STATUS:
        /* Write one to clear, except the card interrupt: that one follows the
           line the card drives, and a driver silences it by clearing its
           status enable instead. */
        fNormalIntStatus &= ~(uint16_t)(val & ~SDHCI_INT_CARD_INT);
        UpdateIrq();
        break;
    case SDHCI_ERR_INT_STATUS:
        fErrorIntStatus &= ~(uint16_t)val;
        UpdateIrq();
        break;
    case SDHCI_INT_ENABLE:
        fNormalIntEnable = val;
        fNormalIntStatus &= fNormalIntEnable;
        UpdateIrq();
        break;
    case SDHCI_ERR_INT_ENABLE:
        fErrorIntEnable = val;
        fErrorIntStatus &= fErrorIntEnable;
        UpdateIrq();
        break;
    case SDHCI_SIGNAL_ENABLE:
        fNormalSignalEnable = val;
        UpdateIrq();
        break;
    case SDHCI_ERR_SIGNAL_ENABLE:
        fErrorSignalEnable = val;
        UpdateIrq();
        break;
    case SDHCI_HOST_CONTROL2:
        fHostControl2 = val & ~(uint16_t)SDHCI_CTRL2_V4_MODE;
        break;
    case SDHCI_ADMA_ERROR:
        fAdmaErrStatus = val;
        break;
    case SDHCI_ADMA_ADDRESS:
        fAdmaAddr = (fAdmaAddr & ~(uint64_t)0xffffffffu) | val;
        break;
    case SDHCI_ADMA_ADDRESS_HI:
        fAdmaAddr = (fAdmaAddr & 0xffffffffu) | ((uint64_t)val << 32);
        break;
    case SDHCI_FORCE_AUTO_CMD:
        fAutoCmdErrStatus |= val;
        break;
    case SDHCI_FORCE_ERR_INT:
        RaiseError(val);
        break;
    default:
        /* The response registers, the present state and the capabilities are
           read only, and a write to one of them is simply lost. */
        break;
    }
}


void SDHCIDevice::SoftReset(uint32_t val)
{
    if ((val & SDHCI_RESET_ALL) != 0) {
        /* Everything the host programmed goes; the card does not, because it
           is a device of its own and is reset by GO_IDLE_STATE. */
        TransferAbort(false);
        fSdmaAddr = 0;
        fBlockSize = 0;
        fBlockCount = 0;
        fArgument = 0;
        fTransferMode = 0;
        fCommand = 0;
        memset(fResponse, 0, sizeof(fResponse));
        fHostControl = 0;
        fPowerControl = 0;
        fBlockGapControl = 0;
        fWakeupControl = 0;
        fClockControl = 0;
        fTimeoutControl = 0;
        fNormalIntStatus = 0;
        fErrorIntStatus = 0;
        fNormalIntEnable = 0;
        fErrorIntEnable = 0;
        fNormalSignalEnable = 0;
        fErrorSignalEnable = 0;
        fAutoCmdErrStatus = 0;
        fHostControl2 = 0;
        fAdmaErrStatus = 0;
        fAdmaAddr = 0;
        UpdateIrq();
        return;
    }
    if ((val & SDHCI_RESET_CMD) != 0) {
        fNormalIntStatus &= ~(uint16_t)SDHCI_INT_CMD_COMPLETE;
        fErrorIntStatus &= ~(uint16_t)(SDHCI_ERR_CMD_TIMEOUT |
                                       SDHCI_ERR_CMD_CRC |
                                       SDHCI_ERR_CMD_END_BIT |
                                       SDHCI_ERR_CMD_INDEX);
        UpdateIrq();
    }
    if ((val & SDHCI_RESET_DATA) != 0) {
        TransferAbort(false);
        fNormalIntStatus &= ~(uint16_t)(SDHCI_INT_XFER_COMPLETE |
                                        SDHCI_INT_DMA_END |
                                        SDHCI_INT_BUF_RD_READY |
                                        SDHCI_INT_BUF_WR_READY |
                                        SDHCI_INT_BLOCK_GAP);
        fErrorIntStatus &= ~(uint16_t)(SDHCI_ERR_DATA_TIMEOUT |
                                       SDHCI_ERR_DATA_CRC |
                                       SDHCI_ERR_DATA_END_BIT |
                                       SDHCI_ERR_ADMA);
        UpdateIrq();
    }
}


//#pragma mark - the block buffer, as the guest sees it

uint32_t SDHCIDevice::BufferRead(uint32_t size)
{
    uint32_t val = 0;

    for (uint32_t i = 0; i < size; i++) {
        if (fBufferPos < fBufferLen) {
            val |= (uint32_t)fBuffer[fBufferPos++] << (i * 8);
        }
    }
    if (fTransferActive && !fTransferDma && fTransferIsRead &&
        fBufferPos >= fBufferLen) {
        ReadNextBlock();
    }
    return val;
}


void SDHCIDevice::BufferWrite(uint32_t val, uint32_t size)
{
    for (uint32_t i = 0; i < size; i++) {
        if (fBufferPos < fBufferLen) {
            fBuffer[fBufferPos++] = (uint8_t)(val >> (i * 8));
        }
    }
    if (fTransferActive && !fTransferDma && !fTransferIsRead &&
        fBufferPos >= fBufferLen) {
        WriteCurrentBlock();
    }
}


//#pragma mark - commands

void SDHCIDevice::StoreResponse(const uint8_t *resp, int len)
{
    if (len == SD_RESPONSE_LONG) {
        /* The register the host keeps is the card's own 128 bits without the
           CRC byte that ends them, so everything shifts down by one byte. */
        fResponse[0] = ((uint32_t)resp[11] << 24) | ((uint32_t)resp[12] << 16) |
                       ((uint32_t)resp[13] << 8) | resp[14];
        fResponse[1] = ((uint32_t)resp[7] << 24) | ((uint32_t)resp[8] << 16) |
                       ((uint32_t)resp[9] << 8) | resp[10];
        fResponse[2] = ((uint32_t)resp[3] << 24) | ((uint32_t)resp[4] << 16) |
                       ((uint32_t)resp[5] << 8) | resp[6];
        fResponse[3] = ((uint32_t)resp[0] << 16) | ((uint32_t)resp[1] << 8) |
                       resp[2];
    } else if (len == SD_RESPONSE_SHORT) {
        fResponse[0] = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) |
                       ((uint32_t)resp[2] << 8) | resp[3];
    }
}


bool SDHCIDevice::SendAutoCmd23()
{
    SDCommand cmd;
    uint8_t resp[SD_RESPONSE_LONG];

    /* Version 3 puts the block count for the automatic SET_BLOCK_COUNT in the
       register that otherwise holds the SDMA address, which is why the two
       features cannot be used together. */
    cmd.index = SD_CMD_SET_BLOCK_COUNT;
    cmd.arg = fSdmaAddr;
    if (fCard->Command(cmd, resp) < 0) {
        fAutoCmdErrStatus |= SDHCI_AUTOCMD_TIMEOUT;
        RaiseError(SDHCI_ERR_AUTO_CMD);
        return false;
    }
    return true;
}


void SDHCIDevice::SendAutoCmd12()
{
    SDCommand cmd;
    uint8_t resp[SD_RESPONSE_LONG];

    cmd.index = SD_CMD_STOP_TRANSMISSION;
    cmd.arg = 0;
    int len = fCard->Command(cmd, resp);
    if (len < 0) {
        fAutoCmdErrStatus |= SDHCI_AUTOCMD_TIMEOUT;
        RaiseError(SDHCI_ERR_AUTO_CMD);
        return;
    }
    /* The response to a command the controller issued by itself goes in the
       last response register, which is where a driver looks for it. */
    if (len == SD_RESPONSE_SHORT) {
        fResponse[3] = ((uint32_t)resp[0] << 24) | ((uint32_t)resp[1] << 16) |
                       ((uint32_t)resp[2] << 8) | resp[3];
    }
}


void SDHCIDevice::SendCommand()
{
    SDCommand cmd;
    uint8_t resp[SD_RESPONSE_LONG];

    uint8_t index = (fCommand >> SDHCI_CMD_INDEX_SHIFT) & 0x3f;
    bool data_present = (fCommand & SDHCI_CMD_DATA) != 0;
    bool is_abort = (fCommand & SDHCI_CMD_TYPE_MASK) == SDHCI_CMD_TYPE_ABORT ||
                    index == SD_CMD_STOP_TRANSMISSION;

    fAutoCmdErrStatus = 0;

    if (fCard == nullptr) {
        /* An empty slot answers nothing, which is exactly what a driver
           probing for a card is waiting to find out. */
        RaiseError(SDHCI_ERR_CMD_TIMEOUT);
        return;
    }

    if (data_present && (fTransferMode & SDHCI_TRNS_AUTO_MASK) ==
                            SDHCI_TRNS_AUTO_CMD23) {
        if (!SendAutoCmd23()) {
            return;
        }
    }

    cmd.index = index;
    cmd.arg = fArgument;
    int len = fCard->Command(cmd, resp);
    if (len < 0) {
        RaiseError(SDHCI_ERR_CMD_TIMEOUT);
        return;
    }
    StoreResponse(resp, len);
    RaiseNormal(SDHCI_INT_CMD_COMPLETE);

    if (is_abort) {
        TransferAbort(true);
        return;
    }
    if (data_present) {
        TransferStart();
        return;
    }
    if ((fCommand & SDHCI_CMD_RESP_MASK) == SDHCI_CMD_RESP_BUSY) {
        /* Nothing here holds the data line down, so the busy the response
           promised is over before the driver can look. */
        RaiseNormal(SDHCI_INT_XFER_COMPLETE);
    }
}


//#pragma mark - the data engine

void SDHCIDevice::TransferStart()
{
    uint32_t block_len = fBlockSize & 0x0fff;

    if (block_len == 0 || block_len > SD_BLOCK_SIZE) {
        /* The capabilities say 512 bytes is the longest block, so anything
           else is a driver that did not read them. */
        RaiseError(SDHCI_ERR_DATA_CRC);
        return;
    }

    /* The command that was just answered decided what really happens; the
       transfer mode register only says how the host means to move it. */
    SDDataDirEnum dir = fCard->DataDir();
    if (dir == SD_DATA_NONE) {
        RaiseError(SDHCI_ERR_DATA_TIMEOUT);
        return;
    }

    fBlockLen = block_len;
    fTransferIsRead = dir == SD_DATA_READ;
    fTransferDma = (fTransferMode & SDHCI_TRNS_DMA) != 0;
    fWaitingCard = false;
    fDmaPaused = false;
    fSdmaAtBoundary = false;
    fAdmaEnd = false;
    fAdmaBufLen = 0;
    fAdmaBufAddr = 0;
    fBufferPos = 0;
    fBufferLen = 0;

    if ((fTransferMode & SDHCI_TRNS_MULTI) == 0) {
        fBlocksLeft = 1;
        fBlockCountLimited = true;
    } else if ((fTransferMode & SDHCI_TRNS_BLK_CNT_EN) != 0) {
        fBlocksLeft = fBlockCount;
        fBlockCountLimited = true;
    } else {
        /* An open ended transfer runs until STOP_TRANSMISSION ends it. */
        fBlocksLeft = 0;
        fBlockCountLimited = false;
    }

    fTransferActive = true;

    if (fTransferIsRead) {
        if (fTransferDma) {
            DmaRun();
        } else {
            ReadNextBlock();
        }
        return;
    }
    if (fBlockCountLimited && fBlocksLeft == 0) {
        TransferFinish();
        return;
    }
    fBufferLen = fBlockLen;
    if (fTransferDma) {
        DmaRun();
    } else {
        RaiseNormal(SDHCI_INT_BUF_WR_READY);
    }
}


void SDHCIDevice::CountBlock()
{
    if (!fBlockCountLimited || fBlocksLeft == 0) {
        return;
    }
    fBlocksLeft--;
    /* The block count register counts down as the transfer runs, which is
       what a driver reads back after an SDMA boundary interrupt. */
    fBlockCount = (uint16_t)fBlocksLeft;
}


bool SDHCIDevice::ReadNextBlock()
{
    if (fBlockCountLimited && fBlocksLeft == 0) {
        TransferFinish();
        return false;
    }
    SDXferStatusEnum status = fCard->ReadBlock(fBuffer, fBlockLen, this);
    if (status == SD_XFER_PENDING) {
        fWaitingCard = true;
        return false;
    }
    if (status == SD_XFER_ERROR) {
        TransferError();
        return false;
    }
    ReadBlockArrived();
    return true;
}


void SDHCIDevice::ReadBlockArrived()
{
    fBufferPos = 0;
    fBufferLen = fBlockLen;
    CountBlock();
    if (!fTransferDma) {
        RaiseNormal(SDHCI_INT_BUF_RD_READY);
    }
}


bool SDHCIDevice::WriteCurrentBlock()
{
    SDXferStatusEnum status = fCard->WriteBlock(fBuffer, fBlockLen, this);
    if (status == SD_XFER_PENDING) {
        fWaitingCard = true;
        return false;
    }
    if (status == SD_XFER_ERROR) {
        TransferError();
        return false;
    }
    return WriteBlockAccepted();
}


bool SDHCIDevice::WriteBlockAccepted()
{
    CountBlock();
    if (fBlockCountLimited && fBlocksLeft == 0) {
        TransferFinish();
        return false;
    }
    fBufferPos = 0;
    fBufferLen = fBlockLen;
    if (!fTransferDma) {
        RaiseNormal(SDHCI_INT_BUF_WR_READY);
    }
    return true;
}


void SDHCIDevice::DataComplete(bool ok)
{
    if (!fWaitingCard) {
        return;
    }
    fWaitingCard = false;
    if (!fTransferActive) {
        /* The transfer was abandoned while the block was with the card. */
        return;
    }
    if (!ok) {
        TransferError();
        return;
    }
    if (fTransferIsRead) {
        ReadBlockArrived();
    } else if (!WriteBlockAccepted()) {
        return;
    }
    if (fTransferDma) {
        DmaRun();
    }
}


void SDHCIDevice::TransferFinish()
{
    fTransferActive = false;
    fWaitingCard = false;
    fDmaPaused = false;
    fBufferPos = 0;
    fBufferLen = 0;

    /* Auto CMD12 ends a multiple block transfer without the driver having to
       send the command itself. */
    if ((fTransferMode & SDHCI_TRNS_MULTI) != 0 &&
        (fTransferMode & SDHCI_TRNS_AUTO_MASK) == SDHCI_TRNS_AUTO_CMD12) {
        SendAutoCmd12();
    }
    RaiseNormal(SDHCI_INT_XFER_COMPLETE);
}


void SDHCIDevice::TransferError()
{
    if (fCard != nullptr) {
        fCard->StopTransfer();
    }
    fTransferActive = false;
    fWaitingCard = false;
    fDmaPaused = false;
    fBufferPos = 0;
    fBufferLen = 0;
    RaiseError(SDHCI_ERR_DATA_CRC);
}


void SDHCIDevice::TransferAbort(bool complete)
{
    if (fCard != nullptr) {
        fCard->StopTransfer();
    }
    if (!fTransferActive) {
        return;
    }
    fTransferActive = false;
    fWaitingCard = false;
    fDmaPaused = false;
    fBufferPos = 0;
    fBufferLen = 0;
    if (complete) {
        RaiseNormal(SDHCI_INT_XFER_COMPLETE);
    }
}


//#pragma mark - the DMA engines

bool SDHCIDevice::AdmaSelected() const
{
    uint32_t mode = fHostControl & SDHCI_CTRL_DMA_MASK;

    return mode == SDHCI_CTRL_ADMA32 || mode == SDHCI_CTRL_ADMA64;
}


uint32_t SDHCIDevice::SdmaBoundary() const
{
    /* The block size register carries the boundary the SDMA engine breaks a
       transfer at, as a power of two from 4 KB up. */
    return 4096u << ((fBlockSize >> 12) & 7);
}


void SDHCIDevice::AdmaError(uint8_t state)
{
    fAdmaErrStatus = state;
    RaiseError(SDHCI_ERR_ADMA);
    TransferError();
}


bool SDHCIDevice::AdmaFetch()
{
    bool wide = (fHostControl & SDHCI_CTRL_DMA_MASK) == SDHCI_CTRL_ADMA64;
    uint32_t desc_size = wide ? 12 : 8;
    uint8_t desc[12];

    if (fAdmaEnd) {
        /* The list ran out before the data did. */
        AdmaError(SDHCI_ADMA_ERR_STATE_ST_TFR);
        return false;
    }
    if (!DmaCopy(fAdmaAddr, desc, desc_size, false)) {
        AdmaError(SDHCI_ADMA_ERR_STATE_ST_FDS);
        return false;
    }

    uint16_t attr = get_le16(desc);
    uint32_t length = get_le16(desc + 2);
    uint64_t addr = wide ? get_le64(desc + 4) : get_le32(desc + 4);

    fAdmaAddr += desc_size;

    if ((attr & SDHCI_ADMA_VALID) == 0) {
        AdmaError(SDHCI_ADMA_ERR_STATE_ST_FDS);
        return false;
    }
    /* A length field of zero asks for the whole 64 KB it cannot hold. */
    if (length == 0) {
        length = 65536;
    }

    switch ((attr >> SDHCI_ADMA_ACT_SHIFT) & 3) {
    case SDHCI_ADMA_ACT_NOP:
        break;
    case SDHCI_ADMA_ACT_TRAN:
        fAdmaBufAddr = addr;
        fAdmaBufLen = length;
        break;
    case SDHCI_ADMA_ACT_LINK:
        fAdmaAddr = addr;
        break;
    default:
        AdmaError(SDHCI_ADMA_ERR_STATE_ST_FDS | SDHCI_ADMA_ERR_LENGTH);
        return false;
    }

    if ((attr & SDHCI_ADMA_INT) != 0) {
        RaiseNormal(SDHCI_INT_DMA_END);
    }
    if ((attr & SDHCI_ADMA_END) != 0) {
        fAdmaEnd = true;
    }
    return true;
}


uint32_t SDHCIDevice::DmaMove(uint8_t *buf, uint32_t len, bool to_guest)
{
    if (AdmaSelected()) {
        int fetches = 0;
        while (fAdmaBufLen == 0) {
            if (++fetches > SDHCI_ADMA_MAX_FETCHES) {
                AdmaError(SDHCI_ADMA_ERR_STATE_ST_FDS);
                return 0;
            }
            if (!AdmaFetch()) {
                return 0;
            }
        }
        uint32_t n = len < fAdmaBufLen ? len : fAdmaBufLen;
        if (!DmaCopy(fAdmaBufAddr, buf, n, to_guest)) {
            AdmaError(SDHCI_ADMA_ERR_STATE_ST_TFR);
            return 0;
        }
        fAdmaBufAddr += n;
        fAdmaBufLen -= n;
        return n;
    }

    /* SDMA: one address that walks forward, broken at the buffer boundary so
       that the driver can hand over the next region. The pause is taken when
       more data is asked for rather than when the boundary is reached, so a
       transfer that ends exactly on it does not raise a spurious
       interrupt. */
    if (fSdmaAtBoundary) {
        fDmaPaused = true;
        RaiseNormal(SDHCI_INT_DMA_END);
        return 0;
    }

    uint32_t boundary = SdmaBoundary();
    uint32_t to_boundary = boundary - (fSdmaAddr & (boundary - 1));
    uint32_t n = len < to_boundary ? len : to_boundary;
    if (!DmaCopy(fSdmaAddr, buf, n, to_guest)) {
        RaiseError(SDHCI_ERR_DATA_CRC);
        TransferError();
        return 0;
    }
    fSdmaAddr += n;
    if ((fSdmaAddr & (boundary - 1)) == 0) {
        fSdmaAtBoundary = true;
    }
    return n;
}


void SDHCIDevice::DmaRun()
{
    while (fTransferActive && !fDmaPaused && !fWaitingCard) {
        if (fBufferPos < fBufferLen) {
            /* A read drains the block buffer into guest memory, a write
               fills it from there. */
            uint32_t n = DmaMove(fBuffer + fBufferPos, fBufferLen - fBufferPos,
                                 fTransferIsRead);
            if (n == 0) {
                return;
            }
            fBufferPos += n;
            continue;
        }
        if (fTransferIsRead) {
            if (!ReadNextBlock()) {
                return;
            }
        } else if (!WriteCurrentBlock()) {
            return;
        }
    }
}


//#pragma mark - memory mapped access

uint32_t SDHCIDevice::DeviceRead(uint32_t offset, int size_log2)
{
    uint32_t size = 1u << size_log2;

    offset &= SDHCI_REG_SIZE - 1;

    if (offset >= SDHCI_REG_FILE_SIZE) {
        /* Above the register file are the two MSI-X windows, which exist only
           on the PCI binding. */
        if (fPciDev == nullptr || offset < SDHCI_MSIX_TABLE_OFFSET) {
            return 0;
        }
        if (offset < SDHCI_MSIX_PBA_OFFSET) {
            return fMsix.TableRead(offset - SDHCI_MSIX_TABLE_OFFSET, 2);
        }
        return fMsix.PbaRead(offset - SDHCI_MSIX_PBA_OFFSET, 2);
    }

    /* The data port is a window on the block buffer rather than a register:
       a read takes exactly as many bytes out of it as the cycle is wide. */
    if (offset == SDHCI_BUFFER) {
        return BufferRead(size);
    }

    /* Taken a byte at a time out of whichever register each byte belongs to,
       which is what makes a cycle of any width at any alignment come out the
       way the register file really is laid out. Reading a register has no
       side effect, so there is nothing to lose by touching one twice. */
    uint32_t val = 0;
    for (uint32_t i = 0; i < size; i++) {
        uint32_t byte_offset = offset + i;
        uint32_t width = sdhci_reg_width(byte_offset);
        uint32_t base = byte_offset & ~(width - 1);
        uint32_t byte = (ReadReg(base) >> ((byte_offset - base) * 8)) & 0xff;
        val |= byte << (i * 8);
    }
    return val;
}


void SDHCIDevice::DeviceWrite(uint32_t offset, uint32_t val, int size_log2)
{
    uint32_t size = 1u << size_log2;

    offset &= SDHCI_REG_SIZE - 1;

    if (offset >= SDHCI_REG_FILE_SIZE) {
        if (fPciDev == nullptr || offset < SDHCI_MSIX_TABLE_OFFSET ||
            offset >= SDHCI_MSIX_PBA_OFFSET) {
            return;
        }
        fMsix.TableWrite(offset - SDHCI_MSIX_TABLE_OFFSET, val, 2);
        return;
    }

    if (offset == SDHCI_BUFFER) {
        BufferWrite(val, size);
        return;
    }

    /* Break the access into the registers it really covers, lowest address
       first, so that writing the transfer mode and the command with one 32
       bit cycle behaves as the pair of 16 bit writes it stands for and the
       command is issued last. */
    while (size > 0) {
        uint32_t width = sdhci_reg_width(offset);
        if (width > size) {
            /* Narrower than the register it lands in: fold the bytes into
               what the register holds, so that a register with a side effect
               still sees one whole write. */
            uint32_t base = offset & ~(width - 1);
            uint32_t shift = (offset - base) * 8;
            uint32_t mask = sdhci_size_mask(size) << shift;
            WriteReg(base, (ReadReg(base) & ~mask) | ((val << shift) & mask));
            return;
        }
        WriteReg(offset, val & sdhci_size_mask(width));
        if (width >= 4) {
            return;
        }
        val >>= width * 8;
        offset += width;
        size -= width;
    }
}


void SDHCIDevice::SetBar(int bar_num, uint64_t addr, bool enabled)
{
    (void)bar_num;
    fMemRange->SetAddr(addr, enabled);
}


//#pragma mark - the slot

bool SDHCIDevice::AttachDevice(SDDevice *dev)
{
    if (fCard != nullptr) {
        vm_error("%s: the slot already holds '%s'\n", Name(), fCard->Name());
        return false;
    }
    fCard = dev;
    dev->SetHost(this);
    dev->Reset();
    return true;
}


//#pragma mark - lifecycle

bool SDHCIDevice::Prepare()
{
    if (ParentBus()->AsPCIBus() == nullptr) {
        /* On a device tree machine the controller is described rather than
           enumerated, so it needs a register window and a line of its own.
           On PCI the guest places the base address register and the bridge
           routes INTx, so it declares neither. */
        fMmioRes = AddResource(RES_MMIO, SDHCI_REG_SIZE, 0x1000);
        fIrqRes = AddResource(RES_IRQ, 1);
        if (fMmioRes == nullptr || fIrqRes == nullptr) {
            return false;
        }
    }
    fChildBus = new SDBus(this, this);
    return true;
}


bool SDHCIDevice::Realize()
{
    PCIBus *pci_bus = ParentBus()->AsPCIBus();
    int devio_flags = DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32;

    if (pci_bus != nullptr) {
        /* Red Hat's identifiers for a standard SD host controller. What makes
           a driver bind is the class code, which names this a controller of
           the kind the PCI SD Host Controller specification describes. */
        /* Red Hat's identifiers, and a capability list moved up out of the
           way of the slot information register the binding puts at 0x40. */
        fPciDev = pci_register_device(pci_bus, "sdhci", -1, 0x1b36, 0x0007,
                                      0x01, 0x0805, SDHCI_PCI_FIRST_CAP);
        if (fPciDev == nullptr) {
            vm_error("%s: could not register the PCI device\n", Name());
            return false;
        }
        /* Programming interface 1 is a controller that does DMA of its own. */
        pci_device_set_config8(fPciDev, PCI_CLASS_PROG, 0x01);
        pci_device_set_config8(fPciDev, PCI_INTERRUPT_PIN, 1);
        /* The slot information register: one slot, whose registers are in
           base address register 0, which is what both of its fields being
           zero means. */
        pci_device_set_config8(fPciDev, SDHCI_PCI_SLOT_INFO, 0x00);

        fMsix.Init(fPciDev, 0, 1, SDHCI_MSIX_TABLE_OFFSET,
                   SDHCI_MSIX_PBA_OFFSET);

        fIrq = pci_device_get_irq(fPciDev, 0);
        fMemMap = pci_device_get_mem_map(fPciDev);
        fMemRange = fMemMap->RegisterDevice(0, SDHCI_REG_SIZE, this,
                                            devio_flags | DEVIO_DISABLED);
        pci_register_bar(fPciDev, 0, SDHCI_REG_SIZE, PCI_ADDRESS_SPACE_MEM,
                         this);
        return true;
    }

    SystemBus *sys = static_cast<SystemBus *>(ParentBus());
    fMemMap = sys->MemMap();
    fIrq = sys->IrqSignalFor(fIrqRes->base);
    if (fIrq == nullptr) {
        vm_error("%s: bad interrupt line %d\n", Name(), (int)fIrqRes->base);
        return false;
    }
    fMemRange = fMemMap->RegisterDevice(fMmioRes->base, SDHCI_REG_SIZE, this,
                                        devio_flags);
    return true;
}


void SDHCIDevice::BuildFDT(FDTContext &ctx)
{
    uint32_t tab[2];

    if (fMmioRes == nullptr) {
        /* On PCI the guest finds the controller by enumerating configuration
           space, so it must not also appear as a node. */
        return;
    }

    /* The drivers that bind to a controller like this one all want a clock,
       and there is no clock controller in this machine to take it from, so
       one fixed clock is emitted alongside. It is named after the register
       window so that a machine with two controllers has two of them. */
    uint32_t clock_phandle = ctx.fdt->AllocPhandle();
    ctx.fdt->BeginNodeNum("mmc-clock", fMmioRes->base);
    ctx.fdt->PropStr("compatible", "fixed-clock");
    ctx.fdt->PropU32("#clock-cells", 0);
    ctx.fdt->PropU32("clock-frequency", fClockHz);
    ctx.fdt->PropU32("phandle", clock_phandle);
    ctx.fdt->EndNode();

    ctx.fdt->BeginNodeNum("mmc", fMmioRes->base);
    ctx.fdt->PropStr("compatible", fCompatible);
    ctx.fdt->PropU64Range("reg", fMmioRes->base, fMmioRes->size);
    tab[0] = ctx.plic_phandle;
    tab[1] = (uint32_t)fIrqRes->base;
    ctx.fdt->PropTabU32("interrupts-extended", tab, 2);

    /* Both the register clock and the card clock are that one fixed clock. */
    tab[0] = clock_phandle;
    tab[1] = clock_phandle;
    ctx.fdt->PropTabU32("clocks", tab, 2);
    ctx.fdt->PropStrList("clock-names", "clk_xin", "clk_ahb", NULL);
    ctx.fdt->PropU32("max-frequency", fClockHz);

    /* What the slot really holds, so that a driver does not have to find out
       the hard way. */
    if (fCard != nullptr) {
        ctx.fdt->PropU32("bus-width", fCard->BusWidth());
        if (!fCard->Removable()) {
            ctx.fdt->PropEmpty("non-removable");
        }
        if (fCard->CardType() == SD_CARD_MMC) {
            ctx.fdt->PropEmpty("cap-mmc-highspeed");
        } else {
            ctx.fdt->PropEmpty("cap-sd-highspeed");
        }
    }
    /* Only 3.3 V signalling is modelled, so a driver must not try to switch
       the bus to 1.8 V, and there is no write protect pin to read. */
    ctx.fdt->PropEmpty("no-1-8-v");
    ctx.fdt->PropEmpty("disable-wp");
    ctx.fdt->EndNode();
}


//#pragma mark - factory

Device *sdhci_node_create(const char *name, const char *compatible,
                          uint32_t clock_hz)
{
    return new SDHCIDevice(name, compatible, clock_hz);
}
