/*
 * SCSI CD-ROM drive
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
#include <algorithm>
#include <memory>
#include <vector>

#include "cutils.h"
#include "machine.h"
#include "scsi.h"
#include "virtio.h"

/* The image is a plain ISO: the 2048 byte user data of one data track, block
   after block, and so four of the back end's 512 byte sectors per block. */
#define SCSI_CD_BLOCK_SIZE 2048
#define SCSI_CD_SECTORS_PER_BLOCK (SCSI_CD_BLOCK_SIZE / 512)

/* 80 minutes at 75 blocks a second. A larger image can only be a DVD. */
#define SCSI_CD_MAX_BLOCKS (80 * 60 * 75)

/* MSF addresses count from the start of the two second pregap. */
#define SCSI_CD_MSF_OFFSET 150

/* Where the data area of a DVD starts, in its physical sector numbers. */
#define SCSI_CD_DVD_START_SECTOR 0x30000

/* What the capabilities page reports, in kilobytes a second: 16x. */
#define SCSI_CD_SPEED 2816

#define SCSI_CD_VENDOR   "TinyEMU "         /*  8 characters, padded */
#define SCSI_CD_PRODUCT  "CD-ROM          " /* 16 characters, padded */
#define SCSI_CD_REVISION "1.0 "             /*  4 characters, padded */
#define SCSI_CD_SERIAL   "TEMU00000002"

/* MMC profiles, which GET CONFIGURATION reports. */
#define MMC_PROFILE_NONE    0x0000
#define MMC_PROFILE_CD_ROM  0x0008
#define MMC_PROFILE_DVD_ROM 0x0010

/* Track control nibble with ADR 1: a data track, recorded uninterrupted. */
#define MMC_ADR_CONTROL_DATA 0x14

/* Media class events of GET EVENT STATUS NOTIFICATION. */
#define MMC_MEDIA_EVENT_NONE    0
#define MMC_MEDIA_EVENT_NEW     2
#define MMC_MEDIA_EVENT_REMOVED 3

/* Event class bits and numbers. */
#define MMC_EVENT_CLASS_MEDIA 4


//#pragma mark - SCSICD

class SCSICD final: public SCSIDevice {
private:
    /* One read the back end has, and the completion it answers through. */
    struct IO final: public BlockCompletion {
        SCSICD &cd;
        SCSIRequest *req = nullptr;
        uint32_t length = 0;

        IO(SCSICD &cd): cd(cd) {}

        void Complete(int ret) override {cd.BlockDone(this, ret);}
    };

    /* The disc the drive was given; null for a drive that has none. It stays
       open while the guest has the tray out, so that closing it again puts
       the same disc back. */
    std::unique_ptr<HostBlockDevice> fImage;
    uint64_t fImageBlocks = 0;

    bool fMediumPresent = false;
    bool fTrayOpen = false;
    bool fLocked = false;
    /* A disc went in since the guest last looked, which the next command is
       told about as a unit attention. */
    bool fMediumChanged = false;
    /* What GET EVENT STATUS NOTIFICATION has to report, once. */
    uint8_t fMediaEvent = MMC_MEDIA_EVENT_NONE;

    std::vector<std::unique_ptr<IO>> fInFlight;

    /* The sense data a failed command left behind, for REQUEST SENSE. */
    uint8_t fSense[SCSI_SENSE_LEN] {};

    bool IsDVD() const {return fImageBlocks > SCSI_CD_MAX_BLOCKS;}
    uint16_t CurrentProfile() const;

    void Fail(SCSIRequest *req, uint8_t key, uint16_t asc_ascq);
    void Good(SCSIRequest *req, uint32_t length);
    void Reply(SCSIRequest *req, const uint8_t *data, uint32_t len,
               uint32_t alloc);
    bool NeedMedium(SCSIRequest *req);
    void BlockDone(IO *io, int ret);

    void Eject();
    void Load();

    bool RequestSense(SCSIRequest *req);
    bool Inquiry(SCSIRequest *req);
    bool ModeSense(SCSIRequest *req, bool is_10);
    bool StartStop(SCSIRequest *req);
    bool ReadCapacity(SCSIRequest *req);
    bool ReadBlocks(SCSIRequest *req, uint64_t lba, uint32_t blocks);
    bool ReadCD(SCSIRequest *req);
    bool ReadSubChannel(SCSIRequest *req);
    bool ReadTOC(SCSIRequest *req);
    bool GetConfiguration(SCSIRequest *req);
    bool GetEventStatus(SCSIRequest *req);
    bool ReadDiscInformation(SCSIRequest *req);
    bool ReadTrackInformation(SCSIRequest *req);
    bool ReadDVDStructure(SCSIRequest *req);
    bool MechanismStatus(SCSIRequest *req);

public:
    SCSICD(std::unique_ptr<HostBlockDevice> image);

