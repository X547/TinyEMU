/*
 * SCSI direct access block device
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
#include <stdio.h>
#include <string.h>

#include "cutils.h"
#include "machine.h"
#include "scsi.h"
#include "virtio.h"

/* The backing HostBlockDevice counts in 512 byte sectors, so that is also the
   logical block size this reports. */
#define SCSI_DISK_BLOCK_SIZE 512

#define SCSI_DISK_VENDOR   "TinyEMU "         /*  8 characters, padded */
#define SCSI_DISK_PRODUCT  "USB Disk        " /* 16 characters, padded */
#define SCSI_DISK_REVISION "1.0 "             /*  4 characters, padded */
#define SCSI_DISK_SERIAL   "TEMU00000001"


//#pragma mark - SCSIDisk

class SCSIDisk final: public SCSIDevice {
private:
    /* One block request may be outstanding, which is all a bulk-only
       transport can produce: it will not send the next command until it has
       collected the status of this one. */
    class Completion final: public BlockCompletion {
    private:
        SCSIDisk &fDisk;

    public:
        Completion(SCSIDisk &disk): fDisk(disk) {}

        void Complete(int ret) override {fDisk.BlockDone(ret);}
    };

    std::unique_ptr<HostBlockDevice> fBlockDev;
    Completion fCompletion {*this};
    SCSIRequest *fPending = nullptr;
    uint32_t fPendingLength = 0;

    /* The sense data a failed command left behind, for the REQUEST SENSE that
       a bulk-only transport's host sends afterwards. */
    uint8_t fSense[SCSI_SENSE_LEN] {};

    void Fail(SCSIRequest *req, uint8_t key, uint16_t asc_ascq);
    void Good(SCSIRequest *req, uint32_t length);
    void BlockDone(int ret);

    bool Inquiry(SCSIRequest *req);
    bool ModeSense(SCSIRequest *req, bool is_10);
    bool ReadCapacity10(SCSIRequest *req);
    bool ReadCapacity16(SCSIRequest *req);
    bool ReadWrite(SCSIRequest *req, uint64_t lba, uint32_t blocks,
                   bool is_write);

public:
    SCSIDisk(std::unique_ptr<HostBlockDevice> bs):
        SCSIDevice("scsi-disk"), fBlockDev(std::move(bs)) {}

    void Reset() override;
    bool Submit(SCSIRequest *req) override;

    uint32_t BlockSize() override {return SCSI_DISK_BLOCK_SIZE;}
    uint64_t BlockCount() override {return fBlockDev->SectorCount();}
};


void SCSIDisk::Reset()
{
    /* A request in flight cannot be recalled from the back end, so it is
       disowned instead: BlockDone() drops a completion whose request has
       already gone. */
    fPending = nullptr;
    fPendingLength = 0;
    memset(fSense, 0, sizeof(fSense));
}


/* Record the failure both in the request, for a transport that reports sense
   itself, and in the unit, for the REQUEST SENSE that follows. */
void SCSIDisk::Fail(SCSIRequest *req, uint8_t key, uint16_t asc_ascq)
{
    scsi_set_sense(req, key, asc_ascq);
    memcpy(fSense, req->sense, SCSI_SENSE_LEN);
}


void SCSIDisk::Good(SCSIRequest *req, uint32_t length)
{
    scsi_set_good(req, length);
    memset(fSense, 0, sizeof(fSense));
    fSense[0] = 0x70;
    fSense[7] = SCSI_SENSE_LEN - 8;
}


/* Copy a reply into the initiator's buffer, clamped to the room it has. Coming
   up short is a short transfer, not an error: the transport reports the
   residue and the host decides what to make of it. */
static uint32_t scsi_reply(SCSIRequest *req, const uint8_t *data, uint32_t len)
{
    if (len > req->buf_len) {
        len = req->buf_len;
    }
    if (len > 0 && req->buf != nullptr) {
        memcpy(req->buf, data, len);
    }
    return len;
}


