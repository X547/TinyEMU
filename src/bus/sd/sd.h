/*
 * SD/MMC/SDIO device model and command requests
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
#pragma once

#include <stdint.h>

#include "device.h"

class BlockDevice;
class BlockDeviceCompletion;

/* The data block every card here moves. Both SD and MMC oblige a card to
   take 512 bytes, and a high capacity card of either kind fixes the length at
   exactly that, so it is also the size of a host controller's block buffer
   and of the largest register a card hands over the data lines (the extended
   CSD). */
#define SD_BLOCK_SIZE 512

/* A response is either the short form, of which only the 32 bit payload
   survives, or the long form, which carries a whole 128 bit card register
   including the CRC byte that ends it. A card hands the bytes back in the
   order they go on the wire, most significant first, and the host controller
   lays them out the way its own response registers want them. */
#define SD_RESPONSE_SHORT 4
#define SD_RESPONSE_LONG  16

/* The card registers that travel as a long response, or over the data lines
   in the extended CSD's case. */
#define SD_CID_SIZE     16
#define SD_CSD_SIZE     16
#define SD_SCR_SIZE      8
#define SD_STATUS_SIZE  64  /* SD status, over the data lines */
#define SD_SWITCH_SIZE  64  /* CMD6 switch status, over the data lines */
#define MMC_EXT_CSD_SIZE 512


/* Commands in the basic and block oriented classes, numbered as both SD and
   MMC number them. The two protocols agree on most of these and differ on the
   few marked below, which is why a card decodes the index itself rather than
   being handed a decoded request. */
#define SD_CMD_GO_IDLE_STATE        0
#define MMC_CMD_SEND_OP_COND        1  /* MMC only; SD answers nothing */
#define SD_CMD_ALL_SEND_CID         2
#define SD_CMD_SEND_RELATIVE_ADDR   3  /* MMC: the host assigns the address */
#define SD_CMD_SET_DSR              4
#define SDIO_CMD_SEND_OP_COND       5  /* MMC: sleep/awake */
#define SD_CMD_SWITCH_FUNC          6  /* MMC: SWITCH, writes the ext CSD */
#define SD_CMD_SELECT_CARD          7
#define SD_CMD_SEND_IF_COND         8  /* MMC: SEND_EXT_CSD, over the data */
#define SD_CMD_SEND_CSD             9
#define SD_CMD_SEND_CID            10
#define SD_CMD_STOP_TRANSMISSION   12
#define SD_CMD_SEND_STATUS         13
#define SD_CMD_GO_INACTIVE_STATE   15
#define SD_CMD_SET_BLOCKLEN        16
#define SD_CMD_READ_SINGLE_BLOCK   17
#define SD_CMD_READ_MULTIPLE_BLOCK 18
#define SD_CMD_SEND_TUNING_BLOCK   19
#define SD_CMD_SET_BLOCK_COUNT     23
#define SD_CMD_WRITE_BLOCK         24
#define SD_CMD_WRITE_MULTIPLE_BLOCK 25
#define SD_CMD_PROGRAM_CSD         27
#define SD_CMD_SET_WRITE_PROT      28
#define SD_CMD_CLR_WRITE_PROT      29
#define SD_CMD_SEND_WRITE_PROT     30
#define SD_CMD_ERASE_WR_BLK_START  32
#define SD_CMD_ERASE_WR_BLK_END    33
#define MMC_CMD_ERASE_GROUP_START  35
#define MMC_CMD_ERASE_GROUP_END    36
#define SD_CMD_ERASE               38
#define SD_CMD_LOCK_UNLOCK         42
#define SDIO_CMD_IO_RW_DIRECT      52
#define SDIO_CMD_IO_RW_EXTENDED    53
#define SD_CMD_APP_CMD             55
#define SD_CMD_GEN_CMD             56