    void Reset() override;
    bool Submit(SCSIRequest *req) override;
    void Cancel(SCSIRequest *req) override;

    uint32_t BlockSize() override {return SCSI_CD_BLOCK_SIZE;}
    uint64_t BlockCount() override {return fMediumPresent ? fImageBlocks : 0;}
    uint8_t PeripheralType() override {return SCSI_TYPE_CDROM;}
    bool Removable() override {return true;}
    bool DataPhase(const uint8_t *cdb, SCSIDirEnum *dir,
                   uint32_t *len) override;
};


SCSICD::SCSICD(std::unique_ptr<HostBlockDevice> image):
    SCSIDevice("scsi-cd"), fImage(std::move(image))
{
    if (fImage != nullptr) {
        fImageBlocks = fImage->SectorCount() / SCSI_CD_SECTORS_PER_BLOCK;
        fMediumPresent = true;
    }
}


void SCSICD::Reset()
{
    for (auto &io : fInFlight) {
        fImage->Cancel(io.get());
    }
    fInFlight.clear();
    memset(fSense, 0, sizeof(fSense));
    /* A reset lets go of the tray, as it does on a real drive. */
    fLocked = false;
}


void SCSICD::Cancel(SCSIRequest *req)
{
    auto it = std::find_if(fInFlight.begin(), fInFlight.end(),
                           [req](const std::unique_ptr<IO> &io) {
                               return io->req == req;
                           });
    if (it == fInFlight.end()) {
        return;
    }
    fImage->Cancel(it->get());
    fInFlight.erase(it);
}


uint16_t SCSICD::CurrentProfile() const
{
    if (!fMediumPresent) {
        return MMC_PROFILE_NONE;
    }
    return IsDVD() ? MMC_PROFILE_DVD_ROM : MMC_PROFILE_CD_ROM;
}


//#pragma mark - answers

void SCSICD::Fail(SCSIRequest *req, uint8_t key, uint16_t asc_ascq)
{
    scsi_set_sense(req, key, asc_ascq);
    memcpy(fSense, req->sense, SCSI_SENSE_LEN);
}


void SCSICD::Good(SCSIRequest *req, uint32_t length)
{
    scsi_set_good(req, length);
    memset(fSense, 0, sizeof(fSense));
    fSense[0] = 0x70;
    fSense[7] = SCSI_SENSE_LEN - 8;
}


/* A reply, cut to the allocation length the CDB gave as well as to the
   initiator's buffer. */
void SCSICD::Reply(SCSIRequest *req, const uint8_t *data, uint32_t len,
                   uint32_t alloc)
{
    Good(req, scsi_reply(req, data, std::min(len, alloc)));
}


/* Commands that read the disc fail without one, saying whether the tray is
   open, which is how a host tells "no disc" from "tray out". */
bool SCSICD::NeedMedium(SCSIRequest *req)
{
    if (fMediumPresent) {
        return true;
    }
    Fail(req, SCSI_SENSE_NOT_READY,
         fTrayOpen ? SCSI_ASC_MEDIUM_NOT_PRESENT_OPEN
                   : SCSI_ASC_MEDIUM_NOT_PRESENT_CLOSED);
    return false;
}


static void cd_lba_to_msf(uint8_t *p, uint64_t lba)
{
    lba += SCSI_CD_MSF_OFFSET;
    p[0] = std::min<uint64_t>(lba / (60 * 75), 0xff);
    p[1] = (lba / 75) % 60;
    p[2] = lba % 75;
}


/* A four byte address field, as a block number or as minute, second and
   frame. */
static void cd_put_address(uint8_t *p, uint64_t lba, bool msf)
{
    if (msf) {
        p[0] = 0;
        cd_lba_to_msf(p + 1, lba);
    } else {
        put_be32(p, (uint32_t)lba);
    }
}


//#pragma mark - tray

void SCSICD::Eject()
{
    if (fMediumPresent) {
        fMediumPresent = false;
        fMediaEvent = MMC_MEDIA_EVENT_REMOVED;
    }
    fTrayOpen = true;
    fMediumChanged = false;
}


void SCSICD::Load()
{
    if (!fTrayOpen) {
        return;
    }
    fTrayOpen = false;
    if (fImage != nullptr) {
        fMediumPresent = true;
        fMediumChanged = true;
        fMediaEvent = MMC_MEDIA_EVENT_NEW;
    }
}


bool SCSICD::StartStop(SCSIRequest *req)
{
    uint8_t flags = req->cdb[4];

    /* A power condition change leaves the tray alone. */
    if ((flags & 0xf0) == 0 && (flags & 0x02) != 0) {
        if ((flags & 0x01) != 0) {
            Load();
        } else if (fLocked) {
            Fail(req, SCSI_SENSE_ILLEGAL_REQUEST,
                 SCSI_ASC_MEDIUM_REMOVAL_PREVENTED);
            return true;
        } else {
            Eject();
        }
    }
    Good(req, 0);
    return true;
}


