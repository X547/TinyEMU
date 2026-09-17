/*
 * USB descriptor builder
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

#include "usb.h"

/* bmAttributes of a configuration: bit 7 reads as one on every device. */
#define USB_CONFIG_ATTR_ONE          0x80
#define USB_CONFIG_ATTR_SELF_POWERED 0x40
#define USB_CONFIG_ATTR_REMOTE_WAKEUP 0x20

/* bmAttributes of an endpoint; the transfer type is its low two bits. */
#define USB_ENDPOINT_ATTR_CONTROL   0x00
#define USB_ENDPOINT_ATTR_ISOCH     0x01
#define USB_ENDPOINT_ATTR_BULK      0x02
#define USB_ENDPOINT_ATTR_INTERRUPT 0x03


/* The fields of a device descriptor, in the order they are written. The
   builder fills in the lengths and the descriptor type. */
struct USBDeviceDescInfo {
    uint16_t usb_version = 0x0200;
    uint8_t device_class = 0;
    uint8_t device_subclass = 0;
    uint8_t device_protocol = 0;
    uint8_t max_packet0 = 64;
    uint16_t vendor_id = 0;
    uint16_t product_id = 0;
    uint16_t device_version = 0x0100;
    uint8_t manufacturer_str = 0;
    uint8_t product_str = 0;
    uint8_t serial_str = 0;
    uint8_t config_count = 1;
};

struct USBConfigDescInfo {
    uint8_t config_value = 1;
    uint8_t config_str = 0;
    uint8_t attributes = USB_CONFIG_ATTR_ONE;
    /* What the configuration draws from the bus, in milliamps. */
    uint16_t max_power_ma = 0;
};

struct USBInterfaceDescInfo {
    uint8_t number = 0;
    uint8_t alternate = 0;
    uint8_t iface_class = 0;
    uint8_t iface_subclass = 0;
    uint8_t iface_protocol = 0;
    uint8_t iface_str = 0;
};


/* Assembles the descriptor bytes a device hands to the host.
 *
 * Every length is the builder's business: bLength as each descriptor is
 * closed, and a configuration's wTotalLength, bNumInterfaces and each
 * interface's bNumEndpoints as the tree is walked. A class specific
 * descriptor that nothing here knows about -- the HID one, a hub's -- is
 * written between BeginDescriptor() and EndDescriptor() and counts toward
 * those totals like any other.
 *
 * The bytes live in the builder until Take() moves them out, so a device
 * keeps the vector it was given and points the descriptor at its data. */
class USBDescBuilder {
private:
    std::vector<uint8_t> fData;
    int fDescStart = -1;   /* the descriptor BeginDescriptor() opened */
    int fLastDescStart = -1; /* and the last one it opened, once closed */
    int fConfigStart = -1; /* the configuration descriptor, for its totals */
    int fIfaceStart = -1;  /* the interface whose endpoints are counted */

    void CloseDescriptor();
    void CloseInterface();

public:
    /* One descriptor, of a type the builder has no method for. */
    void BeginDescriptor(uint8_t type);
    void EndDescriptor() {CloseDescriptor();}

    void Byte(uint8_t v) {fData.push_back(v);}
    void Word(uint16_t v);
    void Bytes(const void *data, int len);

    void Device(const USBDeviceDescInfo &info);

    /* A configuration and the interfaces and endpoints inside it. Nesting is
       closed for the caller: the next BeginInterface() ends the interface
       before it, and EndConfig() ends both. */
    void BeginConfig(const USBConfigDescInfo &info);
    void EndConfig();
    void BeginInterface(const USBInterfaceDescInfo &info);
    void Endpoint(uint8_t address, uint8_t attributes, uint16_t max_packet,
                  uint8_t interval);

    /* Where the descriptor last opened begins, so that a device answering a
       request for it can find it again in the finished blob. */
    int DescriptorOffset() const {return fLastDescStart;}

    int Length() const {return (int)fData.size();}
    const uint8_t *Data() const {return fData.data();}

    std::vector<uint8_t> Take() {return std::move(fData);}
};
