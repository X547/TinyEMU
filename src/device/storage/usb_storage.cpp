/*
 * USB mass storage class device, bulk-only transport
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
#include <stdlib.h>
#include <string.h>

#include "cutils.h"
#include "machine.h"
#include "scsi.h"
#include "usb.h"

/* Command and status wrappers, as they go over the bulk endpoints. */
#define CBW_SIGNATURE 0x43425355 /* "USBC" */
#define CSW_SIGNATURE 0x53425355 /* "USBS" */
#define CBW_SIZE 31
#define CSW_SIZE 13

#define CSW_STATUS_PASS  0
#define CSW_STATUS_FAIL  1
#define CSW_STATUS_PHASE 2

/* Class specific requests on the interface. */
#define MSC_REQUEST_RESET       0xff
#define MSC_REQUEST_GET_MAX_LUN 0xfe

/* The largest data phase one command may carry. A host asking for more than
   this is refused rather than being allowed to size an allocation here. */
#define MSC_MAX_TRANSFER (1 << 20)

#define MSC_EP_IN  1
#define MSC_EP_OUT 2


typedef enum {
    MSC_STATE_CBW,      /* waiting for the next command */
    MSC_STATE_DATA_OUT, /* collecting the data the command writes */
    MSC_STATE_DATA_IN,  /* handing back the data the command read */
    MSC_STATE_STATUS,   /* waiting to hand back the status wrapper */
} MSCStateEnum;


static const uint8_t kDeviceDesc[] = {
    18, USB_DT_DEVICE,
    0x00, 0x02,             /* USB 2.00 */
    0x00,                   /* class is declared per interface */
    0x00, 0x00,
    0x40,                   /* 64 byte default control endpoint */
    0xf4, 0x46,             /* vendor: not one any driver carries a quirk for */
    0x01, 0x00,             /* product */
    0x00, 0x01,             /* device release 1.00 */
    0x01, 0x02, 0x03,       /* manufacturer, product, serial number strings */
    0x01,                   /* one configuration */
};

static const uint8_t kConfigDesc[] = {
    9, USB_DT_CONFIG,
    32, 0x00,               /* total length of the whole tree */
    0x01,                   /* one interface */
    0x01,                   /* configuration value */
    0x00,
    0xc0,                   /* self powered */
    50,                     /* 100 mA */

    9, USB_DT_INTERFACE,
    0x00, 0x00,
    0x02,                   /* two endpoints */
    USB_CLASS_MASS_STORAGE,
    0x06,                   /* SCSI transparent command set */
    0x50,                   /* bulk-only transport */
    0x00,

    7, USB_DT_ENDPOINT,
    0x80 | MSC_EP_IN,
    0x02,                   /* bulk */
    0x00, 0x02,             /* 512 bytes, as high speed requires */
    0x00,

    7, USB_DT_ENDPOINT,
    MSC_EP_OUT,
    0x02,
    0x00, 0x02,
    0x00,
};


//#pragma mark - USBStorage

class USBStorage final: public USBDevice, public SCSIBusTarget,
                        public SCSICompletion {
private:
    SCSIDevice *fUnits[SCSI_MAX_LUN] {};
    int fMaxLun = -1;

    MSCStateEnum fState = MSC_STATE_CBW;
    uint32_t fTag = 0;
    uint32_t fDataLen = 0;   /* what the command wrapper promised */
    bool fDataIn = false;
    uint32_t fLun = 0;

    uint8_t *fBuf = nullptr;
    uint32_t fBufSize = 0;
    uint32_t fDataPos = 0;   /* how much of fBuf has crossed the bulk pipe */
    uint32_t fDataDone = 0;  /* how much the unit actually moved */
    uint8_t fStatus = CSW_STATUS_PASS;

    SCSIRequest fRequest {};
    bool fCommandRunning = false;
    /* A data or status transfer that arrived while the unit still had the
       command. It is answered from the completion instead of being retried,
       which is what removes any need for a timer here. */
    URB *fParkedUrb = nullptr;

    bool EnsureBuffer(uint32_t size);
    void StartCommand(const uint8_t *cdb, int cdb_len);
    void FinishCommand();
    bool ReportLuns();

    USBStatusEnum HandleControl(URB *urb);
    USBStatusEnum HandleBulkOut(URB *urb);
    USBStatusEnum HandleBulkIn(URB *urb);
    USBStatusEnum ServeDataIn(URB *urb);
    USBStatusEnum ServeStatus(URB *urb);

public:
    USBStorage();
    ~USBStorage() override;

    void Reset() override;
    USBStatusEnum Submit(URB *urb) override;
    void Cancel(URB *urb) override;

    /* SCSIBusTarget */
    int FindFreeLun() override;
    bool AttachDevice(SCSIDevice *dev, uint32_t lun) override;

    /* SCSICompletion */
    void Complete(SCSIRequest *req) override;

    int MaxLun() const {return fMaxLun < 0 ? 0 : fMaxLun;}
    bool HasUnit() const {return fMaxLun >= 0;}
};