//#pragma mark - primary commands

bool SCSICD::RequestSense(SCSIRequest *req)
{
    /* A pending unit attention is what the sense data reports. */
    if (fMediumChanged) {
        fMediumChanged = false;
        Fail(req, SCSI_SENSE_UNIT_ATTENTION, SCSI_ASC_MEDIUM_CHANGED);
    }
    /* Reporting sense clears it, so the next command starts clean. */
    uint32_t len = scsi_reply(req, fSense,
                              std::min<uint32_t>(SCSI_SENSE_LEN, req->cdb[4]));
    memset(fSense, 0, sizeof(fSense));
    fSense[0] = 0x70;
    fSense[7] = SCSI_SENSE_LEN - 8;
    req->status = SCSI_STATUS_GOOD;
    req->sense_len = 0;
    req->actual_length = len;
    return true;
}


bool SCSICD::Inquiry(SCSIRequest *req)
{
    uint8_t buf[96];
    uint32_t len;
    uint32_t alloc = get_be16(req->cdb + 3);

    memset(buf, 0, sizeof(buf));

    if ((req->cdb[1] & 0x01) != 0) {
        /* Vital product data. */
        switch (req->cdb[2]) {
        case 0x00: /* supported pages */
            buf[1] = 0x00;
            buf[3] = 3;
            buf[4] = 0x00;
            buf[5] = 0x80;
            buf[6] = 0x83;
            len = 7;
            break;

        case 0x80: /* unit serial number */
            buf[1] = 0x80;
            buf[3] = strlen(SCSI_CD_SERIAL);
            memcpy(buf + 4, SCSI_CD_SERIAL, buf[3]);
            len = 4 + buf[3];
            break;

        case 0x83: { /* device identification */
            int id_len = 8 + strlen(SCSI_CD_PRODUCT) + strlen(SCSI_CD_SERIAL);
            buf[1] = 0x83;
            buf[3] = 4 + id_len;
            buf[4] = 0x02; /* ASCII, protocol identifier not valid */
            buf[5] = 0x01; /* T10 vendor identification */
            buf[7] = id_len;
            memcpy(buf + 8, SCSI_CD_VENDOR, 8);
            memcpy(buf + 16, SCSI_CD_PRODUCT, strlen(SCSI_CD_PRODUCT));
            memcpy(buf + 16 + strlen(SCSI_CD_PRODUCT), SCSI_CD_SERIAL,
                   strlen(SCSI_CD_SERIAL));
            len = 8 + id_len;
            break;
        }

        default:
            Fail(req, SCSI_SENSE_ILLEGAL_REQUEST,
                 SCSI_ASC_INVALID_FIELD_IN_CDB);
            return true;
        }
        Reply(req, buf, len, alloc);
        return true;
    }

    /* Standard inquiry data. */
    buf[0] = SCSI_TYPE_CDROM;
    buf[1] = 0x80; /* removable */
    buf[2] = 0x05; /* claims conformance to SPC-3 */
    buf[3] = 0x02; /* response data format 2 */
    buf[4] = 31;   /* additional length, making 36 in total */
    memcpy(buf + 8, SCSI_CD_VENDOR, 8);
    memcpy(buf + 16, SCSI_CD_PRODUCT, 16);
    memcpy(buf + 32, SCSI_CD_REVISION, 4);

    Reply(req, buf, 36, alloc);
    return true;
}


bool SCSICD::ModeSense(SCSIRequest *req, bool is_10)
{
    uint8_t buf[64];
    uint8_t control = req->cdb[2] >> 6;
    uint8_t page = req->cdb[2] & 0x3f;
    uint32_t alloc = is_10 ? get_be16(req->cdb + 7) : req->cdb[4];
    int len = is_10 ? 8 : 4;

    memset(buf, 0, sizeof(buf));

    if (control == 3) {
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_SAVING_NOT_SUPPORTED);
        return true;
    }
    /* Nothing here can be changed, which is what a page of zeros says when
       the host asks for the changeable values. */
    bool values = control != 1;

    if (page != 0x01 && page != 0x2a && page != 0x3f) {
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB);
        return true;
    }
    if (page == 0x01 || page == 0x3f) {
        /* Read error recovery, with nothing to recover: the defaults. */
        buf[len + 0] = 0x01;
        buf[len + 1] = 0x0a;
        len += 12;
    }
    if (page == 0x2a || page == 0x3f) {
        /* Capabilities and mechanical status. */
        uint8_t *p = buf + len;
        p[0] = 0x2a;
        p[1] = 0x14; /* page length, making 22 in total */
        if (values) {
            p[2] = 0x3b; /* reads CD-R, CD-RW, method 2, DVD-ROM, DVD-R */
            p[4] = 0x70; /* mode 2 form 1 and 2, multi-session */
            /* Tray loading, eject, lock, and whether it is locked now. */
            p[6] = 0x29 | (fLocked ? 0x02 : 0);
            put_be16(p + 8, SCSI_CD_SPEED);  /* maximum read speed */
            put_be16(p + 12, 512);           /* buffer size in KB */
            put_be16(p + 14, SCSI_CD_SPEED); /* current read speed */
        }
        len += 22;
    }

    /* The mode data length counts everything after the field itself. */
    if (is_10) {
        put_be16(buf, len - 2);
    } else {
        buf[0] = len - 1;
    }
    Reply(req, buf, len, alloc);
    return true;
}


