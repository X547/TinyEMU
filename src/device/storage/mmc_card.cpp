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
#include "config_props.h"
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
#define MMC_EXT_CSD_CACHE_CTRL       33
#define MMC_EXT_CSD_POWER_OFF_NOTIFY 34
#define MMC_EXT_CSD_PARTITION_SUPPORT 160
#define MMC_EXT_CSD_ERASE_GROUP_DEF  175
#define MMC_EXT_CSD_PART_CONFIG      179
#define MMC_EXT_CSD_ERASED_MEM_CONT  181
#define MMC_EXT_CSD_BUS_WIDTH        183
#define MMC_EXT_CSD_HS_TIMING        185
#define MMC_EXT_CSD_POWER_CLASS      187
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

/* Whether 'value' is one the device takes in the modes segment byte 'index'.
   Only the fields a device without boot partitions, cache, DDR or HS200
   has are writable, and each only with the values its capabilities in the
   extended CSD allow; anything else is a SWITCH_ERROR. */
static bool mmc_switch_value_ok(uint32_t index, uint8_t value)
{
    switch (index) {
    case MMC_EXT_CSD_CACHE_CTRL:
        return value == 0; /* CACHE_SIZE is 0 */
    case MMC_EXT_CSD_POWER_OFF_NOTIFY:
        return value <= 3;
    case MMC_EXT_CSD_ERASE_GROUP_DEF:
        return value <= 1;
    case MMC_EXT_CSD_PART_CONFIG: {
        /* Only the user area can be accessed or booted from. */
        uint32_t boot = get_bits(value, 3, 3);
        return get_bits(value, 0, 3) == 0 && (boot == 0 || boot == 7) &&
               get_bit(value, 7) == 0;
    }
    case MMC_EXT_CSD_BUS_WIDTH:
        return value <= 2; /* 1, 4 or 8 bits, single data rate */
    case MMC_EXT_CSD_HS_TIMING:
        return value <= 1; /* backward compatible or high speed */
    case MMC_EXT_CSD_POWER_CLASS:
        return value <= 15;
    default:
        return false;
    }
}


int MMCCard::Switch(const SDCommand &cmd, uint8_t *response)
{
    uint32_t access = get_bits(cmd.arg, MMC_SWITCH_ACCESS_SHIFT, 2);
    uint32_t index = get_bits(cmd.arg, MMC_SWITCH_INDEX_SHIFT, 8);
    uint8_t value = (uint8_t)(cmd.arg >> MMC_SWITCH_VALUE_SHIFT);

    if (fState != SD_STATE_TRAN) {
        return Illegal();
    }

    /* The response goes out before the switch is carried out, so a switch
       that fails says so in the response to the next command, which is the
       SEND_STATUS a driver polls with. */
    BuildR1(response);

    if (access == MMC_SWITCH_CMD_SET) {
        /* Only the standard command set exists here. */
        if (get_bits(cmd.arg, 0, 3) != 0) {
            Fail(MMC_STATUS_SWITCH_ERROR);
        }
        return SD_RESPONSE_SHORT;
    }

    uint8_t old = fExtCsd[index];
    switch (access) {
    case MMC_SWITCH_SET_BITS:
        value = old | value;
        break;
    case MMC_SWITCH_CLEAR_BITS:
        value = old & (uint8_t)~value;
        break;
    default:
        break;
    }
    if (!mmc_switch_value_ok(index, value)) {
        Fail(MMC_STATUS_SWITCH_ERROR);
        return SD_RESPONSE_SHORT;
    }
    fExtCsd[index] = value;

    if (index == MMC_EXT_CSD_HS_TIMING) {
        /* The CSD reports the rate the device now runs at. */
        fHighSpeed = value != 0;
        BuildCsd();
    }
    return SD_RESPONSE_SHORT;
}