bool SCSIDisk::Inquiry(SCSIRequest *req)
{
    uint8_t buf[96];
    uint32_t len;

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
            buf[3] = strlen(SCSI_DISK_SERIAL);
            memcpy(buf + 4, SCSI_DISK_SERIAL, buf[3]);
            len = 4 + buf[3];
            break;

        case 0x83: { /* device identification */
            /* One T10 vendor identification designator: the vendor string
               followed by the product and the serial, which is what makes the
               unit distinguishable from any other. */
            int id_len = 8 + strlen(SCSI_DISK_PRODUCT)
                           + strlen(SCSI_DISK_SERIAL);
            buf[1] = 0x83;
            buf[3] = 4 + id_len;
            buf[4] = 0x02; /* ASCII, protocol identifier not valid */
            buf[5] = 0x01; /* T10 vendor identification */
            buf[7] = id_len;
            memcpy(buf + 8, SCSI_DISK_VENDOR, 8);
            memcpy(buf + 16, SCSI_DISK_PRODUCT, strlen(SCSI_DISK_PRODUCT));
            memcpy(buf + 16 + strlen(SCSI_DISK_PRODUCT), SCSI_DISK_SERIAL,
                   strlen(SCSI_DISK_SERIAL));
            len = 8 + id_len;
            break;
        }

        default:
            Fail(req, SCSI_SENSE_ILLEGAL_REQUEST,
                 SCSI_ASC_INVALID_FIELD_IN_CDB);
            return true;
        }
        Good(req, scsi_reply(req, buf, len));
        return true;
    }

    /* Standard inquiry data. */
    buf[0] = 0x00; /* direct access block device, connected */
    buf[1] = 0x00; /* not removable: the image is always there */
    buf[2] = 0x05; /* claims conformance to SPC-3 */
    buf[3] = 0x02; /* response data format 2, as everything since SCSI-2 */
    buf[4] = 31;   /* additional length, making 36 in total */
    memcpy(buf + 8, SCSI_DISK_VENDOR, 8);
    memcpy(buf + 16, SCSI_DISK_PRODUCT, 16);
    memcpy(buf + 32, SCSI_DISK_REVISION, 4);

    Good(req, scsi_reply(req, buf, 36));
    return true;
}


bool SCSIDisk::ModeSense(SCSIRequest *req, bool is_10)
{
    uint8_t buf[64];
    uint8_t page = req->cdb[2] & 0x3f;
    int header_len = is_10 ? 8 : 4;
    int len = header_len;

    memset(buf, 0, sizeof(buf));

    if (page == 0x08 || page == 0x3f) {
        /* Caching mode page. The write cache is reported disabled because
           writes here really do reach the back end before the command is
           answered, so a host has nothing to flush. */
        buf[len + 0] = 0x08;
        buf[len + 1] = 0x12; /* page length, making 20 in total */
        len += 20;
    } else if (page != 0x00) {
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_FIELD_IN_CDB);
        return true;
    }

    /* The mode data length counts everything after the field itself. */
    if (is_10) {
        put_be16(buf, len - 2);
    } else {
        buf[0] = len - 1;
    }

    Good(req, scsi_reply(req, buf, len));
    return true;
}


bool SCSIDisk::ReadCapacity10(SCSIRequest *req)
{
    uint8_t buf[8];
    uint64_t last_lba = BlockCount() - 1;

    /* A disk too large for the ten byte form reports all ones, which is how
       the host is told to ask again with READ CAPACITY (16). */
    put_be32(buf, last_lba > 0xffffffff ? 0xffffffff : (uint32_t)last_lba);
    put_be32(buf + 4, SCSI_DISK_BLOCK_SIZE);

    Good(req, scsi_reply(req, buf, sizeof(buf)));
    return true;
}


bool SCSIDisk::ReadCapacity16(SCSIRequest *req)
{
    uint8_t buf[32];
    uint64_t last_lba = BlockCount() - 1;

    memset(buf, 0, sizeof(buf));
    put_be32(buf, last_lba >> 32);
    put_be32(buf + 4, last_lba & 0xffffffff);
    put_be32(buf + 8, SCSI_DISK_BLOCK_SIZE);

    Good(req, scsi_reply(req, buf, sizeof(buf)));
    return true;
}


bool SCSIDisk::ReadWrite(SCSIRequest *req, uint64_t lba, uint32_t blocks,
                         bool is_write)
{
    uint64_t count = BlockCount();

    if (blocks == 0) {
        /* A zero length transfer is legal and moves nothing. */
        Good(req, 0);
        return true;
    }
    if (lba >= count || blocks > count - lba) {
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_LBA_OUT_OF_RANGE);
        return true;
    }

    uint32_t length = blocks * SCSI_DISK_BLOCK_SIZE;
    if (length > req->buf_len) {
        /* The initiator promised less room than the command needs. Serve what
           fits, in whole blocks, and let the residue say so. */
        blocks = req->buf_len / SCSI_DISK_BLOCK_SIZE;
        if (blocks == 0) {
            Fail(req, SCSI_SENSE_ILLEGAL_REQUEST,
                 SCSI_ASC_INVALID_FIELD_IN_CDB);
            return true;
        }
        length = blocks * SCSI_DISK_BLOCK_SIZE;
    }

    if (fPending != nullptr) {
        /* Nothing should be able to reach here: the transport waits for a
           command's status before sending the next one. */
        req->status = SCSI_STATUS_BUSY;
        req->actual_length = 0;
        return true;
    }

    int ret;
    if (is_write) {
        ret = fBlockDev->WriteAsync(lba, req->buf, blocks, &fCompletion);
    } else {
        ret = fBlockDev->ReadAsync(lba, req->buf, blocks, &fCompletion);
    }

    if (ret > 0) {
        /* In flight; the completion finishes the request. */
        fPending = req;
        fPendingLength = length;
        return false;
    }
    if (ret < 0) {
        Fail(req, SCSI_SENSE_MEDIUM_ERROR,
             is_write ? SCSI_ASC_WRITE_FAULT : SCSI_ASC_UNRECOVERED_READ_ERROR);
        return true;
    }
    Good(req, length);
    return true;
}


