/*
 * USB HID class device: carries HID functions over USB
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
#include <string.h>

#include <vector>

#include "bits.h"
#include "cutils.h"
#include "hid.h"
#include "machine.h"
#include "usb.h"
#include "usb_desc.h"

/* HID specific descriptor types. */
#define USB_DT_HID      0x21
#define USB_DT_REPORT   0x22
#define USB_DT_PHYSICAL 0x23

/* Class specific requests on the interface. */
#define HID_REQUEST_GET_REPORT   0x01
#define HID_REQUEST_GET_IDLE     0x02
#define HID_REQUEST_GET_PROTOCOL 0x03
#define HID_REQUEST_SET_REPORT   0x09
#define HID_REQUEST_SET_IDLE     0x0a
#define HID_REQUEST_SET_PROTOCOL 0x0b

/* bInterfaceSubClass and bInterfaceProtocol for a device that also speaks the
   boot protocol, which is what a BIOS talks to before a driver is loaded. */
#define HID_SUBCLASS_BOOT     0x01
#define HID_PROTOCOL_KEYBOARD 0x01
#define HID_PROTOCOL_MOUSE    0x02

/* The largest control transfer this answers is a report descriptor. */
#define HID_CONTROL_BUF_SIZE 256

/* HID 1.11, as the version field of a HID descriptor spells it. */
#define HID_VERSION 0x0111


static std::vector<uint8_t> hid_device_desc()
{
    USBDescBuilder b;

    b.Device({
        .device_class = 0, /* it is declared per interface */
        .vendor_id = 0x46f4, /* not one any driver carries a quirk for */
        .product_id = 0x0003,
        .manufacturer_str = 1,
        .product_str = 2,
    });
    return b.Take();
}


//#pragma mark - USBHID

/* One USB interface per HID function, each with an interrupt IN endpoint
   carrying that function's input reports. */
class USBHID final: public USBDevice, public HIDBusTarget, public HIDTransport {
private:
    struct Function {
        HIDDevice *dev = nullptr;
        /* The host's poll waits here while the function has nothing to
           give. */
        URB *parked_urb = nullptr;
        uint8_t idle = 0; /* the idle rate the host set, in 4 ms units */
        /* Where this function's HID descriptor sits in the configuration,
           which is the copy a request for it is answered from. */
        int hid_desc_offset = 0;
    };

    Function fFunctions[HID_MAX_FUNCTIONS] {};
    int fFunctionCount = 0;

    std::vector<uint8_t> fDeviceDesc = hid_device_desc();
    std::vector<uint8_t> fConfigDesc;

    void BuildConfigDescriptor();
    Function *FunctionForInterface(int index);
    Function *FunctionForEndpoint(int endpoint);

    USBStatusEnum HIDDescriptor(URB *urb, Function *fn);
    USBStatusEnum ReportDescriptor(URB *urb, Function *fn);
    USBStatusEnum GetReport(URB *urb, Function *fn);
    USBStatusEnum SetReport(URB *urb, Function *fn);
    USBStatusEnum HandleControl(URB *urb);
    USBStatusEnum HandleInterruptIn(URB *urb, Function *fn);

public:
    USBHID();

    void Reset() override;
    USBStatusEnum Submit(URB *urb) override;
    void Cancel(URB *urb) override;

    bool HasFunction() const {return fFunctionCount > 0;}

    /* HIDBusTarget */
    int FindFreeIndex() override;
    bool AttachDevice(HIDDevice *dev, int index) override;

    /* HIDTransport */
    void HandleInputReport(HIDDevice *dev) override;
};


USBHID::USBHID(): USBDevice("usb-hid", USB_SPEED_FULL)
{
    SetStringDescriptor(1, "TinyEMU");
    SetStringDescriptor(2, "USB HID");
    BuildConfigDescriptor();
}


/* The configuration is whatever was attached, so it is assembled rather than
   being a table. */
void USBHID::BuildConfigDescriptor()
{
    USBDescBuilder b;

    b.BeginConfig({
        .attributes = USB_CONFIG_ATTR_ONE | USB_CONFIG_ATTR_REMOTE_WAKEUP,
        .max_power_ma = 100,
    });

    for (int i = 0; i < fFunctionCount; i++) {
        HIDDevice *dev = fFunctions[i].dev;
        int desc_len;
        dev->ReportDescriptor(&desc_len);

        uint8_t subclass = 0;
        uint8_t protocol = 0;
        switch (dev->BootProtocol()) {
        case HID_BOOT_KEYBOARD:
            subclass = HID_SUBCLASS_BOOT;
            protocol = HID_PROTOCOL_KEYBOARD;
            break;
        case HID_BOOT_MOUSE:
            subclass = HID_SUBCLASS_BOOT;
            protocol = HID_PROTOCOL_MOUSE;
            break;
        case HID_BOOT_NONE:
            break;
        }

        b.BeginInterface({
            .number = (uint8_t)i,
            .iface_class = USB_CLASS_HID,
            .iface_subclass = subclass,
            .iface_protocol = protocol,
        });

        b.BeginDescriptor(USB_DT_HID);
        b.Word(HID_VERSION);
        b.Byte(dev->CountryCode());
        b.Byte(1); /* one subordinate descriptor */
        b.Byte(USB_DT_REPORT);
        b.Word(desc_len);
        b.EndDescriptor();
        fFunctions[i].hid_desc_offset = b.DescriptorOffset();

        /* the input report endpoint. At full speed the interval is in
           frames, so in milliseconds. */
        b.Endpoint(USB_DIR_IN | (i + 1), USB_ENDPOINT_ATTR_INTERRUPT,
                   dev->InputReportSize(), dev->PollInterval());
    }

    b.EndConfig();
    fConfigDesc = b.Take();
    /* The bytes moved, so the device has to be pointed at them again. */
    SetDescriptors(fDeviceDesc.data(), fConfigDesc.data());
}