int MMCCard::ReadWrite(const SDCommand &cmd, uint8_t *response, bool is_write,
                       bool multiple)
{
    uint64_t sector;

    if (fState != SD_STATE_TRAN) {
        return Illegal();
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


/* The states each command may be taken in follow the device state
   transitions of JESD84-B451; a command outside them is illegal, and one
   naming another device's address is not this one's to answer. */
int MMCCard::Command(const SDCommand &cmd, uint8_t *response)
{
    bool addressed = cmd.arg >> 16 == fRca;

    if (fState == SD_STATE_INA) {
        return 0;
    }
    BeginCommand();

    /* Asleep, the device takes nothing but a reset and the command that
       wakes it. */
    if (fState == MMC_STATE_SLP && cmd.index != SD_CMD_GO_IDLE_STATE &&
        cmd.index != SDIO_CMD_SEND_OP_COND) {
        return Illegal();
    }

    switch (cmd.index) {
    case SD_CMD_GO_IDLE_STATE:
        Reset();
        return 0; /* answered by silence */

    case MMC_CMD_SEND_OP_COND: {
        if (fState != SD_STATE_IDLE) {
            return Illegal();
        }
        if (!CheckVoltage(cmd.arg)) {
            return 0;
        }
        uint32_t ocr = SD_OCR_VOLTAGE_WINDOW;
        if (fHighCapacity) {
            ocr |= MMC_OCR_SECTOR_MODE;
        }
        /* An argument of zero is the host asking what the device wants
           rather than offering it; only an offer moves the state on, and
           until then the device reports itself still powering up. */
        if (get_bits(cmd.arg, 0, 24) != 0) {
            fState = SD_STATE_READY;
            ocr |= SD_OCR_POWER_UP_DONE;
        }
        BuildShort(response, ocr);
        return SD_RESPONSE_SHORT;
    }

    case SD_CMD_ALL_SEND_CID:
        if (fState != SD_STATE_READY) {
            return Illegal();
        }
        fState = SD_STATE_IDENT;
        BuildLong(response, fCid);
        return SD_RESPONSE_LONG;

    case SD_CMD_SEND_RELATIVE_ADDR:
        /* Unlike SD, the host chooses the address and the device is told
           what it is. */
        if (fState != SD_STATE_IDENT && fState != SD_STATE_STBY) {
            return Illegal();
        }
        fRca = cmd.arg >> 16;
        fState = SD_STATE_STBY;
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    case SD_CMD_SET_DSR:
        if (fState != SD_STATE_STBY) {
            return Illegal();
        }
        return 0;

    case SDIO_CMD_SEND_OP_COND: {
        /* On this bus that number is SLEEP_AWAKE: bit 15 sends an addressed
           device in standby to sleep, and clear wakes it again. */
        bool sleep = get_bit(cmd.arg, 15) != 0;
        if (fState != (sleep ? SD_STATE_STBY : MMC_STATE_SLP)) {
            return Illegal();
        }
        if (!addressed) {
            return 0;
        }
        BuildR1(response);
        fState = sleep ? MMC_STATE_SLP : SD_STATE_STBY;
        return SD_RESPONSE_SHORT;
    }

    case SD_CMD_SWITCH_FUNC:
        return Switch(cmd, response);

    case SD_CMD_SELECT_CARD:
        if (fState != SD_STATE_STBY && fState != SD_STATE_TRAN &&
            fState != SD_STATE_DATA) {
            return Illegal();
        }
        if (addressed && fRca != 0) {
            if (fState != SD_STATE_STBY) {
                return Illegal();
            }
            fState = SD_STATE_TRAN;
            BuildR1(response);
            return SD_RESPONSE_SHORT;
        }
        HaltDataTransfer();
        fState = SD_STATE_STBY;
        return 0;

    case SD_CMD_SEND_IF_COND:
        /* On this bus that number is SEND_EXT_CSD, which hands the register
           over the data lines rather than in the response. */
        if (fState != SD_STATE_TRAN) {
            return Illegal();
        }
        memcpy(fRegData, fExtCsd, MMC_EXT_CSD_SIZE);
        StartRegisterRead(MMC_EXT_CSD_SIZE);
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    case SD_CMD_SEND_CSD:
    case SD_CMD_SEND_CID:
        if (fState != SD_STATE_STBY) {
            return Illegal();
        }
        if (!addressed) {
            return 0;
        }
        BuildLong(response, cmd.index == SD_CMD_SEND_CSD ? fCsd : fCid);
        return SD_RESPONSE_LONG;

    case SD_CMD_STOP_TRANSMISSION:
        if (!StopCommand()) {
            return Illegal();
        }
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    case SD_CMD_SEND_STATUS:
        if (fState < SD_STATE_STBY) {
            return Illegal();
        }
        if (!addressed) {
            return 0;
        }
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    case SD_CMD_GO_INACTIVE_STATE:
        if (fState < SD_STATE_STBY) {
            return Illegal();
        }
        if (addressed) {
            StopTransfer();
            fState = SD_STATE_INA;
        }
        return 0;

    case SD_CMD_SET_BLOCKLEN:
        if (fState != SD_STATE_TRAN) {
            return Illegal();
        }
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
        if (fState != SD_STATE_TRAN) {
            return Illegal();
        }
        /* The count is the low sixteen bits; above them are the reliable
           write and packed command flags, which change nothing here. */
        fNextBlockCount = cmd.arg & 0xffff;
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    default:
        /* Application commands are an SD invention, so CMD55 lands here too
           and goes unanswered, which is what tells a driver which bus it is
           on. */
        return Illegal();
    }
}


//#pragma mark - class

class MMCCardClass final: public DeviceClass {
public:
    MMCCardClass(): DeviceClass("mmc-card") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        int read_only;
        if (!cfg.GetInt("read_only", &read_only, 0)) {
            return nullptr;
        }
        auto bs = config_open_block(cfg, ctx);
        if (bs == nullptr) {
            return nullptr;
        }
        if (bs->SectorCount() < 4) {
            vm_error("mmc-card: the image is too small to be a device\n");
            return nullptr;
        }
        return new SDDeviceNode(
            "mmc-card",
            std::make_unique<MMCCard>(std::move(bs), read_only != 0));
    }
};

static const MMCCardClass sMMCCardClass;
