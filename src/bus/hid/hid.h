/*
 * HID device model and report transport
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

#include <vector>

#include "device.h"

class HIDReportBuilder;

/* Top-level collections one HID device may hold. */
#define HID_MAX_COLLECTIONS 8

/* The longest report here is the eight byte boot keyboard one, and its
   report ID. */
#define HID_MAX_REPORT_SIZE 16

/* Reports a device may have outstanding before the oldest are dropped. It
   only fills up while the host is not collecting them. */
#define HID_REPORT_QUEUE_SIZE 32

/* Report types, as GET_REPORT and SET_REPORT number them. */
#define HID_REPORT_INPUT   1
#define HID_REPORT_OUTPUT  2
#define HID_REPORT_FEATURE 3


/* Which boot protocol report format a collection can also produce. One whose
   format has no boot equivalent -- an absolute pointer, for one -- says
   none. */
typedef enum {
    HID_BOOT_NONE,
    HID_BOOT_KEYBOARD,
    HID_BOOT_MOUSE,
} HIDBootProtocolEnum;


class HIDDevice;

/* Implemented by the transport, so that one holding a poll can answer it the
   moment a report appears rather than needing a timer. */
class HIDTransport {
public:
    virtual ~HIDTransport() = default;

    virtual void HandleInputReport(HIDDevice *dev) = 0;

    /* A collection was added, so the report descriptor changed; false when
       the transport cannot carry it. */
    virtual bool CollectionsChanged(HIDDevice *dev)
        {(void)dev; return true;}
};


/* One top-level collection: a keyboard, a pointer. Its reports carry no
   report ID; the device it is in adds one where it has to. */
class HIDCollection {
private:
    const char *fName;
    HIDDevice *fDevice = nullptr;

    friend class HIDDevice;

protected:
    /* Hand one input report to the device. */
    void SendInputReport(const uint8_t *data, int len);

public:
    HIDCollection(const char *name): fName(name) {}
    virtual ~HIDCollection() = default;

    const char *Name() const {return fName;}

    /* The collection's items. It sets every global item its own items rely
       on, since another collection may come before it. */
    virtual void BuildReportDescriptor(HIDReportBuilder &r) const = 0;

    /* How long the reports are. */
    virtual int InputReportSize() const = 0;
    virtual int OutputReportSize() const {return 0;}

    virtual HIDBootProtocolEnum BootProtocol() const {return HID_BOOT_NONE;}

    /* Key localisation, as the HID descriptor reports it; 0 for none. */
    virtual uint8_t CountryCode() const {return 0;}

    /* How often a host that polls should, in milliseconds. */
    virtual int PollInterval() const {return 10;}

    /* Answer GET_REPORT: bytes produced, or -1 for a report this collection
       does not have. The default hands back the current input report. */
    virtual int GetReport(uint8_t type, uint8_t *buf, int size);

    /* Take a report the host sent; false for one this collection refuses. */
    virtual bool SetReport(uint8_t type, const uint8_t *buf, int len)
        {(void)type; (void)buf; (void)len; return false;}

    /* Drops any state the host set. */
    virtual void Reset() {}

protected:
    /* The current state, as an input report. */
    virtual int BuildInputReport(uint8_t *buf, int size) const = 0;
};


/* A HID device: one report descriptor over the collections in it, and the
   reports that go with them, as a transport carries them. With more than one
   collection every report starts with its collection's report ID, numbered
   from 1 in the order they were added. The boot protocol has no IDs and only
   the boot format, so while it is active the collection that has that format
   is the only one heard.

   How the reports reach the host is the transport's business, so the same
   device works over USB and I2C. */
class HIDDevice {
private:
    struct QueuedReport {
        uint8_t data[HID_MAX_REPORT_SIZE];
        int len;
        int collection;
    };

    const char *fName;
    HIDTransport *fTransport;
    HIDCollection *fCollections[HID_MAX_COLLECTIONS] {};
    int fCount = 0;
    std::vector<uint8_t> fReportDesc;