//#pragma mark - reading

bool SCSICD::ReadCapacity(SCSIRequest *req)
{
    uint8_t buf[8];

    if (!NeedMedium(req)) {
        return true;
    }
    put_be32(buf, (uint32_t)(fImageBlocks - 1));
    put_be32(buf + 4, SCSI_CD_BLOCK_SIZE);
    Good(req, scsi_reply(req, buf, sizeof(buf)));
    return true;
}


bool SCSICD::ReadBlocks(SCSIRequest *req, uint64_t lba, uint32_t blocks)
{
    if (!NeedMedium(req)) {
        return true;
    }
    if (blocks == 0) {
        Good(req, 0);
        return true;
    }
    if (lba >= fImageBlocks || blocks > fImageBlocks - lba) {
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_LBA_OUT_OF_RANGE);
        return true;
    }

    /* A data phase in the wrong direction or too short for the command cannot
       be served in part. */
    uint64_t length64 = (uint64_t)blocks * SCSI_CD_BLOCK_SIZE;
    if (req->dir != SCSI_DIR_FROM_DEV || length64 > req->buf_len) {
        scsi_set_phase_error(req);
        return true;
    }

    auto io = std::make_unique<IO>(*this);
    io->req = req;
    io->length = (uint32_t)length64;

    int ret = fImage->ReadAsync(lba * SCSI_CD_SECTORS_PER_BLOCK, req->buf,
                                blocks * SCSI_CD_SECTORS_PER_BLOCK, io.get());
    if (ret > 0) {
        fInFlight.push_back(std::move(io));
        return false;
    }
    if (ret < 0) {
        Fail(req, SCSI_SENSE_MEDIUM_ERROR, SCSI_ASC_UNRECOVERED_READ_ERROR);
        return true;
    }
    Good(req, io->length);
    return true;
}


void SCSICD::BlockDone(IO *io, int ret)
{
    SCSIRequest *req = io->req;

    if (ret < 0) {
        Fail(req, SCSI_SENSE_MEDIUM_ERROR, SCSI_ASC_UNRECOVERED_READ_ERROR);
    } else {
        Good(req, io->length);
    }

    /* Gone before the initiator hears of it, which may submit again or
       cancel from the completion. */
    auto it = std::find_if(fInFlight.begin(), fInFlight.end(),
                           [io](const std::unique_ptr<IO> &r) {
                               return r.get() == io;
                           });
    fInFlight.erase(it);

    if (req->completion != nullptr) {
        req->completion->Complete(req);
    }
}


/* READ CD, of which only the user data can be had: the image does not keep
   the sync, headers, error correction or subchannels of a raw sector. */
bool SCSICD::ReadCD(SCSIRequest *req)
{
    const uint8_t *cdb = req->cdb;
    uint8_t sector_type = (cdb[1] >> 2) & 0x07;
    uint32_t blocks = ((uint32_t)cdb[6] << 16) | ((uint32_t)cdb[7] << 8)
                    | cdb[8];
    uint8_t fields = cdb[9];

    if (!NeedMedium(req)) {
        return true;
    }
    /* Any type, or mode 1, which is what every block here is. */
    if (sector_type != 0 && sector_type != 2) {
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_ILLEGAL_MODE_FOR_TRACK);
        return true;
    }
    if (fields == 0) {
        /* No fields asked for: nothing moves. */
        Good(req, 0);
        return true;
    }
    if (fields != 0x10 || (cdb[10] & 0x07) != 0) {
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB);
        return true;
    }
    return ReadBlocks(req, get_be32(cdb + 2), blocks);
}


