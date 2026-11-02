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
#include "usb.h"

#include <stdio.h>
#include <string.h>

#include <vector>

#include "bits.h"
#include "cutils.h"
#include "machine.h"

/* Longest descriptor this assembles in one go: a string descriptor is two
   header bytes plus two per character. */
#define USB_DESC_BUF_SIZE 256


//#pragma mark - USBDevice

USBDevice::USBDevice(const char *name, USBSpeedEnum speed):
    fName(name), fSpeed(speed)
{
}


void USBDevice::SetDescriptors(const uint8_t *device_desc,
                               const uint8_t *config_desc)
{
    fDeviceDesc = device_desc;
    fConfigDesc = config_desc;
}


void USBDevice::SetStringDescriptor(int index, const char *str)
{
    if (index <= 0 || index >= USB_MAX_STRINGS) {
        return;
    }
    fStrings[index] = str;
}


void USBDevice::ResetAltSettings()
{
    for (int i = 0; i < USB_MAX_INTERFACES; i++) {
        if (fAltSettings[i] != 0) {
            SetInterface(i, 0);
            fAltSettings[i] = 0;
        }
    }
}


void USBDevice::Reset()
{
    fAddress = 0;
    fConfigValue = 0;
    fRemoteWakeup = false;
    fHalted = 0;
    fWedged = 0;
    ResetAltSettings();
}


static uint32_t usb_endpoint_bit(uint8_t address)
{
    return 1u << (get_bits(address, 0, 4) * 2 +
                  ((address & USB_DIR_IN) != 0 ? 1 : 0));
}


void USBDevice::HaltEndpoint(uint8_t address, bool wedge)
{
    fHalted |= usb_endpoint_bit(address);
    if (wedge) {
        fWedged |= usb_endpoint_bit(address);
    }
}


/* An interface or endpoint of the configuration, alternate settings
   included; nothing while unconfigured, the Address state having no
   interfaces to speak of (9.4). */
bool USBDevice::HasInterface(int iface) const
{
    if (fConfigValue == 0 || fConfigDesc == nullptr) {
        return false;
    }
    uint32_t total = get_le16(fConfigDesc + 2);
    for (uint32_t off = 0; off + 2 <= total && fConfigDesc[off] >= 2;
         off += fConfigDesc[off]) {
        const uint8_t *d = fConfigDesc + off;
        if (d[1] == USB_DT_INTERFACE && d[2] == iface) {
            return true;
        }
    }
    return false;
}


bool USBDevice::HasEndpoint(uint8_t address) const
{
    if (fConfigValue == 0 || fConfigDesc == nullptr) {
        return false;
    }
    uint32_t total = get_le16(fConfigDesc + 2);
    for (uint32_t off = 0; off + 2 <= total && fConfigDesc[off] >= 2;
         off += fConfigDesc[off]) {
        const uint8_t *d = fConfigDesc + off;
        if (d[1] == USB_DT_ENDPOINT && d[2] == address) {
            return true;
        }
    }
    return false;
}


USBStatusEnum USBDevice::SubmitURB(URB *urb)
{
    if (urb->type == USB_ENDPOINT_CONTROL ||
        urb->type == USB_ENDPOINT_ISOCH) {
        /* Endpoint 0 clears a stall with its next setup packet, and an
           isochronous endpoint has no handshake to stall with. */
        return Submit(urb);
    }
    uint32_t bit = usb_endpoint_bit(urb->endpoint |
                                    (urb->is_in ? USB_DIR_IN : 0));
    if ((fHalted & bit) != 0) {
        return USB_STATUS_STALL;
    }
    USBStatusEnum status = Submit(urb);
    if (status == USB_STATUS_STALL) {
        /* A functional stall lasts until the host clears it (8.4.5). */
        fHalted |= bit;
    }
    return status;
}


/* Copy out of a descriptor blob, honouring the length the request asked for.
   A control IN transfer that returns less than wLength is a short packet, not
   an error, so the caller reports what was actually moved and lets the
   transport decide what to call it. */
