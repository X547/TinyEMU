/*
 * USB functions and the device that carries them
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

#include "device.h"
#include "usb.h"
#include "usb_desc.h"

/* What every device here reports, unless a function says otherwise. */
#define USB_VENDOR_ID 0x46f4 /* not one any driver carries a quirk for */


class USBFunctionDevice;

/* What a function asks of the device it is in: its share of the
   configuration's attributes and power, and how a device carrying only this
   function describes itself. */
struct USBFunctionInfo {
    USBSpeedEnum speed = USB_SPEED_FULL;
    uint16_t product_id = 0;
    const char *product = nullptr;
    const char *serial = nullptr;
    uint8_t config_attributes = 0; /* USB_CONFIG_ATTR_*, beyond bit 7 */
    uint16_t max_power_ma = 100;
};


/* One function of a USB device: the interfaces and endpoints of one class,
   and the requests and transfers that reach them. The device numbers them,
   so a function works out its interface numbers and endpoint addresses from
   FirstInterface() and FirstEndpoint() rather than fixing them. */
class USBFunction {
private:
    const char *fName;
    USBFunctionDevice *fDevice = nullptr;
    uint8_t fFirstInterface = 0;
    uint8_t fFirstEndpoint = 1;

    friend class USBFunctionDevice;

protected:
    uint8_t FirstInterface() const {return fFirstInterface;}
    uint8_t FirstEndpoint() const {return fFirstEndpoint;}

    /* The endpoint halt state is the device's, as are the descriptors. */
    void HaltEndpoint(uint8_t address, bool wedge = false);
    void UnwedgeEndpoints();

    /* The interfaces or endpoints changed, so the configuration has to be
       assembled again. Only valid before the guest runs. */
    void DescriptorsChanged();

public:
    USBFunction(const char *name): fName(name) {}
    virtual ~USBFunction() = default;

    const char *Name() const {return fName;}

    virtual USBFunctionInfo Info() const = 0;

    /* How many interface numbers and endpoint numbers the function takes.
       An endpoint number counts once for both directions. */
    virtual int InterfaceCount() const = 0;
    virtual int EndpointCount() const = 0;

    /* The interfaces and the endpoints, into the configuration. */
    virtual void BuildDescriptors(USBDescBuilder &b) = 0;

    /* A request to one of the function's interfaces or endpoints: a class
       or vendor one, or a standard GET_DESCRIPTOR for a class descriptor.
       Anything else goes to the device. */
    virtual USBStatusEnum HandleControl(URB *urb) = 0;

    /* A transfer on one of the function's endpoints. */
    virtual USBStatusEnum Submit(URB *urb) = 0;
    virtual void Cancel(URB *urb) {(void)urb;}

    /* 'iface' counts from the function's first interface. */
    virtual bool SetInterface(int iface, int alt)
        {(void)iface; return alt == 0;}

    /* The device was reset. */
    virtual void Reset() {}
};


/* A USB device made of functions. It answers the standard requests, and
   sends the rest to the function whose interface or endpoint they name. */
class USBFunctionDevice final: public USBDevice {
private:
    std::vector<USBFunction *> fFunctions;
    std::vector<uint8_t> fDeviceDesc;
    std::vector<uint8_t> fConfigDesc;

    USBFunction *FunctionForInterface(int iface);
    USBFunction *FunctionForEndpoint(int endpoint);

protected:
    bool SetInterface(int iface, int alt) override;

public:
    USBFunctionDevice(USBFunction *fn);

    void Rebuild();

    using USBDevice::HaltEndpoint;
    using USBDevice::UnwedgeEndpoints;

    void Reset() override;
    USBStatusEnum Submit(URB *urb) override;
    void Cancel(URB *urb) override;
};


/* Attaches one USB function to the bus it was declared on, with a device of
   its own around it. */
class USBFunctionNode: public Device {
private:
    USBFunction *fFunction;
    USBFunctionDevice *fDev;
    Bus *fChildBus = nullptr;
    int fPort; /* 0 asks for the first free port */

public:
    USBFunctionNode(const char *name, USBFunction *fn, int port);
    ~USBFunctionNode() override;

    /* Takes ownership, as USBDeviceNode::SetChildBus() does. */
    void SetChildBus(Bus *bus) {fChildBus = bus;}

    bool Realize() override;
    Bus *ChildBus() override {return fChildBus;}
};