USBStorage::USBStorage(): USBDevice("usb-storage", USB_SPEED_HIGH)
{
    SetDescriptors(kDeviceDesc, kConfigDesc);
    SetStringDescriptor(1, "TinyEMU");
    SetStringDescriptor(2, "USB Mass Storage");
    SetStringDescriptor(3, "TEMU00000001");
}


USBStorage::~USBStorage()
{
    free(fBuf);
}


void USBStorage::Reset()
{
    USBDevice::Reset();
    fState = MSC_STATE_CBW;
    fCommandRunning = false;
    fParkedUrb = nullptr;
    fDataPos = 0;
    fDataDone = 0;
    for (int i = 0; i <= fMaxLun; i++) {
        if (fUnits[i] != nullptr) {
            fUnits[i]->Reset();
        }
    }
}


int USBStorage::FindFreeLun()
{
    for (int i = 0; i < SCSI_MAX_LUN; i++) {
        if (fUnits[i] == nullptr) {
            return i;
        }
    }
    return -1;
}


bool USBStorage::AttachDevice(SCSIDevice *dev, uint32_t lun)
{
    if (lun >= SCSI_MAX_LUN) {
        vm_error("usb-storage: logical unit %u is out of range\n", lun);
        return false;
    }
    if (fUnits[lun] != nullptr) {
        vm_error("usb-storage: logical unit %u already has a device\n", lun);
        return false;
    }
    fUnits[lun] = dev;
    dev->SetLun(lun);
    if ((int)lun > fMaxLun) {
        fMaxLun = lun;
    }
    return true;
}


bool USBStorage::EnsureBuffer(uint32_t size)
{
    if (size <= fBufSize) {
        return true;
    }
    uint8_t *buf = static_cast<uint8_t *>(realloc(fBuf, size));
    if (buf == nullptr) {
        return false;
    }
    fBuf = buf;
    fBufSize = size;
    return true;
}


/* REPORT LUNS asks the target, not one of its units, which units exist, so it
   is answered here where the map lives rather than by any one unit. */
bool USBStorage::ReportLuns()
{
    uint32_t count = 0;

    memset(fBuf, 0, fDataLen < 8 ? fDataLen : 8);
    for (int i = 0; i <= fMaxLun; i++) {
        if (fUnits[i] == nullptr) {
            continue;
        }
        uint32_t offset = 8 + count * 8;
        if (offset + 8 <= fDataLen) {
            memset(fBuf + offset, 0, 8);
            /* Single level addressing: the unit number goes in byte one. */
            fBuf[offset + 1] = i;
        }
        count++;
    }
    if (fDataLen >= 4) {
        put_be32(fBuf, count * 8);
    }
    fDataDone = 8 + count * 8;
    if (fDataDone > fDataLen) {
        fDataDone = fDataLen;
    }
    fStatus = CSW_STATUS_PASS;
    return true;
}


