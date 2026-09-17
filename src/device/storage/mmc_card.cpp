/*
 * eMMC / MultiMediaCard storage device
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
#include <string.h>

#include "bits.h"
#include "cutils.h"
#include "machine.h"
#include "sd.h"
#include "virtio.h"

/* The identity the guest reads out of the card registers. */
#define MMC_CARD_MANFID   0x74   /* not an assigned identifier */
#define MMC_CARD_OEMID    0x5445 /* "TE" */
#define MMC_CARD_PRODUCT  "TEMMC0" /* six characters */
#define MMC_CARD_REVISION 0x10
#define MMC_CARD_SERIAL   0x54454d4d
#define MMC_CARD_YEAR     2024
#define MMC_CARD_MONTH    1

/* Command classes the device claims. Class 5, erase, is deliberately absent:
   nothing here erases, and a device that does not claim the class is one a
   driver never asks to. */
#define MMC_CARD_CCC 0x0d5

#define MMC_TRAN_SPEED_26MHZ 0x32
#define MMC_TRAN_SPEED_52MHZ 0x5a

/* Above two gigabytes a device is addressed in sectors rather than bytes,
   because that is all the CSD's capacity fields can describe. */
#define MMC_BYTE_MAX_BLOCKS (2ull * 1024 * 1024 * 1024 / SD_BLOCK_SIZE)

/* Extended CSD byte offsets, as the specification numbers them. Everything
   from 192 up is read only; below that is the modes segment, which SWITCH
   writes one byte of at a time. */
#define MMC_EXT_CSD_MODES_END        192
#define MMC_EXT_CSD_PARTITION_SUPPORT 160
#define MMC_EXT_CSD_ERASE_GROUP_DEF  175
#define MMC_EXT_CSD_PART_CONFIG      179
#define MMC_EXT_CSD_ERASED_MEM_CONT  181
#define MMC_EXT_CSD_BUS_WIDTH        183
#define MMC_EXT_CSD_HS_TIMING        185
#define MMC_EXT_CSD_REV              192
#define MMC_EXT_CSD_STRUCTURE        194
#define MMC_EXT_CSD_CARD_TYPE        196
#define MMC_EXT_CSD_PART_SWITCH_TIME 199
#define MMC_EXT_CSD_SEC_COUNT        212 /* four bytes, least significant
                                            first */
#define MMC_EXT_CSD_S_A_TIMEOUT      217
#define MMC_EXT_CSD_HC_WP_GRP_SIZE   221
#define MMC_EXT_CSD_REL_WR_SEC_C     222
#define MMC_EXT_CSD_ERASE_TIMEOUT_MULT 223
#define MMC_EXT_CSD_HC_ERASE_GRP_SIZE 224
#define MMC_EXT_CSD_BOOT_MULT        226
#define MMC_EXT_CSD_SEC_FEATURE      231
#define MMC_EXT_CSD_GENERIC_CMD6_TIME 248
#define MMC_EXT_CSD_S_CMD_SET        504

/* Card types the device reports it can be clocked at. */
#define MMC_CARD_TYPE_26MHZ bit_at(0)
#define MMC_CARD_TYPE_52MHZ bit_at(1)

/* SWITCH argument. */
#define MMC_SWITCH_ACCESS_SHIFT 24
#define  MMC_SWITCH_CMD_SET     0
#define  MMC_SWITCH_SET_BITS    1
#define  MMC_SWITCH_CLEAR_BITS  2
#define  MMC_SWITCH_WRITE_BYTE  3
#define MMC_SWITCH_INDEX_SHIFT  16
#define MMC_SWITCH_VALUE_SHIFT  8


//#pragma mark - MMCCard

class MMCCard final: public SDMemoryCard {
private:
    bool fHighSpeed = false;
    uint32_t fNextBlockCount = 0;
    uint8_t fExtCsd[MMC_EXT_CSD_SIZE] {};

    void BuildCid();
    void BuildCsd();
    void BuildExtCsd();

    int Illegal(uint8_t *response);
    int Switch(const SDCommand &cmd, uint8_t *response);
    int ReadWrite(const SDCommand &cmd, uint8_t *response, bool is_write,
                  bool multiple);

public:
    MMCCard(std::unique_ptr<HostBlockDevice> bs, bool read_only);

    SDCardTypeEnum CardType() const override {return SD_CARD_MMC;}
    /* An embedded device is soldered down, with all eight data lines wired
       and no way to take it out. */
    int BusWidth() const override {return 8;}
    bool Removable() const override {return false;}

    void Reset() override;
    int Command(const SDCommand &cmd, uint8_t *response) override;
};