static USBStatusEnum usb_control_reply(URB *urb, const uint8_t *data,
                                       uint32_t len)
{
    if (len > urb->setup.length) {
        len = urb->setup.length;
    }
    if (len == 0 || urb->buffer == nullptr) {
        urb->actual_length = 0;
        return USB_STATUS_OK;
    }
    urb->actual_length = urb->buffer->Write(0, data, len);
    return USB_STATUS_OK;
}


/* A string descriptor, built on the spot from the plain C string the device
   registered. Index 0 is the language table, which every device answers the
   same way. */
static USBStatusEnum usb_string_descriptor(URB *urb, const char *str)
{
    uint8_t buf[USB_DESC_BUF_SIZE];
    int len;

    if (str == nullptr) {
        /* Index 0, the language table: US English only. */
        buf[0] = 4;
        buf[1] = USB_DT_STRING;
        put_le16(buf + 2, 0x0409);
        return usb_control_reply(urb, buf, 4);
    }

    len = strlen(str);
    if (len > (USB_DESC_BUF_SIZE - 2) / 2) {
        len = (USB_DESC_BUF_SIZE - 2) / 2;
    }
    buf[0] = 2 + 2 * len;
    buf[1] = USB_DT_STRING;
    for (int i = 0; i < len; i++) {
        put_le16(buf + 2 + 2 * i, (uint8_t)str[i]);
    }
    return usb_control_reply(urb, buf, buf[0]);
}


