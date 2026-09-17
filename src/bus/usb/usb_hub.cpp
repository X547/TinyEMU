/*
 * USB 2.0 hub
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
#include "machine.h"
#include "usb.h"
#include "usb_desc.h"

/* Hub class requests use the standard request codes with a class request
   type; only the descriptor type and the features are its own. */
#define HUB_EP_IN 1

/* Port feature selectors. */
#define PORT_CONNECTION     0
#define PORT_ENABLE         1
#define PORT_SUSPEND        2
#define PORT_OVER_CURRENT   3
#define PORT_RESET          4
#define PORT_POWER          8
#define PORT_LOW_SPEED      9
#define C_PORT_CONNECTION   16
#define C_PORT_ENABLE       17
#define C_PORT_SUSPEND      18
#define C_PORT_OVER_CURRENT 19
#define C_PORT_RESET        20
#define PORT_INDICATOR      22

/* wPortStatus and wPortChange bits. */
#define PORT_STAT_CONNECTION bit_at(0)
#define PORT_STAT_ENABLE     bit_at(1)
#define PORT_STAT_SUSPEND    bit_at(2)
#define PORT_STAT_OVERCURRENT bit_at(3)
#define PORT_STAT_RESET      bit_at(4)
#define PORT_STAT_POWER      bit_at(8)
#define PORT_STAT_LOW_SPEED  bit_at(9)
#define PORT_STAT_HIGH_SPEED bit_at(10)

#define PORT_CHG_CONNECTION  bit_at(0)
#define PORT_CHG_ENABLE      bit_at(1)
#define PORT_CHG_SUSPEND     bit_at(2)
#define PORT_CHG_OVERCURRENT bit_at(3)
#define PORT_CHG_RESET       bit_at(4)


static std::vector<uint8_t> hub_device_desc()
{
    USBDescBuilder b;

    b.Device({
        .device_class = USB_CLASS_HUB,
        .device_protocol = 0x01, /* single transaction translator */
        .vendor_id = 0x46f4,
        .product_id = 0x0002,
        .manufacturer_str = 1,
        .product_str = 2,
    });
    return b.Take();
}


//#pragma mark - USBHub

/* Under xHCI a hub never carries a transfer for the devices below it: the
   controller addresses those directly, by the route string that names the path
   down to them. So all this has to be is a hub the host can enumerate and
   whose ports it can read and reset. */
class USBHub final: public USBDevice, public USBPortTarget {
private:
    struct HubPort {
        USBPort port {};
        uint16_t status = 0;
        uint16_t change = 0;
    };

    int fPortCount;
    HubPort fPorts[USB_MAX_PORTS] {};

    /* The status change endpoint's bitmap is one bit per port plus one for
       the hub itself. */
    int fBitmapSize;
    std::vector<uint8_t> fDeviceDesc = hub_device_desc();
    std::vector<uint8_t> fConfigDesc;

    /* The host's interrupt transfer waits here until something changes.
       Nothing is ever unplugged in this model, so it is only ever completed
       by the connect changes that a hub reset leaves behind. */
    URB *fParkedUrb = nullptr;

    bool AnyChange() const;
    void BuildChangeBitmap(uint8_t *buf) const;
    void NotifyChange();
    USBStatusEnum HubDescriptor(URB *urb);
    USBStatusEnum PortStatus(URB *urb, int port);
    USBStatusEnum SetPortFeature(int port, int feature);
    USBStatusEnum ClearPortFeature(int port, int feature);

public:
    USBHub(int port_count);

    void Reset() override;
    USBStatusEnum Submit(URB *urb) override;
    void Cancel(URB *urb) override;
    USBDevice *DownstreamDevice(int port) override;

    /* USBPortTarget */
    int FindFreePort(USBSpeedEnum speed) override;
    bool AttachDevice(USBDevice *dev, int port) override;
};


