/*
 * VIRTIO SCSI host device
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

#include "cutils.h"
#include "machine.h"
#include "scsi.h"
#include "virtio.h"
#include "virtio_priv.h"

#define VIRTIO_SCSI_DEVICE_ID 8

#define VSCSI_QUEUE_CONTROL 0
#define VSCSI_QUEUE_EVENT   1
#define VSCSI_QUEUE_REQUEST 2

/* Targets 0 to 15, as on a wide parallel bus. */
#define VSCSI_MAX_TARGET 16

/* The largest data phase one command may carry. A driver asking for more than
   this is refused rather than being allowed to size an allocation here. */
#define VSCSI_MAX_TRANSFER (1 << 20)

/* Defaults for the sizes the driver may change, and how far it may. */
#define VSCSI_DEFAULT_SENSE_SIZE 96
#define VSCSI_DEFAULT_CDB_SIZE   32
#define VSCSI_MAX_SENSE_SIZE     256
#define VSCSI_MAX_CDB_SIZE       256

/* Configuration space. */
#define VSCSI_CFG_NUM_QUEUES      0
#define VSCSI_CFG_SEG_MAX         4
#define VSCSI_CFG_MAX_SECTORS     8
#define VSCSI_CFG_CMD_PER_LUN     12
#define VSCSI_CFG_EVENT_INFO_SIZE 16
#define VSCSI_CFG_SENSE_SIZE      20
#define VSCSI_CFG_CDB_SIZE        24
#define VSCSI_CFG_MAX_CHANNEL     28
#define VSCSI_CFG_MAX_TARGET      30
#define VSCSI_CFG_MAX_LUN         32
#define VSCSI_CFG_SIZE            36

/* The command request header ahead of the CDB, and the response header ahead
   of the sense data. */
#define VSCSI_REQ_CMD_HDR  19
#define VSCSI_RESP_CMD_HDR 12

#define VSCSI_EVENT_SIZE 16

/* Control queue request types. */
#define VIRTIO_SCSI_T_TMF          0
#define VIRTIO_SCSI_T_AN_QUERY     1
#define VIRTIO_SCSI_T_AN_SUBSCRIBE 2

/* Task management functions. */
#define VIRTIO_SCSI_T_TMF_ABORT_TASK         0
#define VIRTIO_SCSI_T_TMF_ABORT_TASK_SET     1
#define VIRTIO_SCSI_T_TMF_CLEAR_ACA          2
#define VIRTIO_SCSI_T_TMF_CLEAR_TASK_SET     3
#define VIRTIO_SCSI_T_TMF_I_T_NEXUS_RESET    4
#define VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET 5
#define VIRTIO_SCSI_T_TMF_QUERY_TASK         6
#define VIRTIO_SCSI_T_TMF_QUERY_TASK_SET     7

/* Response codes. */
#define VIRTIO_SCSI_S_OK                0
#define VIRTIO_SCSI_S_FUNCTION_COMPLETE 0
#define VIRTIO_SCSI_S_ABORTED           2
#define VIRTIO_SCSI_S_BAD_TARGET        3
#define VIRTIO_SCSI_S_RESET             4
#define VIRTIO_SCSI_S_FAILURE           9
#define VIRTIO_SCSI_S_FUNCTION_SUCCEEDED 10
#define VIRTIO_SCSI_S_FUNCTION_REJECTED 11
#define VIRTIO_SCSI_S_INCORRECT_LUN     12


//#pragma mark - VIRTIOSCSIDevice

