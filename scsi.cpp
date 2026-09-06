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

#include "machine.h"


void scsi_set_sense(SCSIRequest *req, uint8_t sense_key, uint16_t asc_ascq)
{
    memset(req->sense, 0, sizeof(req->sense));
    req->sense[0] = 0x70;              /* current error, fixed format */
    req->sense[2] = sense_key & 0x0f;
    req->sense[7] = SCSI_SENSE_LEN - 8; /* additional sense length */
    req->sense[12] = asc_ascq >> 8;
    req->sense[13] = asc_ascq & 0xff;
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

SCSIDeviceNode::SCSIDeviceNode(const char *name, SCSIDevice *dev, int lun):
    Device(name), fDev(dev), fLun(lun)
{
}


SCSIDeviceNode::~SCSIDeviceNode()
{
    delete fDev;
}


bool SCSIDeviceNode::Realize()
{
    SCSIBus *bus = dynamic_cast<SCSIBus *>(ParentBus());
    if (bus == nullptr) {
        vm_error("%s: must be attached to a SCSI bus\n", Name());
        return false;
    }

    SCSIBusTarget *target = bus->Target();
    int lun = fLun;
    if (lun < 0) {
        lun = target->FindFreeLun();
        if (lun < 0) {
            vm_error("%s: no free logical unit number\n", Name());
            return false;
        }
    }
    return target->AttachDevice(fDev, lun);
}
