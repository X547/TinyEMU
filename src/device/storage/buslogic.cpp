/*
 * BusLogic MultiMaster (BT-958) PCI SCSI host adapter
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <deque>
#include <memory>
#include <vector>

#include "bits.h"
#include "cutils.h"
#include "device_class.h"
#include "machine.h"
#include "pci.h"
#include "scsi.h"

#define BL_PCI_VENDOR_ID 0x104b
#define BL_PCI_DEVICE_ID 0x1040 /* MultiMaster */
#define BL_PCI_CLASS     0x0100 /* SCSI storage controller */

/* Four registers, in I/O space and again in memory space. */
#define BL_IO_BAR_SIZE  4
#define BL_MEM_BAR_SIZE 32

/* Registers. */
#define BL_REG_CONTROL 0 /* write */
#define BL_REG_STATUS  0 /* read */
#define BL_REG_COMMAND 1 /* write: command and parameter bytes */
#define BL_REG_DATA    1 /* read: reply bytes */
#define BL_REG_INTR    2 /* read */
#define BL_REG_GEOM    3 /* read */

#define BL_CONTROL_RHARD 0x80 /* hard reset */
#define BL_CONTROL_RSOFT 0x40 /* soft reset */
#define BL_CONTROL_RINT  0x20 /* acknowledge the interrupt */
#define BL_CONTROL_RSBUS 0x10 /* reset the SCSI bus */

#define BL_STATUS_DACT   0x80 /* diagnostics active */
#define BL_STATUS_DFAIL  0x40 /* diagnostics failed */
#define BL_STATUS_INREQ  0x20 /* mailboxes not initialized */
#define BL_STATUS_HARDY  0x10 /* ready for a command */
#define BL_STATUS_CPRBSY 0x08 /* parameter register busy */
#define BL_STATUS_DIRRDY 0x04 /* a reply byte is ready */
#define BL_STATUS_CMDINV 0x01 /* the last command was invalid */

#define BL_INTR_INTV 0x80 /* any of the below */
#define BL_INTR_RSTS 0x08 /* SCSI bus reset */
#define BL_INTR_CMDC 0x04 /* command complete */
#define BL_INTR_MBOR 0x02 /* an outgoing mailbox was freed */
#define BL_INTR_IMBL 0x01 /* an incoming mailbox was filled */

/* Host adapter commands. */
#define BL_CMD_TEST_CMDC_INTR      0x00
#define BL_CMD_START_MAILBOX       0x02
#define BL_CMD_INQUIRE_BOARD_ID    0x04
#define BL_CMD_ENABLE_MBOR_INTR    0x05
#define BL_CMD_SELECTION_TIMEOUT   0x06
#define BL_CMD_INQUIRE_DEVICES_0_7 0x0a
#define BL_CMD_INQUIRE_CONFIG      0x0b
#define BL_CMD_INQUIRE_SETUP       0x0d
#define BL_CMD_ECHO                0x1f
#define BL_CMD_INQUIRE_DEVICES_8_15 0x23
#define BL_CMD_INQUIRE_TARGETS     0x24
#define BL_CMD_SET_INTR            0x25
#define BL_CMD_INIT_EXT_MAILBOX    0x81
#define BL_CMD_INQUIRE_FW_DIGIT3   0x84
#define BL_CMD_INQUIRE_FW_LETTER   0x85
#define BL_CMD_INQUIRE_PCI_INFO    0x86
#define BL_CMD_INQUIRE_MODEL       0x8b
#define BL_CMD_INQUIRE_SYNC_PERIOD 0x8c
#define BL_CMD_INQUIRE_EXT_SETUP   0x8d
#define BL_CMD_STRICT_RR           0x8f
#define BL_CMD_FETCH_LOCAL_RAM     0x91
#define BL_CMD_MODIFY_IO_ADDR      0x95
#define BL_CMD_SET_CCB_FORMAT      0x96

/* Mailbox action and completion codes. */
#define BL_MBO_FREE  0x00
#define BL_MBO_START 0x01
#define BL_MBO_ABORT 0x02

#define BL_MBI_FREE      0x00
#define BL_MBI_OK        0x01
#define BL_MBI_ABORTED   0x02
#define BL_MBI_NOT_FOUND 0x03
#define BL_MBI_ERROR     0x04

/* The 32 bit CCB. */
#define BL_CCB_SIZE        40
#define BL_CCB_OPCODE      0
#define BL_CCB_DIRECTION   1
#define BL_CCB_CDB_LEN     2
#define BL_CCB_SENSE_LEN   3
#define BL_CCB_DATA_LEN    4
#define BL_CCB_DATA        8
#define BL_CCB_BTSTAT      14
#define BL_CCB_SDSTAT      15
#define BL_CCB_TARGET      16
#define BL_CCB_LUN         17
#define BL_CCB_CDB         18
#define BL_CCB_SENSE       36
#define BL_CCB_MAX_CDB     12

#define BL_OP_INITIATE           0x00
#define BL_OP_INITIATE_SG        0x02
#define BL_OP_INITIATE_RETLEN    0x03
#define BL_OP_INITIATE_RETLEN_SG 0x04
#define BL_OP_BUS_DEVICE_RESET   0x81

