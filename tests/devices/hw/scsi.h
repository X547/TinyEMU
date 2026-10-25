/*
 * SCSI initiators
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

/* Commands to SCSI logical units, over whichever host adapter the machine
   has, so that the tests of the units' command sets run on each. */

#define SCSI_GOOD            0x00
#define SCSI_CHECK_CONDITION 0x02

#define SCSI_KEY_NO_SENSE        0x0
#define SCSI_KEY_NOT_READY       0x2
#define SCSI_KEY_MEDIUM_ERROR    0x3
#define SCSI_KEY_ILLEGAL_REQUEST 0x5
#define SCSI_KEY_UNIT_ATTENTION  0x6
#define SCSI_KEY_DATA_PROTECT    0x7

enum ScsiDir {
    SCSI_NONE,
    SCSI_IN,   /* from the unit */
    SCSI_OUT,  /* to the unit */
};

struct ScsiResult {
    /* false when the adapter reported a failure of its own, and nothing
       below means anything */
    bool delivered;
    uint32_t adapter;     /* the adapter's own response code */
    uint8_t status;
    uint8_t sense[32];
    uint32_t sense_len;
    uint32_t residual;

    uint8_t Key() const {return sense_len > 2 ? sense[2] & 0x0f : 0xff;}
    /* ASC << 8 | ASCQ */
    uint16_t Asc() const
        {return sense_len > 13 ? sense[12] << 8 | sense[13] : 0xffff;}
};

class ScsiInitiator {
public:
    virtual ~ScsiInitiator() = default;

    virtual const char *Name() const = 0;
    /* The targets it can address, and the longest CDB it carries. */
    virtual int TargetCount() const = 0;
    virtual int MaxCdb() const = 0;
    virtual bool Command(int target, int lun, const uint8_t *cdb, int cdb_len,
                         ScsiDir dir, void *buf, uint32_t len,
                         ScsiResult *res) = 0;
};

/* A logical unit an initiator reaches, as INQUIRY and READ CAPACITY
   describe it. */
struct ScsiUnit {
    ScsiInitiator *init;
    int target;
    int lun;
    uint8_t type;        /* peripheral device type */
    bool removable;
    uint32_t block_size; /* 0 when READ CAPACITY failed */
    uint64_t blocks;

    bool Command(const uint8_t *cdb, int cdb_len, ScsiDir dir, void *buf,
                 uint32_t len, ScsiResult *res) const
        {return init->Command(target, lun, cdb, cdb_len, dir, buf, len, res);}
};

/* The 'index'th unit of peripheral type 'type' on any initiator, LUN 0 of
   each target in turn; false when there is none. An initiator is set up
   for the test the first time it is needed, and reset after it. */
bool scsi_find_unit(uint8_t type, int index, ScsiUnit *unit);
void scsi_reset();
