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
#include "mdio.h"

#include "bits.h"
#include "fdt.h"
#include "machine.h"


//#pragma mark - MDIODevice

bool MDIODevice::Prepare()
{
    if (ParentBus() == nullptr || ParentBus()->AsMDIOBus() == nullptr) {
        vm_error("%s: must be attached to an MDIO bus\n", Name());
        return false;
    }

    if (fWantedAddress >= 0) {
        if (fWantedAddress >= MDIO_ADDRESS_COUNT) {
            vm_error("%s: MDIO address %d is out of range\n", Name(),
                     fWantedAddress);
            return false;
        }
        fAddrRes = AddFixedResource(RES_MDIO_ADDR, fWantedAddress, 1);
    } else {
        fAddrRes = AddResource(RES_MDIO_ADDR, 1, 1);
    }
    return fAddrRes != nullptr;
}


//#pragma mark - MDIOBus

MDIOBus::MDIOBus(Device *owner):
    Bus(owner)
{
    fAddrAlloc.SetWindow(0, MDIO_ADDRESS_COUNT);
}


bool MDIOBus::AssignResources(Device *dev)
{
    for (int i = 0; i < dev->ResourceCount(); i++) {
        Resource *res = dev->ResourceAt(i);
        switch (res->type) {
        case RES_MDIO_ADDR:
            if (!fAddrAlloc.Assign(res, dev->Name())) {
                return false;
            }
            break;
        case RES_MMIO:
        case RES_IRQ:
            /* An MDIO device is reached only through its MAC's register
               pair; it has no window and no line of its own. */
            vm_error("%s: an MDIO device cannot take memory or interrupt "
                     "resources\n", dev->Name());
            return false;
        default:
            break;
        }
    }
    return true;
}


MDIODevice *MDIOBus::DeviceAtAddress(int address)
{
    for (int i = 0; i < DeviceCount(); i++) {
        MDIODevice *dev = MDIODeviceAt(i);
        if (dev != nullptr && dev->Address() == address) {
            return dev;
        }
    }
    return nullptr;
}


/* A device that is not an MDIO device cannot have been prepared on this bus,
   but nothing stops a configuration from naming one, so the cast is checked
   rather than assumed. */
MDIODevice *MDIOBus::MDIODeviceAt(int index)
{
    if (index >= DeviceCount()) {
        return nullptr;
    }
    return dynamic_cast<MDIODevice *>(DeviceAt(index));
}


//#pragma mark - PHYDevice

PHYDevice::PHYDevice(int address, uint32_t phy_id):
    MDIODevice("ethernet-phy", address),
    fPhyId(phy_id),
    fBmcr(MII_BMCR_ANEG_ENABLE | MII_BMCR_SPEED1000 | MII_BMCR_FULLDUPLEX),
    fAnar(0x01e1),  /* 100BASE-TX and 10BASE-T, both duplexes, 802.3 */
    fGtcr(0x0300)   /* 1000BASE-T, both duplexes */
{
}


int PHYDevice::Speed() const
{
    if ((fBmcr & MII_BMCR_ANEG_ENABLE) != 0) {
        /* The link partner is modelled as advertising everything, so the
           result is the fastest thing this PHY advertises. */
        if (get_bits(fGtcr, 8, 2) != 0) {
            return 1000;
        }
        if (get_bits(fAnar, 7, 2) != 0) {
            return 100;
        }
        return 10;
    }
    if ((fBmcr & MII_BMCR_SPEED1000) != 0) {
        return 1000;
    }
    return (fBmcr & MII_BMCR_SPEED100) != 0 ? 100 : 10;
}


bool PHYDevice::FullDuplex() const
{
    if ((fBmcr & MII_BMCR_ANEG_ENABLE) != 0) {
        switch (Speed()) {
        case 1000:
            return get_bit(fGtcr, 9);
        case 100:
            return get_bit(fAnar, 8);
        default:
            return get_bit(fAnar, 6);
        }
    }
    return (fBmcr & MII_BMCR_FULLDUPLEX) != 0;
}


uint16_t PHYDevice::MdioRead(int reg)
{
    switch (reg) {
    case MII_BMCR:
        return fBmcr;
    case MII_BMSR:
        /* Negotiation is instantaneous, so it is complete exactly when there
           is a link to have negotiated over. */
        return MII_BMSR_STATIC |
            (fCarrier ? (MII_BMSR_LINK | MII_BMSR_ANEG_DONE) : 0);
    case MII_PHYID1:
        return fPhyId >> 16;
    case MII_PHYID2:
        return get_bits(fPhyId, 0, 16);
    case MII_ANAR:
        return fAnar;
    case MII_ANLPAR:
        /* The partner advertises everything this PHY can do, and
           acknowledges. */
        return 0x41e1;
    case MII_ANER:
        return 0x0001; /* the link partner is auto negotiation able */
    case MII_GTCR:
        return fGtcr;
    case MII_GTSR:
        /* Both receivers ok, partner capable of 1000BASE-T full and half. */
        return 0x3c00;
    case MII_EXTSR:
        return 0x3000; /* 1000BASE-T full and half */
    default:
        return 0;
    }
}


void PHYDevice::MdioWrite(int reg, uint16_t val)
{
    switch (reg) {
    case MII_BMCR:
        if ((val & MII_BMCR_RESET) != 0) {
            fBmcr = MII_BMCR_ANEG_ENABLE | MII_BMCR_SPEED1000 |
                MII_BMCR_FULLDUPLEX;
            fAnar = 0x01e1;
            fGtcr = 0x0300;
            return;
        }
        /* Both the reset and the restart bits are self clearing, and a
           restart has nothing to do here because negotiation never takes any
           time. */
        fBmcr = val & ~(MII_BMCR_RESET | MII_BMCR_ANEG_RESTART);
        break;
    case MII_ANAR:
        fAnar = val;
        break;
    case MII_GTCR:
        fGtcr = val;
        break;
    default:
        break;
    }
}


void PHYDevice::BuildFDT(FDTContext &ctx)
{
    ctx.fdt->BeginNodeNum("ethernet-phy", Address());
    ctx.fdt->PropU32("reg", Address());
    if (Phandle() != 0) {
        ctx.fdt->PropU32("phandle", Phandle());
    }
    ctx.fdt->EndNode();
}
