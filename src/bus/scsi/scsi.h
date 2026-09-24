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
#pragma once

#include <stdint.h>

#include "device.h"

class HostBlockDevice;

#define SCSI_MAX_CDB 16
/* Fixed format sense data: the eight byte header plus ten additional bytes,
   which is what "additional sense length = 10" in byte 7 promises. */
#define SCSI_SENSE_LEN 18

/* A logical unit number is one byte on the wire here, so this is as many units
   as one target can carry. */
#define SCSI_MAX_LUN 16

/* Status byte values. */
#define SCSI_STATUS_GOOD            0x00
#define SCSI_STATUS_CHECK_CONDITION 0x02
#define SCSI_STATUS_BUSY            0x08

/* Sense keys. */
#define SCSI_SENSE_NO_SENSE         0x00
#define SCSI_SENSE_NOT_READY        0x02
#define SCSI_SENSE_MEDIUM_ERROR     0x03
#define SCSI_SENSE_HARDWARE_ERROR   0x04
#define SCSI_SENSE_ILLEGAL_REQUEST  0x05
#define SCSI_SENSE_UNIT_ATTENTION   0x06
#define SCSI_SENSE_DATA_PROTECT     0x07

/* Additional sense code / qualifier pairs, packed as (ASC << 8) | ASCQ. */
#define SCSI_ASC_NO_ADDITIONAL_SENSE        0x0000
#define SCSI_ASC_LBA_OUT_OF_RANGE           0x2100
#define SCSI_ASC_INVALID_COMMAND_OPERATION  0x2000
#define SCSI_ASC_INVALID_FIELD_IN_CDB       0x2400
#define SCSI_ASC_LUN_NOT_SUPPORTED          0x2500
#define SCSI_ASC_UNRECOVERED_READ_ERROR     0x1100
#define SCSI_ASC_WRITE_ERROR                0x0c00
#define SCSI_ASC_MEDIUM_NOT_PRESENT         0x3a00
#define SCSI_ASC_POWER_ON_RESET             0x2900

/* Command operation codes. */
#define SCSI_TEST_UNIT_READY        0x00
#define SCSI_REQUEST_SENSE          0x03
#define SCSI_FORMAT_UNIT            0x04
#define SCSI_READ_6                 0x08
#define SCSI_WRITE_6                0x0a
#define SCSI_INQUIRY                0x12
#define SCSI_MODE_SELECT_6          0x15
#define SCSI_MODE_SENSE_6           0x1a
#define SCSI_START_STOP_UNIT        0x1b
#define SCSI_PREVENT_ALLOW_REMOVAL  0x1e
#define SCSI_READ_CAPACITY_10       0x25
#define SCSI_READ_10                0x28
#define SCSI_WRITE_10               0x2a
#define SCSI_SEEK_10                0x2b
#define SCSI_VERIFY_10              0x2f
#define SCSI_SYNCHRONIZE_CACHE_10   0x35
#define SCSI_MODE_SELECT_10         0x55
#define SCSI_MODE_SENSE_10          0x5a
#define SCSI_READ_16                0x88
#define SCSI_WRITE_16               0x8a
#define SCSI_VERIFY_16              0x8f
#define SCSI_SYNCHRONIZE_CACHE_16   0x91
#define SCSI_SERVICE_ACTION_IN_16   0x9e
#define  SCSI_SAI_READ_CAPACITY_16  0x10
#define SCSI_REPORT_LUNS            0xa0
#define SCSI_READ_12                0xa8
#define SCSI_WRITE_12               0xaa
#define SCSI_VERIFY_12              0xaf


typedef enum {
    SCSI_DIR_NONE,
    SCSI_DIR_TO_DEV,   /* the initiator sends data */
    SCSI_DIR_FROM_DEV, /* the target sends data */
} SCSIDirEnum;


struct SCSIRequest;

class SCSICompletion {
public:
    virtual ~SCSICompletion() = default;

    virtual void Complete(SCSIRequest *req) = 0;
};


/* One command. The initiator fills in the CDB, the unit, the direction and the
   buffer, then submits; the target fills in the status, how much data it
   actually moved and, when the command failed, the sense data that says why.

   The payload is a flat host buffer rather than a scatter list on purpose. A
   transport splits its data phase however it likes -- a bulk-only transport
   may answer one SCSI command with any number of USB transfers -- and staging
   the whole command's data here is what keeps that choice from reaching the
   target at all. */