USBHub::USBHub(int port_count):
    USBDevice("usb-hub", USB_SPEED_HIGH), fPortCount(port_count)
{
    fBitmapSize = (fPortCount + 1 + 7) / 8;

    for (int i = 0; i < fPortCount; i++) {
        fPorts[i].port.index = i + 1;
        fPorts[i].status = PORT_STAT_POWER;
    }

    USBDescBuilder b;
    b.BeginConfig({
        .attributes = USB_CONFIG_ATTR_ONE | USB_CONFIG_ATTR_SELF_POWERED |
            USB_CONFIG_ATTR_REMOTE_WAKEUP,
        .max_power_ma = 0, /* it draws no bus power */
    });
    b.BeginInterface({.iface_class = USB_CLASS_HUB});
    /* the status change endpoint, polled every 2^11 microframes as a hub
       usually is */
    b.Endpoint(USB_DIR_IN | HUB_EP_IN, USB_ENDPOINT_ATTR_INTERRUPT,
               fBitmapSize, 12);
    b.EndConfig();
    fConfigDesc = b.Take();

    SetDescriptors(fDeviceDesc.data(), fConfigDesc.data());
    SetStringDescriptor(1, "TinyEMU");
    SetStringDescriptor(2, "USB Hub");
}


void USBHub::Reset()
{
    USBDevice::Reset();
    fParkedUrb = nullptr;

    for (int i = 0; i < fPortCount; i++) {
        fPorts[i].status = PORT_STAT_POWER;
        fPorts[i].change = 0;
        if (fPorts[i].port.dev != nullptr) {
            fPorts[i].status |= PORT_STAT_CONNECTION;
            fPorts[i].change |= PORT_CHG_CONNECTION;
        }
    }
}


int USBHub::FindFreePort(USBSpeedEnum speed)
{
    if (speed == USB_SPEED_SUPER) {
        /* A USB 2.0 hub has no SuperSpeed ports; one would need a companion
           hub on a SuperSpeed root port. */
        return 0;
    }
    for (int i = 0; i < fPortCount; i++) {
        if (fPorts[i].port.dev == nullptr) {
            return i + 1;
        }
    }
    return 0;
}


bool USBHub::AttachDevice(USBDevice *dev, int port)
{
    if (port < 1 || port > fPortCount) {
        vm_error("usb-hub: port %d is out of range\n", port);
        return false;
    }
    if (dev->Speed() == USB_SPEED_SUPER) {
        vm_error("usb-hub: port %d does not take a SuperSpeed device\n", port);
        return false;
    }
    HubPort &p = fPorts[port - 1];
    if (p.port.dev != nullptr) {
        vm_error("usb-hub: port %d already has a device\n", port);
        return false;
    }

    p.port.dev = dev;
    dev->SetPort(&p.port);
    p.status |= PORT_STAT_CONNECTION;
    p.change |= PORT_CHG_CONNECTION;
    NotifyChange();
    return true;
}


USBDevice *USBHub::DownstreamDevice(int port)
{
    if (port < 1 || port > fPortCount) {
        return nullptr;
    }
    return fPorts[port - 1].port.dev;
}


bool USBHub::AnyChange() const
{
    for (int i = 0; i < fPortCount; i++) {
        if (fPorts[i].change != 0) {
            return true;
        }
    }
    return false;
}


void USBHub::BuildChangeBitmap(uint8_t *buf) const
{
    memset(buf, 0, fBitmapSize);
    /* Bit zero is the hub's own status, which never changes here. */
    for (int i = 0; i < fPortCount; i++) {
        if (fPorts[i].change != 0) {
            int bit = i + 1;
            buf[bit / 8] |= 1 << (bit % 8);
        }
    }
}


/* A real hub answers the status change endpoint only when something has
   changed, and otherwise makes the host wait. There is no timer here to retry
   a refused transfer on, so the transfer is held instead and answered the
   moment a change appears. */