MMCCard::MMCCard(std::unique_ptr<HostBlockDevice> bs, bool read_only):
    SDMemoryCard("mmc-card", std::move(bs), read_only)
{
    /* The CSD's capacity fields stop at two gigabytes, so a device larger
       than that is addressed in sectors and reports its real size in the
       extended CSD instead. */
    fHighCapacity = BlockCount() > MMC_BYTE_MAX_BLOCKS;
    BuildCid();
    BuildCsd();
    BuildExtCsd();
}


void MMCCard::Reset()
{
    SDMemoryCard::Reset();
    fHighSpeed = false;
    fNextBlockCount = 0;
    BuildCsd();
    BuildExtCsd();
}


//#pragma mark - device registers

void MMCCard::BuildCid()
{
    uint8_t *cid = fCid;

    memset(cid, 0, SD_CID_SIZE);
    cid[0] = MMC_CARD_MANFID;
    cid[1] = MMC_CARD_OEMID >> 8;
    cid[2] = MMC_CARD_OEMID & 0xff;
    memcpy(cid + 3, MMC_CARD_PRODUCT, 6);
    cid[9] = MMC_CARD_REVISION;
    cid[10] = (uint8_t)(MMC_CARD_SERIAL >> 24);
    cid[11] = (uint8_t)(MMC_CARD_SERIAL >> 16);
    cid[12] = (uint8_t)(MMC_CARD_SERIAL >> 8);
    cid[13] = (uint8_t)MMC_CARD_SERIAL;
    /* The manufacturing date is a month and a year counted from 1997. */
    cid[14] = (uint8_t)((MMC_CARD_MONTH << 4) | ((MMC_CARD_YEAR - 1997) & 0xf));
}


void MMCCard::BuildCsd()
{
    uint8_t *csd = fCsd;
    uint8_t speed = fHighSpeed ? MMC_TRAN_SPEED_52MHZ : MMC_TRAN_SPEED_26MHZ;

    memset(csd, 0, SD_CSD_SIZE);

    /* A device whose real size does not fit says so with the largest value
       the fields hold, and the extended CSD carries the truth. */
    uint32_t read_bl_len = 9;
    uint64_t units = BlockCount();
    uint64_t csize;
    int mult;

    if (fHighCapacity) {
        csize = 0xfff;
        mult = 7;
        read_bl_len = 10;
    } else {
        if (units > 2097152) {
            read_bl_len = 10;
            units /= 2;
        }
        mult = 0;
        while (mult < 7 && (units >> (mult + 2)) > 4096) {
            mult++;
        }
        csize = units >> (mult + 2);
        if (csize == 0) {
            csize = 1;
        }
        if (csize > 4096) {
            csize = 4096;
        }
        csize--;
        /* Below the two gigabyte line the CSD is the only description of the
           capacity, so the device holds exactly what it says it does. */
        fBlockCount = ((csize + 1) << (mult + 2)) << (read_bl_len - 9);
    }

    sd_reg_set_bits(csd, SD_CSD_SIZE, 127, 126, 3);    /* CSD_STRUCTURE 1.2 */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 125, 122, 4);    /* SPEC_VERS 4.x */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 119, 112, 0x0e); /* TAAC */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 103, 96, speed);
    sd_reg_set_bits(csd, SD_CSD_SIZE, 95, 84, MMC_CARD_CCC);
    sd_reg_set_bits(csd, SD_CSD_SIZE, 83, 80, read_bl_len);
    sd_reg_set_bits(csd, SD_CSD_SIZE, 73, 62, csize);
    sd_reg_set_bits(csd, SD_CSD_SIZE, 61, 59, 6);      /* VDD_R_CURR_MIN */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 58, 56, 6);      /* VDD_R_CURR_MAX */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 55, 53, 6);      /* VDD_W_CURR_MIN */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 52, 50, 6);      /* VDD_W_CURR_MAX */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 49, 47, mult);   /* C_SIZE_MULT */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 46, 42, 0);      /* ERASE_GRP_SIZE */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 41, 37, 0);      /* ERASE_GRP_MULT */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 28, 26, 2);      /* R2W_FACTOR */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 25, 22, read_bl_len);
}


