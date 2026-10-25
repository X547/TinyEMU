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
#include "scsi.h"

#include "heap.h"
#include "test.h"
#include "virtio.h"

/* The virtio SCSI host of the virtio specification (1.2, 5.6): a command
   is the request header with the CDB, the data out, then the response
   header with the sense data, and the data in. */

#define VSCSI_QUEUE_CONTROL 0
#define VSCSI_QUEUE_EVENT   1
#define VSCSI_QUEUE_REQUEST 2
#define VSCSI_QUEUE_SIZE    16

#define VSCSI_CDB_SIZE   32
#define VSCSI_SENSE_SIZE 96
#define VSCSI_REQ_SIZE   (19 + VSCSI_CDB_SIZE)
#define VSCSI_RESP_SIZE  (12 + VSCSI_SENSE_SIZE)

#define VSCSI_S_OK 0

namespace {

class VirtioScsi final: public ScsiInitiator {
private:
    VirtioDev fDev {};
    Virtq fCtrl {};
    Virtq fEvent {};
    Virtq fReq {};
    bool fReady = false;
    uint8_t *fReqHdr = nullptr;
    uint8_t *fRespHdr = nullptr;

public:
    const char *Name() const override {return "virtio-scsi";}
    int TargetCount() const override {return 16;}

    bool SetUp()
    {
        if (fReady)
            return true;
        if (!virtio_find(VIRTIO_ID_SCSI, 0, &fDev))
            return false;
        if (!virtio_negotiate(&fDev, VIRTIO_F_VERSION_1) ||
            !virtq_setup(&fDev, VSCSI_QUEUE_CONTROL, 8, &fCtrl) ||
            !virtq_setup(&fDev, VSCSI_QUEUE_EVENT, 8, &fEvent) ||
            !virtq_setup(&fDev, VSCSI_QUEUE_REQUEST, VSCSI_QUEUE_SIZE, &fReq))
            return false;
        fReqHdr = (uint8_t *)heap_alloc(VSCSI_REQ_SIZE, 16);
        fRespHdr = (uint8_t *)heap_alloc(VSCSI_RESP_SIZE, 16);
        if (fReqHdr == nullptr || fRespHdr == nullptr)
            return false;
        virtio_set_status(&fDev,
                          virtio_status(&fDev) | VIRTIO_STATUS_DRIVER_OK);
        /* what it allocated goes with the test */
        t_on_cleanup(scsi_reset);
        fReady = true;
        return true;
    }

    void Reset()
    {
        if (!fReady)
            return;
        virtio_reset(&fDev);
        if (fDev.pci)
            pci_restore_all();
        fReady = false;
    }

    bool Command(int target, int lun, const uint8_t *cdb, int cdb_len,
                 ScsiDir dir, void *buf, uint32_t len,
                 ScsiResult *res) override
    {
        *res = ScsiResult {};
        memset(fReqHdr, 0, VSCSI_REQ_SIZE);
        fReqHdr[0] = 1;
        fReqHdr[1] = target;
        fReqHdr[2] = 0x40 | (lun >> 8);
        fReqHdr[3] = lun;
        memcpy(fReqHdr + 19, cdb, cdb_len);
        memset(fRespHdr, 0xee, VSCSI_RESP_SIZE);

        VirtqDesc *d = fReq.desc;
        uint16_t n = 0;
        d[n] = VirtqDesc {(uintptr_t)fReqHdr, VSCSI_REQ_SIZE,
                          VIRTQ_DESC_F_NEXT, (uint16_t)(n + 1)};
        n++;
        if (dir == SCSI_OUT && len) {
            d[n] = VirtqDesc {(uintptr_t)buf, len, VIRTQ_DESC_F_NEXT,
                              (uint16_t)(n + 1)};
            n++;
        }
        d[n] = VirtqDesc {(uintptr_t)fRespHdr, VSCSI_RESP_SIZE,
                          VIRTQ_DESC_F_WRITE, 0};
        if (dir == SCSI_IN && len) {
            d[n].flags |= VIRTQ_DESC_F_NEXT;
            d[n].next = n + 1;
            n++;
            d[n] = VirtqDesc {(uintptr_t)buf, len, VIRTQ_DESC_F_WRITE, 0};
        }
        virtq_publish(&fReq, 0);
        virtq_kick(&fReq);
        VirtqUsedElem e;
        if (!virtq_get_used(&fReq, &e, 5000000))
            return false;
        res->adapter = fRespHdr[11];
        res->delivered = res->adapter == VSCSI_S_OK;
        res->status = fRespHdr[10];
        res->residual = le32(fRespHdr + 4);
        res->sense_len = le32(fRespHdr);
        if (res->sense_len > sizeof(res->sense))
            res->sense_len = sizeof(res->sense);
        memcpy(res->sense, fRespHdr + 12, res->sense_len);
        return true;
    }
};

VirtioScsi sVirtioScsi;

ScsiInitiator *initiator(int i)
{
    switch (i) {
    case 0:
        return sVirtioScsi.SetUp() ? &sVirtioScsi : nullptr;
    default:
        return nullptr;
    }
}

const int kInitiators = 1;

}


void scsi_reset()
{
    sVirtioScsi.Reset();
}


bool scsi_find_unit(uint8_t type, int index, ScsiUnit *unit)
{
    uint8_t *inq = (uint8_t *)heap_alloc(36, 16);
    uint8_t *cap = (uint8_t *)heap_alloc(8, 16);
    if (inq == nullptr || cap == nullptr)
        return false;
    for (int i = 0; i < kInitiators; i++) {
        ScsiInitiator *init = initiator(i);
        if (init == nullptr)
            continue;
        for (int t = 0; t < init->TargetCount(); t++) {
            static const uint8_t inquiry[6] = {0x12, 0, 0, 0, 36, 0};
            ScsiResult r;
            memset(inq, 0, 36);
            if (!init->Command(t, 0, inquiry, 6, SCSI_IN, inq, 36, &r) ||
                !r.delivered || r.status != SCSI_GOOD ||
                (inq[0] & 0xe0) != 0 || (inq[0] & 0x1f) != type)
                continue;
            if (index-- > 0)
                continue;
            *unit = ScsiUnit {init, t, 0, type, (inq[1] & 0x80) != 0, 0, 0};
            static const uint8_t capacity[10] = {0x25};
            /* twice, past a unit attention */
            for (int k = 0; k < 2; k++) {
                if (init->Command(t, 0, capacity, 10, SCSI_IN, cap, 8, &r) &&
                    r.delivered && r.status == SCSI_GOOD) {
                    unit->blocks = (uint64_t)be32(cap) + 1;
                    unit->block_size = be32(cap + 4);
                    break;
                }
            }
            return true;
        }
    }
    return false;
}
