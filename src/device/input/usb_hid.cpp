/*
 * USB HID class function: carries a HID device over USB
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
#include <string.h>

#include <memory>

#include "bits.h"
#include "cutils.h"
#include "device_class.h"
#include "devices.h"
#include "hid.h"
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

/* HID 1.11, as the version field of a HID descriptor spells it. */
#define HID_VERSION 0x0111

/* With one report descriptor. */
#define HID_DESC_SIZE 9

/* GET_IDLE and SET_IDLE count in these. */
#define HID_IDLE_UNIT_MS 4


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

/* One HID device on one USB interface, with an interrupt IN endpoint carrying
   its input reports. */
class USBHID final: public USBFunction, public HIDTransport,
                    public TimerTarget {
private:
    HIDDevice fDev;
    /* The host's poll waits here while the device has nothing to give. */
    URB *fParkedUrb = nullptr;
    /* The HID descriptor as the configuration has it, which is what a
       request for it is answered with. */
    uint8_t fHIDDesc[HID_DESC_SIZE] {};

    std::unique_ptr<HostTimer> fTimer;

    void ArmIdleTimer();
    void CompleteParked(const uint8_t *buf, int len);

    USBStatusEnum Reply(URB *urb, const void *data, uint32_t len);
    USBStatusEnum GetReport(URB *urb);
    USBStatusEnum SetReport(URB *urb);
    USBStatusEnum HandleInterruptIn(URB *urb);

public:
    USBHID(std::unique_ptr<HostTimer> timer);

    HIDDevice *Dev() {return &fDev;}

    USBFunctionInfo Info() const override;
    bool SupportsSpeed(USBSpeedEnum speed) const override
        {return speed == USB_SPEED_FULL || speed == USB_SPEED_HIGH;}
    int InterfaceCount() const override {return 1;}
    int EndpointCount() const override {return 1;}
    void BuildDescriptors(USBDescBuilder &b) override;

    void Reset() override;
    void TimerExpired() override;
    USBStatusEnum HandleControl(URB *urb) override;
    USBStatusEnum Submit(URB *urb) override;
    void Cancel(URB *urb) override;

    /* HIDTransport */
    void HandleInputReport(HIDDevice *dev) override;
    bool CollectionsChanged(HIDDevice *dev) override;
};


USBHID::USBHID(std::unique_ptr<HostTimer> timer):
    USBFunction("usb-hid"), fDev("usb-hid", this), fTimer(std::move(timer))
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


void USBHID::BuildDescriptors(USBDescBuilder &b)
{
    int desc_len;
    fDev.ReportDescriptor(&desc_len);

    uint8_t subclass = 0;
    uint8_t protocol = 0;
    switch (fDev.BootProtocol()) {
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
        .number = FirstInterface(),
        .iface_class = USB_CLASS_HID,
        .iface_subclass = subclass,
        .iface_protocol = protocol,
    });

    b.BeginDescriptor(USB_DT_HID);
    b.Word(HID_VERSION);
    b.Byte(fDev.CountryCode());
    b.Byte(1); /* one subordinate descriptor */
    b.Byte(USB_DT_REPORT);
    b.Word(desc_len);
    b.EndDescriptor();
    memcpy(fHIDDesc, b.Data() + b.DescriptorOffset(), HID_DESC_SIZE);

    b.Endpoint(USB_DIR_IN | FirstEndpoint(), USB_ENDPOINT_ATTR_INTERRUPT,
               fDev.MaxInputReportSize(),
               hid_interval(Speed(), fDev.PollInterval()));
}


bool USBHID::CollectionsChanged(HIDDevice *dev)
{
    (void)dev;
    return DescriptorsChanged();
}


void USBHID::Reset()
{
    fParkedUrb = nullptr;
    fDev.Reset();
    fTimer->Disarm();
}


/* Wake when the waiting poll is due a report on its idle rate. */
void USBHID::ArmIdleTimer()
{
    uint64_t due = fParkedUrb != nullptr ? fDev.NextIdleDue() : UINT64_MAX;
    if (due == UINT64_MAX) {
        fTimer->Disarm();
    } else {
        fTimer->Arm(due);
    }
}


/* Hand a report to the poll that waited for it. Detaching the transfer
   before completing it matters: the completion runs the endpoint's ring on,
   which comes straight back here. */
void USBHID::CompleteParked(const uint8_t *buf, int len)
{
    URB *urb = fParkedUrb;
    fParkedUrb = nullptr;
    uint32_t actual =
        urb->buffer != nullptr ? urb->buffer->Write(0, buf, len) : 0;
    usb_urb_complete(urb, USB_STATUS_OK, actual);
}


void USBHID::TimerExpired()
{
    if (fParkedUrb != nullptr) {
        /* The period ran out without a change: the current state again. */
        uint8_t buf[HID_MAX_REPORT_SIZE];
        int len = fDev.TakeIdleReport(buf, sizeof(buf));
        if (len > 0) {
            CompleteParked(buf, len);
        }
    }
    ArmIdleTimer();
}


void USBHID::HandleInputReport(HIDDevice *dev)
{
    if (fParkedUrb == nullptr) {
        return;
    }
    uint8_t buf[HID_MAX_REPORT_SIZE];
    int len = dev->TakeInputReport(buf, sizeof(buf));
    if (len <= 0) {
        return;
    }
    CompleteParked(buf, len);
    ArmIdleTimer();
}