struct VIRTIOSCSIDevice final: public VIRTIODevice, public SCSIBusTarget,
                               public SCSICompletion {
private:
    SCSIDevice *fUnits[VSCSI_MAX_TARGET][SCSI_MAX_LUN] {};

    uint32_t fSenseSize = VSCSI_DEFAULT_SENSE_SIZE;
    uint32_t fCdbSize = VSCSI_DEFAULT_CDB_SIZE;

    /* The command the units have. One runs at a time, as on virtio-block: a
       unit takes one request at a time, and the rest wait in the ring. */
    bool fBusy = false;
    int fDescIdx = 0;
    uint64_t fId = 0;
    int fTarget = 0;
    uint32_t fLun = 0;
    SCSIDevice *fUnit = nullptr;
    bool fDataIn = false;
    uint32_t fDataLen = 0;
    std::unique_ptr<uint8_t[]> fBuf;
    SCSIRequest fRequest {};

    SCSIDevice *Unit(int target, uint32_t lun);
    bool HasTarget(int target);

    void WriteConfig();
    void Respond(int desc_idx, uint8_t response, const SCSIRequest *req);
    void Finish(uint8_t response);
    void Abort(uint8_t response);

    int Command(int desc_idx, int read_size, int write_size);
    void Control(int desc_idx, int read_size, int write_size);
    uint8_t TaskManagement(uint32_t subtype, const uint8_t *lun, uint64_t id);

public:
    VIRTIOSCSIDevice() {WriteConfig();}

    int RecvRequest(int queue_idx, int desc_idx, int read_size,
                    int write_size) override;
    void ConfigWrite() override;
    void Reset() override;

    /* SCSIBusTarget */
    bool FindFreeAddress(int *target, int *lun) override;
    bool AttachDevice(SCSIDevice *dev, uint32_t target, uint32_t lun) override;

    /* SCSICompletion */
    void Complete(SCSIRequest *req) override;
};


/* The single level LUN structure: 1, the target, then the unit in flat space
   addressing. */
static bool vscsi_decode_lun(const uint8_t *lun, int *target, uint32_t *unit)
{
    if (lun[0] != 1) {
        return false;
    }
    *target = lun[1];
    *unit = ((lun[2] << 8) | lun[3]) & 0x3fff;
    return true;
}


SCSIDevice *VIRTIOSCSIDevice::Unit(int target, uint32_t lun)
{
    if (target >= VSCSI_MAX_TARGET || lun >= SCSI_MAX_LUN) {
        return nullptr;
    }
    return fUnits[target][lun];
}


bool VIRTIOSCSIDevice::HasTarget(int target)
{
    if (target >= VSCSI_MAX_TARGET) {
        return false;
    }
    for (int i = 0; i < SCSI_MAX_LUN; i++) {
        if (fUnits[target][i] != nullptr) {
            return true;
        }
    }
    return false;
}


void VIRTIOSCSIDevice::WriteConfig()
{
    uint8_t *cfg = config_space;

    put_le32(cfg + VSCSI_CFG_NUM_QUEUES, 1);
    /* the ring less the two headers */
    put_le32(cfg + VSCSI_CFG_SEG_MAX, MAX_QUEUE_NUM - 2);
    put_le32(cfg + VSCSI_CFG_MAX_SECTORS, VSCSI_MAX_TRANSFER / 512);
    put_le32(cfg + VSCSI_CFG_CMD_PER_LUN, MAX_QUEUE_NUM);
    put_le32(cfg + VSCSI_CFG_EVENT_INFO_SIZE, VSCSI_EVENT_SIZE);
    put_le32(cfg + VSCSI_CFG_SENSE_SIZE, fSenseSize);
    put_le32(cfg + VSCSI_CFG_CDB_SIZE, fCdbSize);
    put_le16(cfg + VSCSI_CFG_MAX_CHANNEL, 0);
    put_le16(cfg + VSCSI_CFG_MAX_TARGET, VSCSI_MAX_TARGET - 1);
    put_le32(cfg + VSCSI_CFG_MAX_LUN, SCSI_MAX_LUN - 1);
}


