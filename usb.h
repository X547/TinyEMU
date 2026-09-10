/*
 * USB device model and transfer requests
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

#include "databuf.h"
#include "device.h"

/* A hub may have at most this many downstream ports: the port number is one
   nibble of an xHCI route string, and 0 means "no more tiers". */
#define USB_MAX_PORTS 15

/* Index 0 holds the language table, so 1..7 are available to a device. */
#define USB_MAX_STRINGS 8

/* Descriptor types. */
#define USB_DT_DEVICE               0x01
#define USB_DT_CONFIG               0x02
#define USB_DT_STRING               0x03
#define USB_DT_INTERFACE            0x04
#define USB_DT_ENDPOINT             0x05
#define USB_DT_DEVICE_QUALIFIER     0x06
#define USB_DT_OTHER_SPEED_CONFIG   0x07
#define USB_DT_BOS                  0x0f
#define USB_DT_HUB                  0x29

/* bmRequestType fields. */
#define USB_DIR_IN                  0x80
#define USB_TYPE_MASK               0x60
#define USB_TYPE_STANDARD           0x00
#define USB_TYPE_CLASS              0x20
#define USB_TYPE_VENDOR             0x40
#define USB_RECIP_MASK              0x1f
#define USB_RECIP_DEVICE            0x00
#define USB_RECIP_INTERFACE         0x01
#define USB_RECIP_ENDPOINT          0x02
#define USB_RECIP_OTHER             0x03

/* Standard request codes. */
#define USB_REQ_GET_STATUS          0x00
#define USB_REQ_CLEAR_FEATURE       0x01
#define USB_REQ_SET_FEATURE         0x03
#define USB_REQ_SET_ADDRESS         0x05
#define USB_REQ_GET_DESCRIPTOR      0x06
#define USB_REQ_SET_DESCRIPTOR      0x07
#define USB_REQ_GET_CONFIGURATION   0x08
#define USB_REQ_SET_CONFIGURATION   0x09
#define USB_REQ_GET_INTERFACE       0x0a
#define USB_REQ_SET_INTERFACE       0x0b

/* Features addressable with SET_FEATURE / CLEAR_FEATURE. */
#define USB_FEATURE_ENDPOINT_HALT   0x00
#define USB_FEATURE_REMOTE_WAKEUP   0x01

/* Interface class codes this tree uses. */
#define USB_CLASS_HID               0x03
#define USB_CLASS_HUB               0x09
#define USB_CLASS_MASS_STORAGE      0x08


typedef enum {
    USB_SPEED_LOW,
    USB_SPEED_FULL,
    USB_SPEED_HIGH,
    USB_SPEED_SUPER,
} USBSpeedEnum;


/* How a transfer ended. USB_STATUS_ASYNC is not an outcome but a promise: the
   device took the request and will report the real outcome later through the
   URB's completion. Everything else means the URB is finished and the
   submitter owns it again. */
typedef enum {
    USB_STATUS_OK,
    USB_STATUS_STALL,   /* the endpoint refused the request */
    USB_STATUS_BABBLE,  /* the device sent more than was asked for */
    USB_STATUS_NODEV,   /* nothing is plugged in there */
    USB_STATUS_IOERROR, /* the buffer could not be reached */
    USB_STATUS_ASYNC,
} USBStatusEnum;


typedef enum {
    USB_ENDPOINT_CONTROL,
    USB_ENDPOINT_ISOCH,
    USB_ENDPOINT_BULK,
    USB_ENDPOINT_INTERRUPT,
} USBEndpointTypeEnum;


/* The eight byte SETUP packet, already decoded. */
struct USBSetup {
    uint8_t request_type = 0;
    uint8_t request = 0;
    uint16_t value = 0;
    uint16_t index = 0;
    uint16_t length = 0;
};


struct URB;
class USBDevice;

/* How a device hands back an URB it answered asynchronously. */
class URBCompletion {
public:
    virtual ~URBCompletion() = default;

    virtual void Complete(URB *urb) = 0;
};


/* One transfer request block: everything one endpoint needs to move one
   transfer, and where the answer goes.

   The submitter owns the URB and the buffer; a device that answers
   asynchronously keeps the pointer only until it calls the completion. The
   payload is a DataBuffer rather than a pointer so that a controller can
   describe the guest's scattered descriptor chain without copying it first,
   and so that a device never has to know how the transfer reached it. */
struct URB {
    USBDevice *dev = nullptr;
    uint8_t endpoint = 0; /* endpoint number, 0..15; direction is 'is_in' */
    bool is_in = false;
    USBEndpointTypeEnum type = USB_ENDPOINT_CONTROL;

    USBSetup setup {};       /* control transfers only */
    DataBuffer *buffer = nullptr; /* null for a zero length transfer */

    uint32_t actual_length = 0;   /* filled in by the device */
    USBStatusEnum status = USB_STATUS_OK;

    /* Called only when Submit() returned USB_STATUS_ASYNC. */
    URBCompletion *completion = nullptr;
};


