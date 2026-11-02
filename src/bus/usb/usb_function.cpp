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
#include "usb_function.h"

#include "bits.h"
#include "device_class.h"
#include "machine.h"

/* The most a configuration may draw from the bus (USB 2.0 7.2.1). */
#define USB_MAX_POWER_MA 500

/* Endpoint numbers 1 to 15 are there for the functions to share. */
#define USB_MAX_ENDPOINT_NUMBERS 15


//#pragma mark - USBFunction

USBSpeedEnum USBFunction::Speed() const
{
    return fDevice != nullptr ? fDevice->Speed() : Info().speed;
}


void USBFunction::HaltEndpoint(uint8_t address, bool wedge)
{
    if (fDevice != nullptr) {
        fDevice->HaltEndpoint(address, wedge);
    }
}


void USBFunction::UnwedgeEndpoints()
{
    if (fDevice != nullptr) {
        fDevice->UnwedgeEndpoints();
    }
}


bool USBFunction::DescriptorsChanged()
{
    return fDevice == nullptr || fDevice->Rebuild();
}


//#pragma mark - USBFunctionDevice

USBFunctionDevice::USBFunctionDevice(const char *name):
    USBDevice(name, USB_SPEED_FULL)
{
}


void USBFunctionDevice::AddFunction(USBFunction *fn)
{
    fFunctions.push_back(fn);
    fn->fDevice = this;
}


/* The functions' own speed when they agree on one, and otherwise the
   fastest they all can run at. */
bool USBFunctionDevice::ChooseSpeed()
{
    USBSpeedEnum speed = fFunctions[0]->Info().speed;
    bool agree = true;
    for (USBFunction *fn : fFunctions) {
        agree = agree && fn->Info().speed == speed;
    }
    if (agree) {
        SetSpeed(speed);
        return true;
    }

    for (USBSpeedEnum s : {USB_SPEED_HIGH, USB_SPEED_FULL}) {
        bool all = true;
        for (USBFunction *fn : fFunctions) {
            all = all && fn->SupportsSpeed(s);
        }
        if (all) {
            SetSpeed(s);
            return true;
        }
    }
    for (USBFunction *fn : fFunctions) {
        if (!fn->SupportsSpeed(speed)) {
            vm_error("%s: %s and %s cannot run at the same speed\n", Name(),
                     fFunctions[0]->Name(), fn->Name());
            break;
        }
    }
    return false;
}


bool USBFunctionDevice::Finalize()
{
    if (fFunctions.empty()) {
        vm_error("%s: needs a nested usb-function bus with at least one "
                 "function on it\n", Name());
        return false;
    }
    return ChooseSpeed() && Rebuild();
}


/* The class of a function's first interface, which names the function as a
   whole in its association descriptor. */
static void usb_function_class(USBFunction *fn, uint8_t cls[3])
{
    USBDescBuilder b;

    cls[0] = cls[1] = cls[2] = 0;
    fn->BuildDescriptors(b);
    const uint8_t *d = b.Data();
    for (int off = 0; off + 8 <= b.Length() && d[off] >= 2; off += d[off]) {
        if (d[off + 1] == USB_DT_INTERFACE) {
            cls[0] = d[off + 5];
            cls[1] = d[off + 6];
            cls[2] = d[off + 7];
            return;
        }
    }
}


/* Number the functions' interfaces and endpoints in the order they came, and
   assemble the descriptors from them. */