void VIRTIOSCSIDevice::ConfigWrite()
{
    /* Only the two sizes are the driver's to set; anything else it wrote is
       put back. */
    fSenseSize = get_le32(config_space + VSCSI_CFG_SENSE_SIZE);
    if (fSenseSize > VSCSI_MAX_SENSE_SIZE) {
        fSenseSize = VSCSI_MAX_SENSE_SIZE;
    }
    fCdbSize = get_le32(config_space + VSCSI_CFG_CDB_SIZE);
    if (fCdbSize > VSCSI_MAX_CDB_SIZE) {
        fCdbSize = VSCSI_MAX_CDB_SIZE;
    }
    WriteConfig();
}


void VIRTIOSCSIDevice::Reset()
{
    /* the queues the command came from are gone */
    if (fBusy) {
        fUnit->Cancel(&fRequest);
        fBusy = false;
    }
    fBuf.reset();
    for (int t = 0; t < VSCSI_MAX_TARGET; t++) {
        for (int l = 0; l < SCSI_MAX_LUN; l++) {
            if (fUnits[t][l] != nullptr) {
                fUnits[t][l]->Reset();
            }
        }
    }
    fSenseSize = VSCSI_DEFAULT_SENSE_SIZE;
    fCdbSize = VSCSI_DEFAULT_CDB_SIZE;
    WriteConfig();
}


/* Answer a command. 'req' is null when the command never reached a unit, and
   then only the response code means anything. */
void VIRTIOSCSIDevice::Respond(int desc_idx, uint8_t response,
                               const SCSIRequest *req)
{
    uint8_t resp[VSCSI_RESP_CMD_HDR];
    uint32_t sense_len = 0;
    uint32_t moved = 0;
    uint32_t residual = 0;

    memset(resp, 0, sizeof(resp));
    if (req != nullptr) {
        sense_len = req->sense_len < (int)fSenseSize ? req->sense_len
                                                     : fSenseSize;
        moved = req->actual_length < fDataLen ? req->actual_length : fDataLen;
        residual = fDataLen - moved;
        resp[10] = req->status;
        if (sense_len > 0) {
            memcpy_to_queue(this, VSCSI_QUEUE_REQUEST, desc_idx,
                            VSCSI_RESP_CMD_HDR, req->sense, sense_len);
        }
        if (!fDataIn) {
            moved = 0;
        } else if (moved > 0) {
            memcpy_to_queue(this, VSCSI_QUEUE_REQUEST, desc_idx,
                            VSCSI_RESP_CMD_HDR + fSenseSize, fBuf.get(), moved);
        }
    }
    put_le32(resp, sense_len);
    put_le32(resp + 4, residual);
    resp[11] = response;
    memcpy_to_queue(this, VSCSI_QUEUE_REQUEST, desc_idx, 0, resp,
                    sizeof(resp));
    virtio_consume_desc(this, VSCSI_QUEUE_REQUEST, desc_idx,
                        VSCSI_RESP_CMD_HDR + fSenseSize + moved);
}


void VIRTIOSCSIDevice::Finish(uint8_t response)
{
    Respond(fDescIdx, response, &fRequest);
    fBuf.reset();
    fBusy = false;
}


/* Take the running command back from its unit and answer it with 'response'
   instead. The ones behind it wait until the task management function has
   been answered, so none of them can reach a unit that is about to be
   reset. */
void VIRTIOSCSIDevice::Abort(uint8_t response)
{
    fUnit->Cancel(&fRequest);
    Respond(fDescIdx, response, nullptr);
    fBuf.reset();
    fBusy = false;
}


void VIRTIOSCSIDevice::Complete(SCSIRequest *req)
{
    (void)req;
    Finish(VIRTIO_SCSI_S_OK);
    queue_notify(this, VSCSI_QUEUE_REQUEST);
}


/* A command to a unit number nothing is attached to, on a target that exists.
   INQUIRY has to succeed and say so, which is how a driver scanning the target
   learns the unit is absent. */
static void vscsi_no_unit(SCSIRequest *req)
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


