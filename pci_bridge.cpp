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
#include "pci_bridge.h"

#include <stdio.h>

#include "machine.h"


//#pragma mark - PCIBusWrapper

bool PCIBusWrapper::AssignResources(Device *dev)
{
    /* A PCI device's BARs are placed by the guest, so nothing here consumes
       host address space. Anything a device does declare is a modelling
       mistake worth reporting rather than silently dropping. */
    for (int i = 0; i < dev->ResourceCount(); i++) {
        Resource *res = dev->ResourceAt(i);
        if (res->type != RES_NONE) {
            vm_error("pci bus: device '%s' declared a resource, but PCI "
                     "resources are assigned by the guest\n", dev->Name());
            return false;
        }
    }
    return true;
}


//#pragma mark - PCIePortDevice

/* One downstream port of a switch, and the bus behind it. These are not
   written in the configuration file: the switch makes one for each device
   nested inside it, because a PCI Express link carries exactly one device and
   a second one put on the same bus would be invisible to the guest. */
class PCIePortDevice final: public Device {
private:
    PCIBusWrapper *fChildBus = nullptr;

public:
    PCIePortDevice(const char *name): Device(name) {}

    ~PCIePortDevice() override {delete fChildBus;}

    bool Prepare() override
    {
        PCIBus *parent = ParentBus()->AsPCIBus();
        PCIBus *sec = pci_bridge_init(parent, -1, Name(),
                                      PCI_BRIDGE_VENDOR_ID,
                                      PCI_BRIDGE_DEVICE_ID,
                                      pci_bus_bridge_port_type(parent),
                                      nullptr);
        if (sec == nullptr) {
            vm_error("%s: could not create the port\n", Name());
            return false;
        }
        fChildBus = new PCIBusWrapper(this, sec);
        return true;
    }

    bool Realize() override {return true;}

    Bus *ChildBus() override {return fChildBus;}
};


//#pragma mark - PCISwitchBus

/* The bus inside a switch. On a PCI Express hierarchy every device added to
   it is given a downstream port of its own; on a conventional bus, where a
   guest scans all thirty two slots, they sit side by side as they would on
   real hardware. */
class PCISwitchBus final: public PCIBusWrapper {
private:
    int fPortCount = 0;

public:
    PCISwitchBus(Device *owner, PCIBus *bus): PCIBusWrapper(owner, bus) {}

    bool AddDevice(Device *dev) override
    {
        char name[64];
        PCIePortDevice *port;

        if (!pci_bus_is_pcie(AsPCIBus())) {
            return Bus::AddDevice(dev);
        }
        if (dev == nullptr) {
            return false;
        }

        snprintf(name, sizeof(name), "%s-port%d", Owner()->Name(),
                 fPortCount++);
        port = new PCIePortDevice(name);
        if (!Bus::AddDevice(port)) {
            delete dev;
            return false;
        }
        return port->ChildBus()->AddDevice(dev);
    }
};


//#pragma mark - pci_attach_bus_create

Bus *pci_attach_bus_create(Device *owner, PCIBus *bus)
{
    char name[64];

    if (pci_bus_is_root(bus) || !pci_bus_is_pcie(bus)) {
        return new PCIBusWrapper(owner, bus);
    }

    if (pci_bus_bridge_port_type(bus) == PCI_EXP_TYPE_UPSTREAM) {
        /* One end of a link, so the single device it can carry is the
           upstream port of a switch and the configuration's devices go on
           the bus inside that. */
        snprintf(name, sizeof(name), "%s-up", owner->Name());
        PCIBus *inner = pci_bridge_init(bus, 0, name, PCI_BRIDGE_VENDOR_ID,
                                        PCI_BRIDGE_DEVICE_ID,
                                        PCI_EXP_TYPE_UPSTREAM, nullptr);
        if (inner == nullptr) {
            vm_error("%s: could not create the upstream port\n",
                     owner->Name());
            return nullptr;
        }
        bus = inner;
    }
    return new PCISwitchBus(owner, bus);
}


//#pragma mark - PCIBridgeDevice

/* The bridge is created in Prepare() rather than Realize(), because the bus
   behind it has to exist before the devices nested inside it are added. */
class PCIBridgeDevice final: public Device {
private:
    Bus *fChildBus = nullptr;

public:
    PCIBridgeDevice(const char *name): Device(name) {}

    ~PCIBridgeDevice() override {delete fChildBus;}

    bool Prepare() override
    {
        PCIBus *parent = ParentBus()->AsPCIBus();
        if (parent == nullptr) {
            vm_error("%s: must be attached to a PCI bus\n", Name());
            return false;
        }

        PCIBus *inner = pci_bridge_init(parent, -1, Name(),
                                        PCI_BRIDGE_VENDOR_ID,
                                        PCI_BRIDGE_DEVICE_ID,
                                        pci_bus_bridge_port_type(parent),
                                        nullptr);
        if (inner == nullptr) {
            vm_error("%s: could not create the bridge\n", Name());
            return false;
        }

        fChildBus = pci_attach_bus_create(this, inner);
        return fChildBus != nullptr;
    }

    bool Realize() override {return true;}

    Bus *ChildBus() override {return fChildBus;}
};


//#pragma mark - factory

Device *pci_bridge_node_create(const char *name)
{
    return new PCIBridgeDevice(name);
}