    bool fBootProtocol = false;
    /* the idle period the host set for each report, in milliseconds, and
       when the report is due again without a change (HID 1.11 7.2.4) */
    uint16_t fIdle[HID_MAX_COLLECTIONS] {};
    uint64_t fIdleDue[HID_MAX_COLLECTIONS] {};

    QueuedReport fQueue[HID_REPORT_QUEUE_SIZE] {};
    int fQueueHead = 0;
    int fQueueCount = 0;

    int IndexOf(const HIDCollection *c) const;
    int BootCollection() const;
    int CollectionForId(int id) const;
    int BuildReport(int index, uint8_t type, uint8_t *buf, int size);
    void Enqueue(int index, const uint8_t *data, int len);
    void ReportSent(int index);
    int DueCollection() const;

    friend class HIDCollection;
    void QueueInputReport(HIDCollection *c, const uint8_t *data, int len);

public:
    HIDDevice(const char *name, HIDTransport *transport):
        fName(name), fTransport(transport) {}

    const char *Name() const {return fName;}

    bool AddCollection(HIDCollection *c);
    int CollectionCount() const {return fCount;}
    bool UsesReportIds() const {return fCount > 1;}

    const uint8_t *ReportDescriptor(int *len) const
        {*len = fReportDesc.size(); return fReportDesc.data();}

    /* The longest reports, with their report IDs. */
    int MaxInputReportSize() const;
    int MaxOutputReportSize() const;

    /* What the device as a whole tells the host. */
    HIDBootProtocolEnum BootProtocol() const;
    uint8_t CountryCode() const;
    int PollInterval() const;

    bool Protocol() const {return fBootProtocol;}
    void SetProtocol(bool boot_protocol);

    /* Reports as they go on the wire: the report ID first where there is
       one. 'id' is the one the request named, or for SetReport() -1 when the
       transport has none to give and the report itself says. GetReport()
       returns -1, and SetReport() false, for a report the device does not
       have. */
    int GetReport(uint8_t type, int id, uint8_t *buf, int size);
    bool SetReport(uint8_t type, int id, const uint8_t *buf, int len);

    /* The idle period of one report, or of all of them for ID 0, in
       milliseconds; 0 for none. Idle() is -1 and SetIdle() false for a
       report ID the device does not have. */
    int Idle(int id) const;
    bool SetIdle(int id, uint16_t ms);

    /* When the next report is due again without a change, or UINT64_MAX;
       and the oldest one that is due by now, 0 if none is. A transport that
       interrupts the host rather than being polled queues that report
       instead, which is false when none is due. */
    uint64_t NextIdleDue() const;
    int TakeIdleReport(uint8_t *buf, int size);
    bool QueueIdleReport();

    /* Whether a report is waiting, and taking the oldest one. */
    bool HasInputReport() const {return fQueueCount > 0;}
    int TakeInputReport(uint8_t *buf, int size);

    /* Drops any state the host set, as a bus reset does. */
    void Reset();
};


/* The bus a HID transport provides: the collections of its device. Like the
   USB and SCSI buses it assigns no resources. */
class HIDBus final: public Bus {
private:
    HIDDevice *fDev;

public:
    HIDBus(Device *owner, HIDDevice *dev): Bus(owner), fDev(dev) {}

    const char *Type() const override {return "hid";}
    HIDDevice *Dev() const {return fDev;}

    bool AssignResources(Device *dev) override;
};


/* Adds one collection to the device of the bus it was declared on, the way
   USBDeviceNode attaches a USB device. Subclassed by a collection that
   claims a machine role once it is added. */
class HIDCollectionNode: public Device {
private:
    HIDCollection *fCollection;

public:
    HIDCollectionNode(const char *name, HIDCollection *c);
    ~HIDCollectionNode() override;

    bool Realize() override;
};