bool SCSICD::ReadSubChannel(SCSIRequest *req)
{
    const uint8_t *cdb = req->cdb;
    bool msf = (cdb[1] & 0x02) != 0;
    uint32_t alloc = get_be16(cdb + 7);
    uint8_t buf[24];
    int len = 4;

    if (!NeedMedium(req)) {
        return true;
    }
    memset(buf, 0, sizeof(buf));
    buf[1] = 0x15; /* no audio status to report: nothing plays */

    if ((cdb[2] & 0x40) != 0) {
        uint8_t *p = buf + 4;
        p[0] = cdb[3];
        switch (cdb[3]) {
        case 0x01: /* current position: the start of the data track */
            p[1] = MMC_ADR_CONTROL_DATA;
            p[2] = 1;
            p[3] = 1;
            cd_put_address(p + 4, 0, msf);
            len += 12;
            break;
        case 0x02: /* media catalogue number, which there is none of */
            len += 20;
            break;
        case 0x03: /* track ISRC, likewise */
            p[1] = MMC_ADR_CONTROL_DATA;
            p[2] = 1;
            len += 20;
            break;
        default:
            Fail(req, SCSI_SENSE_ILLEGAL_REQUEST,
                 SCSI_ASC_INVALID_FIELD_IN_CDB);
            return true;
        }
    }
    put_be16(buf + 2, len - 4);
    Reply(req, buf, len, alloc);
    return true;
}


//#pragma mark - disc structure

/* One track descriptor of the formatted TOC. */
static void cd_toc_entry(uint8_t *p, uint8_t track, uint64_t lba, bool msf)
{
    p[0] = 0;
    p[1] = MMC_ADR_CONTROL_DATA;
    p[2] = track;
    p[3] = 0;
    cd_put_address(p + 4, lba, msf);
}


/* One descriptor of the raw TOC, as the Q subchannel of the lead-in has it.
   The point's own address is all that is filled in. */
static void cd_raw_toc_entry(uint8_t *p, uint8_t point, uint8_t pmin,
                             uint8_t psec, uint8_t pframe)
{
    memset(p, 0, 11);
    p[0] = 1; /* session */
    p[1] = MMC_ADR_CONTROL_DATA;
    p[3] = point;
    p[8] = pmin;
    p[9] = psec;
    p[10] = pframe;
}


/* The disc is one session holding one data track; the TOC is made up from
   that and the image size. */
bool SCSICD::ReadTOC(SCSIRequest *req)
{
    const uint8_t *cdb = req->cdb;
    bool msf = (cdb[1] & 0x02) != 0;
    uint8_t format = cdb[2] & 0x0f;
    uint8_t track = cdb[6];
    uint32_t alloc = get_be16(cdb + 7);
    uint8_t buf[64];
    int len;

    if (!NeedMedium(req)) {
        return true;
    }
    /* Before MMC the format was in the top bits of the control byte, and
       some hosts still put it there. */
    if (format == 0) {
        format = cdb[9] >> 6;
    }

    memset(buf, 0, sizeof(buf));
    buf[2] = 1; /* first track, or first session */
    buf[3] = 1; /* last track, or last session */

    switch (format) {
    case 0: /* formatted TOC */
        if (track > 1 && track != 0xaa) {
            Fail(req, SCSI_SENSE_ILLEGAL_REQUEST,
                 SCSI_ASC_INVALID_FIELD_IN_CDB);
            return true;
        }
        len = 4;
        if (track <= 1) {
            cd_toc_entry(buf + len, 1, 0, msf);
            len += 8;
        }
        cd_toc_entry(buf + len, 0xaa, fImageBlocks, msf);
        len += 8;
        break;

    case 1: /* multi-session information: the one session */
        cd_toc_entry(buf + 4, 1, 0, msf);
        len = 12;
        break;

    case 2: { /* raw TOC, always in MSF */
        uint8_t leadout[3];
        cd_lba_to_msf(leadout, fImageBlocks);
        len = 4;
        cd_raw_toc_entry(buf + len, 0xa0, 1, 0x00, 0); /* first track, CD-ROM */
        len += 11;
        cd_raw_toc_entry(buf + len, 0xa1, 1, 0, 0);    /* last track */
        len += 11;
        cd_raw_toc_entry(buf + len, 0xa2, leadout[0], leadout[1], leadout[2]);
        len += 11;
        cd_raw_toc_entry(buf + len, 1, 0, 2, 0);       /* track 1 at 00:02:00 */
        len += 11;
        break;
    }

    default:
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB);
        return true;
    }

    put_be16(buf, len - 2);
    Reply(req, buf, len, alloc);
    return true;
}


bool SCSICD::ReadDiscInformation(SCSIRequest *req)
{
    uint8_t buf[34];

    if ((req->cdb[1] & 0x07) != 0) {
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB);
        return true;
    }
    if (!NeedMedium(req)) {
        return true;
    }
    memset(buf, 0, sizeof(buf));
    put_be16(buf, sizeof(buf) - 2);
    buf[2] = 0x0e; /* last session complete, disc complete */
    buf[3] = 1;    /* first track */
    buf[4] = 1;    /* sessions */
    buf[5] = 1;    /* first track of the last session */
    buf[6] = 1;    /* last track of the last session */
    /* No lead-in to start and no lead-out to come: the disc is closed. */
    memset(buf + 16, 0xff, 8);
    Reply(req, buf, sizeof(buf), get_be16(req->cdb + 7));
    return true;
}


