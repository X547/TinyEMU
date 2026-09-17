/*
 * MDIO bus and PHY devices
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
#pragma once

#include "bits.h"
#include "device.h"

/* Clause 22 addresses five bits of PHY address. */
#define MDIO_ADDRESS_COUNT 32

/* What a read returns when nothing answers at an address. A driver scanning
   the bus takes an all ones reply as "no device here", so an address with no
   declared PHY has to read this rather than zero. */
#define MDIO_NO_DEVICE 0xffff

/* Clause 22 register numbers. Only the ones the generic PHY implements. */
#define MII_BMCR     0x00 /* basic mode control */
#define MII_BMSR     0x01 /* basic mode status */
#define MII_PHYID1   0x02
#define MII_PHYID2   0x03
#define MII_ANAR     0x04 /* auto negotiation advertisement */
#define MII_ANLPAR   0x05 /* auto negotiation link partner ability */
#define MII_ANER     0x06 /* auto negotiation expansion */
#define MII_GTCR     0x09 /* 1000BASE-T control */
#define MII_GTSR     0x0a /* 1000BASE-T status */
#define MII_EXTSR    0x0f /* extended status */

#define MII_BMCR_RESET       bit_at(15)
#define MII_BMCR_LOOPBACK    bit_at(14)
#define MII_BMCR_SPEED100    bit_at(13)
#define MII_BMCR_ANEG_ENABLE bit_at(12)
#define MII_BMCR_POWERDOWN   bit_at(11)
#define MII_BMCR_ISOLATE     bit_at(10)
#define MII_BMCR_ANEG_RESTART bit_at(9)
#define MII_BMCR_FULLDUPLEX  bit_at(8)
#define MII_BMCR_SPEED1000   bit_at(6)

/* Everything but the link and the negotiation result, which follow the
   carrier: 100BASE-TX full and half, 10 Mb/s full and half, extended status,
   preamble suppression, auto negotiation ability, extended capability. */
#define MII_BMSR_STATIC      0x7949
#define MII_BMSR_ANEG_DONE   bit_at(5)
#define MII_BMSR_LINK        bit_at(2)

/* A synthetic identifier: no real vendor owns it, so Linux binds its generic
   PHY driver, which is the behaviour a model with no vendor quirks wants. It
   must be neither zero nor all ones, both of which mean "absent". */
#define PHY_GENERIC_ID 0x00000041


class MDIOBus;


/* One device on an MDIO bus. The address is a resource so that a
   configuration which does not name one still cannot collide with a
   configuration which does. */
class MDIODevice: public Device {
private:
    int fWantedAddress;
    Resource *fAddrRes = nullptr;
    uint32_t fPhandle = 0;

public:
    /* 'address' of -1 lets the bus place the device. */
    MDIODevice(const char *name, int address):
        Device(name), fWantedAddress(address) {}

    bool Prepare() override;

    int Address() const
    {
        return fAddrRes != nullptr ? (int)fAddrRes->base : -1;
    }

    /* Handed down by the MAC before it emits its own properties, because
       "phy-handle" has to be written before the node it names is opened. */
    void SetPhandle(uint32_t phandle) {fPhandle = phandle;}
    uint32_t Phandle() const {return fPhandle;}

    virtual uint16_t MdioRead(int reg) = 0;
    virtual void MdioWrite(int reg, uint16_t val) = 0;

    /* The negotiated state of the link. A MAC reports this to its driver
       through its own in band status registers, which is the only path some
       drivers use, so it is asked of the PHY directly rather than being
       inferred from the register file. */
    virtual bool LinkUp() const = 0;
    virtual int Speed() const = 0; /* 10, 100 or 1000 */
    virtual bool FullDuplex() const = 0;

    /* Follows the host side carrier. */
    virtual void SetCarrier(bool carrier) = 0;
};


/* Devices here are enumerated by us rather than by the guest, so unlike a PCI
   bus this one both assigns the addresses and describes the children in the
   device tree. */
class MDIOBus final: public Bus {
private:
    RangeAllocator fAddrAlloc {"MDIO"};

public:
    MDIOBus(Device *owner);

    const char *Type() const override {return "mdio";}
    MDIOBus *AsMDIOBus() override {return this;}

    bool AssignResources(Device *dev) override;

    /* nullptr when nothing answers at that address. */
    MDIODevice *DeviceAtAddress(int address);

    /* The nth child, or nullptr when there is no nth child or it is not an
       MDIO device after all. */
    MDIODevice *MDIODeviceAt(int index);
};


/* A generic 10/100/1000 PHY: auto negotiation always succeeds at the highest
   common speed, and the link follows the host carrier. Enough for a driver to
   bind, bring the interface up and read a plausible link state; there is no
   vendor register page because nothing here has vendor behaviour to model. */
class PHYDevice final: public MDIODevice {
private:
    uint32_t fPhyId;
    uint16_t fBmcr;
    uint16_t fAnar;
    uint16_t fGtcr;
    bool fCarrier = false;

public:
    PHYDevice(int address, uint32_t phy_id);

    bool Realize() override {return true;}
    void BuildFDT(FDTContext &ctx) override;

    uint16_t MdioRead(int reg) override;
    void MdioWrite(int reg, uint16_t val) override;

    bool LinkUp() const override {return fCarrier;}
    int Speed() const override;
    bool FullDuplex() const override;
    void SetCarrier(bool carrier) override {fCarrier = carrier;}
};
