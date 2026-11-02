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

#include <memory>
#include <vector>

#include "bits.h"
#include "cutils.h"
#include "device_class.h"
#include "devices.h"
#include "hid.h"
#include "host_time.h"
#include "host_timer.h"
#include "machine.h"
#include "usb.h"
#include "usb_desc.h"
#include "usb_function.h"

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

/* With one report descriptor. */
#define HID_DESC_SIZE 9


/* An interrupt endpoint's interval for a poll period in milliseconds. At full
   speed it is in frames, so in milliseconds; at high speed it is n for a
   period of 2^(n-1) microframes, the longest that does not exceed it. */
static uint8_t hid_interval(USBSpeedEnum speed, int ms)
{
    if (speed != USB_SPEED_HIGH) {
        return ms;
    }
    int n = 1;
    while (n < 16 && (1 << n) <= ms * 8) {
        n++;
    }
    return n;
}


//#pragma mark - USBHID

/* One USB interface per HID function, each with an interrupt IN endpoint
   carrying that function's input reports. */
class USBHID final: public USBFunction, public HIDBusTarget,
                    public HIDTransport, public TimerTarget {
private:
    struct Function {
        HIDDevice *dev = nullptr;
        /* The host's poll waits here while the function has nothing to
           give. */
        URB *parked_urb = nullptr;
        uint8_t idle = 0; /* the idle rate the host set, in 4 ms units */
        /* With a non-zero idle rate, when the report is due again even
           without a change (HID 1.11 7.2.4). */
        uint64_t idle_due_us = 0;
        /* This function's HID descriptor as the configuration has it, which
           is what a request for it is answered with. */
        uint8_t hid_desc[HID_DESC_SIZE] {};
    };

    Function fFunctions[HID_MAX_FUNCTIONS] {};
    int fFunctionCount = 0;

    std::unique_ptr<HostTimer> fTimer;

    void ArmIdleTimer();
    void CompleteParked(Function *fn, const uint8_t *buf, int len);
    Function *FunctionAt(int index);
    Function *FunctionForInterface(int iface);
    Function *FunctionForEndpoint(int endpoint);

    USBStatusEnum HIDDescriptor(URB *urb, Function *fn);
    USBStatusEnum ReportDescriptor(URB *urb, Function *fn);
    USBStatusEnum GetReport(URB *urb, Function *fn);
    USBStatusEnum SetReport(URB *urb, Function *fn);
    USBStatusEnum HandleInterruptIn(URB *urb, Function *fn);

public:
    USBHID(std::unique_ptr<HostTimer> timer);

    USBFunctionInfo Info() const override;
    bool SupportsSpeed(USBSpeedEnum speed) const override
        {return speed == USB_SPEED_FULL || speed == USB_SPEED_HIGH;}
    int InterfaceCount() const override {return fFunctionCount;}
    int EndpointCount() const override {return fFunctionCount;}
    void BuildDescriptors(USBDescBuilder &b) override;

    void Reset() override;
    void TimerExpired() override;
    USBStatusEnum HandleControl(URB *urb) override;
    USBStatusEnum Submit(URB *urb) override;
    void Cancel(URB *urb) override;

    /* HIDBusTarget */
    int FindFreeIndex() override;
    bool AttachDevice(HIDDevice *dev, int index) override;

    /* HIDTransport */
    void HandleInputReport(HIDDevice *dev) override;
};


USBHID::USBHID(std::unique_ptr<HostTimer> timer):
    USBFunction("usb-hid"), fTimer(std::move(timer))
{
    fTimer->target = this;
}


USBFunctionInfo USBHID::Info() const
{
    return {
        .speed = USB_SPEED_FULL,
        .product_id = 0x0003,
        .product = "USB HID",
        .config_attributes = USB_CONFIG_ATTR_REMOTE_WAKEUP,
    };
}


/* The interfaces are whatever was attached, so they are assembled rather than
   being a table. */
void USBHID::BuildDescriptors(USBDescBuilder &b)
{
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
            .number = (uint8_t)(FirstInterface() + i),
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
        memcpy(fFunctions[i].hid_desc, b.Data() + b.DescriptorOffset(),
               HID_DESC_SIZE);

        b.Endpoint(USB_DIR_IN | (FirstEndpoint() + i),
                   USB_ENDPOINT_ATTR_INTERRUPT, dev->InputReportSize(),
                   hid_interval(Speed(), dev->PollInterval()));
    }
}


void USBHID::Reset()
{
    for (int i = 0; i < fFunctionCount; i++) {
        fFunctions[i].parked_urb = nullptr;
        fFunctions[i].idle = 0;
        fFunctions[i].dev->Reset();
    }
    fTimer->Disarm();
}


/* Wake for the first function whose poll waits on its idle rate. */
void USBHID::ArmIdleTimer()
{
    uint64_t due = UINT64_MAX;
    for (int i = 0; i < fFunctionCount; i++) {
        const Function &fn = fFunctions[i];
        if (fn.parked_urb != nullptr && fn.idle != 0 && fn.idle_due_us < due) {
            due = fn.idle_due_us;
        }
    }
    if (due == UINT64_MAX) {
        fTimer->Disarm();
    } else {
        fTimer->Arm(due);
    }
}


/* Hand a report to the poll that waited for it. Detaching the transfer
   before completing it matters: the completion runs the endpoint's ring on,
   which comes straight back here. */