bool SCSICD::ReadTrackInformation(SCSIRequest *req)
{
    const uint8_t *cdb = req->cdb;
    uint32_t address = get_be32(cdb + 2);
    uint8_t buf[36];
    bool found;

    if (!NeedMedium(req)) {
        return true;
    }
    switch (cdb[1] & 0x03) {
    case 0:  found = address < fImageBlocks; break; /* by block */
    case 1:  found = address == 1; break;           /* by track */
    case 2:  found = address == 1; break;           /* by session */
    default: found = false; break;
    }
    if (!found) {
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB);
        return true;
    }

    memset(buf, 0, sizeof(buf));
    put_be16(buf, sizeof(buf) - 2);
    buf[2] = 1;    /* track */
    buf[3] = 1;    /* session */
    buf[5] = 0x04; /* data track, recorded uninterrupted */
    buf[6] = 0x01; /* data mode 1 */
    put_be32(buf + 24, (uint32_t)fImageBlocks);     /* track size */
    put_be32(buf + 28, (uint32_t)fImageBlocks - 1); /* last recorded */
    Reply(req, buf, sizeof(buf), get_be16(cdb + 7));
    return true;
}


bool SCSICD::ReadDVDStructure(SCSIRequest *req)
{
    const uint8_t *cdb = req->cdb;
    uint8_t format = cdb[7];
    uint32_t alloc = get_be16(cdb + 8);
    uint8_t buf[4 + 2048];
    int len = 4;

    /* Media type 0 is a DVD; the others are formats this drive cannot
       read. */
    if ((cdb[1] & 0x0f) != 0) {
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB);
        return true;
    }
    if (format != 0xff) {
        if (!NeedMedium(req)) {
            return true;
        }
        if (!IsDVD()) {
            Fail(req, SCSI_SENSE_ILLEGAL_REQUEST,
                 SCSI_ASC_INCOMPATIBLE_MEDIUM);
            return true;
        }
        if (cdb[6] != 0) {
            /* There is only the one layer. */
            Fail(req, SCSI_SENSE_ILLEGAL_REQUEST,
                 SCSI_ASC_INVALID_FIELD_IN_CDB);
            return true;
        }
    }

    memset(buf, 0, sizeof(buf));
    uint8_t *p = buf + 4;
    switch (format) {
    case 0x00: /* physical format */
        p[0] = 0x01; /* DVD-ROM, version 1 */
        p[1] = 0x0f; /* 120 mm, no maximum rate given */
        p[2] = 0x01; /* one layer, embossed */
        put_be32(p + 4, SCSI_CD_DVD_START_SECTOR);
        put_be32(p + 8, SCSI_CD_DVD_START_SECTOR + (uint32_t)fImageBlocks - 1);
        len += 2048;
        break;
    case 0x01: /* copyright: no protection, every region */
        len += 4;
        break;
    case 0x04: /* manufacturer's information: none */
        len += 2048;
        break;
    case 0xff: { /* the structures above, and that each can be read */
        static const struct {
            uint8_t format;
            uint16_t length;
        } list[] = {{0x00, 2048}, {0x01, 4}, {0x04, 2048}, {0xff, 16}};
        for (const auto &entry : list) {
            p[0] = entry.format;
            p[1] = 0x40;
            put_be16(p + 2, entry.length);
            p += 4;
            len += 4;
        }
        break;
    }
    default:
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB);
        return true;
    }

    put_be16(buf, len - 2);
    Reply(req, buf, len, alloc);
    return true;
}


//#pragma mark - configuration and events