#define BL_DIR_MASK 0x18
#define BL_DIR_CDB  0x00 /* as the command implies */
#define BL_DIR_IN   0x08
#define BL_DIR_OUT  0x10
#define BL_DIR_NONE 0x18

/* Host adapter status. */
#define BL_BT_OK               0x00
#define BL_BT_SEL_TIMEOUT      0x11
#define BL_BT_DATA_RUN         0x12 /* data overrun or underrun */
#define BL_BT_INVALID_OPCODE   0x16
#define BL_BT_INVALID_PARAM    0x1a
#define BL_BT_HOST_BUS_RESET   0x22
#define BL_BT_HOST_DEV_RESET   0x23

#define BL_MAX_TARGET 16 /* wide */
#define BL_MAX_LUN    8  /* three bits in the CCB */
#define BL_SCSI_ID    7

#define BL_MAX_SG       8192
#define BL_MAX_TRANSFER (16 << 20)

#define BL_REPLY_MAX 256


class BusLogicDevice;

/* One CCB the adapter has started. It is also the completion its unit answers
   through. */
struct BLCommand final: public SCSICompletion {
    struct Segment {
        uint32_t addr;
        uint32_t len;
    };

    BusLogicDevice &dev;
    uint32_t ccb_addr = 0;
    uint8_t ccb[BL_CCB_SIZE] {};
    int target = 0;
    SCSIDevice *unit = nullptr;
    std::vector<Segment> segs;
    uint32_t data_len = 0;
    bool data_in = false;
    std::unique_ptr<uint8_t[]> buf;
    SCSIRequest req {};

    BLCommand(BusLogicDevice &dev): dev(dev) {}

    void Complete(SCSIRequest *req) override;
};


class BusLogicDevice final: public Device, public PCIBarTarget,
                            public DeviceIO, public SCSIBusTarget {
private:
    struct InMail {
        uint32_t ccb_addr;
        uint8_t btstat;
        uint8_t sdstat;
        uint8_t code;
    };

    PCIDevice *fPciDev = nullptr;
    PhysMemoryRange *fIoRange = nullptr;
    PhysMemoryRange *fMemRange = nullptr;
    IRQSignal *fIrq = nullptr;
    std::unique_ptr<SCSIBus> fChildBus;
    SCSIDevice *fUnits[BL_MAX_TARGET][BL_MAX_LUN] {};

    uint8_t fIntr = 0;
    bool fIrqLevel = false;
    bool fIntrEnabled = true;
    bool fMborEnabled = false;
    /* an incoming mailbox was filled while IMBL was already up */
    bool fImblAgain = false;
    bool fCmdInvalid = false;
    /* status reads left that show diagnostics running after a hard reset,
       which a driver may wait to see */
    int fDiagReads = 0;
    /* the LUN takes five bits of its CCB byte rather than three */
    bool fWideLun = false;

    /* the command being given */
    bool fInCommand = false;
    uint8_t fCmd = 0;
    uint8_t fParams[8] {};
    int fParamCount = 0;
    int fParamLen = 0;
    uint8_t fReply[BL_REPLY_MAX] {};
    int fReplyLen = 0;
    int fReplyPos = 0;

    /* mailboxes, outgoing then incoming, 8 bytes each */
    bool fMailboxReady = false;
    uint32_t fMailboxAddr = 0;
    int fMailboxCount = 0;
    int fOutIndex = 0;
    int fInIndex = 0;
    bool fStrictRR = false;
    /* completions waiting for a free incoming mailbox */
    std::deque<InMail> fPending;

    std::vector<std::unique_ptr<BLCommand>> fInFlight;

    bool HasTarget(int target) const;
    bool DmaRead(uint32_t addr, void *buf, uint32_t len);
    bool DmaWrite(uint32_t addr, const void *buf, uint32_t len);

    void RaiseIntr(uint8_t bits);
    void UpdateIrq();
    void Reset(bool hard);
    void ResetBus();

    void WriteControl(uint8_t val);
    void WriteCommand(uint8_t val);
    void Execute();
    void CommandDone();
    int ParamLength(uint8_t cmd) const;

    void ProcessMailboxes();
    void PostInMail(uint32_t ccb_addr, uint8_t btstat, uint8_t sdstat,
                    uint8_t code);
    void FlushPending();
    void StartCcb(uint32_t ccb_addr);
    void AbortCcb(uint32_t ccb_addr);
    void FailCcb(uint32_t ccb_addr, uint8_t btstat);
    bool ReadSegments(BLCommand *cmd);
    void Finish(BLCommand *cmd);
    std::unique_ptr<BLCommand> TakeInFlight(BLCommand *cmd);
    /* Take every command for which 'match' holds back from its unit and
       answer it with 'btstat'. */
    template<typename Match>
    void Abort(Match match, uint8_t btstat);

public:
    BusLogicDevice(const char *name): Device(name) {}
    ~BusLogicDevice() override;

    void CommandFinished(BLCommand *cmd);

    bool Prepare() override;
    bool Realize() override;
    Bus *ChildBus() override {return fChildBus.get();}

    void SetBar(int bar_num, uint64_t addr, bool enabled) override;
    uint64_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint64_t val, int size_log2) override;

    bool FindFreeAddress(int *target, int *lun) override;
    bool AttachDevice(SCSIDevice *dev, uint32_t target, uint32_t lun) override;
};