void USBHub::NotifyChange()
{
    URB *urb = fParkedUrb;

    if (urb == nullptr || !AnyChange()) {
        return;
    }
    fParkedUrb = nullptr;

    uint8_t buf[4];
    BuildChangeBitmap(buf);
    uint32_t len = urb->buffer != nullptr
                       ? urb->buffer->Write(0, buf, fBitmapSize) : 0;
    usb_urb_complete(urb, USB_STATUS_OK, len);
}


USBStatusEnum USBHub::HubDescriptor(URB *urb)
{
    USBDescBuilder b;

    b.BeginDescriptor(USB_DT_HUB);
    b.Byte(fPortCount);
    /* Per port power switching and per port over-current reporting. */
    b.Word(0x0009);
    b.Byte(50); /* 100 ms until power is good, in 2 ms units */
    b.Byte(0);  /* the hub controller draws no bus current */
    /* DeviceRemovable: every port is removable, so the bits stay clear. The
       PortPwrCtrlMask that follows is all ones for historical reasons. */
    for (int i = 0; i < fBitmapSize; i++) {
        b.Byte(0);
    }
    for (int i = 0; i < fBitmapSize; i++) {
        b.Byte(0xff);
    }
    b.EndDescriptor();

    uint32_t len = b.Length();
    if (len > urb->setup.length) {
        len = urb->setup.length;
    }
    urb->actual_length =
        urb->buffer != nullptr ? urb->buffer->Write(0, b.Data(), len) : 0;
    return USB_STATUS_OK;
}


USBStatusEnum USBHub::PortStatus(URB *urb, int port)
{
    uint8_t buf[4];

    if (port < 1 || port > fPortCount) {
        return USB_STATUS_STALL;
    }
    put_le16(buf, fPorts[port - 1].status);
    put_le16(buf + 2, fPorts[port - 1].change);

    uint32_t len = urb->setup.length < 4 ? urb->setup.length : 4;
    urb->actual_length =
        urb->buffer != nullptr ? urb->buffer->Write(0, buf, len) : 0;
    return USB_STATUS_OK;
}


USBStatusEnum USBHub::SetPortFeature(int port, int feature)
{
    if (port < 1 || port > fPortCount) {
        return USB_STATUS_STALL;
    }
    HubPort &p = fPorts[port - 1];

    switch (feature) {
    case PORT_RESET:
        if (p.port.dev != nullptr) {
            p.port.dev->Reset();
            /* The reset completes at once, so the port comes back enabled
               with the reset change pending. The speed is known only after the
               reset; neither bit set means full speed. */
            p.status |= PORT_STAT_ENABLE;
            p.status &= ~(PORT_STAT_LOW_SPEED | PORT_STAT_HIGH_SPEED);
            if (p.port.dev->Speed() == USB_SPEED_LOW) {
                p.status |= PORT_STAT_LOW_SPEED;
            } else if (p.port.dev->Speed() == USB_SPEED_HIGH) {
                p.status |= PORT_STAT_HIGH_SPEED;
            }
            p.status &= ~PORT_STAT_SUSPEND;
            p.change |= PORT_CHG_RESET;
            NotifyChange();
        }
        return USB_STATUS_OK;

    case PORT_POWER:
        p.status |= PORT_STAT_POWER;
        return USB_STATUS_OK;

    case PORT_SUSPEND:
        p.status |= PORT_STAT_SUSPEND;
        return USB_STATUS_OK;

    case PORT_INDICATOR:
        return USB_STATUS_OK;
    }
    return USB_STATUS_STALL;
}


