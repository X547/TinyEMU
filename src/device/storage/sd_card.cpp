/*
 * SD memory card
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

#include "machine.h"
#include "sd.h"
#include "virtio.h"

/* The identity the guest reads out of the card registers. */
#define SD_CARD_MANFID   0x74   /* not an assigned identifier */
#define SD_CARD_OEMID    "TE"
#define SD_CARD_PRODUCT  "TEMU0" /* five characters */
#define SD_CARD_REVISION 0x10   /* 1.0, as two BCD nibbles */
#define SD_CARD_SERIAL   0x54454d55
#define SD_CARD_YEAR     24     /* manufacturing date, years since 2000 */
#define SD_CARD_MONTH    1

/* Command classes the card claims. Class 5, erase, is deliberately absent:
   nothing here erases, and a card that does not claim the class is one a
   driver never asks to. */
#define SD_CARD_CCC 0x595

/* Transfer rates, in the exponent and mantissa the field is coded in. */
#define SD_TRAN_SPEED_25MHZ 0x32
#define SD_TRAN_SPEED_50MHZ 0x5a

/* The largest medium a standard capacity card can describe, in 512 byte
   blocks: anything above it has to be a high capacity card. */
#define SD_SDSC_MAX_BLOCKS (2ull * 1024 * 1024 * 1024 / SD_BLOCK_SIZE)

/* What an application command handler returns for a command that is not one
   of them after all, which the specification says is then the ordinary
   command of that number. */
#define SD_CMD_NOT_HANDLED (-2)

/* CMD6 function groups. Only the first, the access mode, does anything. */
#define SD_SWITCH_NO_CHANGE 0xf
#define SD_SWITCH_DEFAULT   0x0
#define SD_SWITCH_HIGH_SPEED 0x1


//#pragma mark - SDCard

class SDCard final: public SDMemoryCard {
private:
    /* The bus width ACMD6 set, and the access mode CMD6 selected. Neither
       changes anything here -- there are no wires to drive and no clock to
       raise -- but both are read back, so both are kept. */
    int fBusWidth = 1;
    bool fHighSpeed = false;
    /* Set by SEND_IF_COND, which is how a version 2 card knows it is talking
       to a host that understands high capacity. */
    bool fIfCondSeen = false;
    /* The count SET_BLOCK_COUNT fixed for the next transfer. */
    uint32_t fNextBlockCount = 0;

    uint8_t fScr[SD_SCR_SIZE] {};

    void BuildCid();
    void BuildCsd();
    void BuildScr();
    void BuildSdStatus();
    void BuildSwitchStatus(uint32_t arg, bool set);

    int NormalCommand(const SDCommand &cmd, uint8_t *response);
    int AppCommand(const SDCommand &cmd, uint8_t *response);

    /* The R1 an addressed command answers with, together with the state
       check every one of them makes. */
    int Illegal(uint8_t *response);
    int ReadWrite(const SDCommand &cmd, uint8_t *response, bool is_write,
                  bool multiple);

public:
    SDCard(std::unique_ptr<BlockDevice> bs, bool read_only);

    SDCardTypeEnum CardType() const override {return SD_CARD_SD;}
    int BusWidth() const override {return 4;}

    void Reset() override;
    int Command(const SDCommand &cmd, uint8_t *response) override;
};


SDCard::SDCard(std::unique_ptr<BlockDevice> bs, bool read_only):
    SDMemoryCard("sd-card", std::move(bs), read_only)
{
    /* A medium larger than two gigabytes cannot be described by a version 1
       CSD at all, so it has to be a high capacity card; a smaller one is a
       standard capacity card, addressed in bytes. */
    fHighCapacity = BlockCount() > SD_SDSC_MAX_BLOCKS;
    BuildCid();
    BuildCsd();
    BuildScr();
}


void SDCard::Reset()
{
    SDMemoryCard::Reset();
    fBusWidth = 1;
    fHighSpeed = false;
    fIfCondSeen = false;
    fNextBlockCount = 0;
    BuildCsd();
}


//#pragma mark - card registers