struct SCSIRequest {
    uint8_t cdb[SCSI_MAX_CDB] {};
    int cdb_len = 0;
    uint32_t lun = 0;
    SCSIDirEnum dir = SCSI_DIR_NONE;

    uint8_t *buf = nullptr;     /* owned by the initiator */
    uint32_t buf_len = 0;       /* how much room the initiator has */
    uint32_t actual_length = 0; /* how much the target moved */

    uint8_t status = SCSI_STATUS_GOOD;
    uint8_t sense[SCSI_SENSE_LEN] {};
    int sense_len = 0;
    /* The data phase the initiator offered does not fit the command: the
       wrong direction, or less room than the command moves. Nothing was
       transferred, and the transport reports a failure of its own. */
    bool phase_error = false;

    SCSICompletion *completion = nullptr;
};


/* A logical unit. Submit() returning true means the request is finished and
   the initiator owns it again; false means the answer arrives later through
   req->completion, unless the initiator takes it back with Cancel(). */
class SCSIDevice {
private:
    const char *fName;
    uint32_t fLun = 0;

public:
    SCSIDevice(const char *name): fName(name) {}
    virtual ~SCSIDevice() = default;

    const char *Name() const {return fName;}
    uint32_t Lun() const {return fLun;}
    void SetLun(uint32_t lun) {fLun = lun;}

    virtual void Reset() = 0;
    virtual bool Submit(SCSIRequest *req) = 0;
    /* Its completion is not called and its buffer is not touched again.
       Nothing happens if 'req' is not in flight. */
    virtual void Cancel(SCSIRequest *req) = 0;

    virtual uint32_t BlockSize() = 0;
    virtual uint64_t BlockCount() = 0;
};


/* Implemented by the initiator, which owns the units attached to it. A unit is
   addressed by a target and a logical unit number within it; an initiator
   that reaches only one target has only target 0. */
class SCSIBusTarget {
public:
    virtual ~SCSIBusTarget() = default;

    /* Fill in whichever of 'target' and 'lun' is negative with the first
       address that is free, or return false when there is none. */
    virtual bool FindFreeAddress(int *target, int *lun) = 0;
    virtual bool AttachDevice(SCSIDevice *dev, uint32_t target,
                              uint32_t lun) = 0;
};


/* The bus a SCSI initiator provides. Like a USB bus it hands out unit numbers
   rather than host address space, so it assigns no resource records. */
class SCSIBus final: public Bus {
private:
    SCSIBusTarget *fTarget;

public:
    SCSIBus(Device *owner, SCSIBusTarget *target):
        Bus(owner), fTarget(target) {}

    const char *Type() const override {return "scsi";}
    SCSIBusTarget *Target() const {return fTarget;}

    bool AssignResources(Device *dev) override;
};


/* Attaches one logical unit to the bus it was declared on, the way
   USBDeviceNode attaches a USB device. */
class SCSIDeviceNode final: public Device {
private:
    std::unique_ptr<SCSIDevice> fDev;
    /* < 0 asks for the first free one */
    int fTarget;
    int fLun;

public:
    SCSIDeviceNode(const char *name, std::unique_ptr<SCSIDevice> dev,
                   int target, int lun);
    ~SCSIDeviceNode() override;

    SCSIDevice *Dev() const {return fDev.get();}

    bool Realize() override;
};


/* Fill in the fixed format sense data for a failed command, and set the status
   that goes with it. Kept in one place so every target reports a failure the
   same way. */
void scsi_set_sense(SCSIRequest *req, uint8_t sense_key, uint16_t asc_ascq);

/* Mark a command successful, having moved 'length' bytes. */
void scsi_set_good(SCSIRequest *req, uint32_t length);

/* Refuse a command whose data phase does not fit it, without running it. */
void scsi_set_phase_error(SCSIRequest *req);

/* How long a CDB with this operation code is, from the group code in its top
   three bits. Returns 0 for the two vendor specific groups, whose length only
   the transport knows. */
int scsi_cdb_len(uint8_t opcode);

/* Answer REPORT LUNS for a target whose units are 'units', null where there
   is none, into a buffer of 'buf_len' bytes. REPORT LUNS asks the target
   rather than one of its units, so the initiator answers it where the map
   lives. Returns how many bytes were moved. */
uint32_t scsi_report_luns(uint8_t *buf, uint32_t buf_len,
                          SCSIDevice *const *units, int count);

/* scsi_disk.cpp */
Device *scsi_disk_node_create(std::unique_ptr<HostBlockDevice> bs, int target,
                              int lun);