int VIRTIOSCSIDevice::Command(int desc_idx, int read_size, int write_size)
{
    uint8_t hdr[VSCSI_REQ_CMD_HDR];
    uint32_t req_hdr = VSCSI_REQ_CMD_HDR + fCdbSize;
    uint32_t resp_hdr = VSCSI_RESP_CMD_HDR + fSenseSize;
    int target;
    uint32_t lun;

    if (fBusy) {
        return -1;
    }
    if ((uint32_t)read_size < req_hdr || (uint32_t)write_size < resp_hdr) {
        /* No room for the headers: nothing can be said about it, but it is
           still handed back so the ring keeps moving. */
        virtio_consume_desc(this, VSCSI_QUEUE_REQUEST, desc_idx, 0);
        return 0;
    }

    uint32_t data_out = read_size - req_hdr;
    uint32_t data_in = write_size - resp_hdr;
    fDescIdx = desc_idx;
    fDataIn = data_in > 0;
    fDataLen = fDataIn ? data_in : data_out;

    fRequest = SCSIRequest();
    memcpy_from_queue(this, hdr, VSCSI_QUEUE_REQUEST, desc_idx, 0,
                      sizeof(hdr));
    memcpy_from_queue(this, fRequest.cdb, VSCSI_QUEUE_REQUEST, desc_idx,
                      VSCSI_REQ_CMD_HDR,
                      fCdbSize < SCSI_MAX_CDB ? fCdbSize : SCSI_MAX_CDB);

    if (!vscsi_decode_lun(hdr, &target, &lun) || !HasTarget(target)) {
        Respond(desc_idx, VIRTIO_SCSI_S_BAD_TARGET, nullptr);
        return 0;
    }
    if ((data_in > 0 && data_out > 0) || fDataLen > VSCSI_MAX_TRANSFER) {
        /* Bidirectional commands were not offered. */
        Respond(desc_idx, VIRTIO_SCSI_S_FAILURE, nullptr);
        return 0;
    }

    if (fDataLen > 0) {
        /* not zeroed, the unit fills what it reports */
        fBuf.reset(new uint8_t[fDataLen]);
        if (!fDataIn) {
            memcpy_from_queue(this, fBuf.get(), VSCSI_QUEUE_REQUEST, desc_idx,
                              req_hdr, fDataLen);
        }
    }

    int cdb_len = scsi_cdb_len(fRequest.cdb[0]);
    fRequest.cdb_len = cdb_len != 0 ? cdb_len : SCSI_MAX_CDB;
    fRequest.lun = lun;
    fRequest.dir = fDataLen == 0 ? SCSI_DIR_NONE
                                 : (fDataIn ? SCSI_DIR_FROM_DEV
                                            : SCSI_DIR_TO_DEV);
    fRequest.buf = fBuf.get();
    fRequest.buf_len = fDataLen;
    fRequest.completion = this;

    fId = get_le64(hdr + 8);
    fTarget = target;
    fLun = lun;
    fUnit = Unit(target, lun);

    if (fRequest.cdb[0] == SCSI_REPORT_LUNS) {
        scsi_set_good(&fRequest,
                      scsi_report_luns(fBuf.get(), fDataIn ? fDataLen : 0,
                                       fUnits[target], SCSI_MAX_LUN));
        Finish(VIRTIO_SCSI_S_OK);
        return 0;
    }
    if (fUnit == nullptr) {
        vscsi_no_unit(&fRequest);
        Finish(VIRTIO_SCSI_S_OK);
        return 0;
    }

    fBusy = true;
    if (fUnit->Submit(&fRequest)) {
        Finish(VIRTIO_SCSI_S_OK);
    }
    return 0;
}