void USBStorage::StartCommand(const uint8_t *cdb, int cdb_len)
{
    SCSIDevice *unit = fLun < SCSI_MAX_LUN ? fUnits[fLun] : nullptr;

    fDataDone = 0;
    fStatus = CSW_STATUS_PASS;

    if (cdb[0] == SCSI_REPORT_LUNS && fDataIn) {
        ReportLuns();
        return;
    }

    if (unit == nullptr) {
        /* No such unit. There is nothing to take the command, so the wrapper
           reports the failure and a REQUEST SENSE would find nothing. */
        fStatus = CSW_STATUS_FAIL;
        return;
    }

    fRequest = SCSIRequest();
    memcpy(fRequest.cdb, cdb, cdb_len < SCSI_MAX_CDB ? cdb_len : SCSI_MAX_CDB);
    fRequest.cdb_len = cdb_len;
    fRequest.lun = fLun;
    fRequest.dir = fDataLen == 0 ? SCSI_DIR_NONE
                                 : (fDataIn ? SCSI_DIR_FROM_DEV
                                            : SCSI_DIR_TO_DEV);
    fRequest.buf = fBuf;
    fRequest.buf_len = fDataLen;
    fRequest.completion = this;

    fCommandRunning = true;
    if (unit->Submit(&fRequest)) {
        fCommandRunning = false;
        FinishCommand();
    }
}


void USBStorage::FinishCommand()
{
    fDataDone = fRequest.actual_length;
    fStatus = fRequest.status == SCSI_STATUS_GOOD ? CSW_STATUS_PASS
                                                  : CSW_STATUS_FAIL;
}


void USBStorage::Complete(SCSIRequest *req)
{
    (void)req;
    fCommandRunning = false;
    FinishCommand();

    URB *urb = fParkedUrb;
    if (urb == nullptr) {
        return;
    }
    fParkedUrb = nullptr;

    USBStatusEnum status;
    if (fState == MSC_STATE_DATA_IN) {
        status = ServeDataIn(urb);
    } else {
        status = ServeStatus(urb);
    }
    usb_urb_complete(urb, status, urb->actual_length);
}


USBStatusEnum USBStorage::HandleControl(URB *urb)
{
    const USBSetup &setup = urb->setup;

    if ((setup.request_type & USB_TYPE_MASK) == USB_TYPE_CLASS) {
        switch (setup.request) {
        case MSC_REQUEST_GET_MAX_LUN: {
            uint8_t val = MaxLun();
            urb->actual_length =
                urb->buffer != nullptr ? urb->buffer->Write(0, &val, 1) : 0;
            return USB_STATUS_OK;
        }

        case MSC_REQUEST_RESET:
            /* Abandon whatever was in flight and go back to waiting for a
               command wrapper. */
            fState = MSC_STATE_CBW;
            fParkedUrb = nullptr;
            fDataPos = 0;
            fDataDone = 0;
            urb->actual_length = 0;
            return USB_STATUS_OK;
        }
        return USB_STATUS_STALL;
    }

    return HandleStandardControl(urb);
}


USBStatusEnum USBStorage::HandleBulkOut(URB *urb)
{
    if (fState == MSC_STATE_CBW) {
        uint8_t cbw[CBW_SIZE];

        if (urb->buffer == nullptr ||
            urb->buffer->Read(0, cbw, CBW_SIZE) != CBW_SIZE ||
            get_le32(cbw) != CBW_SIGNATURE) {
            /* A wrapper that is not a wrapper: the host has lost sync, and
               stalling is how it is told to reset the interface. */
            return USB_STATUS_STALL;
        }

        fTag = get_le32(cbw + 4);
        fDataLen = get_le32(cbw + 8);
        fDataIn = (cbw[12] & 0x80) != 0;
        fLun = cbw[13] & 0x0f;
        int cdb_len = cbw[14] & 0x1f;

        if (fDataLen > MSC_MAX_TRANSFER || !EnsureBuffer(fDataLen)) {
            return USB_STATUS_STALL;
        }

        urb->actual_length = CBW_SIZE;
        fDataPos = 0;

        if (fDataLen == 0 || fDataIn) {
            /* The command can run now: it either moves nothing, or it
               produces the data the host is about to ask for. */
            fState = fDataLen == 0 ? MSC_STATE_STATUS : MSC_STATE_DATA_IN;
            StartCommand(cbw + 15, cdb_len);
        } else {
            /* The command needs the data first, so it waits for it. */
            fState = MSC_STATE_DATA_OUT;
            memcpy(fRequest.cdb, cbw + 15,
                   cdb_len < SCSI_MAX_CDB ? cdb_len : SCSI_MAX_CDB);
            fRequest.cdb_len = cdb_len;
        }
        return USB_STATUS_OK;
    }

    if (fState == MSC_STATE_DATA_OUT) {
        uint32_t len = urb->buffer != nullptr ? urb->buffer->Length() : 0;
        if (len > fDataLen - fDataPos) {
            len = fDataLen - fDataPos;
        }
        if (len > 0) {
            len = urb->buffer->Read(0, fBuf + fDataPos, len);
        }
        fDataPos += len;
        urb->actual_length = len;

        if (fDataPos >= fDataLen) {
            uint8_t cdb[SCSI_MAX_CDB];
            int cdb_len = fRequest.cdb_len;
            memcpy(cdb, fRequest.cdb, SCSI_MAX_CDB);
            fState = MSC_STATE_STATUS;
            StartCommand(cdb, cdb_len);
        }
        return USB_STATUS_OK;
    }

    return USB_STATUS_STALL;
}


