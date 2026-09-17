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
#include "usb_desc.h"

#include <string.h>

#include "bits.h"
#include "cutils.h"

/* Offsets of the fields the builder goes back and fills in. */
#define USB_DESC_LENGTH       0
#define USB_CONFIG_TOTAL_LEN  2
#define USB_CONFIG_NUM_IFACE  4
#define USB_IFACE_NUM_EP      4


void USBDescBuilder::Word(uint16_t v)
{
    fData.push_back(get_bits(v, 0, 8));
    fData.push_back(get_bits(v, 8, 8));
}


void USBDescBuilder::Bytes(const void *data, int len)
{
    const uint8_t *p = (const uint8_t *)data;
    fData.insert(fData.end(), p, p + len);
}


void USBDescBuilder::BeginDescriptor(uint8_t type)
{
    CloseDescriptor();
    fDescStart = (int)fData.size();
    fLastDescStart = fDescStart;
    Byte(0); /* bLength, filled in on close */
    Byte(type);
}


/* Every descriptor carries its own length, so it is written once the next one
   starts or the tree ends rather than being counted by hand. */
void USBDescBuilder::CloseDescriptor()
{
    if (fDescStart < 0) {
        return;
    }
    fData[fDescStart + USB_DESC_LENGTH] = fData.size() - fDescStart;
    fDescStart = -1;
}


void USBDescBuilder::CloseInterface()
{
    if (fIfaceStart < 0) {
        return;
    }
    fIfaceStart = -1;
}


void USBDescBuilder::Device(const USBDeviceDescInfo &info)
{
    BeginDescriptor(USB_DT_DEVICE);
    Word(info.usb_version);
    Byte(info.device_class);
    Byte(info.device_subclass);
    Byte(info.device_protocol);
    Byte(info.max_packet0);
    Word(info.vendor_id);
    Word(info.product_id);
    Word(info.device_version);
    Byte(info.manufacturer_str);
    Byte(info.product_str);
    Byte(info.serial_str);
    Byte(info.config_count);
    CloseDescriptor();
}


void USBDescBuilder::BeginConfig(const USBConfigDescInfo &info)
{
    BeginDescriptor(USB_DT_CONFIG);
    fConfigStart = fDescStart;
    Word(0); /* wTotalLength */
    Byte(0); /* bNumInterfaces */
    Byte(info.config_value);
    Byte(info.config_str);
    Byte(info.attributes);
    /* The unit is 2 mA, so a configuration asking for an odd number of
       milliamps is granted the next even one. */
    Byte((info.max_power_ma + 1) / 2);
}


void USBDescBuilder::EndConfig()
{
    CloseInterface();
    CloseDescriptor();
    if (fConfigStart < 0) {
        return;
    }
    put_le16(&fData[fConfigStart + USB_CONFIG_TOTAL_LEN],
             fData.size() - fConfigStart);
    fConfigStart = -1;
}


void USBDescBuilder::BeginInterface(const USBInterfaceDescInfo &info)
{
    CloseInterface();
    BeginDescriptor(USB_DT_INTERFACE);
    fIfaceStart = fDescStart;
    Byte(info.number);
    Byte(info.alternate);
    Byte(0); /* bNumEndpoints */
    Byte(info.iface_class);
    Byte(info.iface_subclass);
    Byte(info.iface_protocol);
    Byte(info.iface_str);

    if (fConfigStart >= 0) {
        fData[fConfigStart + USB_CONFIG_NUM_IFACE]++;
    }
}


void USBDescBuilder::Endpoint(uint8_t address, uint8_t attributes,
                              uint16_t max_packet, uint8_t interval)
{
    BeginDescriptor(USB_DT_ENDPOINT);
    Byte(address);
    Byte(attributes);
    Word(max_packet);
    Byte(interval);
    CloseDescriptor();

    if (fIfaceStart >= 0) {
        fData[fIfaceStart + USB_IFACE_NUM_EP]++;
    }
}