/* Application commands, which only mean this after a CMD55. */
#define SD_ACMD_SET_BUS_WIDTH           6
#define SD_ACMD_SD_STATUS              13
#define SD_ACMD_SEND_NUM_WR_BLOCKS     22
#define SD_ACMD_SET_WR_BLK_ERASE_COUNT 23
#define SD_ACMD_SD_SEND_OP_COND        41
#define SD_ACMD_SET_CLR_CARD_DETECT    42
#define SD_ACMD_SEND_SCR               51


/* Card status, as a short response carries it and as SEND_STATUS reports it.
   The error bits are sticky: they are cleared when they are read. */
#define SD_STATUS_OUT_OF_RANGE      (1u << 31)
#define SD_STATUS_ADDRESS_ERROR     (1u << 30)
#define SD_STATUS_BLOCK_LEN_ERROR   (1u << 29)
#define SD_STATUS_ERASE_SEQ_ERROR   (1u << 28)
#define SD_STATUS_ERASE_PARAM       (1u << 27)
#define SD_STATUS_WP_VIOLATION      (1u << 26)
#define SD_STATUS_CARD_IS_LOCKED    (1u << 25)
#define SD_STATUS_LOCK_UNLOCK_FAILED (1u << 24)
#define SD_STATUS_COM_CRC_ERROR     (1u << 23)
#define SD_STATUS_ILLEGAL_COMMAND   (1u << 22)
#define SD_STATUS_CARD_ECC_FAILED   (1u << 21)
#define SD_STATUS_CC_ERROR          (1u << 20)
#define SD_STATUS_ERROR             (1u << 19)
#define SD_STATUS_CSD_OVERWRITE     (1u << 16)
#define SD_STATUS_WP_ERASE_SKIP     (1u << 15)
#define SD_STATUS_ERASE_RESET       (1u << 13)
#define SD_STATUS_CURRENT_STATE_SHIFT 9
#define SD_STATUS_CURRENT_STATE_MASK  (0xfu << SD_STATUS_CURRENT_STATE_SHIFT)
#define SD_STATUS_READY_FOR_DATA    (1u << 8)
#define MMC_STATUS_SWITCH_ERROR     (1u << 7)
#define SD_STATUS_APP_CMD           (1u << 5)
#define SD_STATUS_AKE_SEQ_ERROR     (1u << 3)

/* Every bit that says something went wrong, so that one mask decides whether
   a response reports an error at all. */
#define SD_STATUS_ERROR_MASK \
    (SD_STATUS_OUT_OF_RANGE | SD_STATUS_ADDRESS_ERROR | \
     SD_STATUS_BLOCK_LEN_ERROR | SD_STATUS_ERASE_SEQ_ERROR | \
     SD_STATUS_ERASE_PARAM | SD_STATUS_WP_VIOLATION | \
     SD_STATUS_LOCK_UNLOCK_FAILED | SD_STATUS_COM_CRC_ERROR | \
     SD_STATUS_ILLEGAL_COMMAND | SD_STATUS_CARD_ECC_FAILED | \
     SD_STATUS_CC_ERROR | SD_STATUS_ERROR | SD_STATUS_CSD_OVERWRITE | \
     SD_STATUS_WP_ERASE_SKIP | SD_STATUS_ERASE_RESET | \
     MMC_STATUS_SWITCH_ERROR | SD_STATUS_AKE_SEQ_ERROR)


/* The operation conditions register, which is what an initialisation command
   answers with. */
#define SD_OCR_VOLTAGE_WINDOW   0x00ff8000u /* 2.7 V to 3.6 V */
#define SD_OCR_S18A             (1u << 24)  /* 1.8 V signalling accepted */
#define SD_OCR_CCS              (1u << 30)  /* SD: high capacity */
#define MMC_OCR_SECTOR_MODE     (2u << 29)  /* MMC: sector addressing */
#define MMC_OCR_ACCESS_MASK     (3u << 29)
#define SD_OCR_POWER_UP_DONE    (1u << 31)  /* clear while still busy */