USBStatusEnum USBStorage::ServeDataIn(URB *urb)
{
    uint32_t want = urb->buffer != nullptr ? urb->buffer->Length() : 0;
    uint32_t avail = fDataDone > fDataPos ? fDataDone - fDataPos : 0;
    uint32_t len = want < avail ? want : avail;

    if (len > 0) {
        len = urb->buffer->Write(0, fBuf + fDataPos, len);
    }
    fDataPos += len;
    urb->actual_length = len;

    /* Everything the unit produced has now crossed the pipe. Anything the
       host still expected is reported as residue in the status wrapper. */
    if (fDataPos >= fDataDone) {
        fState = MSC_STATE_STATUS;
    }
    return USB_STATUS_OK;
}


USBStatusEnum USBStorage::ServeStatus(URB *urb)
{
    uint8_t csw[CSW_SIZE];
    uint32_t residue = fDataLen > fDataDone ? fDataLen - fDataDone : 0;

    put_le32(csw, CSW_SIGNATURE);
    put_le32(csw + 4, fTag);
    put_le32(csw + 8, residue);
    csw[12] = fStatus;

    urb->actual_length =
        urb->buffer != nullptr ? urb->buffer->Write(0, csw, CSW_SIZE) : 0;
    fState = MSC_STATE_CBW;
    return USB_STATUS_OK;
}


USBStatusEnum USBStorage::HandleBulkIn(URB *urb)
{
    if (fCommandRunning) {
        /* The unit still has the command. Hold the transfer and answer it
           from the completion. */
        if (fParkedUrb != nullptr) {
            return USB_STATUS_STALL;
        }
        fParkedUrb = urb;
        return USB_STATUS_ASYNC;
    }

    if (fState == MSC_STATE_DATA_IN) {
        return ServeDataIn(urb);
    }
    if (fState == MSC_STATE_STATUS) {
        return ServeStatus(urb);
    }
    return USB_STATUS_STALL;
}


USBStatusEnum USBStorage::Submit(URB *urb)
{
    if (urb->type == USB_ENDPOINT_CONTROL) {
        return HandleControl(urb);
    }
    if (urb->endpoint == MSC_EP_IN && urb->is_in) {
        return HandleBulkIn(urb);
    }
    if (urb->endpoint == MSC_EP_OUT && !urb->is_in) {
        return HandleBulkOut(urb);
    }
    return USB_STATUS_STALL;
}


void USBStorage::Cancel(URB *urb)
{
    if (fParkedUrb == urb) {
        fParkedUrb = nullptr;
    }
}


//#pragma mark - factory

Device *usb_storage_node_create(int port)
{
    USBStorage *storage = new USBStorage();
    USBDeviceNode *node = new USBDeviceNode("usb-storage", storage, port);

    node->SetChildBus(new SCSIBus(node, storage));
    return node;
}