uint8_t VIRTIOSCSIDevice::TaskManagement(uint32_t subtype, const uint8_t *lun,
                                         uint64_t id)
{
    int target;
    uint32_t unit_lun;

    if (!vscsi_decode_lun(lun, &target, &unit_lun) || !HasTarget(target)) {
        return VIRTIO_SCSI_S_BAD_TARGET;
    }

    SCSIDevice *unit = Unit(target, unit_lun);
    bool running = fBusy && fTarget == target && fLun == unit_lun;

    if (subtype == VIRTIO_SCSI_T_TMF_I_T_NEXUS_RESET) {
        for (int i = 0; i < SCSI_MAX_LUN; i++) {
            if (fUnits[target][i] != nullptr) {
                fUnits[target][i]->Reset();
            }
        }
        if (fBusy && fTarget == target) {
            Abort(VIRTIO_SCSI_S_RESET);
        }
        return VIRTIO_SCSI_S_FUNCTION_COMPLETE;
    }
    if (unit == nullptr) {
        return VIRTIO_SCSI_S_INCORRECT_LUN;
    }

    switch (subtype) {
    case VIRTIO_SCSI_T_TMF_ABORT_TASK:
        if (running && fId == id) {
            Abort(VIRTIO_SCSI_S_ABORTED);
        }
        return VIRTIO_SCSI_S_FUNCTION_COMPLETE;

    case VIRTIO_SCSI_T_TMF_ABORT_TASK_SET:
    case VIRTIO_SCSI_T_TMF_CLEAR_TASK_SET:
        if (running) {
            Abort(VIRTIO_SCSI_S_ABORTED);
        }
        return VIRTIO_SCSI_S_FUNCTION_COMPLETE;

    case VIRTIO_SCSI_T_TMF_LOGICAL_UNIT_RESET:
        unit->Reset();
        if (running) {
            Abort(VIRTIO_SCSI_S_RESET);
        }
        return VIRTIO_SCSI_S_FUNCTION_COMPLETE;

    case VIRTIO_SCSI_T_TMF_QUERY_TASK:
        return running && fId == id ? VIRTIO_SCSI_S_FUNCTION_SUCCEEDED
                                    : VIRTIO_SCSI_S_FUNCTION_COMPLETE;

    case VIRTIO_SCSI_T_TMF_QUERY_TASK_SET:
        return running ? VIRTIO_SCSI_S_FUNCTION_SUCCEEDED
                       : VIRTIO_SCSI_S_FUNCTION_COMPLETE;

    case VIRTIO_SCSI_T_TMF_CLEAR_ACA:
        /* ACA is never established here */
        return VIRTIO_SCSI_S_FUNCTION_COMPLETE;
    }
    return VIRTIO_SCSI_S_FUNCTION_REJECTED;
}


void VIRTIOSCSIDevice::Control(int desc_idx, int read_size, int write_size)
{
    uint8_t buf[24];

    if (read_size < 4 ||
        memcpy_from_queue(this, buf, VSCSI_QUEUE_CONTROL, desc_idx, 0, 4) < 0) {
        virtio_consume_desc(this, VSCSI_QUEUE_CONTROL, desc_idx, 0);
        return;
    }

    switch (get_le32(buf)) {
    case VIRTIO_SCSI_T_TMF:
        /* type, subtype, lun, id; answered with one response byte */
        if (read_size >= 24 && write_size >= 1 &&
            memcpy_from_queue(this, buf, VSCSI_QUEUE_CONTROL, desc_idx, 0,
                              24) == 0) {
            bool was_busy = fBusy;
            uint8_t response = TaskManagement(get_le32(buf + 4), buf + 8,
                                              get_le64(buf + 16));
            memcpy_to_queue(this, VSCSI_QUEUE_CONTROL, desc_idx, 0,
                            &response, 1);
            virtio_consume_desc(this, VSCSI_QUEUE_CONTROL, desc_idx, 1);
            if (was_busy && !fBusy) {
                /* the aborted command held up the ones behind it */
                queue_notify(this, VSCSI_QUEUE_REQUEST);
            }
            return;
        }
        break;

    case VIRTIO_SCSI_T_AN_QUERY:
    case VIRTIO_SCSI_T_AN_SUBSCRIBE:
        /* No asynchronous notification is ever sent, so none is offered:
           the events actually granted are none, and the request succeeds. */
        if (write_size >= 5) {
            uint8_t resp[5] = {0, 0, 0, 0, VIRTIO_SCSI_S_OK};
            memcpy_to_queue(this, VSCSI_QUEUE_CONTROL, desc_idx, 0, resp,
                            sizeof(resp));
            virtio_consume_desc(this, VSCSI_QUEUE_CONTROL, desc_idx,
                                sizeof(resp));
            return;
        }
        break;
    }
    virtio_consume_desc(this, VSCSI_QUEUE_CONTROL, desc_idx, 0);
}


