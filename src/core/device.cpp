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
#include "device.h"

#include <stdlib.h>
#include <string.h>

#include "fdt.h"
#include "machine.h"


//#pragma mark - FDT helpers

/* the lines are level triggered, active high */
#define FDT_IRQ_TYPE_LEVEL_HIGH 4

int fdt_irq_spec(const FDTContext &ctx, uint32_t *tab, uint64_t line)
{
    tab[0] = line;
    if (ctx.irq_cells > 1)
        tab[1] = FDT_IRQ_TYPE_LEVEL_HIGH;
    return ctx.irq_cells;
}

void fdt_prop_irq(FDTContext &ctx, uint64_t line)
{
    uint32_t tab[1 + FDT_IRQ_SPEC_MAX];
    tab[0] = ctx.irq_phandle;
    int n = 1 + fdt_irq_spec(ctx, tab + 1, line);
    ctx.fdt->PropTabU32("interrupts-extended", tab, n);
}


//#pragma mark - Device

Device::Device(const char *name):
    fName(strdup(name))
{
}


Device::~Device()
{
    free(fName);
}


Resource *Device::AddResource(ResourceTypeEnum type, uint64_t size,
                              uint64_t align, bool high)
{
    if (fResourceCount >= RESOURCE_MAX_PER_DEVICE) {
        vm_error("%s: too many resources\n", fName);
        return nullptr;
    }
    Resource *res = &fResources[fResourceCount++];
    res->type = type;
    res->name = fName;
    res->size = size;
    res->align = align;
    res->fixed = false;
    res->high = high;
    res->assigned = false;
    return res;
}


Resource *Device::AddFixedResource(ResourceTypeEnum type, uint64_t base,
                                   uint64_t size)
{
    Resource *res = AddResource(type, size, 0);
    if (res == nullptr) {
        return nullptr;
    }
    res->base = base;
    res->fixed = true;
    return res;
}


Resource *Device::FindResource(ResourceTypeEnum type, int index)
{
    for (int i = 0; i < fResourceCount; i++) {
        if (fResources[i].type == type) {
            if (index == 0) {
                return &fResources[i];
            }
            index--;
        }
    }
    return nullptr;
}


//#pragma mark - Bus

Bus::~Bus()
{
    for (int i = 0; i < fDeviceCount; i++) {
        delete fDevices[i];
    }
}


bool Bus::AddDevice(Device *dev)
{
    if (dev == nullptr) {
        return false;
    }
    if (fDeviceCount >= BUS_MAX_DEVICES) {
        vm_error("%s bus: too many devices (max %d)\n", Type(),
                 BUS_MAX_DEVICES);
        delete dev;
        return false;
    }
    fDevices[fDeviceCount++] = dev;
    dev->SetParentBus(this);
    if (!dev->Prepare()) {
        return false;
    }
    return true;
}


Bus *Bus::Root()
{
    Bus *bus = this;

    while (bus->Owner() != nullptr && bus->Owner()->ParentBus() != nullptr) {
        bus = bus->Owner()->ParentBus();
    }
    return bus;
}


bool Bus::AllocateAll()
{
    for (int i = 0; i < fDeviceCount; i++) {
        Device *dev = fDevices[i];
        if (!AssignResources(dev)) {
            return false;
        }
        Bus *child = dev->ChildBus();
        if (child != nullptr && !child->AllocateAll()) {
            return false;
        }
    }
    return true;
}


bool Bus::RealizeAll()
{
    for (int i = 0; i < fDeviceCount; i++) {
        Device *dev = fDevices[i];
        if (!dev->Realize()) {
            vm_error("failed to realize device '%s'\n", dev->Name());
            return false;
        }
        Bus *child = dev->ChildBus();
        if (child != nullptr && !child->RealizeAll()) {
            return false;
        }
    }
    return true;
}


void Bus::BuildFDTAll(FDTContext &ctx)
{
    for (int i = 0; i < fDeviceCount; i++) {
        fDevices[i]->BuildFDT(ctx);
    }
}


//#pragma mark - SystemBus

SystemBus::SystemBus(PhysMemoryMap *mem_map, IRQTarget *irq_target,
                     int irq_count):
    Bus(nullptr),
    fMemMap(mem_map),
    fIrqCount(irq_count)
{
    if (fIrqCount > SYSTEM_BUS_MAX_IRQ) {
        fIrqCount = SYSTEM_BUS_MAX_IRQ;
    }
    for (int i = 0; i < fIrqCount; i++) {
        fIrqSignals[i].Init(irq_target, i);
    }
    /* IRQ 0 is not a usable PLIC source. */
    fIrqAlloc.SetWindow(1, fIrqCount - 1);
    fIrqAlloc.Claim(0, 1, "reserved");
}


SystemBus::SystemBus(PhysMemoryMap *mem_map, PhysMemoryMap *port_map,
                     IRQSignal *irqs, int irq_count):
    Bus(nullptr),
    fMemMap(mem_map),
    fPortMap(port_map),
    fOwnsPortMap(false),
    fPortBased(true),
    fIrqTable(irqs),
    fIrqCount(irq_count)
{
    /* Every line is a real one here, line 0 included: on a PC that is the
       timer. Which of them are spoken for is the machine's to claim. */
    fIrqAlloc.SetWindow(0, fIrqCount);
}


SystemBus::~SystemBus()
{
    if (fOwnsPortMap) {
        delete fPortMap;
    }
}


PhysMemoryMap *SystemBus::PortMap()
{
    if (fPortMap == nullptr) {
        fPortMap = new PhysMemoryMap();
    }
    return fPortMap;
}


IRQSignal *SystemBus::IrqSignalFor(uint64_t line)
{
    if (line >= (uint64_t)fIrqCount) {
        return nullptr;
    }
    if (line == 0 && !fPortBased) {
        return nullptr;
    }
    return &fIrqTable[line];
}


bool SystemBus::AssignOne(Resource *res, const char *owner)
{
    switch (res->type) {
    case RES_MMIO: return fMmioAlloc.Assign(res, owner);
    case RES_IO:   return fIoAlloc.Assign(res, owner);
    case RES_IRQ:  return fIrqAlloc.Assign(res, owner);
    default:       return true;
    }
}


bool SystemBus::AssignResources(Device *dev)
{
    for (int i = 0; i < dev->ResourceCount(); i++) {
        if (!AssignOne(dev->ResourceAt(i), dev->Name())) {
            return false;
        }
    }
    return true;
}
