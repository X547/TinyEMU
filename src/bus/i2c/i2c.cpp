/*
 * I2C bus and target devices
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
#include "i2c.h"

#include "machine.h"


//#pragma mark - I2CDevice

bool I2CDevice::Prepare()
{
    if (ParentBus() == nullptr || ParentBus()->AsI2CBus() == nullptr) {
        vm_error("%s: must be attached to an I2C bus\n", Name());
        return false;
    }

    if (fWantedAddress >= 0) {
        if (fWantedAddress < I2C_ADDRESS_FIRST ||
            fWantedAddress > I2C_ADDRESS_LAST) {
            vm_error("%s: I2C address 0x%02x is reserved or out of range; "
                     "it must be between 0x%02x and 0x%02x\n", Name(),
                     fWantedAddress, I2C_ADDRESS_FIRST, I2C_ADDRESS_LAST);
            return false;
        }
        fAddrRes = AddFixedResource(RES_I2C_ADDR, fWantedAddress, 1);
    } else {
        fAddrRes = AddResource(RES_I2C_ADDR, 1, 1);
    }
    return fAddrRes != nullptr;
}


//#pragma mark - I2CBus

I2CBus::I2CBus(Device *owner):
    Bus(owner)
{
    fAddrAlloc.SetWindow(I2C_ADDRESS_FIRST,
                         I2C_ADDRESS_LAST - I2C_ADDRESS_FIRST + 1);
}


bool I2CBus::ClaimFixedAddresses()
{
    for (int i = 0; i < DeviceCount(); i++) {
        Device *dev = DeviceAt(i);
        for (int j = 0; j < dev->ResourceCount(); j++) {
            Resource *res = dev->ResourceAt(j);
            if (res->type == RES_I2C_ADDR && res->fixed &&
                !fAddrAlloc.Assign(res, dev->Name())) {
                return false;
            }
        }
    }
    return true;
}


bool I2CBus::AssignResources(Device *dev)
{
    SystemBus *sys = dynamic_cast<SystemBus *>(Root());

    /* Every address the configuration names is taken before any is handed
       out, so a target placed by the bus never lands on one named later. */
    if (!fFixedClaimed) {
        fFixedClaimed = true;
        if (!ClaimFixedAddresses()) {
            return false;
        }
    }

    for (int i = 0; i < dev->ResourceCount(); i++) {
        Resource *res = dev->ResourceAt(i);
        switch (res->type) {
        case RES_I2C_ADDR:
            if (!res->assigned && !fAddrAlloc.Assign(res, dev->Name())) {
                return false;
            }
            break;
        case RES_IRQ:
            /* The line a target raises is wired to the machine's interrupt
               controller, not carried by the bus. */
            if (sys == nullptr || !sys->AssignOne(res, dev->Name())) {
                return false;
            }
            break;
        case RES_MMIO:
        case RES_IO:
            vm_error("%s: an I2C target has no address space of its own\n",
                     dev->Name());
            return false;
        default:
            break;
        }
    }
    return true;
}


I2CDevice *I2CBus::DeviceAtAddress(int address)
{
    for (int i = 0; i < DeviceCount(); i++) {
        I2CDevice *dev = dynamic_cast<I2CDevice *>(DeviceAt(i));
        if (dev != nullptr && dev->Address() == address) {
            return dev;
        }
    }
    return nullptr;
}