/* The card state machine, as the current state field of a response reports
   it. A memory card here never reaches disconnect, which only a card that was
   deselected mid-program can be in. */
typedef enum {
    SD_STATE_IDLE = 0,
    SD_STATE_READY = 1,
    SD_STATE_IDENT = 2,
    SD_STATE_STBY = 3,
    SD_STATE_TRAN = 4,
    SD_STATE_DATA = 5,
    SD_STATE_RCV = 6,
    SD_STATE_PRG = 7,
    SD_STATE_DIS = 8,
} SDCardStateEnum;


/* Which protocol a card speaks. The host controller uses it to describe the
   slot -- an embedded device is soldered down and may have eight data lines,
   a card in a slot has four and can be taken out -- and nothing else. */
typedef enum {
    SD_CARD_SD,
    SD_CARD_MMC,
    SD_CARD_SDIO,
} SDCardTypeEnum;


/* Which way the card wants to move data once a command has been answered.
   The host controller is told the same thing by its own transfer mode
   register; a disagreement between the two is the guest's mistake, and the
   card's view is the one that decides what actually happens. */
typedef enum {
    SD_DATA_NONE,
    SD_DATA_READ,  /* the card sends */
    SD_DATA_WRITE, /* the card receives */
} SDDataDirEnum;


/* How a block transfer ended. SD_XFER_PENDING is not an outcome but a
   promise: the card took the block and will report the real outcome later
   through the completion it was handed. */
typedef enum {
    SD_XFER_OK,
    SD_XFER_PENDING,
    SD_XFER_ERROR,
} SDXferStatusEnum;


/* How a card hands back a block it moved asynchronously. */
class SDDataCompletion {
public:
    virtual ~SDDataCompletion() = default;

    virtual void DataComplete(bool ok) = 0;
};


/* Implemented by the host controller, so that a card can raise the interrupt
   an SDIO function signals on DAT[1] outside any command of its own. */
class SDHost {
public:
    virtual ~SDHost() = default;

    virtual void SetCardInterrupt(bool level) = 0;
};


/* One command on the command line: the index and its argument, which is all a
   card is given. Whether the command is an application command is the card's
   own business, because only the card knows whether the CMD55 that would make
   it one was addressed to it. */
struct SDCommand {
    uint8_t index = 0;
    uint32_t arg = 0;
};


/* A card on an SD, MMC or SDIO bus.

   Command() answers on the command line and, for a command that moves data,
   leaves the card in a state where DataDir() says which way it goes; the host
   controller then moves the data one block at a time. Splitting it that way
   is what lets one card model work behind any host controller: how the block
   reaches the guest -- a programmed I/O buffer, a scatter list, a single DMA
   address -- is entirely the controller's business. */
class SDDevice {
private:
    const char *fName;
    SDHost *fHost = nullptr;

protected:
    /* Raise or drop the SDIO interrupt line. Does nothing until the card has
       been attached to a controller. */
    void SetCardInterrupt(bool level)
    {
        if (fHost != nullptr) {
            fHost->SetCardInterrupt(level);
        }
    }

public:
    SDDevice(const char *name): fName(name) {}
    virtual ~SDDevice() = default;

    const char *Name() const {return fName;}

    /* Called by the controller as it attaches the card. */
    void SetHost(SDHost *host) {fHost = host;}

    virtual SDCardTypeEnum CardType() const = 0;

    /* How many data lines the card has, and whether it can be taken out of
       the slot. Both only describe the card to the guest. */
    virtual int BusWidth() const = 0;
    virtual bool Removable() const = 0;
    virtual bool ReadOnly() const = 0;

    /* Drops everything the host programmed, as powering the bus down does. */
    virtual void Reset() = 0;