void SDCard::BuildCid()
{
    uint8_t *cid = fCid;

    memset(cid, 0, SD_CID_SIZE);
    cid[0] = SD_CARD_MANFID;
    cid[1] = SD_CARD_OEMID[0];
    cid[2] = SD_CARD_OEMID[1];
    memcpy(cid + 3, SD_CARD_PRODUCT, 5);
    cid[8] = SD_CARD_REVISION;
    cid[9] = (uint8_t)(SD_CARD_SERIAL >> 24);
    cid[10] = (uint8_t)(SD_CARD_SERIAL >> 16);
    cid[11] = (uint8_t)(SD_CARD_SERIAL >> 8);
    cid[12] = (uint8_t)SD_CARD_SERIAL;
    /* The manufacturing date is twelve bits: eight of year counted from 2000
       and four of month, ending one byte short of the CRC. */
    cid[13] = (SD_CARD_YEAR >> 4) & 0x0f;
    cid[14] = (uint8_t)((SD_CARD_YEAR << 4) | SD_CARD_MONTH);
}


void SDCard::BuildCsd()
{
    uint8_t *csd = fCsd;
    uint8_t speed = fHighSpeed ? SD_TRAN_SPEED_50MHZ : SD_TRAN_SPEED_25MHZ;

    memset(csd, 0, SD_CSD_SIZE);

    if (fHighCapacity) {
        /* Version 2, which describes the capacity in units of 512 KB and
           fixes the block length at 512 bytes. */
        sd_reg_set_bits(csd, SD_CSD_SIZE, 127, 126, 1);   /* CSD_STRUCTURE */
        sd_reg_set_bits(csd, SD_CSD_SIZE, 119, 112, 0x0e); /* TAAC, 1 ms */
        sd_reg_set_bits(csd, SD_CSD_SIZE, 103, 96, speed);
        sd_reg_set_bits(csd, SD_CSD_SIZE, 95, 84, SD_CARD_CCC);
        sd_reg_set_bits(csd, SD_CSD_SIZE, 83, 80, 9);     /* READ_BL_LEN */

        uint64_t csize = BlockCount() / 1024;
        if (csize > 0) {
            csize--;
        }
        if (csize > 0x3fffff) {
            csize = 0x3fffff;
        }
        sd_reg_set_bits(csd, SD_CSD_SIZE, 69, 48, csize);
        /* The capacity the CSD can express is what the card has, so that a
           guest is never told about blocks it cannot then address. */
        fBlockCount = (csize + 1) * 1024;

        sd_reg_set_bits(csd, SD_CSD_SIZE, 46, 46, 1);     /* ERASE_BLK_EN */
        sd_reg_set_bits(csd, SD_CSD_SIZE, 45, 39, 0x7f);  /* SECTOR_SIZE */
        sd_reg_set_bits(csd, SD_CSD_SIZE, 28, 26, 2);     /* R2W_FACTOR */
        sd_reg_set_bits(csd, SD_CSD_SIZE, 25, 22, 9);     /* WRITE_BL_LEN */
        return;
    }

    /* Version 1, whose capacity is a mantissa and two exponents. The block
       length here is always 512 bytes, so a medium over one gigabyte needs
       the larger read block length to be described at all. */
    uint32_t read_bl_len = 9;
    uint64_t units = BlockCount();
    if (units > 2097152) {
        read_bl_len = 10;
        units /= 2;
    }
    int mult = 0;
    while (mult < 7 && (units >> (mult + 2)) > 4096) {
        mult++;
    }
    uint64_t csize = units >> (mult + 2);
    if (csize == 0) {
        csize = 1;
    }
    if (csize > 4096) {
        csize = 4096;
    }
    csize--;
    /* The fields cannot express an arbitrary number of blocks, so the card
       holds what they say it holds and the tail of the image is unused. */
    fBlockCount = ((csize + 1) << (mult + 2)) << (read_bl_len - 9);

    sd_reg_set_bits(csd, SD_CSD_SIZE, 127, 126, 0);       /* CSD_STRUCTURE */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 119, 112, 0x0e);    /* TAAC */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 103, 96, speed);
    sd_reg_set_bits(csd, SD_CSD_SIZE, 95, 84, SD_CARD_CCC);
    sd_reg_set_bits(csd, SD_CSD_SIZE, 83, 80, read_bl_len);
    sd_reg_set_bits(csd, SD_CSD_SIZE, 73, 62, csize);
    sd_reg_set_bits(csd, SD_CSD_SIZE, 61, 59, 6);         /* VDD_R_CURR_MIN */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 58, 56, 6);         /* VDD_R_CURR_MAX */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 55, 53, 6);         /* VDD_W_CURR_MIN */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 52, 50, 6);         /* VDD_W_CURR_MAX */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 49, 47, mult);      /* C_SIZE_MULT */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 46, 46, 1);         /* ERASE_BLK_EN */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 45, 39, 0x7f);      /* SECTOR_SIZE */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 28, 26, 2);         /* R2W_FACTOR */
    sd_reg_set_bits(csd, SD_CSD_SIZE, 25, 22, read_bl_len);
}