struct USBPort;

/* A device on a USB bus. The standard device requests are answered from the
   descriptors a subclass installs, so a class device only has to implement
   what is specific to it and defer the rest. */
class USBDevice {
private:
    const char *fName;
    USBSpeedEnum fSpeed;
    USBPort *fPort = nullptr;

    uint8_t fAddress = 0;
    uint8_t fConfigValue = 0;

    const uint8_t *fDeviceDesc = nullptr;
    const uint8_t *fConfigDesc = nullptr;
    const char *fStrings[USB_MAX_STRINGS] {};

protected:
    /* Both blobs are the bytes that go on the wire, and both must outlive the
       device; a static table in the class's source is the intended shape. The
       configuration blob is the whole tree: configuration, interfaces and
       endpoints, with wTotalLength already correct. */
    void SetDescriptors(const uint8_t *device_desc, const uint8_t *config_desc);
    void SetStringDescriptor(int index, const char *str);

    /* Answer one standard device request from those descriptors. A request
       this cannot serve gets USB_STATUS_STALL, which is what a real device
       does with a request it does not implement. */
    USBStatusEnum HandleStandardControl(URB *urb);

public:
    USBDevice(const char *name, USBSpeedEnum speed);
    virtual ~USBDevice() = default;

    const char *Name() const {return fName;}
    USBSpeedEnum Speed() const {return fSpeed;}
    uint8_t Address() const {return fAddress;}
    uint8_t ConfigValue() const {return fConfigValue;}

    USBPort *Port() const {return fPort;}
    void SetPort(USBPort *port) {fPort = port;}

    /* Drops the address and the configuration, as the bus reset this models
       does. An override must call up. */
    virtual void Reset();

    /* USB_STATUS_ASYNC means the answer arrives through urb->completion. */
    virtual USBStatusEnum Submit(URB *urb) = 0;

    /* Give back an URB the submitter no longer wants: the endpoint was
       stopped, or the controller is being reset. The device must not call the
       completion afterwards. */
    virtual void Cancel(URB *urb) {(void)urb;}

    /* Non-null only for a hub, so that a controller can walk a route string
       down to the device it names. 'port' is 1-based. */
    virtual USBDevice *DownstreamDevice(int port) {(void)port; return nullptr;}
};


/* One downstream port, on a controller's root hub or on a hub device. */
struct USBPort {
    USBDevice *dev = nullptr;
    int index = 0; /* 1-based, as the guest numbers it */
};


/* Implemented by whatever provides downstream ports. Attaching is a build time
   operation: every device in the configuration is plugged in before the guest
   runs, and nothing is ever unplugged, which is what lets a hub report its
   port changes without a timer to poll on. */
class USBPortTarget {
public:
    virtual ~USBPortTarget() = default;

    /* The lowest numbered free port that will take a device of this speed, or
       0 when there is none. */
    virtual int FindFreePort(USBSpeedEnum speed) = 0;
    virtual bool AttachDevice(USBDevice *dev, int port) = 0;
};


/* The bus a controller or a hub provides. What it hands out is a port number,
   not host address space, so it assigns no resource records and reports a
   child that declares one as the modelling mistake it is. */
class USBBus final: public Bus {
private:
    USBPortTarget *fTarget;

public:
    USBBus(Device *owner, USBPortTarget *target):
        Bus(owner), fTarget(target) {}

    const char *Type() const override {return "usb";}
    USBPortTarget *Target() const {return fTarget;}

    bool AssignResources(Device *dev) override;
};


/* Attaches one USB device to the bus it was declared on. Which device this is
   comes from the factory, so there is one node class rather than one per
   device type; the node knows only how to plug the device in and, when it
   provides a bus of its own, to hand that on. */
class USBDeviceNode final: public Device {
private:
    USBDevice *fDev;
    Bus *fChildBus = nullptr;
    int fPort; /* 0 asks for the first free port */

public:
    USBDeviceNode(const char *name, USBDevice *dev, int port);
    ~USBDeviceNode() override;

    USBDevice *Dev() const {return fDev;}

    /* Takes ownership. Called by the factory, before the node is added to a
       bus, for a device that provides one. */
    void SetChildBus(Bus *bus) {fChildBus = bus;}

    bool Realize() override;
    Bus *ChildBus() override {return fChildBus;}
};


/* Fill in an URB's outcome and hand it back. A device that answered
   asynchronously calls this from its own callback; one that answered on the
   spot never needs it. */
void usb_urb_complete(URB *urb, USBStatusEnum status, uint32_t actual_length);


/* Each USB device type builds its own node, so the factory in devices.cpp
   stays a table of names and these keep their internals to themselves. 'port'
   is the port asked for in the configuration, or 0 for the first free one. */

/* usb_hub.cpp */
Device *usb_hub_node_create(int port_count, int port);

/* usb_storage.cpp */
Device *usb_storage_node_create(int port);
