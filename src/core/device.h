/*
 * Device and bus objects
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

#include <memory>
#include <string>

#include "iomem.h"
#include "resource.h"

#define BUS_MAX_DEVICES 32
#define SYSTEM_BUS_MAX_IRQ 32

class Bus;
class Device;
class FDTBuilder;
class MDIOBus;
class PCIMsiTarget;
struct PCIBus;


/* Carried down the tree while the FDT is emitted. Nodes that need to refer to
   an interrupt controller take its phandle from here rather than inventing
   one, and whichever device wants to be the console fills in stdout_path so
   that /chosen can never name a node that was not emitted. */
struct FDTContext {
    FDTBuilder *fdt = nullptr;
    /* The controller the wired interrupt lines go to, and the number of cells
       its specifiers take: 1 is the line alone, 2 adds the trigger type. */
    uint32_t irq_phandle = 0;
    int irq_cells = 1;
    /* The controller PCI MSIs are delivered to, or 0 if there is none. */
    uint32_t msi_phandle = 0;
    /* Handed out to PCI host bridges as they emit their nodes, so that a
       machine with more than one names its devices unambiguously. */
    uint32_t pci_domain = 0;
    char stdout_path[128] = {};
};


/* Interrupt specifiers can be up to this many cells. */
#define FDT_IRQ_SPEC_MAX 2

/* Store the specifier for 'line', without the phandle, in 'tab'; returns the
   number of cells written. */
int fdt_irq_spec(const FDTContext &ctx, uint32_t *tab, uint64_t line);

/* Emit the "interrupts-extended" property naming the line a resource was
   assigned. Kept in one place so that every device describes the line it
   actually got. */
void fdt_prop_irq(FDTContext &ctx, uint64_t line);


class Device {
private:
    std::string fName;
    Bus *fParentBus = nullptr;
    Resource fResources[RESOURCE_MAX_PER_DEVICE] {};
    int fResourceCount = 0;

public:
    Device(const char *name): fName(name) {}
    virtual ~Device() = default;

    const char *Name() const {return fName.c_str();}
    Bus *ParentBus() const {return fParentBus;}
    void SetParentBus(Bus *bus) {fParentBus = bus;}

    /* Resource records. Add them from Prepare(); read base back from
       Realize() and BuildFDT() so the mapping and its description come from
       one value. */
    Resource *AddResource(ResourceTypeEnum type, uint64_t size,
                          uint64_t align = 0, bool high = false);
    Resource *AddFixedResource(ResourceTypeEnum type, uint64_t base,
                               uint64_t size);
    int ResourceCount() const {return fResourceCount;}
    Resource *ResourceAt(int index) {return &fResources[index];}
    /* Nth resource of a type, or nullptr. */
    Resource *FindResource(ResourceTypeEnum type, int index = 0);

    /* Runs once the parent bus is known. Declare resources here, and create
       the child bus if this device provides one. */
    virtual bool Prepare() {return true;}

    /* Install the device at its assigned resources. Parents run before
       children. */
    virtual bool Realize() = 0;

    /* Emit this device's node. A device that provides a child bus decides
       for itself whether the children appear in the tree: PCI devices are
       enumerated by the guest and so are deliberately not described. */
    virtual void BuildFDT(FDTContext &ctx) {(void)ctx;}

    virtual Bus *ChildBus() {return nullptr;}
};


class Bus {
private:
    std::unique_ptr<Device> fDevices[BUS_MAX_DEVICES];
    int fDeviceCount = 0;
    Device *fOwner; /* the device providing this bus, null for the root */

public:
    Bus(Device *owner): fOwner(owner) {}
    virtual ~Bus();

    virtual const char *Type() const = 0;

    Device *Owner() const {return fOwner;}
    int DeviceCount() const {return fDeviceCount;}
    Device *DeviceAt(int index) {return fDevices[index].get();}

    /* Calls Prepare() once the parent link is set. A bus that has to
       interpose something between itself and its children (a PCI Express
       switch puts each of them behind a port of its own) overrides this and
       adds the device further down. */
    virtual bool AddDevice(std::unique_ptr<Device> dev);