void SDCard::BuildScr()
{
    memset(fScr, 0, sizeof(fScr));

    sd_reg_set_bits(fScr, SD_SCR_SIZE, 63, 60, 0);   /* SCR_STRUCTURE */
    sd_reg_set_bits(fScr, SD_SCR_SIZE, 59, 56, 2);   /* SD_SPEC, 2.00 or up */
    sd_reg_set_bits(fScr, SD_SCR_SIZE, 55, 55, 0);   /* erased data is zero */
    sd_reg_set_bits(fScr, SD_SCR_SIZE, 51, 48, 0x5); /* one and four bits */
    sd_reg_set_bits(fScr, SD_SCR_SIZE, 47, 47, 1);   /* SD_SPEC3, 3.0x */
    /* SET_BLOCK_COUNT is supported, which is what lets a host bound a
       multiple block transfer without having to stop it afterwards. */
    sd_reg_set_bits(fScr, SD_SCR_SIZE, 35, 32, 0x2);
}


void SDCard::BuildSdStatus()
{
    uint8_t *ssr = fRegData;

    memset(ssr, 0, SD_STATUS_SIZE);
    sd_reg_set_bits(ssr, SD_STATUS_SIZE, 511, 510, fBusWidth == 4 ? 2 : 0);
    /* Card type 0 is a regular read/write card, which is the only kind
       here. */
    sd_reg_set_bits(ssr, SD_STATUS_SIZE, 495, 480, 0);
    sd_reg_set_bits(ssr, SD_STATUS_SIZE, 447, 440, 4); /* speed class 10 */
    sd_reg_set_bits(ssr, SD_STATUS_SIZE, 439, 432, 0); /* performance move */
    sd_reg_set_bits(ssr, SD_STATUS_SIZE, 431, 428, 9); /* AU size, 4 MB */
}