int VIRTIOSCSIDevice::RecvRequest(int queue_idx, int desc_idx, int read_size,
                                  int write_size)
{
    switch (queue_idx) {
    case VSCSI_QUEUE_CONTROL:
        Control(desc_idx, read_size, write_size);
        return 0;
    case VSCSI_QUEUE_REQUEST:
        return Command(desc_idx, read_size, write_size);
    }
    /* A queue that does not exist: hand the buffer back unused. */
    virtio_consume_desc(this, queue_idx, desc_idx, 0);
    return 0;
}


bool VIRTIOSCSIDevice::FindFreeAddress(int *target, int *lun)
{
    /* A target or unit out of range is left for AttachDevice() to report. */
    if (*target < 0 && *lun < 0) {
        /* A target of its own, at unit 0, which is where every driver
           looks first. */
        for (int t = 0; t < VSCSI_MAX_TARGET; t++) {
            if (!HasTarget(t)) {
                *target = t;
                *lun = 0;
                return true;
            }
        }
        return false;
    }
    if (*target < 0) {
        if (*lun >= SCSI_MAX_LUN) {
            *target = 0;
            return true;
        }
        for (int t = 0; t < VSCSI_MAX_TARGET; t++) {
            if (fUnits[t][*lun] == nullptr) {
                *target = t;
                return true;
            }
        }
        return false;
    }
    if (*target >= VSCSI_MAX_TARGET) {
        *lun = 0;
        return true;
    }
    for (int l = 0; l < SCSI_MAX_LUN; l++) {
        if (fUnits[*target][l] == nullptr) {
            *lun = l;
            return true;
        }
    }
    return false;
}


bool VIRTIOSCSIDevice::AttachDevice(SCSIDevice *dev, uint32_t target,
                                    uint32_t lun)
{
    if (target >= VSCSI_MAX_TARGET) {
        vm_error("virtio-scsi: target %u is out of range\n", target);
        return false;
    }
    if (lun >= SCSI_MAX_LUN) {
        vm_error("virtio-scsi: logical unit %u is out of range\n", lun);
        return false;
    }
    if (fUnits[target][lun] != nullptr) {
        vm_error("virtio-scsi: target %u logical unit %u already has a "
                 "device\n", target, lun);
        return false;
    }
    fUnits[target][lun] = dev;
    dev->SetLun(lun);
    return true;
}


//#pragma mark - factory

std::unique_ptr<VIRTIODevice> virtio_scsi_init(VIRTIOBusDef *bus)
{
    auto s = std::make_unique<VIRTIOSCSIDevice>();
    virtio_init(s.get(), bus, VIRTIO_SCSI_DEVICE_ID, VSCSI_CFG_SIZE);
    /* Nothing is ever reported on the event queue, since neither hotplug nor
       notification is offered, so its buffers are only held. */
    s->queue[VSCSI_QUEUE_EVENT].manual_recv = true;
    return s;
}


SCSIBusTarget *virtio_scsi_bus_target(VIRTIODevice *s)
{
    return static_cast<VIRTIOSCSIDevice *>(s);
}