void USBHID::CompleteParked(Function *fn, const uint8_t *buf, int len)
{
    URB *urb = fn->parked_urb;
    fn->parked_urb = nullptr;
    fn->idle_due_us = host_monotonic_us() + fn->idle * 4000ULL;
    uint32_t actual =
        urb->buffer != nullptr ? urb->buffer->Write(0, buf, len) : 0;
    usb_urb_complete(urb, USB_STATUS_OK, actual);
}


void USBHID::TimerExpired()
{
    uint64_t now = host_monotonic_us();
    for (int i = 0; i < fFunctionCount; i++) {
        Function *fn = &fFunctions[i];
        if (fn->parked_urb == nullptr || fn->idle == 0 ||
            fn->idle_due_us > now) {
            continue;
        }
        /* The period ran out without a change: the current state again. */
        uint8_t buf[HID_MAX_REPORT_SIZE];
        int len = fn->dev->GetReport(HID_REPORT_INPUT, buf, sizeof(buf));
        if (len > 0) {
            CompleteParked(fn, buf, len);
        }
    }
    ArmIdleTimer();
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
    return DescriptorsChanged();
}


USBHID::Function *USBHID::FunctionAt(int index)
{
    if (index < 0 || index >= fFunctionCount) {
        return nullptr;
    }
    return &fFunctions[index];
}


/* Function i has the i-th interface and the i-th endpoint. */
USBHID::Function *USBHID::FunctionForInterface(int iface)
{
    return FunctionAt(iface - FirstInterface());
}


USBHID::Function *USBHID::FunctionForEndpoint(int endpoint)
{
    return FunctionAt(endpoint - FirstEndpoint());
}


void USBHID::HandleInputReport(HIDDevice *dev)
{
    Function *fn = FunctionAt(dev->Index());
    if (fn == nullptr || fn->parked_urb == nullptr) {
        return;
    }

    uint8_t buf[HID_MAX_REPORT_SIZE];
    int len = dev->TakeInputReport(buf, sizeof(buf));
    if (len <= 0) {
        return;
    }
    CompleteParked(fn, buf, len);
    ArmIdleTimer();
}


USBStatusEnum USBHID::HIDDescriptor(URB *urb, Function *fn)
{
    const uint8_t *desc = fn->hid_desc;
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

    Function *fn = FunctionForInterface(get_bits(setup.index, 0, 8));
    if (fn == nullptr) {
        return USB_STATUS_STALL;
    }

    if ((setup.request_type & USB_TYPE_MASK) == USB_TYPE_STANDARD) {
        /* The HID and report descriptors are fetched with a standard
           GET_DESCRIPTOR naming the interface. */
        if (setup.request != USB_REQ_GET_DESCRIPTOR) {
            return USB_STATUS_STALL;
        }
        switch (setup.value >> 8) {
        case USB_DT_HID:
            return HIDDescriptor(urb, fn);
        case USB_DT_REPORT:
            return ReportDescriptor(urb, fn);
        }
        /* Physical descriptors describe which finger presses which key, and
           a device is free not to have any. */
        return USB_STATUS_STALL;
    }

    if ((setup.request_type & USB_TYPE_MASK) != USB_TYPE_CLASS) {
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
        /* Reports go out when the state changes, and with a non-zero rate
           also when that long passed without one (7.2.4). The new period
           counts from now. */
        fn->idle = setup.value >> 8;
        fn->idle_due_us = host_monotonic_us() + fn->idle * 4000ULL;
        ArmIdleTimer();
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

    int len;
    if (fn->dev->HasInputReport()) {
        len = fn->dev->TakeInputReport(buf, sizeof(buf));
    } else if (fn->idle != 0 && host_monotonic_us() >= fn->idle_due_us) {
        len = fn->dev->GetReport(HID_REPORT_INPUT, buf, sizeof(buf));
    } else {
        /* Hold the transfer rather than refusing it: a refusal here would
           never be retried. */
        if (fn->parked_urb != nullptr) {
            return USB_STATUS_STALL;
        }
        fn->parked_urb = urb;
        ArmIdleTimer();
        return USB_STATUS_ASYNC;
    }

    fn->idle_due_us = host_monotonic_us() + fn->idle * 4000ULL;
    urb->actual_length = urb->buffer != nullptr && len > 0
                             ? urb->buffer->Write(0, buf, len) : 0;
    return USB_STATUS_OK;
}


USBStatusEnum USBHID::Submit(URB *urb)
{
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
    ArmIdleTimer();
}


//#pragma mark - class

/* "port" is the port asked for, or 0 for the first free one. */
class USBHIDClass final: public DeviceClass {
public:
    USBHIDClass(): DeviceClass("usb-hid") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        int port;

        if (!cfg.GetInt("port", &port, 0)) {
            return nullptr;
        }
        if (!cfg.HasChildren()) {
            vm_error("usb-hid: needs a nested HID bus with at least one "
                     "function on it\n");
            return nullptr;
        }
        USBHID *hid = new USBHID(ctx->platform->OpenTimer());
        USBFunctionNode *node = new USBFunctionNode("usb-hid", hid, port);

        node->SetChildBus(new HIDBus(node, hid));
        return node;
    }
};

static const USBHIDClass sUSBHIDClass;