void SDCard::BuildSwitchStatus(uint32_t arg, bool set)
{
    uint8_t *status = fRegData;
    uint32_t group1 = arg & 0xf;

    memset(status, 0, SD_SWITCH_SIZE);

    /* Maximum current the card draws, in milliamps. */
    sd_reg_set_bits(status, SD_SWITCH_SIZE, 511, 496, 100);
    /* Access mode: the default and high speed are both available, and the
       other five groups offer nothing but their own default. */
    sd_reg_set_bits(status, SD_SWITCH_SIZE, 415, 400, 0x0003);
    for (int group = 2; group <= 6; group++) {
        int hi = 415 + (group - 1) * 16;
        sd_reg_set_bits(status, SD_SWITCH_SIZE, hi, hi - 15, 0x0001);
    }

    uint32_t chosen = group1;
    if (group1 == SD_SWITCH_NO_CHANGE) {
        chosen = fHighSpeed ? SD_SWITCH_HIGH_SPEED : SD_SWITCH_DEFAULT;
    } else if (group1 != SD_SWITCH_DEFAULT &&
               group1 != SD_SWITCH_HIGH_SPEED) {
        /* A function the card does not have reads back as 0xf, which is how
           the host is told the switch did not happen. */
        chosen = 0xf;
    } else if (set) {
        fHighSpeed = group1 == SD_SWITCH_HIGH_SPEED;
        /* The CSD reports the rate the card now runs at. */
        BuildCsd();
    }
    sd_reg_set_bits(status, SD_SWITCH_SIZE, 379, 376, chosen);
    for (int group = 2; group <= 6; group++) {
        int hi = 379 + (group - 1) * 4;
        uint32_t sel = (arg >> ((group - 1) * 4)) & 0xf;
        /* Those groups offer only their own default, so asking for it or for
           no change reads back as the default and anything else reads back
           as the refusal it is. */
        bool ok = sel == SD_SWITCH_NO_CHANGE || sel == 0;
        sd_reg_set_bits(status, SD_SWITCH_SIZE, hi, hi - 3, ok ? 0 : 0xf);
    }
    /* Data structure version 1, which is the one that carries the busy
       status fields this leaves at zero. */
    sd_reg_set_bits(status, SD_SWITCH_SIZE, 375, 368, 1);
}


//#pragma mark - commands

int SDCard::Illegal(uint8_t *response)
{
    Fail(SD_STATUS_ILLEGAL_COMMAND);
    BuildR1(response);
    return SD_RESPONSE_SHORT;
}


int SDCard::ReadWrite(const SDCommand &cmd, uint8_t *response, bool is_write,
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
    /* A count fixed by SET_BLOCK_COUNT applies to the next transfer and to
       that one only. */
    fDataBlocksLeft = multiple ? fNextBlockCount : 0;
    fNextBlockCount = 0;
    BuildR1(response);
    return SD_RESPONSE_SHORT;
}


int SDCard::Command(const SDCommand &cmd, uint8_t *response)
{
    if (fAppCmd) {
        int len = AppCommand(cmd, response);
        fAppCmd = false;
        if (len != SD_CMD_NOT_HANDLED) {
            return len;
        }
        /* Not one of the application commands after all, so it is the
           ordinary command of that number, which is what the specification
           says an unrecognised one becomes. */
    }
    return NormalCommand(cmd, response);
}


int SDCard::NormalCommand(const SDCommand &cmd, uint8_t *response)
{
    switch (cmd.index) {
    case SD_CMD_GO_IDLE_STATE:
        Reset();
        return 0; /* answered by silence */

    case MMC_CMD_SEND_OP_COND:
        /* An MMC command. An SD card must not answer it, which is exactly
           how a host tells the two apart. */
        return -1;

    case SD_CMD_ALL_SEND_CID:
        if (fState != SD_STATE_READY) {
            return Illegal(response);
        }
        fState = SD_STATE_IDENT;
        BuildLong(response, fCid);
        return SD_RESPONSE_LONG;

    case SD_CMD_SEND_RELATIVE_ADDR: {
        if (fState != SD_STATE_IDENT && fState != SD_STATE_STBY) {
            return Illegal(response);
        }
        /* The card publishes an address of its own; only zero is reserved,
           and one card on the bus needs no more than one value. */
        fRca = 0x0001;
        fState = SD_STATE_STBY;
        uint32_t status = CurrentStatus();
        ClearStatus();
        /* The status the address response carries is only the three error
           bits and the low thirteen. */
        uint32_t bits = (((status >> 23) & 1) << 15) |
                        (((status >> 22) & 1) << 14) |
                        (((status >> 19) & 1) << 13) | (status & 0x1fff);
        BuildShort(response, (fRca << 16) | bits);
        return SD_RESPONSE_SHORT;
    }

    case SD_CMD_SET_DSR:
        return 0;

    case SDIO_CMD_SEND_OP_COND:
        /* A memory card has no I/O functions, and says so by not answering
           at all. */
        return -1;

    case SD_CMD_SWITCH_FUNC:
        if (fState != SD_STATE_TRAN) {
            return Illegal(response);
        }
        BuildSwitchStatus(cmd.arg, (cmd.arg & (1u << 31)) != 0);
        StartRegisterRead(SD_SWITCH_SIZE);
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    case SD_CMD_SELECT_CARD: {
        uint32_t rca = cmd.arg >> 16;
        if (rca == fRca && rca != 0) {
            if (fState == SD_STATE_STBY) {
                fState = SD_STATE_TRAN;
            }
        } else if (fState == SD_STATE_TRAN || fState == SD_STATE_DATA) {
            /* Addressing another card deselects this one. */
            EndDataTransfer();
            fState = SD_STATE_STBY;
        }
        BuildR1(response);
        return SD_RESPONSE_SHORT;
    }

    case SD_CMD_SEND_IF_COND:
        /* The host names the voltage it can supply and a pattern to echo;
           a card that cannot work at that voltage answers nothing. */
        if (((cmd.arg >> 8) & 0xf) != 1) {
            return -1;
        }
        fIfCondSeen = true;
        BuildShort(response, (1u << 8) | (cmd.arg & 0xff));
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
        /* The CSD says the block length is 512 bytes and that partial blocks
           are not offered, so that is the only length the card takes. */
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

    case SD_CMD_APP_CMD:
        if (cmd.arg >> 16 != fRca) {
            return Illegal(response);
        }
        fAppCmd = true;
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    default:
        return Illegal(response);
    }
}