USBStatusEnum USBDevice::HandleStandardControl(URB *urb)
{
    const USBSetup &setup = urb->setup;
    uint8_t buf[4];

    switch (setup.request_type & (USB_TYPE_MASK | USB_RECIP_MASK)) {
    case USB_TYPE_STANDARD | USB_RECIP_DEVICE:
        switch (setup.request) {
        case USB_REQ_GET_STATUS: {
            /* Self Powered as the configuration says, and Remote Wakeup as
               the host left it (9.4.5). */
            uint16_t status = fRemoteWakeup ? 2 : 0;
            if (fConfigDesc != nullptr &&
                (fConfigDesc[7] & USB_CONFIG_SELF_POWERED) != 0) {
                status |= 1;
            }
            put_le16(buf, status);
            return usb_control_reply(urb, buf, 2);
        }

        case USB_REQ_SET_ADDRESS:
            /* Under xHCI the controller issues this on the guest's behalf as
               part of Address Device; the device just records it. */
            fAddress = get_bits(setup.value, 0, 7);
            urb->actual_length = 0;
            return USB_STATUS_OK;

        case USB_REQ_GET_DESCRIPTOR:
            switch (setup.value >> 8) {
            case USB_DT_DEVICE:
                if (fDeviceDesc == nullptr) {
                    break;
                }
                return usb_control_reply(urb, fDeviceDesc, fDeviceDesc[0]);

            case USB_DT_CONFIG:
                if (fConfigDesc == nullptr ||
                    get_bits(setup.value, 0, 8) != 0) {
                    break;
                }
                /* wTotalLength, not bLength: the whole tree is returned when
                   the host asks for more than the header. */
                return usb_control_reply(urb, fConfigDesc,
                                         get_le16(fConfigDesc + 2));

            case USB_DT_STRING: {
                int index = get_bits(setup.value, 0, 8);
                if (index >= USB_MAX_STRINGS ||
                    (index != 0 && fStrings[index] == nullptr)) {
                    /* a string the device does not have (9.4.3) */
                    break;
                }
                return usb_string_descriptor(urb,
                                             index == 0 ? nullptr
                                                        : fStrings[index]);
            }

            case USB_DT_DEVICE_QUALIFIER:
            case USB_DT_OTHER_SPEED_CONFIG:
                return OtherSpeedDescriptor(urb, setup.value >> 8);

            default:
                break;
            }
            break;

        case USB_REQ_GET_CONFIGURATION:
            buf[0] = fConfigValue;
            return usb_control_reply(urb, buf, 1);

        case USB_REQ_SET_CONFIGURATION: {
            /* 0 or the configuration's own value; anything else is a
               Request Error (9.4.7). */
            uint8_t value = get_bits(setup.value, 0, 8);
            if (value != 0 &&
                (fConfigDesc == nullptr || value != fConfigDesc[5])) {
                break;
            }
            /* Even setting the same configuration again puts every
               interface back on its default setting, and every endpoint
               out of halt. */
            ResetAltSettings();
            fHalted = 0;
            fWedged = 0;
            fConfigValue = value;
            urb->actual_length = 0;
            return USB_STATUS_OK;
        }

        case USB_REQ_CLEAR_FEATURE:
        case USB_REQ_SET_FEATURE:
            /* Remote wakeup, for a configuration that says it can; this
               model never wakes the host, but it remembers (9.4.1). */
            if (setup.value != USB_FEATURE_REMOTE_WAKEUP ||
                fConfigDesc == nullptr ||
                (fConfigDesc[7] & USB_CONFIG_REMOTE_WAKEUP) == 0) {
                break;
            }
            fRemoteWakeup = setup.request == USB_REQ_SET_FEATURE;
            urb->actual_length = 0;
            return USB_STATUS_OK;
        }
        break;

    case USB_TYPE_STANDARD | USB_RECIP_INTERFACE: {
        int iface = get_bits(setup.index, 0, 8);
        if (!HasInterface(iface)) {
            /* not one the configuration has, or none at all (9.4) */
            break;
        }
        switch (setup.request) {
        case USB_REQ_GET_STATUS:
            put_le16(buf, 0);
            return usb_control_reply(urb, buf, 2);

        case USB_REQ_GET_INTERFACE:
            if (iface >= USB_MAX_INTERFACES) {
                break;
            }
            buf[0] = fAltSettings[iface];
            return usb_control_reply(urb, buf, 1);

        case USB_REQ_SET_INTERFACE: {
            int alt = get_bits(setup.value, 0, 8);
            if (iface >= USB_MAX_INTERFACES || !SetInterface(iface, alt)) {
                break;
            }
            fAltSettings[iface] = alt;
            urb->actual_length = 0;
            return USB_STATUS_OK;
        }
        }
        break;
    }

    case USB_TYPE_STANDARD | USB_RECIP_ENDPOINT:
        return EndpointRequest(urb);
    }

    return USB_STATUS_STALL;
}


/* How a high speed device would look at full speed: the device qualifier
   and the other speed configuration (9.6.2, 9.6.4). A full speed only device
   has neither and refuses both. The configuration is the same one, with the
   packet sizes and intervals full speed allows. */
USBStatusEnum USBDevice::OtherSpeedDescriptor(URB *urb, uint8_t type)
{
    if (fSpeed != USB_SPEED_HIGH || fDeviceDesc == nullptr) {
        return USB_STATUS_STALL;
    }
    if (type == USB_DT_DEVICE_QUALIFIER) {
        uint8_t q[10];
        q[0] = sizeof(q);
        q[1] = USB_DT_DEVICE_QUALIFIER;
        memcpy(q + 2, fDeviceDesc + 2, 5); /* bcdUSB, class, protocol */
        q[7] = 64;
        q[8] = fDeviceDesc[17];
        q[9] = 0;
        return usb_control_reply(urb, q, sizeof(q));
    }
    if (fConfigDesc == nullptr || get_bits(urb->setup.value, 0, 8) != 0) {
        return USB_STATUS_STALL;
    }
    uint32_t total = get_le16(fConfigDesc + 2);
    std::vector<uint8_t> c(fConfigDesc, fConfigDesc + total);
    c[1] = USB_DT_OTHER_SPEED_CONFIG;
    for (uint32_t off = 0; off + 7 <= total && c[off] >= 2; off += c[off]) {
        uint8_t *d = &c[off];
        if (d[1] != USB_DT_ENDPOINT) {
            continue;
        }
        uint16_t mps = get_le16(d + 4) & 0x7ff;
        switch (d[3] & 3) {
        case 2: /* bulk: 64 bytes at most */
            put_le16(d + 4, mps > 64 ? 64 : mps);
            break;
        case 3: { /* interrupt: 64 bytes, and the interval in frames */
            put_le16(d + 4, mps > 64 ? 64 : mps);
            uint32_t frames = d[6] >= 4 ? 1u << (d[6] - 4) : 1;
            d[6] = frames > 255 ? 255 : frames;
            break;
        }
        }
    }
    return usb_control_reply(urb, c.data(), total);
}


