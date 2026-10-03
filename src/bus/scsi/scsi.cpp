/*
 * SCSI device model and command requests
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
#include "scsi.h"

#include <string.h>

#include "bits.h"
#include "cutils.h"
#include "machine.h"


void scsi_set_sense(SCSIRequest *req, uint8_t sense_key, uint16_t asc_ascq)
{
    memset(req->sense, 0, sizeof(req->sense));
    req->sense[0] = 0x70;              /* current error, fixed format */
    req->sense[2] = get_bits(sense_key, 0, 4);
    req->sense[7] = SCSI_SENSE_LEN - 8; /* additional sense length */
    req->sense[12] = asc_ascq >> 8;
    req->sense[13] = get_bits(asc_ascq, 0, 8);
    req->sense_len = SCSI_SENSE_LEN;
    req->status = SCSI_STATUS_CHECK_CONDITION;
    req->actual_length = 0;
}


void scsi_set_good(SCSIRequest *req, uint32_t length)
{
    req->status = SCSI_STATUS_GOOD;
    req->sense_len = 0;
    req->actual_length = length;
}


void scsi_set_phase_error(SCSIRequest *req)
{
    req->status = SCSI_STATUS_CHECK_CONDITION;
    req->sense_len = 0;
    req->actual_length = 0;
    req->phase_error = true;
}


uint32_t scsi_reply(SCSIRequest *req, const uint8_t *data, uint32_t len)
{
    if (len > req->buf_len) {
        len = req->buf_len;
    }
    if (len > 0 && req->buf != nullptr) {
        memcpy(req->buf, data, len);
    }
    return len;
}


void scsi_no_unit(SCSIRequest *req)
{
    uint8_t buf[36];
    uint32_t len;

    switch (req->cdb[0]) {
    case SCSI_INQUIRY:
        memset(buf, 0, sizeof(buf));
        buf[0] = 0x7f; /* no unit here, and none can be */
        buf[2] = 0x05;
        buf[3] = 0x02;
        buf[4] = sizeof(buf) - 5;
        len = req->buf_len < sizeof(buf) ? req->buf_len : sizeof(buf);
        if (len > 0) {
            memcpy(req->buf, buf, len);
        }
        scsi_set_good(req, len);
        break;

    case SCSI_REQUEST_SENSE:
        scsi_set_sense(req, SCSI_SENSE_ILLEGAL_REQUEST,
                       SCSI_ASC_LUN_NOT_SUPPORTED);
        len = req->buf_len < SCSI_SENSE_LEN ? req->buf_len : SCSI_SENSE_LEN;
        if (len > 0) {
            memcpy(req->buf, req->sense, len);
        }
        scsi_set_good(req, len);
        break;

    default:
        scsi_set_sense(req, SCSI_SENSE_ILLEGAL_REQUEST,
                       SCSI_ASC_LUN_NOT_SUPPORTED);
        break;
    }
}


int scsi_cdb_len(uint8_t opcode)
{
    /* The group code in the top three bits of the operation code fixes the
       length of every command but the two vendor specific groups. */
    switch (opcode >> 5) {
    case 0: return 6;
    case 1:
    case 2: return 10;
    case 4: return 16;
    case 5: return 12;
    default: return 0;
    }
}


uint32_t scsi_report_luns(uint8_t *buf, uint32_t buf_len,
                          SCSIDevice *const *units, int count)
{
    uint8_t entry[8];
    uint32_t len = 8;

    if (buf_len == 0) {
        return 0;
    }
    memset(buf, 0, buf_len < 8 ? buf_len : 8);
    for (int i = 0; i < count; i++) {
        if (units[i] == nullptr) {
            continue;
        }
        /* Single level addressing: the unit number goes in byte one. */
        memset(entry, 0, sizeof(entry));
        entry[1] = i;
        if (len + 8 <= buf_len) {
            memcpy(buf + len, entry, 8);
        }
        len += 8;
    }
    if (buf_len >= 4) {
        put_be32(buf, len - 8);
    }
    return len < buf_len ? len : buf_len;
}


//#pragma mark - SCSIDevice

