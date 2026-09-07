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

#include "machine.h"


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


IRQSignal *SystemBus::IrqSignalFor(uint64_t line)
{
    if (line == 0 || line >= (uint64_t)fIrqCount) {
        return nullptr;
    }
    return &fIrqSignals[line];
}


bool SystemBus::AssignResources(Device *dev)
{
    for (int i = 0; i < dev->ResourceCount(); i++) {
        Resource *res = dev->ResourceAt(i);
        switch (res->type) {
        case RES_MMIO:
            if (!fMmioAlloc.Assign(res, dev->Name())) {
                return false;
            }
            break;
        case RES_IRQ:
            if (!fIrqAlloc.Assign(res, dev->Name())) {
                return false;
            }
            break;
        default:
            break;
        }
    }
    return true;
}