USBStatusEnum USBHID::Reply(URB *urb, const void *data, uint32_t len)
{
    if (len > urb->setup.length) {
        len = urb->setup.length;
    }
    urb->actual_length =
        urb->buffer != nullptr ? urb->buffer->Write(0, data, len) : 0;
    return USB_STATUS_OK;
}


/* The report type in the high byte of wValue, its ID in the low. */
USBStatusEnum USBHID::GetReport(URB *urb)
{
    uint8_t buf[HID_MAX_REPORT_SIZE];
    const USBSetup &setup = urb->setup;

    int len = fDev.GetReport(setup.value >> 8, get_bits(setup.value, 0, 8),
                             buf, sizeof(buf));
    if (len < 0) {
        return USB_STATUS_STALL;
    }
    return Reply(urb, buf, len);
}


USBStatusEnum USBHID::SetReport(URB *urb)
{
    uint8_t buf[HID_MAX_REPORT_SIZE];
    const USBSetup &setup = urb->setup;
    uint32_t len = setup.length;

    if (len > sizeof(buf)) {
        len = sizeof(buf);
    }
    if (len > 0 && urb->buffer != nullptr) {
        len = urb->buffer->Read(0, buf, len);
    } else {
        len = 0;
    }

    if (!fDev.SetReport(setup.value >> 8, get_bits(setup.value, 0, 8), buf,
                        len)) {
        return USB_STATUS_STALL;
    }
    urb->actual_length = len;
    return USB_STATUS_OK;
}


USBStatusEnum USBHID::HandleControl(URB *urb)
{
    const USBSetup &setup = urb->setup;

    if (get_bits(setup.index, 0, 8) != FirstInterface()) {
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
            return Reply(urb, fHIDDesc, fHIDDesc[0]);
        case USB_DT_REPORT: {
            int len;
            const uint8_t *desc = fDev.ReportDescriptor(&len);
            return Reply(urb, desc, len);
        }
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
        return GetReport(urb);

    case HID_REQUEST_SET_REPORT:
        return SetReport(urb);

    case HID_REQUEST_GET_IDLE: {
        int idle = fDev.Idle(get_bits(setup.value, 0, 8));
        if (idle < 0) {
            return USB_STATUS_STALL;
        }
        uint8_t val = idle / HID_IDLE_UNIT_MS;
        return Reply(urb, &val, 1);
    }

    case HID_REQUEST_SET_IDLE:
        /* Reports go out when the state changes, and with a non-zero rate
           also when that long passed without one (7.2.4). */
        if (!fDev.SetIdle(get_bits(setup.value, 0, 8),
                          (setup.value >> 8) * HID_IDLE_UNIT_MS)) {
            return USB_STATUS_STALL;
        }
        ArmIdleTimer();
        urb->actual_length = 0;
        return USB_STATUS_OK;

    case HID_REQUEST_GET_PROTOCOL: {
        uint8_t val = fDev.Protocol() ? 0 : 1;
        return Reply(urb, &val, 1);
    }

    case HID_REQUEST_SET_PROTOCOL:
        if (fDev.BootProtocol() == HID_BOOT_NONE && setup.value == 0) {
            /* No boot report format, so a host must not ask for it. */
            return USB_STATUS_STALL;
        }
        fDev.SetProtocol(setup.value == 0);
        urb->actual_length = 0;
        return USB_STATUS_OK;
    }

    return USB_STATUS_STALL;
}


USBStatusEnum USBHID::HandleInterruptIn(URB *urb)
{
    uint8_t buf[HID_MAX_REPORT_SIZE];

    int len;
    if (fDev.HasInputReport()) {
        len = fDev.TakeInputReport(buf, sizeof(buf));
    } else {
        len = fDev.TakeIdleReport(buf, sizeof(buf));
    }
    if (len <= 0) {
        /* Hold the transfer rather than refusing it: a refusal here would
           never be retried. */
        if (fParkedUrb != nullptr) {
            return USB_STATUS_STALL;
        }
        fParkedUrb = urb;
        ArmIdleTimer();
        return USB_STATUS_ASYNC;
    }

    urb->actual_length =
        urb->buffer != nullptr ? urb->buffer->Write(0, buf, len) : 0;
    return USB_STATUS_OK;
}


USBStatusEnum USBHID::Submit(URB *urb)
{
    if (urb->type == USB_ENDPOINT_INTERRUPT && urb->is_in &&
        urb->endpoint == FirstEndpoint()) {
        return HandleInterruptIn(urb);
    }
    return USB_STATUS_STALL;
}


void USBHID::Cancel(URB *urb)
{
    if (fParkedUrb == urb) {
        fParkedUrb = nullptr;
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
                     "collection on it\n");
            return nullptr;
        }
        USBHID *hid = new USBHID(ctx->platform->OpenTimer());
        USBFunctionNode *node = new USBFunctionNode("usb-hid", hid, port);

        node->SetChildBus(new HIDBus(node, hid->Dev()));
        return node;
    }
};

static const USBHIDClass sUSBHIDClass;