void BLCommand::Complete(SCSIRequest *)
{
    dev.CommandFinished(this);
}


BusLogicDevice::~BusLogicDevice()
{
    for (auto &cmd : fInFlight) {
        cmd->unit->Cancel(&cmd->req);
    }
}


bool BusLogicDevice::HasTarget(int target) const
{
    if (target < 0 || target >= BL_MAX_TARGET) {
        return false;
    }
    for (int l = 0; l < BL_MAX_LUN; l++) {
        if (fUnits[target][l] != nullptr) {
            return true;
        }
    }
    return false;
}


//#pragma mark - DMA

bool BusLogicDevice::DmaRead(uint32_t addr, void *buf, uint32_t len)
{
    uint8_t *dst = static_cast<uint8_t *>(buf);
    uint64_t a = addr;

    while (len > 0) {
        uint32_t page_left = DEVRAM_PAGE_SIZE - (a & (DEVRAM_PAGE_SIZE - 1));
        uint32_t l = len < page_left ? len : page_left;
        uint8_t *ptr = pci_device_get_dma_ptr(fPciDev, a, false);
        if (ptr == nullptr) {
            return false;
        }
        memcpy(dst, ptr, l);
        a += l;
        dst += l;
        len -= l;
    }
    return true;
}


bool BusLogicDevice::DmaWrite(uint32_t addr, const void *buf, uint32_t len)
{
    const uint8_t *src = static_cast<const uint8_t *>(buf);
    uint64_t a = addr;

    while (len > 0) {
        uint32_t page_left = DEVRAM_PAGE_SIZE - (a & (DEVRAM_PAGE_SIZE - 1));
        uint32_t l = len < page_left ? len : page_left;
        uint8_t *ptr = pci_device_get_dma_ptr(fPciDev, a, true);
        if (ptr == nullptr) {
            return false;
        }
        memcpy(ptr, src, l);
        a += l;
        src += l;
        len -= l;
    }
    return true;
}


//#pragma mark - interrupts and resets

void BusLogicDevice::RaiseIntr(uint8_t bits)
{
    if ((bits & BL_INTR_IMBL) != 0 && (fIntr & BL_INTR_IMBL) != 0) {
        fImblAgain = true;
    }
    fIntr |= bits | BL_INTR_INTV;
    UpdateIrq();
}


void BusLogicDevice::UpdateIrq()
{
    bool level = fIntrEnabled && (fIntr & BL_INTR_INTV) != 0;
    if (fIrq != nullptr && level != fIrqLevel) {
        fIrqLevel = level;
        fIrq->Set(level ? 1 : 0);
    }
}


/* Both leave the adapter waiting for its mailboxes; the commands it had are
   forgotten without an answer. */
void BusLogicDevice::Reset(bool hard)
{
    for (auto &cmd : fInFlight) {
        cmd->unit->Cancel(&cmd->req);
    }
    fInFlight.clear();
    fPending.clear();
    if (hard) {
        for (auto &target : fUnits) {
            for (SCSIDevice *unit : target) {
                if (unit != nullptr) {
                    unit->Reset();
                }
            }
        }
        fIntrEnabled = true;
        fDiagReads = 1;
    }
    fIntr = 0;
    fWideLun = false;
    fImblAgain = false;
    fCmdInvalid = false;
    fInCommand = false;
    fReplyLen = fReplyPos = 0;
    fMborEnabled = false;
    fMailboxReady = false;
    fMailboxAddr = 0;
    fMailboxCount = 0;
    fOutIndex = fInIndex = 0;
    fStrictRR = false;
    UpdateIrq();
}


/* The commands on the bus end with a bus reset status. */
void BusLogicDevice::ResetBus()
{
    for (auto &target : fUnits) {
        for (SCSIDevice *unit : target) {
            if (unit != nullptr) {
                unit->Reset();
            }
        }
    }
    Abort([](const BLCommand *) {return true;}, BL_BT_HOST_BUS_RESET);
    RaiseIntr(BL_INTR_RSTS);
}


//#pragma mark - host adapter commands

int BusLogicDevice::ParamLength(uint8_t cmd) const
{
    switch (cmd) {
    case BL_CMD_INQUIRE_BOARD_ID:
    case BL_CMD_INQUIRE_DEVICES_0_7:
    case BL_CMD_INQUIRE_CONFIG:
    case BL_CMD_INQUIRE_DEVICES_8_15:
    case BL_CMD_INQUIRE_TARGETS:
    case BL_CMD_INQUIRE_FW_DIGIT3:
    case BL_CMD_INQUIRE_FW_LETTER:
    case BL_CMD_INQUIRE_PCI_INFO:
        return 0;
    case BL_CMD_ENABLE_MBOR_INTR:
    case BL_CMD_INQUIRE_SETUP:
    case BL_CMD_ECHO:
    case BL_CMD_SET_INTR:
    case BL_CMD_INQUIRE_MODEL:
    case BL_CMD_INQUIRE_SYNC_PERIOD:
    case BL_CMD_INQUIRE_EXT_SETUP:
    case BL_CMD_STRICT_RR:
    case BL_CMD_MODIFY_IO_ADDR:
    case BL_CMD_SET_CCB_FORMAT:
        return 1;
    case BL_CMD_FETCH_LOCAL_RAM:
        return 2;
    case BL_CMD_SELECTION_TIMEOUT:
        return 4;
    case BL_CMD_INIT_EXT_MAILBOX:
        return 5;
    }
    return -1;
}