void USBHID::Reset()
{
    USBDevice::Reset();
    for (int i = 0; i < fFunctionCount; i++) {
        fFunctions[i].parked_urb = nullptr;
        fFunctions[i].idle = 0;
        fFunctions[i].dev->Reset();
    }
}


int USBHID::FindFreeIndex()
{
    if (fFunctionCount >= HID_MAX_FUNCTIONS) {
        return -1;
    }
    return fFunctionCount;
}


bool USBHID::AttachDevice(HIDDevice *dev, int index)
{
    if (index < 0 || index >= HID_MAX_FUNCTIONS) {
        vm_error("usb-hid: function index %d is out of range\n", index);
        return false;
    }
    /* The interfaces are numbered without a gap. */
    if (index != fFunctionCount) {
        vm_error("usb-hid: function index %d is out of order; the next free "
                 "one is %d\n", index, fFunctionCount);
        return false;
    }

    fFunctions[index].dev = dev;
    fFunctionCount = index + 1;
    dev->SetTransport(this, index);
    BuildConfigDescriptor();
    return true;
}


USBHID::Function *USBHID::FunctionForInterface(int index)
{
    if (index < 0 || index >= fFunctionCount) {
        return nullptr;
    }
    return &fFunctions[index];
}


USBHID::Function *USBHID::FunctionForEndpoint(int endpoint)
{
    /* Interface i owns endpoint i + 1, since endpoint 0 is the control
       endpoint the whole device shares. */
    return FunctionForInterface(endpoint - 1);
}


/* Detaching the transfer before completing it matters: the completion runs
   the endpoint's ring on, which comes straight back here. */
void USBHID::HandleInputReport(HIDDevice *dev)
{
    Function *fn = FunctionForInterface(dev->Index());
    if (fn == nullptr || fn->parked_urb == nullptr) {
        return;
    }

    URB *urb = fn->parked_urb;
    fn->parked_urb = nullptr;

    uint8_t buf[HID_MAX_REPORT_SIZE];
    int len = dev->TakeInputReport(buf, sizeof(buf));
    if (len <= 0) {
        fn->parked_urb = urb;
        return;
    }
    uint32_t actual =
        urb->buffer != nullptr ? urb->buffer->Write(0, buf, len) : 0;
    usb_urb_complete(urb, USB_STATUS_OK, actual);
}


USBStatusEnum USBHID::HIDDescriptor(URB *urb, Function *fn)
{
    /* The HID descriptor is part of the configuration, so it is answered from
       the copy already there. */
    const uint8_t *desc = fConfigDesc.data() + fn->hid_desc_offset;
    uint32_t len = desc[0];

    if (len > urb->setup.length) {
        len = urb->setup.length;
    }
    urb->actual_length =
        urb->buffer != nullptr ? urb->buffer->Write(0, desc, len) : 0;
    return USB_STATUS_OK;
}


USBStatusEnum USBHID::ReportDescriptor(URB *urb, Function *fn)
{
    int desc_len;
    const uint8_t *desc = fn->dev->ReportDescriptor(&desc_len);
    uint32_t len = desc_len;

    if (len > urb->setup.length) {
        len = urb->setup.length;
    }
    urb->actual_length =
        urb->buffer != nullptr ? urb->buffer->Write(0, desc, len) : 0;
    return USB_STATUS_OK;
}


USBStatusEnum USBHID::GetReport(URB *urb, Function *fn)
{
    uint8_t buf[HID_MAX_REPORT_SIZE];
    uint8_t type = urb->setup.value >> 8;

    int len = fn->dev->GetReport(type, buf, sizeof(buf));
    if (len < 0) {
        return USB_STATUS_STALL;
    }
    if ((uint32_t)len > urb->setup.length) {
        len = urb->setup.length;
    }
    urb->actual_length =
        urb->buffer != nullptr ? urb->buffer->Write(0, buf, len) : 0;
    return USB_STATUS_OK;
}