bool SCSICD::GetConfiguration(SCSIRequest *req)
{
    const uint8_t *cdb = req->cdb;
    uint8_t request_type = cdb[1] & 0x03;
    uint16_t start = get_be16(cdb + 2);
    uint8_t buf[128];
    int len = 8;

    if (request_type == 3) {
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB);
        return true;
    }

    memset(buf, 0, sizeof(buf));
    uint16_t profile = CurrentProfile();
    put_be16(buf + 6, profile);

    /* All features from 'start' on, only the current ones, or 'start'
       alone, as the request type asks. */
    auto add = [&](uint16_t code, uint8_t version, bool persistent,
                   bool current, const uint8_t *data, uint8_t data_len) {
        bool wanted = request_type == 2 ? code == start
                                        : code >= start &&
                                          (request_type == 0 || current);
        if (!wanted) {
            return;
        }
        uint8_t *p = buf + len;
        put_be16(p, code);
        p[2] = (version << 2) | (persistent ? 0x02 : 0) | (current ? 0x01 : 0);
        p[3] = data_len;
        if (data_len > 0) {
            memcpy(p + 4, data, data_len);
        }
        len += 4 + data_len;
    };

    bool dvd = fMediumPresent && IsDVD();
    bool cd = fMediumPresent && !IsDVD();

    /* Profile list: the profiles the drive has, highest first. */
    const uint8_t profiles[] = {
        0x00, MMC_PROFILE_DVD_ROM, dvd ? (uint8_t)1 : (uint8_t)0, 0,
        0x00, MMC_PROFILE_CD_ROM,  cd ? (uint8_t)1 : (uint8_t)0, 0,
    };
    add(0x0000, 0, true, true, profiles, sizeof(profiles));

    /* Core: no particular physical interface is claimed, since the same
       unit answers over SCSI and ATAPI. */
    const uint8_t core[8] = {};
    add(0x0001, 1, true, true, core, sizeof(core));

    /* Morphing: media changes are reported through polled events. */
    const uint8_t morphing[4] = {0x02};
    add(0x0002, 1, true, true, morphing, sizeof(morphing));

    /* Removable medium: a tray that ejects and locks. */
    const uint8_t removable[4] = {0x29};
    add(0x0003, 0, true, true, removable, sizeof(removable));

    /* Random readable, in 2048 byte blocks: one block per ECC unit on a CD,
       sixteen on a DVD. */
    uint8_t random[8] = {};
    put_be32(random, SCSI_CD_BLOCK_SIZE);
    put_be16(random + 4, dvd ? 16 : 1);
    add(0x0010, 0, false, fMediumPresent, random, sizeof(random));

    const uint8_t cd_read[4] = {};
    add(0x001e, 0, false, cd, cd_read, sizeof(cd_read));
    add(0x001f, 0, false, dvd, nullptr, 0);

    /* The length counts everything after the field itself. */
    put_be32(buf, len - 4);
    Reply(req, buf, len, get_be16(cdb + 7));
    return true;
}


/* Only the polled form, and only the media class: that is what hosts poll
   to notice a disc going in or out. */
bool SCSICD::GetEventStatus(SCSIRequest *req)
{
    const uint8_t *cdb = req->cdb;
    uint8_t buf[8];
    int len;

    if ((cdb[1] & 0x01) == 0) {
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB);
        return true;
    }

    memset(buf, 0, sizeof(buf));
    buf[3] = 1 << MMC_EVENT_CLASS_MEDIA; /* the classes there are */
    if ((cdb[4] & (1 << MMC_EVENT_CLASS_MEDIA)) != 0) {
        buf[2] = MMC_EVENT_CLASS_MEDIA;
        buf[4] = fMediaEvent;
        buf[5] = (fMediumPresent ? 0x02 : 0) | (fTrayOpen ? 0x01 : 0);
        fMediaEvent = MMC_MEDIA_EVENT_NONE;
        len = 8;
    } else {
        /* None of the classes asked for exists: no event available. */
        buf[2] = 0x80;
        len = 4;
    }
    put_be16(buf, len - 2);
    Reply(req, buf, len, get_be16(cdb + 7));
    return true;
}


bool SCSICD::MechanismStatus(SCSIRequest *req)
{
    uint8_t buf[8];

    memset(buf, 0, sizeof(buf));
    buf[1] = fTrayOpen ? 0x10 : 0; /* idle, and the door */
    Reply(req, buf, sizeof(buf), get_be16(req->cdb + 8));
    return true;
}


//#pragma mark - dispatch

bool SCSICD::DataPhase(const uint8_t *cdb, SCSIDirEnum *dir, uint32_t *len)
{
    switch (cdb[0]) {
    case SCSI_SET_CD_SPEED:
        *dir = SCSI_DIR_NONE;
        *len = 0;
        return true;

    case SCSI_READ_SUB_CHANNEL:
    case SCSI_READ_TOC:
    case SCSI_GET_CONFIGURATION:
    case SCSI_GET_EVENT_STATUS:
    case SCSI_READ_DISC_INFORMATION:
    case SCSI_READ_TRACK_INFORMATION:
        *dir = SCSI_DIR_FROM_DEV;
        *len = get_be16(cdb + 7);
        return true;

    case SCSI_READ_DVD_STRUCTURE:
    case SCSI_MECHANISM_STATUS:
        *dir = SCSI_DIR_FROM_DEV;
        *len = get_be16(cdb + 8);
        return true;

    case SCSI_READ_CD: {
        uint32_t blocks = ((uint32_t)cdb[6] << 16) | ((uint32_t)cdb[7] << 8)
                        | cdb[8];
        /* User data alone is all that is served; anything more is refused,
           but sized as the raw sector it asks for. */
        uint64_t per_block = cdb[9] == 0 ? 0
                           : cdb[9] == 0x10 ? SCSI_CD_BLOCK_SIZE : 2352;
        uint64_t bytes = blocks * per_block;
        *dir = SCSI_DIR_FROM_DEV;
        *len = bytes > 0xffffffff ? 0xffffffff : (uint32_t)bytes;
        return true;
    }
    }
    return SCSIDevice::DataPhase(cdb, dir, len);
}