void MMCCard::BuildExtCsd()
{
    memset(fExtCsd, 0, sizeof(fExtCsd));

    fExtCsd[MMC_EXT_CSD_S_CMD_SET] = 1;
    fExtCsd[MMC_EXT_CSD_GENERIC_CMD6_TIME] = 1; /* in units of ten ms */
    fExtCsd[MMC_EXT_CSD_BOOT_MULT] = 0;         /* no boot partitions */
    fExtCsd[MMC_EXT_CSD_HC_ERASE_GRP_SIZE] = 1;
    fExtCsd[MMC_EXT_CSD_ERASE_TIMEOUT_MULT] = 1;
    fExtCsd[MMC_EXT_CSD_REL_WR_SEC_C] = 1;
    fExtCsd[MMC_EXT_CSD_HC_WP_GRP_SIZE] = 0;
    fExtCsd[MMC_EXT_CSD_S_A_TIMEOUT] = 0x11;
    /* The real capacity, which is the point of this register on a device too
       large for the CSD to describe. */
    put_le32(&fExtCsd[MMC_EXT_CSD_SEC_COUNT],
             (uint32_t)(BlockCount() > 0xffffffffull ? 0xffffffffu
                                                     : BlockCount()));
    fExtCsd[MMC_EXT_CSD_CARD_TYPE] = MMC_CARD_TYPE_26MHZ | MMC_CARD_TYPE_52MHZ;
    fExtCsd[MMC_EXT_CSD_STRUCTURE] = 2;
    fExtCsd[MMC_EXT_CSD_REV] = 6; /* version 4.5 */
    fExtCsd[MMC_EXT_CSD_PART_SWITCH_TIME] = 0;
    fExtCsd[MMC_EXT_CSD_PARTITION_SUPPORT] = 0;
    fExtCsd[MMC_EXT_CSD_SEC_FEATURE] = 0; /* no secure erase or trim */
    fExtCsd[MMC_EXT_CSD_ERASED_MEM_CONT] = 0;
    fExtCsd[MMC_EXT_CSD_BUS_WIDTH] = 0;
    fExtCsd[MMC_EXT_CSD_HS_TIMING] = 0;
    fExtCsd[MMC_EXT_CSD_PART_CONFIG] = 0;
    fExtCsd[MMC_EXT_CSD_ERASE_GROUP_DEF] = 0;
}


//#pragma mark - commands

int MMCCard::Illegal(uint8_t *response)
{
    Fail(SD_STATUS_ILLEGAL_COMMAND);
    BuildR1(response);
    return SD_RESPONSE_SHORT;
}


int MMCCard::Switch(const SDCommand &cmd, uint8_t *response)
{
    uint32_t access = get_bits(cmd.arg, MMC_SWITCH_ACCESS_SHIFT, 2);
    uint32_t index = get_bits(cmd.arg, MMC_SWITCH_INDEX_SHIFT, 8);
    uint8_t value = (uint8_t)(cmd.arg >> MMC_SWITCH_VALUE_SHIFT);

    if (fState != SD_STATE_TRAN) {
        return Illegal(response);
    }
    if (access == MMC_SWITCH_CMD_SET) {
        /* Only the standard command set exists here, and selecting it is a
           request to change nothing. */
        BuildR1(response);
        return SD_RESPONSE_SHORT;
    }
    if (index >= MMC_EXT_CSD_MODES_END) {
        /* Everything from there up is read only, and a device tells a driver
           that it refused the switch rather than pretending. */
        Fail(MMC_STATUS_SWITCH_ERROR);
        BuildR1(response);
        return SD_RESPONSE_SHORT;
    }

    switch (access) {
    case MMC_SWITCH_SET_BITS:
        fExtCsd[index] |= value;
        break;
    case MMC_SWITCH_CLEAR_BITS:
        fExtCsd[index] &= (uint8_t)~value;
        break;
    default:
        fExtCsd[index] = value;
        break;
    }

    if (index == MMC_EXT_CSD_HS_TIMING) {
        /* The CSD reports the rate the device now runs at. */
        fHighSpeed = fExtCsd[index] != 0;
        BuildCsd();
    }
    BuildR1(response);
    return SD_RESPONSE_SHORT;
}


int MMCCard::ReadWrite(const SDCommand &cmd, uint8_t *response, bool is_write,
                       bool multiple)
{
    uint64_t sector;

    if (fState != SD_STATE_TRAN) {
        return Illegal(response);
    }
    if (is_write && ReadOnly()) {
        Fail(SD_STATUS_WP_VIOLATION);
        BuildR1(response);
        return SD_RESPONSE_SHORT;
    }
    if (!SectorFromArg(cmd.arg, &sector)) {
        BuildR1(response);
        return SD_RESPONSE_SHORT;
    }

    StartDataTransfer(is_write ? SD_DATA_WRITE : SD_DATA_READ, sector,
                      multiple);
    fDataBlocksLeft = multiple ? fNextBlockCount : 0;
    fNextBlockCount = 0;
    BuildR1(response);
    return SD_RESPONSE_SHORT;
}