/* GET_STATUS and the halt feature of one endpoint (9.4.1, 9.4.5, 9.4.9).
   Endpoint 0 is never left halted; any other has to be in the
   configuration. */
USBStatusEnum USBDevice::EndpointRequest(URB *urb)
{
    const USBSetup &setup = urb->setup;
    uint8_t address = get_bits(setup.index, 0, 8);
    bool ep0 = get_bits(address, 0, 4) == 0;
    uint8_t buf[2];

    if ((address & 0x70) != 0 || (!ep0 && !HasEndpoint(address))) {
        return USB_STATUS_STALL;
    }
    uint32_t bit = ep0 ? 0 : usb_endpoint_bit(address);

    switch (setup.request) {
    case USB_REQ_GET_STATUS:
        put_le16(buf, (fHalted & bit) != 0 ? 1 : 0);
        return usb_control_reply(urb, buf, 2);

    case USB_REQ_SET_FEATURE:
    case USB_REQ_CLEAR_FEATURE:
        if (setup.value != USB_FEATURE_ENDPOINT_HALT) {
            break;
        }
        if (setup.request == USB_REQ_SET_FEATURE) {
            fHalted |= bit;
        } else if ((fWedged & bit) == 0) {
            fHalted &= ~bit;
        }
        urb->actual_length = 0;
        return USB_STATUS_OK;
    }
    return USB_STATUS_STALL;
}


void usb_urb_complete(URB *urb, USBStatusEnum status, uint32_t actual_length)
{
    urb->status = status;
    urb->actual_length = actual_length;
    if (urb->completion != nullptr) {
        urb->completion->Complete(urb);
    }
}


//#pragma mark - USBBus

bool USBBus::AssignResources(Device *dev)
{
    /* A USB device reaches the host only through its controller, so it holds
       no host address space and no interrupt line of its own. Anything it
       declares is a modelling mistake worth reporting rather than dropping. */
    for (int i = 0; i < dev->ResourceCount(); i++) {
        if (dev->ResourceAt(i)->type != RES_NONE) {
            vm_error("usb bus: device '%s' declared a resource, but a USB "
                     "device has none of its own\n", dev->Name());
            return false;
        }
    }
    return true;
}


//#pragma mark - USBDeviceNode

USBDeviceNode::USBDeviceNode(const char *name, USBDevice *dev, int port):
    Device(name), fDev(dev), fPort(port)
{
}


USBDeviceNode::~USBDeviceNode()
{
    delete fChildBus;
    delete fDev;
}


bool USBDeviceNode::Realize()
{
    USBBus *bus = dynamic_cast<USBBus *>(ParentBus());
    if (bus == nullptr) {
        vm_error("%s: must be attached to a USB bus\n", Name());
        return false;
    }

    USBPortTarget *target = bus->Target();
    int port = fPort;
    if (port == 0) {
        port = target->FindFreePort(fDev->Speed());
        if (port == 0) {
            vm_error("%s: no free port for a %s speed device\n", Name(),
                     fDev->Speed() == USB_SPEED_SUPER ? "super" : "high");
            return false;
        }
    }
    return target->AttachDevice(fDev, port);
}