bool USBFunctionDevice::Rebuild()
{
    int iface = 0;
    int endpoint = 1;
    for (USBFunction *fn : fFunctions) {
        fn->fFirstInterface = iface;
        fn->fFirstEndpoint = endpoint;
        iface += fn->InterfaceCount();
        endpoint += fn->EndpointCount();
    }
    if (iface > USB_MAX_INTERFACES) {
        vm_error("%s: its functions need %d interfaces, but a device has at "
                 "most %d\n", Name(), iface, USB_MAX_INTERFACES);
        return false;
    }
    if (endpoint - 1 > USB_MAX_ENDPOINT_NUMBERS) {
        vm_error("%s: its functions need %d endpoint numbers, but a device "
                 "has %d\n", Name(), endpoint - 1, USB_MAX_ENDPOINT_NUMBERS);
        return false;
    }

    bool composite = fFunctions.size() > 1;
    USBFunctionInfo info = fFunctions[0]->Info();
    uint8_t attributes = 0;
    int power = 0;
    bool associations = false;
    for (USBFunction *fn : fFunctions) {
        USBFunctionInfo i = fn->Info();
        attributes |= i.config_attributes;
        power += i.max_power_ma;
        if (info.serial == nullptr) {
            info.serial = i.serial;
        }
        associations = associations || fn->InterfaceCount() > 1;
    }
    associations = associations && composite;
    if (composite) {
        info.product_id = USB_COMPOSITE_PRODUCT_ID;
        info.product = "USB Composite Device";
    }

    USBDescBuilder b;
    b.Device({
        /* Without associations the class is declared per interface. */
        .device_class = (uint8_t)(associations ? USB_CLASS_MISC : 0),
        .device_subclass =
            (uint8_t)(associations ? USB_MISC_SUBCLASS_COMMON : 0),
        .device_protocol = (uint8_t)(associations ? USB_MISC_PROTOCOL_IAD : 0),
        .vendor_id = USB_VENDOR_ID,
        .product_id = info.product_id,
        .manufacturer_str = 1,
        .product_str = 2,
        .serial_str = (uint8_t)(info.serial != nullptr ? 3 : 0),
    });
    fDeviceDesc = b.Take();

    b.BeginConfig({
        .attributes = (uint8_t)(USB_CONFIG_ATTR_ONE | attributes),
        .max_power_ma =
            (uint16_t)(power < USB_MAX_POWER_MA ? power : USB_MAX_POWER_MA),
    });
    for (USBFunction *fn : fFunctions) {
        if (associations && fn->InterfaceCount() > 1) {
            uint8_t cls[3];
            usb_function_class(fn, cls);
            b.BeginDescriptor(USB_DT_IAD);
            b.Byte(fn->fFirstInterface);
            b.Byte(fn->InterfaceCount());
            b.Bytes(cls, sizeof(cls));
            b.Byte(0); /* no string */
            b.EndDescriptor();
        }
        fn->BuildDescriptors(b);
    }
    b.EndConfig();
    fConfigDesc = b.Take();

    SetDescriptors(fDeviceDesc.data(), fConfigDesc.data());
    SetStringDescriptor(1, "TinyEMU");
    SetStringDescriptor(2, info.product);
    SetStringDescriptor(3, info.serial);
    return true;
}


USBFunction *USBFunctionDevice::FunctionForInterface(int iface)
{
    for (USBFunction *fn : fFunctions) {
        if (iface >= fn->fFirstInterface &&
            iface < fn->fFirstInterface + fn->InterfaceCount()) {
            return fn;
        }
    }
    return nullptr;
}


USBFunction *USBFunctionDevice::FunctionForEndpoint(int endpoint)
{
    for (USBFunction *fn : fFunctions) {
        if (endpoint >= fn->fFirstEndpoint &&
            endpoint < fn->fFirstEndpoint + fn->EndpointCount()) {
            return fn;
        }
    }
    return nullptr;
}


bool USBFunctionDevice::SetInterface(int iface, int alt)
{
    USBFunction *fn = FunctionForInterface(iface);
    if (fn == nullptr) {
        return alt == 0;
    }
    return fn->SetInterface(iface - fn->fFirstInterface, alt);
}


void USBFunctionDevice::Reset()
{
    USBDevice::Reset();
    for (USBFunction *fn : fFunctions) {
        fn->Reset();
    }
}


