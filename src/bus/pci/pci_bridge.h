/*
 * PCI bus attachment and PCI to PCI bridges
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

#include "device.h"
#include "pci.h"

/* What the bridge reports itself as. Red Hat's identifiers for a PCI to PCI
   bridge, which is what a guest expects to find modelled rather than a part
   with errata of its own. */
#define PCI_BRIDGE_VENDOR_ID 0x1b36
#define PCI_BRIDGE_DEVICE_ID 0x0001


/* Wraps a PCIBus so devices can be attached through the generic Bus
   interface. BAR placement is left to the guest, which is why this bus
   assigns no resources of its own. */
class PCIBusWrapper: public Bus {
private:
    PCIBus *fBus;

public:
    PCIBusWrapper(Device *owner, PCIBus *bus): Bus(owner), fBus(bus) {}

    const char *Type() const override {return "pci";}
    PCIBus *AsPCIBus() override {return fBus;}

    bool AssignResources(Device *dev) override;
};


/* The Bus a configuration's devices should be attached to, given the PCI bus
   a host bridge or a bridge owns. What that takes depends on where the bus
   sits:

     - a root complex's own bus, or a conventional one, carries as many
       devices as it has slots, and they go straight on it;
     - a bus behind a root port or a downstream port is one end of a link and
       so carries exactly one device, so a switch is placed on it and the
       devices go on the bus inside that;
     - a switch's internal bus carries downstream ports, so each device is
       given one of its own.

   Returns null on failure. */
Bus *pci_attach_bus_create(Device *owner, PCIBus *bus);


/* The "pci-bridge" configuration node: a port on the bus above and, behind
   it, whatever pci_attach_bus_create() decides that port needs, so the
   devices nested inside end up where a guest can reach all of them. The guest
   numbers the buses and programs the forwarding windows. Nesting one of these
   inside another is how a deeper hierarchy is described. */
Device *pci_bridge_node_create(const char *name);