USBStatusEnum USBHub::ClearPortFeature(int port, int feature)
{
    if (port < 1 || port > fPortCount) {
        return USB_STATUS_STALL;
    }
    HubPort &p = fPorts[port - 1];

    switch (feature) {
    case PORT_ENABLE:
        p.status &= ~PORT_STAT_ENABLE;
        return USB_STATUS_OK;
    case PORT_SUSPEND:
        p.status &= ~PORT_STAT_SUSPEND;
        return USB_STATUS_OK;
    case PORT_POWER:
        p.status &= ~PORT_STAT_POWER;
        return USB_STATUS_OK;
    case PORT_INDICATOR:
        return USB_STATUS_OK;

    case C_PORT_CONNECTION:
        p.change &= ~PORT_CHG_CONNECTION;
        return USB_STATUS_OK;
    case C_PORT_ENABLE:
        p.change &= ~PORT_CHG_ENABLE;
        return USB_STATUS_OK;
    case C_PORT_SUSPEND:
        p.change &= ~PORT_CHG_SUSPEND;
        return USB_STATUS_OK;
    case C_PORT_OVER_CURRENT:
        p.change &= ~PORT_CHG_OVERCURRENT;
        return USB_STATUS_OK;
    case C_PORT_RESET:
        p.change &= ~PORT_CHG_RESET;
        return USB_STATUS_OK;
    }
    return USB_STATUS_STALL;
}


USBStatusEnum USBHub::Submit(URB *urb)
{
    if (urb->type == USB_ENDPOINT_CONTROL) {
        const USBSetup &setup = urb->setup;

        if ((setup.request_type & USB_TYPE_MASK) != USB_TYPE_CLASS) {
            return HandleStandardControl(urb);
        }

        int recipient = setup.request_type & USB_RECIP_MASK;
        switch (setup.request) {
        case USB_REQ_GET_DESCRIPTOR:
            if ((setup.value >> 8) == USB_DT_HUB) {
                return HubDescriptor(urb);
            }
            break;

        case USB_REQ_GET_STATUS:
            if (recipient == USB_RECIP_OTHER) {
                return PortStatus(urb, get_bits(setup.index, 0, 8));
            }
            if (recipient == USB_RECIP_DEVICE) {
                /* Hub status: local power good, no over-current, nothing
                   changed. */
                uint8_t buf[4] = {0, 0, 0, 0};
                uint32_t len = setup.length < 4 ? setup.length : 4;
                urb->actual_length = urb->buffer != nullptr
                                         ? urb->buffer->Write(0, buf, len) : 0;
                return USB_STATUS_OK;
            }
            break;

        case USB_REQ_SET_FEATURE:
            if (recipient == USB_RECIP_OTHER) {
                urb->actual_length = 0;
                return SetPortFeature(get_bits(setup.index, 0, 8),
                                      setup.value);
            }
            urb->actual_length = 0;
            return USB_STATUS_OK;

        case USB_REQ_CLEAR_FEATURE:
            if (recipient == USB_RECIP_OTHER) {
                urb->actual_length = 0;
                return ClearPortFeature(get_bits(setup.index, 0, 8),
                                        setup.value);
            }
            urb->actual_length = 0;
            return USB_STATUS_OK;
        }
        return USB_STATUS_STALL;
    }

    if (urb->type == USB_ENDPOINT_INTERRUPT && urb->is_in &&
        urb->endpoint == HUB_EP_IN) {
        if (!AnyChange()) {
            /* Nothing to report: hold the transfer rather than refusing it,
               since a refusal here would never be retried. */
            if (fParkedUrb != nullptr) {
                return USB_STATUS_STALL;
            }
            fParkedUrb = urb;
            return USB_STATUS_ASYNC;
        }
        uint8_t buf[4];
        BuildChangeBitmap(buf);
        urb->actual_length = urb->buffer != nullptr
                                 ? urb->buffer->Write(0, buf, fBitmapSize) : 0;
        return USB_STATUS_OK;
    }

    return USB_STATUS_STALL;
}


void USBHub::Cancel(URB *urb)
{
    if (fParkedUrb == urb) {
        fParkedUrb = nullptr;
    }
}


//#pragma mark - factory

Device *usb_hub_node_create(int port_count, int port)
{
    USBHub *hub = new USBHub(port_count);
    USBDeviceNode *node = new USBDeviceNode("usb-hub", hub, port);

    node->SetChildBus(new USBBus(node, hub));
    return node;
}