USBStatusEnum USBHID::SetReport(URB *urb, Function *fn)
{
    uint8_t buf[HID_MAX_REPORT_SIZE];
    uint8_t type = urb->setup.value >> 8;
    uint32_t len = urb->setup.length;

    if (len > sizeof(buf)) {
        len = sizeof(buf);
    }
    if (len > 0 && urb->buffer != nullptr) {
        len = urb->buffer->Read(0, buf, len);
    } else {
        len = 0;
    }

    if (!fn->dev->SetReport(type, buf, len)) {
        return USB_STATUS_STALL;
    }
    urb->actual_length = len;
    return USB_STATUS_OK;
}


USBStatusEnum USBHID::HandleControl(URB *urb)
{
    const USBSetup &setup = urb->setup;

    if ((setup.request_type & USB_TYPE_MASK) == USB_TYPE_STANDARD) {
        /* The HID and report descriptors are fetched with a standard
           GET_DESCRIPTOR naming the interface. */
        if (setup.request == USB_REQ_GET_DESCRIPTOR) {
            int type = setup.value >> 8;
            if (type == USB_DT_HID || type == USB_DT_REPORT ||
                type == USB_DT_PHYSICAL) {
                Function *fn =
                    FunctionForInterface(get_bits(setup.index, 0, 8));
                if (fn == nullptr) {
                    return USB_STATUS_STALL;
                }
                if (type == USB_DT_HID) {
                    return HIDDescriptor(urb, fn);
                }
                if (type == USB_DT_REPORT) {
                    return ReportDescriptor(urb, fn);
                }
                /* Physical descriptors describe which finger presses which
                   key, and a device is free not to have any. */
                return USB_STATUS_STALL;
            }
        }
        return HandleStandardControl(urb);
    }

    if ((setup.request_type & USB_TYPE_MASK) != USB_TYPE_CLASS) {
        return USB_STATUS_STALL;
    }

    Function *fn = FunctionForInterface(get_bits(setup.index, 0, 8));
    if (fn == nullptr) {
        return USB_STATUS_STALL;
    }

    switch (setup.request) {
    case HID_REQUEST_GET_REPORT:
        return GetReport(urb, fn);

    case HID_REQUEST_SET_REPORT:
        return SetReport(urb, fn);

    case HID_REQUEST_GET_IDLE: {
        uint8_t val = fn->idle;
        urb->actual_length =
            urb->buffer != nullptr ? urb->buffer->Write(0, &val, 1) : 0;
        return USB_STATUS_OK;
    }

    case HID_REQUEST_SET_IDLE:
        /* Recorded so that GET_IDLE agrees. Reports go out when the state
           changes, which is what an idle rate of zero asks for. */
        fn->idle = setup.value >> 8;
        urb->actual_length = 0;
        return USB_STATUS_OK;

    case HID_REQUEST_GET_PROTOCOL: {
        uint8_t val = fn->dev->Protocol() ? 0 : 1;
        urb->actual_length =
            urb->buffer != nullptr ? urb->buffer->Write(0, &val, 1) : 0;
        return USB_STATUS_OK;
    }

    case HID_REQUEST_SET_PROTOCOL:
        if (fn->dev->BootProtocol() == HID_BOOT_NONE && setup.value == 0) {
            /* No boot report format, so a host must not ask for it. */
            return USB_STATUS_STALL;
        }
        fn->dev->SetProtocol(setup.value == 0);
        urb->actual_length = 0;
        return USB_STATUS_OK;
    }

    return USB_STATUS_STALL;
}


USBStatusEnum USBHID::HandleInterruptIn(URB *urb, Function *fn)
{
    uint8_t buf[HID_MAX_REPORT_SIZE];

    if (!fn->dev->HasInputReport()) {
        /* Hold the transfer rather than refusing it: a refusal here would
           never be retried. */
        if (fn->parked_urb != nullptr) {
            return USB_STATUS_STALL;
        }
        fn->parked_urb = urb;
        return USB_STATUS_ASYNC;
    }

    int len = fn->dev->TakeInputReport(buf, sizeof(buf));
    urb->actual_length =
        urb->buffer != nullptr ? urb->buffer->Write(0, buf, len) : 0;
    return USB_STATUS_OK;
}


USBStatusEnum USBHID::Submit(URB *urb)
{
    if (urb->type == USB_ENDPOINT_CONTROL) {
        return HandleControl(urb);
    }
    if (urb->type == USB_ENDPOINT_INTERRUPT && urb->is_in) {
        Function *fn = FunctionForEndpoint(urb->endpoint);
        if (fn != nullptr) {
            return HandleInterruptIn(urb, fn);
        }
    }
    return USB_STATUS_STALL;
}


void USBHID::Cancel(URB *urb)
{
    for (int i = 0; i < fFunctionCount; i++) {
        if (fFunctions[i].parked_urb == urb) {
            fFunctions[i].parked_urb = nullptr;
        }
    }
}


//#pragma mark - factory

Device *usb_hid_node_create(int port)
{
    USBHID *hid = new USBHID();
    USBDeviceNode *node = new USBDeviceNode("usb-hid", hid, port);

    node->SetChildBus(new HIDBus(node, hid));
    return node;
}
