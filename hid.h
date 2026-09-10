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

#include "device.h"

struct DeviceContext;

/* How many functions one transport may carry. A USB HID device gives each of
   them an interface of its own. */
#define HID_MAX_FUNCTIONS 4

/* The longest report here is the eight byte boot keyboard one. */
#define HID_MAX_REPORT_SIZE 16

/* Reports a function may have outstanding before the oldest are dropped. It
   only fills up while the guest is not polling. */
#define HID_REPORT_QUEUE_SIZE 32

/* Report types, as GET_REPORT and SET_REPORT number them. */
#define HID_REPORT_INPUT   1
#define HID_REPORT_OUTPUT  2
#define HID_REPORT_FEATURE 3


/* Which boot protocol report format a function can also produce. A device
   whose format has no boot equivalent -- an absolute pointer, for one -- says
   none, and the transport must not advertise a boot interface for it. */
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
};


/* One HID function: a report descriptor and the reports that go with it. How
   those reach the guest is the transport's business, so the same object works
   over USB and would work over I2C or SPI. */
class HIDDevice {
private:
    const char *fName;
    HIDTransport *fTransport = nullptr;
    int fIndex = -1; /* which function of the transport this is */
    bool fBootProtocol = false;

    /* Reports the transport has not collected yet. */
    struct QueuedReport {
        uint8_t data[HID_MAX_REPORT_SIZE];
        int len;
    };
    QueuedReport fQueue[HID_REPORT_QUEUE_SIZE] {};
    int fQueueHead = 0;
    int fQueueCount = 0;

protected:
    /* Hand one input report to the transport. A full queue drops its oldest
       report, which for a pointer leaves the current position. */
    void QueueInputReport(const uint8_t *data, int len);

    bool BootProtocolActive() const {return fBootProtocol;}

public:
    HIDDevice(const char *name): fName(name) {}
    virtual ~HIDDevice() = default;

    const char *Name() const {return fName;}
    int Index() const {return fIndex;}

    /* Called by the transport as it attaches the function. */
    void SetTransport(HIDTransport *transport, int index)
        {fTransport = transport; fIndex = index;}

    /* Must outlive the device; a static table in the function's source. */
    virtual const uint8_t *ReportDescriptor(int *len) const = 0;

    /* How long the report is, so a transport can size the pipe for it. */
    virtual int InputReportSize() const = 0;

    virtual HIDBootProtocolEnum BootProtocol() const {return HID_BOOT_NONE;}

    /* Key localisation, as the HID descriptor reports it; 0 for none. */
    virtual uint8_t CountryCode() const {return 0;}

    /* How often the host should poll, in milliseconds. */
    virtual int PollInterval() const {return 10;}

    /* Answer GET_REPORT: bytes produced, or -1 for a report this function
       does not have. The default hands back the current input report. */
    virtual int GetReport(uint8_t type, uint8_t *buf, int size);

    /* Take a report the host sent; false for one this function refuses. */
    virtual bool SetReport(uint8_t type, const uint8_t *buf, int len)
        {(void)type; (void)buf; (void)len; return false;}

    /* Drops any state the guest set, as a bus reset does. An override must
       call up. */
    virtual void Reset();

    bool Protocol() const {return fBootProtocol;}
    void SetProtocol(bool boot_protocol);

    /* Whether a report is waiting, and taking the oldest one. */
    bool HasInputReport() const {return fQueueCount > 0;}
    int TakeInputReport(uint8_t *buf, int size);

protected:
    /* The current state, in the format the active protocol asks for. */
    virtual int BuildInputReport(uint8_t *buf, int size) const = 0;
};


/* Implemented by the transport, which owns the functions attached to it. */
class HIDBusTarget {
public:
    virtual ~HIDBusTarget() = default;

    /* The lowest unused function index, or -1 when the transport is full. */
    virtual int FindFreeIndex() = 0;
    virtual bool AttachDevice(HIDDevice *dev, int index) = 0;
};


/* The bus a HID transport provides. Like the USB and SCSI buses it hands out
   an index rather than host address space, so it assigns no resources. */
class HIDBus final: public Bus {
private:
    HIDBusTarget *fTarget;

public:
    HIDBus(Device *owner, HIDBusTarget *target):
        Bus(owner), fTarget(target) {}

    const char *Type() const override {return "hid";}
    HIDBusTarget *Target() const {return fTarget;}

    bool AssignResources(Device *dev) override;
};


/* Attaches one HID function to the bus it was declared on, the way
   USBDeviceNode attaches a USB device. Subclassed by a function that claims a
   machine role once it is attached. */
class HIDDeviceNode: public Device {
private:
    HIDDevice *fDev;
    int fIndex; /* < 0 asks for the first free one */

public:
    HIDDeviceNode(const char *name, HIDDevice *dev, int index);
    ~HIDDeviceNode() override;

    HIDDevice *Dev() const {return fDev;}

    bool Realize() override;
};


/* Each HID function builds its own node, so the factory in devices.cpp stays
   a table of names. 'index' is -1 for the first free one. */

/* hid_keyboard.cpp */
Device *hid_keyboard_node_create(DeviceContext *ctx, int index);

/* hid_tablet.cpp */
Device *hid_tablet_node_create(DeviceContext *ctx, int index);

/* usb_hid.cpp */
Device *usb_hid_node_create(int port);
