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

#include "cutils.h"
#include "machine.h"
#include "usb.h"

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
#define PORT_STAT_CONNECTION (1 << 0)
#define PORT_STAT_ENABLE     (1 << 1)
#define PORT_STAT_SUSPEND    (1 << 2)
#define PORT_STAT_OVERCURRENT (1 << 3)
#define PORT_STAT_RESET      (1 << 4)
#define PORT_STAT_POWER      (1 << 8)
#define PORT_STAT_LOW_SPEED  (1 << 9)
#define PORT_STAT_HIGH_SPEED (1 << 10)

#define PORT_CHG_CONNECTION  (1 << 0)
#define PORT_CHG_ENABLE      (1 << 1)
#define PORT_CHG_SUSPEND     (1 << 2)
#define PORT_CHG_OVERCURRENT (1 << 3)
#define PORT_CHG_RESET       (1 << 4)


static const uint8_t kHubDeviceDesc[] = {
    18, USB_DT_DEVICE,
    0x00, 0x02,             /* USB 2.00 */
    USB_CLASS_HUB,
    0x00,
    0x01,                   /* single transaction translator */
    0x40,                   /* 64 byte default control endpoint */
    0xf4, 0x46,             /* vendor */
    0x02, 0x00,             /* product */
    0x00, 0x01,
    0x01, 0x02, 0x00,       /* manufacturer and product strings */
    0x01,
};


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
    uint8_t fConfigDesc[25] {};

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

    uint8_t *d = fConfigDesc;
    /* configuration */
    *d++ = 9;
    *d++ = USB_DT_CONFIG;
    *d++ = 25;
    *d++ = 0;
    *d++ = 1;    /* one interface */
    *d++ = 1;    /* configuration value */
    *d++ = 0;
    *d++ = 0xe0; /* self powered, remote wakeup */
    *d++ = 0;    /* draws no bus power */
    /* interface */
    *d++ = 9;
    *d++ = USB_DT_INTERFACE;
    *d++ = 0;
    *d++ = 0;
    *d++ = 1;    /* one endpoint */
    *d++ = USB_CLASS_HUB;
    *d++ = 0;
    *d++ = 0;
    *d++ = 0;
    /* status change endpoint */
    *d++ = 7;
    *d++ = USB_DT_ENDPOINT;
    *d++ = 0x80 | HUB_EP_IN;
    *d++ = 0x03; /* interrupt */
    *d++ = fBitmapSize;
    *d++ = 0;
    *d++ = 12;   /* 2^11 microframes, the usual hub polling interval */

    SetDescriptors(kHubDeviceDesc, fConfigDesc);
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
    uint8_t buf[16];
    int mask_len = fBitmapSize;
    int len = 7 + 2 * mask_len;

    memset(buf, 0, sizeof(buf));
    buf[0] = len;
    buf[1] = USB_DT_HUB;
    buf[2] = fPortCount;
    /* Per port power switching and per port over-current reporting. */
    put_le16(buf + 3, 0x0009);
    buf[5] = 50; /* 100 ms until power is good, in 2 ms units */
    buf[6] = 0;  /* the hub controller draws no bus current */
    /* DeviceRemovable: every port is removable, so the bits stay clear. The
       PortPwrCtrlMask that follows is all ones for historical reasons. */
    for (int i = 0; i < mask_len; i++) {
        buf[7 + mask_len + i] = 0xff;
    }

    if (len > urb->setup.length) {
        len = urb->setup.length;
    }
    urb->actual_length =
        urb->buffer != nullptr ? urb->buffer->Write(0, buf, len) : 0;
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
               with the reset change pending. */
            p.status |= PORT_STAT_ENABLE | PORT_STAT_HIGH_SPEED;
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
                return PortStatus(urb, setup.index & 0xff);
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
                return SetPortFeature(setup.index & 0xff, setup.value);
            }
            urb->actual_length = 0;
            return USB_STATUS_OK;

        case USB_REQ_CLEAR_FEATURE:
            if (recipient == USB_RECIP_OTHER) {
                urb->actual_length = 0;
                return ClearPortFeature(setup.index & 0xff, setup.value);
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