void BusLogicDevice::WriteCommand(uint8_t val)
{
    if (fInCommand) {
        /* a parameter byte */
        if (fParamCount < fParamLen) {
            fParams[fParamCount++] = val;
            if (fParamCount == fParamLen) {
                Execute();
            }
        }
        return;
    }

    fCmdInvalid = false;
    switch (val) {
    case BL_CMD_TEST_CMDC_INTR:
        RaiseIntr(BL_INTR_CMDC);
        return;
    case BL_CMD_START_MAILBOX:
        /* answered through the mailboxes, never with CMDC */
        if (fMailboxReady) {
            ProcessMailboxes();
        }
        return;
    }

    fCmd = val;
    fParamLen = ParamLength(val);
    if (fParamLen < 0) {
        fCmdInvalid = true;
        RaiseIntr(BL_INTR_CMDC);
        return;
    }
    fInCommand = true;
    fParamCount = 0;
    fReplyLen = fReplyPos = 0;
    if (fParamLen == 0) {
        Execute();
    }
}


/* Runs a command once its parameters are in, and leaves its reply to be
   read. */
void BusLogicDevice::Execute()
{
    uint8_t *r = fReply;
    int len = 0;

    memset(fReply, 0, sizeof(fReply));
    switch (fCmd) {
    case BL_CMD_INQUIRE_BOARD_ID:
        r[0] = 'A'; /* board type */
        r[1] = 'A'; /* custom features */
        r[2] = '5'; /* firmware 5.0 */
        r[3] = '0';
        len = 4;
        break;

    case BL_CMD_ENABLE_MBOR_INTR:
        fMborEnabled = fParams[0] != 0;
        break;

    case BL_CMD_SELECTION_TIMEOUT:
        /* a missing target times out at once whatever the setting */
        break;

    case BL_CMD_INQUIRE_DEVICES_0_7:
    case BL_CMD_INQUIRE_DEVICES_8_15: {
        int first = fCmd == BL_CMD_INQUIRE_DEVICES_0_7 ? 0 : 8;
        for (int t = 0; t < 8; t++) {
            for (int l = 0; l < BL_MAX_LUN; l++) {
                if (fUnits[first + t][l] != nullptr) {
                    r[t] |= 1 << l;
                }
            }
        }
        len = 8;
        break;
    }

    case BL_CMD_INQUIRE_CONFIG: {
        int line = pci_device_get_config(fPciDev, PCI_INTERRUPT_LINE, 0);
        r[0] = 0; /* no ISA DMA channel */
        r[1] = line >= 9 && line <= 15 ? 1 << (line - 9) : 0;
        r[2] = BL_SCSI_ID;
        len = 3;
        break;
    }

    case BL_CMD_INQUIRE_SETUP:
        r[4] = fMailboxCount;
        r[5] = fMailboxAddr >> 16;
        r[6] = fMailboxAddr >> 8;
        r[7] = fMailboxAddr;
        len = std::min<int>(fParams[0], BL_REPLY_MAX);
        break;

    case BL_CMD_ECHO:
        r[0] = fParams[0];
        len = 1;
        break;

    case BL_CMD_INQUIRE_TARGETS: {
        uint16_t map = 0;
        for (int t = 0; t < BL_MAX_TARGET; t++) {
            if (HasTarget(t)) {
                map |= 1 << t;
            }
        }
        put_le16(r, map);
        len = 2;
        break;
    }

    case BL_CMD_SET_INTR:
        fIntrEnabled = (fParams[0] & 1) != 0;
        UpdateIrq();
        break;

    case BL_CMD_INIT_EXT_MAILBOX:
        if (fParams[0] == 0) {
            fCmdInvalid = true;
            break;
        }
        fMailboxCount = fParams[0];
        fMailboxAddr = get_le32(fParams + 1);
        fOutIndex = fInIndex = 0;
        fMailboxReady = true;
        break;

    case BL_CMD_INQUIRE_FW_DIGIT3:
        r[0] = '7';
        len = 1;
        break;

    case BL_CMD_INQUIRE_FW_LETTER:
        r[0] = 'B';
        len = 1;
        break;

    case BL_CMD_INQUIRE_MODEL:
        memcpy(r, "958  ", 5);
        len = std::min<int>(fParams[0], BL_REPLY_MAX);
        break;

    case BL_CMD_INQUIRE_EXT_SETUP:
        r[0] = 'E'; /* what a PCI adapter reports */
        r[1] = 0;   /* no BIOS */
        put_le16(r + 2, BL_MAX_SG);
        r[4] = fMailboxCount;
        put_le32(r + 5, fMailboxAddr);
        r[9] = 0x40; /* level triggered interrupt */
        memcpy(r + 10, "07B", 3);
        r[13] = 0x09; /* wide, ultra */
        len = std::min<int>(fParams[0], BL_REPLY_MAX);
        break;

    case BL_CMD_STRICT_RR:
        fStrictRR = fParams[0] == 1;
        break;

    case BL_CMD_INQUIRE_PCI_INFO:
        r[0] = 6; /* no ISA compatible port */
        r[1] = pci_device_get_config(fPciDev, PCI_INTERRUPT_LINE, 0);
        r[2] = 0x03; /* both bytes of the bus terminated */
        len = 4;
        break;

    case BL_CMD_INQUIRE_SYNC_PERIOD:
        /* asynchronous everywhere */
        len = std::min<int>(fParams[0], BL_REPLY_MAX);
        break;

    case BL_CMD_FETCH_LOCAL_RAM:
        /* the AutoSCSI settings and the BIOS's tables: all cleared */
        len = std::min<int>(fParams[1], BL_REPLY_MAX);
        break;

    case BL_CMD_MODIFY_IO_ADDR:
        /* There is no ISA compatible port to move, and this command alone
           completes without CMDC. */
        fInCommand = false;
        return;

    case BL_CMD_SET_CCB_FORMAT:
        fWideLun = fParams[0] == 1;
        break;
    }

    fReplyLen = len;
    fReplyPos = 0;
    if (fReplyLen == 0) {
        CommandDone();
    }
}


