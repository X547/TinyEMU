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


void USBDevice::Reset()
{
    fAddress = 0;
    fConfigValue = 0;
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
        /* Language table: US English only. */
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
        case USB_REQ_GET_STATUS:
            /* Bus powered, no remote wakeup armed. */
            put_le16(buf, 0);
            return usb_control_reply(urb, buf, 2);

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
                if (index >= USB_MAX_STRINGS) {
                    break;
                }
                return usb_string_descriptor(urb,
                                             index == 0 ? nullptr
                                                        : fStrings[index]);
            }

            default:
                /* Device qualifier and the other speed configuration are the
                   common ones to land here. Stalling is the right answer for
                   a single speed device and is what the host expects. */
                break;
            }
            break;

        case USB_REQ_GET_CONFIGURATION:
            buf[0] = fConfigValue;
            return usb_control_reply(urb, buf, 1);

        case USB_REQ_SET_CONFIGURATION:
            fConfigValue = get_bits(setup.value, 0, 8);
            urb->actual_length = 0;
            return USB_STATUS_OK;

        case USB_REQ_CLEAR_FEATURE:
        case USB_REQ_SET_FEATURE:
            /* The only device level feature is remote wakeup, which this
               model does not implement but must not fail. */
            urb->actual_length = 0;
            return USB_STATUS_OK;
        }
        break;

    case USB_TYPE_STANDARD | USB_RECIP_INTERFACE:
        switch (setup.request) {
        case USB_REQ_GET_STATUS:
            put_le16(buf, 0);
            return usb_control_reply(urb, buf, 2);

        case USB_REQ_GET_INTERFACE:
            /* One alternate setting per interface, so always zero. */
            buf[0] = 0;
            return usb_control_reply(urb, buf, 1);

        case USB_REQ_SET_INTERFACE:
            urb->actual_length = 0;
            return USB_STATUS_OK;
        }
        break;

    case USB_TYPE_STANDARD | USB_RECIP_ENDPOINT:
        switch (setup.request) {
        case USB_REQ_GET_STATUS:
            /* Halt state is tracked by the controller in this model, so an
               endpoint here is never reported halted. */
            put_le16(buf, 0);
            return usb_control_reply(urb, buf, 2);

        case USB_REQ_CLEAR_FEATURE:
        case USB_REQ_SET_FEATURE:
            urb->actual_length = 0;
            return USB_STATUS_OK;
        }
        break;
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
