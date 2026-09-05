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

#include "iomem.h"
#include "resource.h"

#define BUS_MAX_DEVICES 32
#define SYSTEM_BUS_MAX_IRQ 32

class Bus;
class Device;
class FDTBuilder;
struct PCIBus;


/* Carried down the tree while the FDT is emitted. Nodes that need to refer to
   an interrupt controller take its phandle from here rather than inventing
   one, and whichever device wants to be the console fills in stdout_path so
   that /chosen can never name a node that was not emitted. */
struct FDTContext {
    FDTBuilder *fdt = nullptr;
    uint32_t intc_phandle = 0; /* per-hart interrupt controller */
    uint32_t plic_phandle = 0;
    char stdout_path[128] = {};
};


class Device {
private:
    char *fName = nullptr;
    Bus *fParentBus = nullptr;
    Resource fResources[RESOURCE_MAX_PER_DEVICE] {};
    int fResourceCount = 0;

public:
    Device(const char *name);
    virtual ~Device();

    const char *Name() const {return fName;}
    Bus *ParentBus() const {return fParentBus;}
    void SetParentBus(Bus *bus) {fParentBus = bus;}

    /* Resource records. Add them from Prepare(); read base back from
       Realize() and BuildFDT() so the mapping and its description come from
       one value. */
    Resource *AddResource(ResourceTypeEnum type, uint64_t size,
                          uint64_t align = 0);
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
    Device *fDevices[BUS_MAX_DEVICES] {};
    int fDeviceCount = 0;
    Device *fOwner; /* the device providing this bus, null for the root */

public:
    Bus(Device *owner): fOwner(owner) {}
    virtual ~Bus();

    virtual const char *Type() const = 0;

    Device *Owner() const {return fOwner;}
    int DeviceCount() const {return fDeviceCount;}
    Device *DeviceAt(int index) {return fDevices[index];}

    /* Takes ownership. Calls Prepare() once the parent link is set. */
    bool AddDevice(Device *dev);

    /* Assign the resource records a child declared. */
    virtual bool AssignResources(Device *dev) = 0;

    /* Non-null only for a PCI bus, so that bus-agnostic devices such as
       virtio can pick their transport. */
    virtual PCIBus *AsPCIBus() {return nullptr;}

    /* Depth-first passes over the whole subtree. */
    bool AllocateAll();
    bool RealizeAll();
    void BuildFDTAll(FDTContext &ctx);
};


/* The root bus of an FDT machine: owns the host MMIO map and the interrupt
   controller's input lines, and hands both out through resource records. */
class SystemBus final: public Bus {
private:
    PhysMemoryMap *fMemMap;
    RangeAllocator fMmioAlloc {"MMIO"};
    RangeAllocator fIrqAlloc {"IRQ"};
    IRQSignal fIrqSignals[SYSTEM_BUS_MAX_IRQ] {};
    int fIrqCount;

public:
    SystemBus(PhysMemoryMap *mem_map, IRQTarget *irq_target, int irq_count);

    const char *Type() const override {return "system";}

    PhysMemoryMap *MemMap() const {return fMemMap;}
    RangeAllocator &MmioAlloc() {return fMmioAlloc;}
    RangeAllocator &IrqAlloc() {return fIrqAlloc;}

    /* Valid for a line returned by an assigned RES_IRQ resource. */
    IRQSignal *IrqSignalFor(uint64_t line);

    bool AssignResources(Device *dev) override;
};
