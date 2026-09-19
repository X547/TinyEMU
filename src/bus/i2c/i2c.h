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
#pragma once

#include <stdint.h>

#include "device.h"

/* The 7 bit addresses a target may take; the eight at either end are
   reserved for special purposes. */
#define I2C_ADDRESS_FIRST 0x08
#define I2C_ADDRESS_LAST  0x77


class I2CBus;


/* One target on an I2C bus. The address is a resource, so a target that
   names none is placed where it cannot collide with one that does.

   A controller drives a transfer as the bus would carry it: Start() for a
   START or repeated START addressed to the target, then bytes one at a time,
   then Stop(). */
class I2CDevice: public Device {
private:
    int fWantedAddress;
    Resource *fAddrRes = nullptr;

public:
    /* 'address' of -1 lets the bus place the device. */
    I2CDevice(const char *name, int address):
        Device(name), fWantedAddress(address) {}

    bool Prepare() override;

    int Address() const
    {
        return fAddrRes != nullptr ? (int)fAddrRes->base : -1;
    }

    /* The target is addressed for reading or writing. False leaves the
       address unacknowledged. */
    virtual bool Start(bool read) = 0;
    /* False leaves the byte unacknowledged. */
    virtual bool Write(uint8_t byte) = 0;
    virtual uint8_t Read() = 0;
    virtual void Stop() = 0;
};


/* Like an MDIO bus, this one is described rather than enumerated, so it both
   assigns the addresses and emits the children into the device tree. A
   target with an interrupt line of its own takes it from the machine. */
class I2CBus final: public Bus {
private:
    RangeAllocator fAddrAlloc {"I2C"};
    bool fFixedClaimed = false;

    bool ClaimFixedAddresses();

public:
    I2CBus(Device *owner);

    const char *Type() const override {return "i2c";}
    I2CBus *AsI2CBus() override {return this;}

    bool AssignResources(Device *dev) override;

    /* nullptr when nothing answers at that address. */
    I2CDevice *DeviceAtAddress(int address);
};