void SCSIDisk::BlockDone(int ret)
{
    SCSIRequest *req = fPending;
    uint32_t length = fPendingLength;

    if (req == nullptr) {
        /* The unit was reset while the back end still had the request. */
        return;
    }
    fPending = nullptr;
    fPendingLength = 0;

    if (ret < 0) {
        Fail(req, SCSI_SENSE_MEDIUM_ERROR, SCSI_ASC_UNRECOVERED_READ_ERROR);
    } else {
        Good(req, length);
    }
    if (req->completion != nullptr) {
        req->completion->Complete(req);
    }
}


bool SCSIDisk::Submit(SCSIRequest *req)
{
    const uint8_t *cdb = req->cdb;

    if (req->lun != Lun()) {
        Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_LUN_NOT_SUPPORTED);
        return true;
    }

    switch (cdb[0]) {
    case SCSI_TEST_UNIT_READY:
        Good(req, 0);
        return true;

    case SCSI_REQUEST_SENSE:
        /* Reporting sense clears it, so the next command starts clean. */
        {
            uint32_t len = scsi_reply(req, fSense, SCSI_SENSE_LEN);
            memset(fSense, 0, sizeof(fSense));
            fSense[0] = 0x70;
            fSense[7] = SCSI_SENSE_LEN - 8;
            req->status = SCSI_STATUS_GOOD;
            req->sense_len = 0;
            req->actual_length = len;
        }
        return true;

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
        Good(req, 0);
        return true;

    case SCSI_START_STOP_UNIT:
    case SCSI_PREVENT_ALLOW_REMOVAL:
    case SCSI_SEEK_10:
        Good(req, 0);
        return true;

    case SCSI_SYNCHRONIZE_CACHE_10:
    case SCSI_SYNCHRONIZE_CACHE_16:
        /* Writes are already through to the back end when their command is
           answered, so there is nothing to flush. */
        Good(req, 0);
        return true;

    case SCSI_VERIFY_10:
    case SCSI_VERIFY_12:
    case SCSI_VERIFY_16:
        Good(req, 0);
        return true;

    case SCSI_READ_CAPACITY_10:
        return ReadCapacity10(req);

    case SCSI_SERVICE_ACTION_IN_16:
        if ((cdb[1] & 0x1f) == SCSI_SAI_READ_CAPACITY_16) {
            return ReadCapacity16(req);
        }
        break;

    case SCSI_READ_6:
    case SCSI_WRITE_6: {
        /* The six byte forms carry a 21 bit block address, and a transfer
           length of zero means 256 blocks rather than none. */
        uint64_t lba = ((uint32_t)(cdb[1] & 0x1f) << 16)
                     | ((uint32_t)cdb[2] << 8) | cdb[3];
        uint32_t blocks = cdb[4] == 0 ? 256 : cdb[4];
        return ReadWrite(req, lba, blocks, cdb[0] == SCSI_WRITE_6);
    }

    case SCSI_READ_10:
    case SCSI_WRITE_10:
        return ReadWrite(req, get_be32(cdb + 2), get_be16(cdb + 7),
                         cdb[0] == SCSI_WRITE_10);

    case SCSI_READ_12:
    case SCSI_WRITE_12:
        return ReadWrite(req, get_be32(cdb + 2), get_be32(cdb + 6),
                         cdb[0] == SCSI_WRITE_12);

    case SCSI_READ_16:
    case SCSI_WRITE_16:
        return ReadWrite(req,
                         ((uint64_t)get_be32(cdb + 2) << 32)
                             | get_be32(cdb + 6),
                         get_be32(cdb + 10), cdb[0] == SCSI_WRITE_16);
    }

    Fail(req, SCSI_SENSE_ILLEGAL_REQUEST, SCSI_ASC_INVALID_COMMAND_OPERATION);
    return true;
}


//#pragma mark - factory

Device *scsi_disk_node_create(std::unique_ptr<HostBlockDevice> bs, int lun)
{
    return new SCSIDeviceNode("scsi-disk",
                              std::make_unique<SCSIDisk>(std::move(bs)), lun);
}