USBStatusEnum USBFunctionDevice::Submit(URB *urb)
{
    if (urb->type != USB_ENDPOINT_CONTROL) {
        USBFunction *fn = FunctionForEndpoint(urb->endpoint);
        return fn != nullptr ? fn->Submit(urb) : USB_STATUS_STALL;
    }

    const USBSetup &setup = urb->setup;
    bool standard = (setup.request_type & USB_TYPE_MASK) == USB_TYPE_STANDARD;
    USBFunction *fn = nullptr;

    switch (setup.request_type & USB_RECIP_MASK) {
    case USB_RECIP_INTERFACE:
        /* A class descriptor is fetched with a standard request naming the
           interface, so the function sees that one too. */
        if (!standard || setup.request == USB_REQ_GET_DESCRIPTOR) {
            fn = FunctionForInterface(get_bits(setup.index, 0, 8));
        }
        break;
    case USB_RECIP_ENDPOINT:
        if (!standard) {
            fn = FunctionForEndpoint(get_bits(setup.index, 0, 4));
        }
        break;
    }

    if (fn != nullptr) {
        return fn->HandleControl(urb);
    }
    return standard ? HandleStandardControl(urb) : USB_STATUS_STALL;
}


void USBFunctionDevice::Cancel(URB *urb)
{
    for (USBFunction *fn : fFunctions) {
        fn->Cancel(urb);
    }
}


//#pragma mark - USBFunctionBus

bool USBFunctionBus::AssignResources(Device *dev)
{
    for (int i = 0; i < dev->ResourceCount(); i++) {
        if (dev->ResourceAt(i)->type != RES_NONE) {
            vm_error("usb-function bus: device '%s' declared a resource, but "
                     "a USB function has none of its own\n", dev->Name());
            return false;
        }
    }
    return true;
}


//#pragma mark - USBFunctionNode

USBFunctionNode::USBFunctionNode(const char *name, USBFunction *fn, int port):
    Device(name), fFunction(fn), fPort(port)
{
}


USBFunctionNode::~USBFunctionNode()
{
    delete fChildBus;
    delete fDev;
    delete fFunction;
}


bool USBFunctionNode::Prepare()
{
    USBFunctionBus *bus = dynamic_cast<USBFunctionBus *>(ParentBus());
    if (bus == nullptr) {
        fDev = new USBFunctionDevice(Name());
        fDev->AddFunction(fFunction);
        return true;
    }
    if (fPort != 0) {
        vm_error("%s: 'port' is the usb-device's to give, not a function's\n",
                 Name());
        return false;
    }
    bus->Dev()->AddFunction(fFunction);
    return true;
}


bool USBFunctionNode::Realize()
{
    if (fDev == nullptr) {
        return true;
    }
    return fDev->Finalize() && usb_attach_device(this, fDev, fPort);
}


//#pragma mark - usb-device

/* A device whose functions are declared on its own bus. */
class USBFunctionDeviceNode final: public Device {
private:
    USBFunctionDevice fDev;
    USBFunctionBus fBus;
    int fPort; /* 0 asks for the first free port */

public:
    USBFunctionDeviceNode(int port):
        Device("usb-device"), fDev("usb-device"), fBus(this, &fDev),
        fPort(port) {}

    bool Realize() override
        {return fDev.Finalize() && usb_attach_device(this, &fDev, fPort);}
    Bus *ChildBus() override {return &fBus;}
};


/* "port" is the port asked for, or 0 for the first free one. */
class USBFunctionDeviceClass final: public DeviceClass {
public:
    USBFunctionDeviceClass(): DeviceClass("usb-device") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        int port;

        (void)ctx;
        if (!cfg.GetInt("port", &port, 0)) {
            return nullptr;
        }
        return new USBFunctionDeviceNode(port);
    }
};

static const USBFunctionDeviceClass sUSBFunctionDeviceClass;