int SDCard::AppCommand(const SDCommand &cmd, uint8_t *response)
{
    switch (cmd.index) {
    case SD_ACMD_SET_BUS_WIDTH:
        fBusWidth = (cmd.arg & 3) == 2 ? 4 : 1;
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    case SD_ACMD_SD_STATUS:
        if (fState != SD_STATE_TRAN) {
            return Illegal(response);
        }
        BuildSdStatus();
        StartRegisterRead(SD_STATUS_SIZE);
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    case SD_ACMD_SEND_NUM_WR_BLOCKS:
        if (fState != SD_STATE_TRAN) {
            return Illegal(response);
        }
        /* Nothing here ever fails a block part way through, so every block
           of the last write was programmed. */
        memset(fRegData, 0, 4);
        StartRegisterRead(4);
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    case SD_ACMD_SET_WR_BLK_ERASE_COUNT:
    case SD_ACMD_SET_CLR_CARD_DETECT:
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    case SD_ACMD_SD_SEND_OP_COND: {
        uint32_t ocr = SD_OCR_VOLTAGE_WINDOW;
        /* A high capacity card only says so to a host that introduced itself
           with SEND_IF_COND; one that did not is a version 1 host, which
           could not address the card anyway. */
        if (fHighCapacity && fIfCondSeen) {
            ocr |= SD_OCR_CCS;
        }
        /* There is nothing to power up, so the card is never busy. */
        ocr |= SD_OCR_POWER_UP_DONE;
        if ((cmd.arg & SD_OCR_VOLTAGE_WINDOW) != 0 &&
            fState == SD_STATE_IDLE) {
            fState = SD_STATE_READY;
        }
        BuildShort(response, ocr);
        return SD_RESPONSE_SHORT;
    }

    case SD_ACMD_SEND_SCR:
        if (fState != SD_STATE_TRAN) {
            return Illegal(response);
        }
        memcpy(fRegData, fScr, SD_SCR_SIZE);
        StartRegisterRead(SD_SCR_SIZE);
        BuildR1(response);
        return SD_RESPONSE_SHORT;

    default:
        return SD_CMD_NOT_HANDLED;
    }
}


//#pragma mark - factory

Device *sd_card_node_create(std::unique_ptr<BlockDevice> bs, bool read_only)
{
    /* The smallest capacity a version 1 CSD can describe is four blocks, and
       an image below that would have the card claiming more than it holds. */
    if (bs->SectorCount() < 4) {
        vm_error("sd-card: the image is too small to be a card\n");
        return nullptr;
    }
    return new SDDeviceNode("sd-card",
                            std::make_unique<SDCard>(std::move(bs), read_only));
}