void BusLogicDevice::CommandDone()
{
    fInCommand = false;
    RaiseIntr(BL_INTR_CMDC);
}


void BusLogicDevice::WriteControl(uint8_t val)
{
    if ((val & (BL_CONTROL_RHARD | BL_CONTROL_RSOFT)) != 0) {
        Reset((val & BL_CONTROL_RHARD) != 0);
        return;
    }
    if ((val & BL_CONTROL_RINT) != 0) {
        fIntr = 0;
        UpdateIrq();
        FlushPending();
        if (fImblAgain) {
            fImblAgain = false;
            RaiseIntr(BL_INTR_IMBL);
        }
    }
    if ((val & BL_CONTROL_RSBUS) != 0) {
        ResetBus();
    }
}


//#pragma mark - mailboxes

void BusLogicDevice::ProcessMailboxes()
{
    /* In strict round robin mode only the next mailbox in turn is looked
       at; otherwise every one is, starting there. */
    for (int n = 0; n < fMailboxCount; n++) {
        uint32_t addr = fMailboxAddr + fOutIndex * 8;
        uint8_t mbo[8];
        if (!DmaRead(addr, mbo, sizeof(mbo))) {
            return;
        }
        if (mbo[7] == BL_MBO_FREE) {
            if (fStrictRR) {
                return;
            }
            fOutIndex = (fOutIndex + 1) % fMailboxCount;
            continue;
        }
        uint8_t free = BL_MBO_FREE;
        DmaWrite(addr + 7, &free, 1);
        fOutIndex = (fOutIndex + 1) % fMailboxCount;
        if (fMborEnabled) {
            RaiseIntr(BL_INTR_MBOR);
        }

        uint32_t ccb_addr = get_le32(mbo);
        if (mbo[7] == BL_MBO_START) {
            StartCcb(ccb_addr);
        } else if (mbo[7] == BL_MBO_ABORT) {
            AbortCcb(ccb_addr);
        }
    }
}


void BusLogicDevice::PostInMail(uint32_t ccb_addr, uint8_t btstat,
                                uint8_t sdstat, uint8_t code)
{
    fPending.push_back(InMail {ccb_addr, btstat, sdstat, code});
    FlushPending();
}


/* A completion waits until the driver has emptied the next incoming
   mailbox. */
void BusLogicDevice::FlushPending()
{
    bool posted = false;

    while (!fPending.empty() && fMailboxReady) {
        uint32_t addr = fMailboxAddr + (fMailboxCount + fInIndex) * 8;
        uint8_t mbi[8];
        if (!DmaRead(addr, mbi, sizeof(mbi)) || mbi[7] != BL_MBI_FREE) {
            break;
        }
        const InMail &m = fPending.front();
        put_le32(mbi, m.ccb_addr);
        mbi[4] = m.btstat;
        mbi[5] = m.sdstat;
        mbi[6] = 0;
        mbi[7] = m.code;
        DmaWrite(addr, mbi, sizeof(mbi));
        fPending.pop_front();
        fInIndex = (fInIndex + 1) % fMailboxCount;
        posted = true;
    }
    if (posted) {
        RaiseIntr(BL_INTR_IMBL);
    }
}


void BusLogicDevice::FailCcb(uint32_t ccb_addr, uint8_t btstat)
{
    uint8_t stat[2] = {btstat, 0};
    DmaWrite(ccb_addr + BL_CCB_BTSTAT, stat, sizeof(stat));
    PostInMail(ccb_addr, btstat, 0, BL_MBI_ERROR);
}


/* The data segments of a CCB: one for the direct forms, the list it points
   at for the scatter/gather ones. */