int MMCCard::Command(const SDCommand &cmd, uint8_t *response)
{
    switch (cmd.index) {
    case SD_CMD_GO_IDLE_STATE:
        Reset();
        return 0; /* answered by silence */

    case MMC_CMD_SEND_OP_COND: {
        uint32_t ocr = SD_OCR_VOLTAGE_WINDOW | SD_OCR_POWER_UP_DONE;
        if (fHighCapacity) {
            ocr |= MMC_OCR_SECTOR_MODE;
        }
        /* An argument of zero is the host asking what the device wants
           rather than offering it; only an offer moves the state on. */
        if ((cmd.arg & SD_OCR_VOLTAGE_WINDOW) != 0 &&
            fState == SD_STATE_IDLE) {
            fState = SD_STATE_READY;
        }
        BuildShort(response, ocr);
        return SD_RESPONSE_SHORT;
    }

    case SD_CMD_ALL_SEND_CID:
        if (fState != SD_STATE_READY) {
            return Illegal(response);
        }
        fState = SD_STATE_IDENT;
        BuildLong(response, fCid);
        return SD_RESPONSE_LONG;

    case SD_CMD_SEND_RELATIVE_ADDR:
        /* Unlike SD, the host chooses the address and the device is told
           what it is. */
        if (fState != SD_STATE_IDENT && fState != SD_STATE_STBY) {
            return Illegal(response);
        }
        fRca = cmd.arg >> 16;
        fState = SD_STATE_STBY;
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    case SD_CMD_SET_DSR:
        return 0;

    case SDIO_CMD_SEND_OP_COND:
        /* On this bus that number is SLEEP_AWAKE, which only means anything
           to a device that has been given an address. Answering nothing
           otherwise is what makes a host probing for an SDIO card find
           none. */
        if (fState != SD_STATE_STBY || cmd.arg >> 16 != fRca) {
            return -1;
        }
        /* There is nothing to power down, so it is accepted and does
           nothing. */
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    case SD_CMD_SWITCH_FUNC:
        return Switch(cmd, response);

    case SD_CMD_SELECT_CARD: {
        uint32_t rca = cmd.arg >> 16;
        if (rca == fRca && rca != 0) {
            if (fState == SD_STATE_STBY) {
                fState = SD_STATE_TRAN;
            }
        } else if (fState == SD_STATE_TRAN || fState == SD_STATE_DATA) {
            EndDataTransfer();
            fState = SD_STATE_STBY;
        }
        BuildR1(response);
        return SD_RESPONSE_SHORT;
    }

    case SD_CMD_SEND_IF_COND:
        /* On this bus that number is SEND_EXT_CSD, which hands the register
           over the data lines rather than in the response. */
        if (fState != SD_STATE_TRAN) {
            return Illegal(response);
        }
        memcpy(fRegData, fExtCsd, MMC_EXT_CSD_SIZE);
        StartRegisterRead(MMC_EXT_CSD_SIZE);
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    case SD_CMD_SEND_CSD:
        if (cmd.arg >> 16 != fRca) {
            return -1;
        }
        BuildLong(response, fCsd);
        return SD_RESPONSE_LONG;

    case SD_CMD_SEND_CID:
        if (cmd.arg >> 16 != fRca) {
            return -1;
        }
        BuildLong(response, fCid);
        return SD_RESPONSE_LONG;

    case SD_CMD_STOP_TRANSMISSION:
        StopTransfer();
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    case SD_CMD_SEND_STATUS:
        if (cmd.arg >> 16 != fRca) {
            return -1;
        }
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    case SD_CMD_GO_INACTIVE_STATE:
        if (cmd.arg >> 16 == fRca) {
            StopTransfer();
            fState = SD_STATE_IDLE;
            fRca = 0;
        }
        return 0;

    case SD_CMD_SET_BLOCKLEN:
        if (cmd.arg != SD_BLOCK_SIZE) {
            Fail(SD_STATUS_BLOCK_LEN_ERROR);
        } else {
            fBlockLen = cmd.arg;
        }
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    case SD_CMD_READ_SINGLE_BLOCK:
        return ReadWrite(cmd, response, false, false);
    case SD_CMD_READ_MULTIPLE_BLOCK:
        return ReadWrite(cmd, response, false, true);
    case SD_CMD_WRITE_BLOCK:
        return ReadWrite(cmd, response, true, false);
    case SD_CMD_WRITE_MULTIPLE_BLOCK:
        return ReadWrite(cmd, response, true, true);

    case SD_CMD_SET_BLOCK_COUNT:
        fNextBlockCount = cmd.arg & 0xffff;
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    default:
        /* Application commands are an SD invention, so CMD55 lands here too
           and is refused, which is what tells a driver which bus it is on. */
        return Illegal(response);
    }
}


//#pragma mark - factory

Device *mmc_card_node_create(std::unique_ptr<HostBlockDevice> bs, bool read_only)
{
    if (bs->SectorCount() < 4) {
        vm_error("mmc-card: the image is too small to be a device\n");
        return nullptr;
    }
    return new SDDeviceNode(
        "mmc-card", std::make_unique<MMCCard>(std::move(bs), read_only));
}