bool SCSIDevice::DataPhase(const uint8_t *cdb, SCSIDirEnum *dir,
                           uint32_t *len)
{
    uint64_t blocks;

    *dir = SCSI_DIR_NONE;
    *len = 0;

    switch (cdb[0]) {
    case SCSI_TEST_UNIT_READY:
    case SCSI_START_STOP_UNIT:
    case SCSI_PREVENT_ALLOW_REMOVAL:
    case SCSI_SEEK_10:
    case SCSI_SYNCHRONIZE_CACHE_10:
    case SCSI_SYNCHRONIZE_CACHE_16:
        return true;

    /* Allocation lengths: the most the initiator will take. */
    case SCSI_REQUEST_SENSE:
    case SCSI_MODE_SENSE_6:
        *dir = SCSI_DIR_FROM_DEV;
        *len = cdb[4];
        return true;
    case SCSI_INQUIRY:
        *dir = SCSI_DIR_FROM_DEV;
        *len = get_be16(cdb + 3);
        return true;
    case SCSI_MODE_SENSE_10:
        *dir = SCSI_DIR_FROM_DEV;
        *len = get_be16(cdb + 7);
        return true;
    case SCSI_READ_CAPACITY_10:
        *dir = SCSI_DIR_FROM_DEV;
        *len = 8;
        return true;
    case SCSI_SERVICE_ACTION_IN_16:
        *dir = SCSI_DIR_FROM_DEV;
        *len = get_be32(cdb + 10);
        return true;
    case SCSI_REPORT_LUNS:
        *dir = SCSI_DIR_FROM_DEV;
        *len = get_be32(cdb + 6);
        return true;

    /* Parameter lists the initiator sends. */
    case SCSI_MODE_SELECT_6:
        *dir = SCSI_DIR_TO_DEV;
        *len = cdb[4];
        return true;
    case SCSI_MODE_SELECT_10:
        *dir = SCSI_DIR_TO_DEV;
        *len = get_be16(cdb + 7);
        return true;

    /* Block transfers. The six byte forms count 0 as 256 blocks. */
    case SCSI_READ_6:
    case SCSI_WRITE_6:
        blocks = cdb[4] == 0 ? 256 : cdb[4];
        *dir = cdb[0] == SCSI_READ_6 ? SCSI_DIR_FROM_DEV : SCSI_DIR_TO_DEV;
        break;
    case SCSI_READ_10:
    case SCSI_WRITE_10:
        blocks = get_be16(cdb + 7);
        *dir = cdb[0] == SCSI_READ_10 ? SCSI_DIR_FROM_DEV : SCSI_DIR_TO_DEV;
        break;
    case SCSI_READ_12:
    case SCSI_WRITE_12:
        blocks = get_be32(cdb + 6);
        *dir = cdb[0] == SCSI_READ_12 ? SCSI_DIR_FROM_DEV : SCSI_DIR_TO_DEV;
        break;
    case SCSI_READ_16:
    case SCSI_WRITE_16:
        blocks = get_be32(cdb + 10);
        *dir = cdb[0] == SCSI_READ_16 ? SCSI_DIR_FROM_DEV : SCSI_DIR_TO_DEV;
        break;

    /* VERIFY carries data only when it compares bytes (BYTCHK). */
    case SCSI_VERIFY_10:
        blocks = (cdb[1] & 0x02) != 0 ? get_be16(cdb + 7) : 0;
        *dir = blocks != 0 ? SCSI_DIR_TO_DEV : SCSI_DIR_NONE;
        break;
    case SCSI_VERIFY_12:
        blocks = (cdb[1] & 0x02) != 0 ? get_be32(cdb + 6) : 0;
        *dir = blocks != 0 ? SCSI_DIR_TO_DEV : SCSI_DIR_NONE;
        break;
    case SCSI_VERIFY_16:
        blocks = (cdb[1] & 0x02) != 0 ? get_be32(cdb + 10) : 0;
        *dir = blocks != 0 ? SCSI_DIR_TO_DEV : SCSI_DIR_NONE;
        break;

    default:
        return false;
    }

    /* Too long for one data phase: no initiator can stage it, so the
       transport refuses it rather than wrapping the length. */
    uint64_t bytes = blocks * BlockSize();
    *len = bytes > 0xffffffff ? 0xffffffff : (uint32_t)bytes;
    return true;
}


//#pragma mark - SCSIBus

bool SCSIBus::AssignResources(Device *dev)
{
    /* A logical unit is reached through its initiator, so it holds no host
       address space and no interrupt line. */
    for (int i = 0; i < dev->ResourceCount(); i++) {
        if (dev->ResourceAt(i)->type != RES_NONE) {
            vm_error("scsi bus: device '%s' declared a resource, but a SCSI "
                     "device has none of its own\n", dev->Name());
            return false;
        }
    }
    return true;
}


//#pragma mark - SCSIDeviceNode

SCSIDeviceNode::SCSIDeviceNode(const char *name,
                               std::unique_ptr<SCSIDevice> dev, int target,
                               int lun):
    Device(name), fDev(std::move(dev)), fTarget(target), fLun(lun)
{
}


SCSIDeviceNode::~SCSIDeviceNode() = default;


bool SCSIDeviceNode::Realize()
{
    SCSIBus *bus = dynamic_cast<SCSIBus *>(ParentBus());
    if (bus == nullptr) {
        vm_error("%s: must be attached to a SCSI bus\n", Name());
        return false;
    }

    SCSIBusTarget *initiator = bus->Target();
    int target = fTarget;
    int lun = fLun;
    if ((target < 0 || lun < 0) &&
        !initiator->FindFreeAddress(&target, &lun)) {
        vm_error("%s: no free SCSI address\n", Name());
        return false;
    }
    return initiator->AttachDevice(fDev.get(), target, lun);
}