bool BusLogicDevice::ReadSegments(BLCommand *cmd)
{
    uint8_t op = cmd->ccb[BL_CCB_OPCODE];
    uint32_t len = get_le32(cmd->ccb + BL_CCB_DATA_LEN);
    uint32_t addr = get_le32(cmd->ccb + BL_CCB_DATA);
    uint64_t total = 0;

    if (op == BL_OP_INITIATE || op == BL_OP_INITIATE_RETLEN) {
        if (len > 0) {
            cmd->segs.push_back({addr, len});
        }
        total = len;
    } else {
        uint32_t count = len / 8;
        if (count > BL_MAX_SG) {
            return false;
        }
        std::vector<uint8_t> list(count * 8);
        if (count > 0 && !DmaRead(addr, list.data(), count * 8)) {
            return false;
        }
        for (uint32_t i = 0; i < count; i++) {
            uint32_t seg_len = get_le32(&list[i * 8]);
            uint32_t seg_addr = get_le32(&list[i * 8 + 4]);
            if (seg_len > 0) {
                cmd->segs.push_back({seg_addr, seg_len});
            }
            total += seg_len;
        }
    }
    if (total > BL_MAX_TRANSFER) {
        return false;
    }
    cmd->data_len = total;
    return true;
}


void BusLogicDevice::StartCcb(uint32_t ccb_addr)
{
    auto cmd = std::make_unique<BLCommand>(*this);
    uint8_t *ccb = cmd->ccb;

    if (!DmaRead(ccb_addr, ccb, BL_CCB_SIZE)) {
        PostInMail(ccb_addr, BL_BT_INVALID_PARAM, 0, BL_MBI_ERROR);
        return;
    }
    cmd->ccb_addr = ccb_addr;
    cmd->target = ccb[BL_CCB_TARGET];
    uint32_t lun = ccb[BL_CCB_LUN] & (fWideLun ? 0x1f : 0x07);
    uint8_t op = ccb[BL_CCB_OPCODE];

    if (op == BL_OP_BUS_DEVICE_RESET) {
        if (!HasTarget(cmd->target)) {
            FailCcb(ccb_addr, BL_BT_SEL_TIMEOUT);
            return;
        }
        int target = cmd->target;
        for (SCSIDevice *unit : fUnits[target]) {
            if (unit != nullptr) {
                unit->Reset();
            }
        }
        Abort([target](const BLCommand *c) {return c->target == target;},
              BL_BT_HOST_DEV_RESET);
        PostInMail(ccb_addr, BL_BT_OK, 0, BL_MBI_OK);
        return;
    }
    if (op != BL_OP_INITIATE && op != BL_OP_INITIATE_SG &&
        op != BL_OP_INITIATE_RETLEN && op != BL_OP_INITIATE_RETLEN_SG) {
        FailCcb(ccb_addr, BL_BT_INVALID_OPCODE);
        return;
    }
    int cdb_len = ccb[BL_CCB_CDB_LEN];
    if (cdb_len == 0 || cdb_len > BL_CCB_MAX_CDB || !ReadSegments(cmd.get())) {
        FailCcb(ccb_addr, BL_BT_INVALID_PARAM);
        return;
    }
    if (!HasTarget(cmd->target) || cmd->target == BL_SCSI_ID) {
        FailCcb(ccb_addr, BL_BT_SEL_TIMEOUT);
        return;
    }

    SCSIRequest &req = cmd->req;
    memcpy(req.cdb, ccb + BL_CCB_CDB, cdb_len);
    req.cdb_len = cdb_len;
    req.lun = lun;
    cmd->unit = lun < BL_MAX_LUN ? fUnits[cmd->target][lun] : nullptr;

    /* Which way the data goes, when the CCB leaves it to the command. A
       command the unit does not know is taken to read. */
    SCSIDirEnum dir;
    switch (ccb[BL_CCB_DIRECTION] & BL_DIR_MASK) {
    case BL_DIR_IN:
        dir = SCSI_DIR_FROM_DEV;
        break;
    case BL_DIR_OUT:
        dir = SCSI_DIR_TO_DEV;
        break;
    case BL_DIR_NONE:
        dir = SCSI_DIR_NONE;
        break;
    default: {
        uint32_t phase_len;
        if (cmd->unit == nullptr ||
            !cmd->unit->DataPhase(req.cdb, &dir, &phase_len)) {
            dir = SCSI_DIR_FROM_DEV;
        }
        break;
    }
    }
    if (cmd->data_len == 0) {
        dir = SCSI_DIR_NONE;
    }
    if (dir == SCSI_DIR_NONE) {
        cmd->data_len = 0;
    }
    cmd->data_in = dir == SCSI_DIR_FROM_DEV;

    if (cmd->data_len > 0) {
        /* not zeroed, the unit fills what it reports */
        cmd->buf.reset(new uint8_t[cmd->data_len]);
        if (!cmd->data_in) {
            uint32_t off = 0;
            for (const auto &seg : cmd->segs) {
                if (!DmaRead(seg.addr, cmd->buf.get() + off, seg.len)) {
                    FailCcb(ccb_addr, BL_BT_INVALID_PARAM);
                    return;
                }
                off += seg.len;
            }
        }
    }
    req.dir = dir;
    req.buf = cmd->buf.get();
    req.buf_len = cmd->data_len;
    req.completion = cmd.get();

    if (req.cdb[0] == SCSI_REPORT_LUNS) {
        scsi_set_good(&req,
                      scsi_report_luns(cmd->buf.get(),
                                       cmd->data_in ? cmd->data_len : 0,
                                       fUnits[cmd->target], BL_MAX_LUN));
        Finish(cmd.get());
        return;
    }
    if (cmd->unit == nullptr) {
        scsi_no_unit(&req);
        Finish(cmd.get());
        return;
    }

    /* Listed first, as the unit may answer before Submit() returns. */
    BLCommand *c = cmd.get();
    fInFlight.push_back(std::move(cmd));
    if (c->unit->Submit(&c->req)) {
        CommandFinished(c);
    }
}