    /* Execute one command. Writes the response bytes, most significant
       first, into 'response' and returns SD_RESPONSE_SHORT or
       SD_RESPONSE_LONG; 0 for a command that is answered by silence, and -1
       when the card does not answer at all, which is what a host reads as a
       command timeout and is how it discovers that a card is not there or
       does not speak this protocol. */
    virtual int Command(const SDCommand &cmd, uint8_t *response) = 0;

    virtual SDDataDirEnum DataDir() const = 0;

    /* Move one block. 'len' is the host's block length, which the card
       checks against its own. A card answering SD_XFER_PENDING calls the
       completion once, later, and must not be asked for another block until
       it has. */
    virtual SDXferStatusEnum ReadBlock(uint8_t *buf, uint32_t len,
                                       SDDataCompletion *completion) = 0;
    virtual SDXferStatusEnum WriteBlock(const uint8_t *buf, uint32_t len,
                                        SDDataCompletion *completion) = 0;

    /* Abandon a transfer in progress: the host sent CMD12, or reset the data
       line. A block already with the block back end cannot be recalled, so
       the card disowns it instead. */
    virtual void StopTransfer() = 0;
};


/* A memory card, of either kind. The two protocols share the state machine,
   the block addressing, the block back end and the way a register is handed
   over the data lines, and differ in their command sets and in the layout of
   the registers themselves -- which is exactly the split between this and its
   two subclasses. */
class SDMemoryCard: public SDDevice {
private:
    /* One block request may be outstanding: the host controller has a single
       block buffer and will not ask for the next block until this one has
       been moved. */
    class Completion;
    friend class Completion;

    BlockDevice *fBlockDev;
    bool fReadOnly;

    Completion *fCompletion = nullptr;
    SDDataCompletion *fPendingCompletion = nullptr;
    bool fPendingIsRead = false;

    void BlockDone(int ret);

protected:
    SDCardStateEnum fState = SD_STATE_IDLE;
    uint32_t fRca = 0;
    /* The sticky error bits a failed command left behind, reported with the
       next response and cleared by it. */
    uint32_t fStatus = 0;
    /* Set by CMD55, consumed by the command after it. */
    bool fAppCmd = false;
    /* The block length the host set with SET_BLOCKLEN. Both card types here
       say in their CSD that the length is 512 bytes and that partial blocks
       are not offered, so this only ever holds that, and a command asking
       for anything else is refused. */
    uint32_t fBlockLen = SD_BLOCK_SIZE;
    bool fHighCapacity = false;
    uint64_t fBlockCount = 0; /* 512 byte blocks the medium holds */

    /* The transfer a read or write command started. */
    SDDataDirEnum fDataDir = SD_DATA_NONE;
    uint64_t fDataSector = 0;
    bool fDataMultiple = false;
    /* Blocks left of a length the host fixed with CMD23, or 0 when the
       transfer runs until it is stopped. */
    uint32_t fDataBlocksLeft = 0;

    /* A reply the card produces from a register rather than from the medium:
       the switch status, the SD status, the SCR, the extended CSD. It travels
       over the data lines like any block, so it shares the data path. */
    uint8_t fRegData[SD_BLOCK_SIZE] {};
    uint32_t fRegDataLen = 0;

    /* The identity the guest reads back. Filled in by the subclass, which
       knows the layout its protocol defines. */
    uint8_t fCid[SD_CID_SIZE] {};
    uint8_t fCsd[SD_CSD_SIZE] {};

    /* The card status as it stands, without disturbing it. */
    uint32_t CurrentStatus() const;
    /* Forget the sticky error bits, which a response reporting them does. */
    void ClearStatus() {fStatus = 0;}

    /* Build the short response nearly every command answers with, and clear
       the sticky error bits it just reported. */
    void BuildR1(uint8_t *response);
    /* Write a 32 bit payload into a short response. */
    static void BuildShort(uint8_t *response, uint32_t value);
    /* Write a 128 bit register into a long response, ending it with the CRC
       a real card computes over it. */
    static void BuildLong(uint8_t *response, const uint8_t *reg);

