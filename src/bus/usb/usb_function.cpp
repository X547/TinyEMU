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
#include "machine.h"


//#pragma mark - USBFunction

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


void USBFunction::DescriptorsChanged()
{
    if (fDevice != nullptr) {
        fDevice->Rebuild();
    }
}


//#pragma mark - USBFunctionDevice

USBFunctionDevice::USBFunctionDevice(USBFunction *fn):
    USBDevice(fn->Name(), fn->Info().speed)
{
    fFunctions.push_back(fn);
    fn->fDevice = this;
    Rebuild();
}


/* Number the functions' interfaces and endpoints in the order they came, and
   assemble the descriptors from them. */
void USBFunctionDevice::Rebuild()
{
    int iface = 0;
    int endpoint = 1;
    for (USBFunction *fn : fFunctions) {
        fn->fFirstInterface = iface;
        fn->fFirstEndpoint = endpoint;
        iface += fn->InterfaceCount();
        endpoint += fn->EndpointCount();
    }

    USBFunctionInfo info = fFunctions[0]->Info();
    USBDescBuilder b;

    b.Device({
        .device_class = 0, /* it is declared per interface */
        .vendor_id = USB_VENDOR_ID,
        .product_id = info.product_id,
        .manufacturer_str = 1,
        .product_str = 2,
        .serial_str = (uint8_t)(info.serial != nullptr ? 3 : 0),
    });
    fDeviceDesc = b.Take();

    b.BeginConfig({
        .attributes = (uint8_t)(USB_CONFIG_ATTR_ONE | info.config_attributes),
        .max_power_ma = info.max_power_ma,
    });
    for (USBFunction *fn : fFunctions) {
        fn->BuildDescriptors(b);
    }
    b.EndConfig();
    fConfigDesc = b.Take();

    SetDescriptors(fDeviceDesc.data(), fConfigDesc.data());
    SetStringDescriptor(1, "TinyEMU");
    SetStringDescriptor(2, info.product);
    SetStringDescriptor(3, info.serial);
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


//#pragma mark - USBFunctionNode

USBFunctionNode::USBFunctionNode(const char *name, USBFunction *fn, int port):
    Device(name), fFunction(fn), fDev(new USBFunctionDevice(fn)), fPort(port)
{
}


USBFunctionNode::~USBFunctionNode()
{
    delete fChildBus;
    delete fDev;
    delete fFunction;
}


bool USBFunctionNode::Realize()
{
    return usb_attach_device(this, fDev, fPort);
}