bool SCSICD::Submit(SCSIRequest *req)
{
    const uint8_t *cdb = req->cdb;

    if (req->lun != Lun()) {
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_LUN_NOT_SUPPORTED);
        return true;
    }

    /* A disc that went in is reported once, to the first command that is
       not one of those a unit attention leaves alone. */
    if (fMediumChanged) {
        switch (cdb[0]) {
        case SCSI_INQUIRY:
        case SCSI_REQUEST_SENSE:
        case SCSI_REPORT_LUNS:
        case SCSI_GET_CONFIGURATION:
        case SCSI_GET_EVENT_STATUS:
            break;
        default:
            fMediumChanged = false;
            Fail(req, SCSI_SENSE_UNIT_ATTENTION, SCSI_ASC_MEDIUM_CHANGED);
            return true;
        }
    }

    switch (cdb[0]) {
    case SCSI_TEST_UNIT_READY:
        if (NeedMedium(req)) {
            Good(req, 0);
        }
        return true;

    case SCSI_REQUEST_SENSE:
        return RequestSense(req);

    case SCSI_INQUIRY:
        return Inquiry(req);

    case SCSI_MODE_SENSE_6:
        return ModeSense(req, false);

    case SCSI_MODE_SENSE_10:
        return ModeSense(req, true);

    case SCSI_MODE_SELECT_6:
    case SCSI_MODE_SELECT_10:
        /* Nothing here has a mode worth setting, but refusing the command
           makes hosts retry it forever. */
    case SCSI_SEEK_10:
    case SCSI_SET_CD_SPEED:
        Good(req, 0);
        return true;

    case SCSI_PREVENT_ALLOW_REMOVAL:
        fLocked = (cdb[4] & 0x01) != 0;        Good(req, 0);
        return true;

    case SCSI_START_STOP_UNIT:
        return StartStop(req);

    case SCSI_READ_CAPACITY_10:
        return ReadCapacity(req);

    case SCSI_READ_6: {
        uint64_t lba = ((uint32_t)(cdb[1] & 0x1f) << 16)
                     | ((uint32_t)cdb[2] << 8) | cdb[3];
        return ReadBlocks(req, lba, cdb[4] == 0 ? 256 : cdb[4]);
    }

    case SCSI_READ_10:
        return ReadBlocks(req, get_be32(cdb + 2), get_be16(cdb + 7));

    case SCSI_READ_12:
        return ReadBlocks(req, get_be32(cdb + 2), get_be32(cdb + 6));

    case SCSI_READ_CD:
        return ReadCD(req);

    case SCSI_READ_SUB_CHANNEL:
        return ReadSubChannel(req);

    case SCSI_READ_TOC:
        return ReadTOC(req);

    case SCSI_GET_CONFIGURATION:
        return GetConfiguration(req);

    case SCSI_GET_EVENT_STATUS:
        return GetEventStatus(req);

    case SCSI_READ_DISC_INFORMATION:
        return ReadDiscInformation(req);

    case SCSI_READ_TRACK_INFORMATION:
        return ReadTrackInformation(req);

    case SCSI_READ_DVD_STRUCTURE:
        return ReadDVDStructure(req);

    case SCSI_MECHANISM_STATUS:
        return MechanismStatus(req);
    }

    Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_COMMAND_OPERATION);
    return true;
}


//#pragma mark - factory

std::unique_ptr<SCSIDevice> scsi_cd_create(std::unique_ptr<HostBlockDevice> bs)
{
    if (bs != nullptr) {
        int64_t sectors = bs->SectorCount();
        /* Only a plain ISO is understood. A raw image has 2352 byte sectors
           and needs its cue sheet to make sense of. */
        if (sectors <= 0 || sectors % SCSI_CD_SECTORS_PER_BLOCK != 0) {
            vm_error("cd: the image is not a whole number of 2048 byte "
                     "blocks; only plain ISO images are supported\n");
            return nullptr;
        }
    }
    return std::make_unique<SCSICD>(std::move(bs));
}


Device *scsi_cd_node_create(std::unique_ptr<HostBlockDevice> bs, int target,
                            int lun)
{
    auto dev = scsi_cd_create(std::move(bs));
    if (dev == nullptr) {
        return nullptr;
    }
    return new SCSIDeviceNode("scsi-cd", std::move(dev), target, lun);
}