    /* Record an error the next response will report. */
    void Fail(uint32_t bits) {fStatus |= bits;}

    /* Start a transfer of the medium at 'sector'. */
    void StartDataTransfer(SDDataDirEnum dir, uint64_t sector, bool multiple);
    /* Start handing over a register that travels on the data lines. The
       bytes must already be in fRegData. */
    void StartRegisterRead(uint32_t len);

    /* Give up whatever transfer is running and go back to the transfer
       state, as the end of a transfer and STOP_TRANSMISSION both do. */
    void EndDataTransfer();

    /* Account for one block having been moved, ending the transfer when the
       count the host fixed runs out or the medium does. */
    void AdvanceBlock();

    /* Translate a command argument into a block number, checking it against
       the medium. Returns false and records the error when it is out of
       range or, on a standard capacity card, not block aligned. */
    bool SectorFromArg(uint32_t arg, uint64_t *sector_out);

    bool HasPending() const {return fPendingCompletion != nullptr;}

public:
    SDMemoryCard(const char *name, BlockDevice *bs, bool read_only);
    ~SDMemoryCard() override;

    BlockDevice *Backend() const {return fBlockDev;}
    uint64_t BlockCount() const {return fBlockCount;}

    bool ReadOnly() const override {return fReadOnly;}
    bool Removable() const override {return true;}

    void Reset() override;

    SDDataDirEnum DataDir() const override {return fDataDir;}

    SDXferStatusEnum ReadBlock(uint8_t *buf, uint32_t len,
                               SDDataCompletion *completion) override;
    SDXferStatusEnum WriteBlock(const uint8_t *buf, uint32_t len,
                                SDDataCompletion *completion) override;
    void StopTransfer() override;
};


/* Implemented by the host controller, which owns the card attached to it. */
class SDBusTarget {
public:
    virtual ~SDBusTarget() = default;

    /* A slot carries one card; a second one is reported rather than
       silently ignored. */
    virtual bool AttachDevice(SDDevice *dev) = 0;
};


/* The bus a host controller provides. Like the USB and SCSI buses what it
   hands out is a place on a bus, not host address space, so it assigns no
   resource records. */
class SDBus final: public Bus {
private:
    SDBusTarget *fTarget;

public:
    SDBus(Device *owner, SDBusTarget *target):
        Bus(owner), fTarget(target) {}

    const char *Type() const override {return "sd";}
    SDBusTarget *Target() const {return fTarget;}

    bool AssignResources(Device *dev) override;
};


/* Attaches one card to the bus it was declared on, the way USBDeviceNode
   attaches a USB device. */
class SDDeviceNode final: public Device {
private:
    SDDevice *fDev;

public:
    SDDeviceNode(const char *name, SDDevice *dev);
    ~SDDeviceNode() override;

    SDDevice *Dev() const {return fDev;}

    bool Realize() override;
};


/* The CRC a card appends to a command response, over the bytes that precede
   it. The host controller checks it in hardware and never shows it to the
   guest, so it exists here only so that a long response carries the byte a
   real card would have put there. */
uint8_t sd_crc7(const uint8_t *data, int len);

/* Set the bits 'hi' down to 'lo' of a 'size' byte register held most
   significant byte first. The CID, the CSD, the SCR and the SD status are all
   tabulated in the specifications as bit ranges of one big register, and this
   is what lets those tables be transcribed the way they are numbered. */
void sd_reg_set_bits(uint8_t *reg, int size, int hi, int lo, uint64_t value);


/* Each card type builds its own node, so the factory in devices.cpp stays a
   table of names. */

/* sd_card.cpp */
Device *sd_card_node_create(BlockDevice *bs, bool read_only);

/* mmc_card.cpp */
Device *mmc_card_node_create(BlockDevice *bs, bool read_only);
