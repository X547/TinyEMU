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
#include "sd.h"

#include <string.h>

#include "machine.h"
#include "virtio.h"


//#pragma mark - CRC

uint8_t sd_crc7(const uint8_t *data, int len)
{
    /* CRC-7 over x^7 + x^3 + 1, computed most significant bit first in a
       register whose seven bits are left aligned, which is how they sit in
       the byte that ends a response. The polynomial is therefore the shifted
       0x12 rather than the 0x09 of a right aligned register. */
    uint8_t crc = 0;

    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int bit = 0; bit < 8; bit++) {
            if ((crc & 0x80) != 0) {
                crc = (uint8_t)((crc << 1) ^ 0x12);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc >> 1;
}


void sd_reg_set_bits(uint8_t *reg, int size, int hi, int lo, uint64_t value)
{
    for (int bit = lo; bit <= hi; bit++) {
        int byte = size - 1 - bit / 8;
        uint8_t mask = (uint8_t)(1u << (bit % 8));
        if (((value >> (bit - lo)) & 1) != 0) {
            reg[byte] |= mask;
        } else {
            reg[byte] &= (uint8_t)~mask;
        }
    }
}


//#pragma mark - SDMemoryCard

/* The block back end answers through this when it takes a request
   asynchronously; the card holds the block until it does. */
class SDMemoryCard::Completion final: public BlockDeviceCompletion {
private:
    SDMemoryCard &fCard;

public:
    Completion(SDMemoryCard &card): fCard(card) {}

    void Complete(int ret) override {fCard.BlockDone(ret);}
};


SDMemoryCard::SDMemoryCard(const char *name, std::unique_ptr<BlockDevice> bs,
                           bool read_only):
    SDDevice(name), fBlockDev(std::move(bs)), fReadOnly(read_only)
{
    fCompletion = std::make_unique<Completion>(*this);
    int64_t sectors = fBlockDev->SectorCount();
    fBlockCount = sectors > 0 ? (uint64_t)sectors : 0;
}


SDMemoryCard::~SDMemoryCard() = default;


void SDMemoryCard::Reset()
{
    /* A block already with the back end cannot be recalled, so it is
       disowned: BlockDone() drops a completion whose card has moved on. */
    fPendingCompletion = nullptr;
    fState = SD_STATE_IDLE;
    fRca = 0;
    fStatus = 0;
    fAppCmd = false;
    fBlockLen = SD_BLOCK_SIZE;
    fDataDir = SD_DATA_NONE;
    fDataSector = 0;
    fDataMultiple = false;
    fDataBlocksLeft = 0;
    fRegDataLen = 0;
}


//#pragma mark - responses

void SDMemoryCard::BuildShort(uint8_t *response, uint32_t value)
{
    response[0] = value >> 24;
    response[1] = value >> 16;
    response[2] = value >> 8;
    response[3] = value;
}


void SDMemoryCard::BuildLong(uint8_t *response, const uint8_t *reg)
{
    /* The register's own last byte is the CRC of the 120 bits before it and
       the end bit, so the subclass fills in fifteen bytes and this completes
       the sixteenth exactly as the card's shift register would. */
    memcpy(response, reg, SD_CID_SIZE - 1);
    response[SD_CID_SIZE - 1] = (sd_crc7(response, SD_CID_SIZE - 1) << 1) | 1;
}


uint32_t SDMemoryCard::CurrentStatus() const
{
    uint32_t status = fStatus;

    status |= (uint32_t)fState << SD_STATUS_CURRENT_STATE_SHIFT;
    /* Nothing here ever needs the host to wait for a program cycle, so the
       card is always ready for the next block. */
    status |= SD_STATUS_READY_FOR_DATA;
    if (fAppCmd) {
        status |= SD_STATUS_APP_CMD;
    }
    if ((status & SD_STATUS_ERROR_MASK) != 0) {
        status |= SD_STATUS_ERROR;
    }
    return status;
}


void SDMemoryCard::BuildR1(uint8_t *response)
{
    BuildShort(response, CurrentStatus());

    /* The error bits are reported once and then cleared, which is what makes
       a SEND_STATUS after a failure the way a driver finds out what went
       wrong. */
    ClearStatus();
}


//#pragma mark - data transfers

void SDMemoryCard::StartDataTransfer(SDDataDirEnum dir, uint64_t sector,
                                     bool multiple)
{
    fDataDir = dir;
    fDataSector = sector;
    fDataMultiple = multiple;
    fRegDataLen = 0;
    fState = dir == SD_DATA_READ ? SD_STATE_DATA : SD_STATE_RCV;
}


void SDMemoryCard::StartRegisterRead(uint32_t len)
{
    if (len > SD_BLOCK_SIZE) {
        len = SD_BLOCK_SIZE;
    }
    fDataDir = SD_DATA_READ;
    fDataMultiple = false;
    fDataBlocksLeft = 0;
    fRegDataLen = len;
    fState = SD_STATE_DATA;
}


void SDMemoryCard::EndDataTransfer()
{
    fDataDir = SD_DATA_NONE;
    fDataMultiple = false;
    fDataBlocksLeft = 0;
    fRegDataLen = 0;
    if (fState == SD_STATE_DATA || fState == SD_STATE_RCV ||
        fState == SD_STATE_PRG) {
        fState = SD_STATE_TRAN;
    }
}


void SDMemoryCard::AdvanceBlock()
{
    fDataSector++;
    if (!fDataMultiple) {
        EndDataTransfer();
        return;
    }
    /* A count the host fixed with SET_BLOCK_COUNT ends the transfer by
       itself; without one it runs until STOP_TRANSMISSION, or until it walks
       off the end of the medium, which is an error the next response
       reports. */
    if (fDataBlocksLeft > 0 && --fDataBlocksLeft == 0) {
        EndDataTransfer();
        return;
    }
    if (fDataSector >= fBlockCount) {
        Fail(SD_STATUS_OUT_OF_RANGE);
        EndDataTransfer();
    }
}


bool SDMemoryCard::SectorFromArg(uint32_t arg, uint64_t *sector_out)
{
    uint64_t sector;

    if (fHighCapacity) {
        sector = arg;
    } else {
        /* A standard capacity card is addressed in bytes. Its CSD says the
           block length is 512 and that misaligned access is not allowed, so
           an argument that is not a multiple of it is an address error
           rather than something to round. */
        if ((arg & (SD_BLOCK_SIZE - 1)) != 0) {
            Fail(SD_STATUS_ADDRESS_ERROR);
            return false;
        }
        sector = arg / SD_BLOCK_SIZE;
    }
    if (sector >= fBlockCount) {
        Fail(SD_STATUS_OUT_OF_RANGE | SD_STATUS_ADDRESS_ERROR);
        return false;
    }
    *sector_out = sector;
    return true;
}


SDXferStatusEnum SDMemoryCard::ReadBlock(uint8_t *buf, uint32_t len,
                                         SDDataCompletion *completion)
{
    if (fDataDir != SD_DATA_READ || HasPending()) {
        return SD_XFER_ERROR;
    }

    if (fRegDataLen > 0) {
        /* A register rather than the medium: one block, and the transfer is
           over. A host that sized its block differently gets what fits and
           zeroes for the rest, which is what a card that clocks out a fixed
           number of bits does. */
        uint32_t n = len < fRegDataLen ? len : fRegDataLen;
        memcpy(buf, fRegData, n);
        if (len > n) {
            memset(buf + n, 0, len - n);
        }
        EndDataTransfer();
        return SD_XFER_OK;
    }

    if (len != SD_BLOCK_SIZE) {
        Fail(SD_STATUS_BLOCK_LEN_ERROR);
        EndDataTransfer();
        return SD_XFER_ERROR;
    }

    fPendingCompletion = completion;
    fPendingIsRead = true;
    int ret = fBlockDev->ReadAsync(fDataSector, buf, 1, fCompletion.get());
    if (ret > 0) {
        return SD_XFER_PENDING;
    }
    fPendingCompletion = nullptr;
    if (ret < 0) {
        Fail(SD_STATUS_CARD_ECC_FAILED);
        EndDataTransfer();
        return SD_XFER_ERROR;
    }
    AdvanceBlock();
    return SD_XFER_OK;
}


SDXferStatusEnum SDMemoryCard::WriteBlock(const uint8_t *buf, uint32_t len,
                                          SDDataCompletion *completion)
{
    if (fDataDir != SD_DATA_WRITE || HasPending()) {
        return SD_XFER_ERROR;
    }
    if (fReadOnly) {
        Fail(SD_STATUS_WP_VIOLATION);
        EndDataTransfer();
        return SD_XFER_ERROR;
    }
    if (len != SD_BLOCK_SIZE) {
        Fail(SD_STATUS_BLOCK_LEN_ERROR);
        EndDataTransfer();
        return SD_XFER_ERROR;
    }

    fPendingCompletion = completion;
    fPendingIsRead = false;
    int ret = fBlockDev->WriteAsync(fDataSector, buf, 1, fCompletion.get());
    if (ret > 0) {
        return SD_XFER_PENDING;
    }
    fPendingCompletion = nullptr;
    if (ret < 0) {
        Fail(SD_STATUS_CC_ERROR);
        EndDataTransfer();
        return SD_XFER_ERROR;
    }
    AdvanceBlock();
    return SD_XFER_OK;
}


void SDMemoryCard::BlockDone(int ret)
{
    SDDataCompletion *completion = fPendingCompletion;

    if (completion == nullptr) {
        /* The transfer was stopped or the card was reset while the block was
           with the back end; nobody is waiting for it any more. */
        return;
    }
    fPendingCompletion = nullptr;

    if (ret < 0) {
        Fail(fPendingIsRead ? SD_STATUS_CARD_ECC_FAILED : SD_STATUS_CC_ERROR);
        EndDataTransfer();
    } else {
        AdvanceBlock();
    }
    completion->DataComplete(ret >= 0);
}


void SDMemoryCard::StopTransfer()
{
    fPendingCompletion = nullptr;
    EndDataTransfer();
}


//#pragma mark - SDBus

bool SDBus::AssignResources(Device *dev)
{
    /* A card is reached through its host controller, so it holds no host
       address space and no interrupt line of its own. */
    for (int i = 0; i < dev->ResourceCount(); i++) {
        if (dev->ResourceAt(i)->type != RES_NONE) {
            vm_error("sd bus: device '%s' declared a resource, but a card has "
                     "none of its own\n", dev->Name());
            return false;
        }
    }
    return true;
}


//#pragma mark - SDDeviceNode

SDDeviceNode::SDDeviceNode(const char *name, std::unique_ptr<SDDevice> dev):
    Device(name), fDev(std::move(dev))
{
}


SDDeviceNode::~SDDeviceNode() = default;


bool SDDeviceNode::Realize()
{
    SDBus *bus = dynamic_cast<SDBus *>(ParentBus());
    if (bus == nullptr) {
        vm_error("%s: must be attached to an SD bus\n", Name());
        return false;
    }
    return bus->Target()->AttachDevice(fDev.get());
}