void BusLogicDevice::AbortCcb(uint32_t ccb_addr)
{
    auto it = std::find_if(fInFlight.begin(), fInFlight.end(),
                           [ccb_addr](const std::unique_ptr<BLCommand> &c) {
                               return c->ccb_addr == ccb_addr;
                           });
    if (it == fInFlight.end()) {
        PostInMail(ccb_addr, BL_BT_OK, 0, BL_MBI_NOT_FOUND);
        return;
    }
    (*it)->unit->Cancel(&(*it)->req);
    fInFlight.erase(it);
    PostInMail(ccb_addr, BL_BT_OK, 0, BL_MBI_ABORTED);
}


std::unique_ptr<BLCommand> BusLogicDevice::TakeInFlight(BLCommand *cmd)
{
    auto it = std::find_if(fInFlight.begin(), fInFlight.end(),
                           [cmd](const std::unique_ptr<BLCommand> &c) {
                               return c.get() == cmd;
                           });
    std::unique_ptr<BLCommand> taken = std::move(*it);
    fInFlight.erase(it);
    return taken;
}


template<typename Match>
void BusLogicDevice::Abort(Match match, uint8_t btstat)
{
    for (auto it = fInFlight.begin(); it != fInFlight.end();) {
        BLCommand *cmd = it->get();
        if (!match(cmd)) {
            ++it;
            continue;
        }
        cmd->unit->Cancel(&cmd->req);
        FailCcb(cmd->ccb_addr, btstat);
        it = fInFlight.erase(it);
    }
}


void BusLogicDevice::CommandFinished(BLCommand *cmd)
{
    std::unique_ptr<BLCommand> done = TakeInFlight(cmd);
    Finish(done.get());
}


/* Writes back the data, the sense data and the status, and posts the
   completion. */
void BusLogicDevice::Finish(BLCommand *cmd)
{
    const SCSIRequest &req = cmd->req;
    uint8_t *ccb = cmd->ccb;
    uint8_t op = ccb[BL_CCB_OPCODE];

    if (req.phase_error) {
        FailCcb(cmd->ccb_addr, BL_BT_DATA_RUN);
        return;
    }

    uint32_t moved = std::min(req.actual_length, cmd->data_len);
    if (cmd->data_in && moved > 0) {
        uint32_t off = 0;
        for (const auto &seg : cmd->segs) {
            if (off >= moved) {
                break;
            }
            uint32_t l = std::min(seg.len, moved - off);
            DmaWrite(seg.addr, cmd->buf.get() + off, l);
            off += l;
        }
    }

    /* Auto sense: a length of 0 means 14 bytes, and 1 means none. */
    int sense_room = ccb[BL_CCB_SENSE_LEN] == 0 ? 14 : ccb[BL_CCB_SENSE_LEN];
    if (req.status == SCSI_STATUS_CHECK_CONDITION && sense_room != 1 &&
        req.sense_len > 0) {
        DmaWrite(get_le32(ccb + BL_CCB_SENSE), req.sense,
                 std::min(sense_room, req.sense_len));
    }

    if (op == BL_OP_INITIATE_RETLEN || op == BL_OP_INITIATE_RETLEN_SG) {
        uint8_t residual[4];
        put_le32(residual, cmd->data_len - moved);
        DmaWrite(cmd->ccb_addr + BL_CCB_DATA_LEN, residual, 4);
    }
    uint8_t stat[2] = {BL_BT_OK, req.status};
    DmaWrite(cmd->ccb_addr + BL_CCB_BTSTAT, stat, sizeof(stat));
    PostInMail(cmd->ccb_addr, BL_BT_OK, req.status,
               req.status == SCSI_STATUS_GOOD ? BL_MBI_OK : BL_MBI_ERROR);
}


//#pragma mark - registers

uint64_t BusLogicDevice::DeviceRead(uint32_t offset, int size_log2)
{
    (void)size_log2;

    switch (offset & 3) {
    case BL_REG_STATUS: {
        uint8_t status = 0;
        if (fDiagReads > 0) {
            fDiagReads--;
            return BL_STATUS_DACT;
        }
        if (!fInCommand) {
            status |= BL_STATUS_HARDY;
        }
        if (!fMailboxReady) {
            status |= BL_STATUS_INREQ;
        }
        if (fReplyPos < fReplyLen) {
            status |= BL_STATUS_DIRRDY;
        }
        if (fCmdInvalid) {
            status |= BL_STATUS_CMDINV;
        }
        return status;
    }
    case BL_REG_DATA:
        if (fReplyPos < fReplyLen) {
            uint8_t val = fReply[fReplyPos++];
            if (fReplyPos == fReplyLen) {
                CommandDone();
            }
            return val;
        }
        return 0;
    case BL_REG_INTR:
        return fIntr;
    case BL_REG_GEOM:
        return 0;
    }
    return 0;
}