    /* Assign the resource records a child declared. */
    virtual bool AssignResources(Device *dev) = 0;

    /* Non-null only for a PCI bus, so that bus-agnostic devices such as
       virtio can pick their transport. */
    virtual PCIBus *AsPCIBus() {return nullptr;}

    /* Non-null only for an MDIO bus, so that a PHY can refuse to be attached
       anywhere else. */
    virtual MDIOBus *AsMDIOBus() {return nullptr;}

    /* The bus at the top of the tree, which is the machine's SystemBus. A
       device that needs the machine's port space or interrupt lines while
       sitting on a bus that has neither -- a PCI function in compatibility
       mode -- reaches them from here. */
    Bus *Root();

    /* Depth-first passes over the whole subtree. */
    bool AllocateAll();
    bool RealizeAll();
    void BuildFDTAll(FDTContext &ctx);
};


/* The root bus of a machine: owns the host MMIO map, the port space and the
   interrupt controller's input lines, and hands them all out through resource
   records.

   Which of the two spaces a plain device's registers land in is the one thing
   that differs between machines. An FDT machine maps everything into host
   physical addresses and reaches port space only through a host bridge's
   aperture; a PC decodes its devices by port number and has had the same
   fixed addresses since the AT. RegisterSpace() is how a device that exists
   on both -- a 16550, say -- asks which it is on. */
class SystemBus final: public Bus {
private:
    PhysMemoryMap *fMemMap;
    /* The machine's port space. Created on demand, because a machine with no
       host bridge and no port instructions never addresses one. */
    PhysMemoryMap *fPortMap = nullptr;
    std::unique_ptr<PhysMemoryMap> fOwnedPortMap;
    bool fPortBased = false;
    RangeAllocator fMmioAlloc {"MMIO"};
    RangeAllocator fIoAlloc {"IO"};
    RangeAllocator fIrqAlloc {"IRQ"};
    IRQSignal fIrqSignals[SYSTEM_BUS_MAX_IRQ] {};
    /* fIrqSignals, or the machine's own array when it wired the lines
       itself. */
    IRQSignal *fIrqTable = fIrqSignals;
    int fIrqCount;
    PCIMsiTarget *fMsiTarget = nullptr;

public:
    SystemBus(PhysMemoryMap *mem_map, IRQTarget *irq_target, int irq_count);
    /* A machine whose port space and interrupt lines exist before any device
       does, because its chipset is fixed: a PC. */
    SystemBus(PhysMemoryMap *mem_map, PhysMemoryMap *port_map,
              IRQSignal *irqs, int irq_count);

    const char *Type() const override {return "system";}

    PhysMemoryMap *MemMap() const {return fMemMap;}
    PhysMemoryMap *PortMap();

    /* Where a device that names no space of its own puts its registers. */
    bool IsPortBased() const {return fPortBased;}
    ResourceTypeEnum RegisterSpace() const
        {return fPortBased ? RES_IO : RES_MMIO;}
    PhysMemoryMap *RegisterMap()
        {return fPortBased ? PortMap() : fMemMap;}

    RangeAllocator &MmioAlloc() {return fMmioAlloc;}
    RangeAllocator &IoAlloc() {return fIoAlloc;}
    RangeAllocator &IrqAlloc() {return fIrqAlloc;}

    /* Valid for a line returned by an assigned RES_IRQ resource. */
    IRQSignal *IrqSignalFor(uint64_t line);

    /* The machine's MSI controller, for host bridges without a receiver of
       their own; null when the machine has none. */
    void SetMsiTarget(PCIMsiTarget *target) {fMsiTarget = target;}
    PCIMsiTarget *MsiTarget() const {return fMsiTarget;}

    /* Assign one record out of whichever space its type names. Buses further
       down pass up the records they cannot serve themselves. */
    bool AssignOne(Resource *res, const char *owner);

    bool AssignResources(Device *dev) override;
};