void BusLogicDevice::DeviceWrite(uint32_t offset, uint64_t val, int size_log2)
{
    (void)size_log2;

    switch (offset & 3) {
    case BL_REG_CONTROL:
        WriteControl(val);
        break;
    case BL_REG_COMMAND:
        WriteCommand(val);
        break;
    }
}


void BusLogicDevice::SetBar(int bar_num, uint64_t addr, bool enabled)
{
    if (bar_num == 0) {
        fIoRange->SetAddr(addr, enabled);
    } else if (bar_num == 1) {
        fMemRange->SetAddr(addr, enabled);
    }
}


//#pragma mark - SCSIBusTarget

bool BusLogicDevice::FindFreeAddress(int *target, int *lun)
{
    /* An address out of range is left for AttachDevice() to report. */
    if (*target < 0 && *lun < 0) {
        for (int t = 0; t < BL_MAX_TARGET; t++) {
            if (t != BL_SCSI_ID && !HasTarget(t)) {
                *target = t;
                *lun = 0;
                return true;
            }
        }
        return false;
    }
    if (*target < 0) {
        if (*lun >= BL_MAX_LUN) {
            *target = 0;
            return true;
        }
        for (int t = 0; t < BL_MAX_TARGET; t++) {
            if (t != BL_SCSI_ID && fUnits[t][*lun] == nullptr) {
                *target = t;
                return true;
            }
        }
        return false;
    }
    if (*target >= BL_MAX_TARGET) {
        *lun = 0;
        return true;
    }
    for (int l = 0; l < BL_MAX_LUN; l++) {
        if (fUnits[*target][l] == nullptr) {
            *lun = l;
            return true;
        }
    }
    return false;
}


bool BusLogicDevice::AttachDevice(SCSIDevice *dev, uint32_t target,
                                  uint32_t lun)
{
    if (target >= BL_MAX_TARGET || target == BL_SCSI_ID) {
        vm_error("%s: target %u is out of range or the adapter's own (%d)\n",
                 Name(), target, BL_SCSI_ID);
        return false;
    }
    if (lun >= BL_MAX_LUN) {
        vm_error("%s: logical unit %u is out of range\n", Name(), lun);
        return false;
    }
    if (fUnits[target][lun] != nullptr) {
        vm_error("%s: target %u logical unit %u already has a device\n",
                 Name(), target, lun);
        return false;
    }
    fUnits[target][lun] = dev;
    dev->SetLun(lun);
    return true;
}


//#pragma mark - lifecycle

bool BusLogicDevice::Prepare()
{
    if (ParentBus()->AsPCIBus() == nullptr) {
        vm_error("%s: must be attached to a PCI bus\n", Name());
        return false;
    }
    fChildBus = std::make_unique<SCSIBus>(this, this);
    return true;
}


bool BusLogicDevice::Realize()
{
    PCIBus *bus = ParentBus()->AsPCIBus();
    int devio_flags = DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32 |
        DEVIO_DISABLED;

    fPciDev = pci_register_device(bus, Name(), -1, BL_PCI_VENDOR_ID,
                                  BL_PCI_DEVICE_ID, 0x08, BL_PCI_CLASS);
    if (fPciDev == nullptr) {
        vm_error("%s: could not register the PCI function\n", Name());
        return false;
    }
    pci_device_set_config8(fPciDev, PCI_INTERRUPT_PIN, 1);

    PhysMemoryMap *port_map = pci_device_get_port_map(fPciDev);
    if (port_map == nullptr) {
        vm_error("%s: this PCI bus has no I/O port space, which is where a "
                 "BusLogic adapter's registers live\n", Name());
        return false;
    }
    fIoRange = port_map->RegisterDevice(0, BL_IO_BAR_SIZE, this, devio_flags);
    pci_register_bar(fPciDev, 0, BL_IO_BAR_SIZE, PCI_ADDRESS_SPACE_IO, this);
    fMemRange = pci_device_get_mem_map(fPciDev)->RegisterDevice(
        0, BL_MEM_BAR_SIZE, this, devio_flags);
    pci_register_bar(fPciDev, 1, BL_MEM_BAR_SIZE, PCI_ADDRESS_SPACE_MEM, this);
    fIrq = pci_device_get_irq(fPciDev, 0);

    Reset(true);
    return true;
}


//#pragma mark - class

class BusLogicClass final: public DeviceClass {
public:
    BusLogicClass(): DeviceClass("buslogic") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        (void)ctx;
        if (!cfg.HasChildren()) {
            vm_error("buslogic: needs a nested SCSI bus with at least one "
                     "device on it\n");
            return nullptr;
        }
        return new BusLogicDevice(cfg.IdOr("buslogic"));
    }
};

static const BusLogicClass sBusLogicClass;
