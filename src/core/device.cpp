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

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "device_class.h"
#include "devices.h"
#include "fdt.h"
#include "machine.h"


//#pragma mark - FDT helpers

/* the lines are level triggered, active high */
#define FDT_IRQ_TYPE_LEVEL_HIGH 4

void fdt_add_irq_spec(const FDTContext &ctx, uint64_t line)
{
    ctx.fdt->AddCellU32(line);
    if (ctx.irq_cells > 1)
        ctx.fdt->AddCellU32(FDT_IRQ_TYPE_LEVEL_HIGH);
}

void fdt_prop_irq(FDTContext &ctx, uint64_t line)
{
    ctx.fdt->AddCellU32(ctx.irq_phandle);
    fdt_add_irq_spec(ctx, line);
    ctx.fdt->PropCells("interrupts-extended");
}


//#pragma mark - Device

Resource *Device::AddResource(ResourceTypeEnum type, uint64_t size,
                              uint64_t align, bool high)
{
    if (fResourceCount >= RESOURCE_MAX_PER_DEVICE) {
        vm_error("%s: too many resources\n", Name());
        return nullptr;
    }
    Resource *res = &fResources[fResourceCount++];
    res->type = type;
    res->name = Name();
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


Bus *Device::ResolveBus(const char *type)
{
    Bus *child = ChildBus();
    if (child == nullptr || strcmp(child->Type(), type) != 0) {
        return nullptr;
    }
    return child;
}


//#pragma mark - Bus

Bus::~Bus()
{
    /* Latest first, so a device goes before the siblings it was added after
       and may refer to. */
    for (int i = fDeviceCount - 1; i >= 0; i--) {
        fDevices[i].reset();
    }
}


bool Bus::AddDevice(std::unique_ptr<Device> dev)
{
    if (dev == nullptr) {
        return false;
    }
    if (fDeviceCount >= BUS_MAX_DEVICES) {
        vm_error("%s bus: too many devices (max %d)\n", Type(),
                 BUS_MAX_DEVICES);
        return false;
    }
    Device *added = dev.get();
    fDevices[fDeviceCount++] = std::move(dev);
    added->SetParentBus(this);
    if (!added->Prepare()) {
        return false;
    }
    return true;
}


bool Bus::Attach(JSONValue devices, DeviceContext *ctx)
{
    for (int i = 0; i < devices.u.array->Length(); i++) {
        JSONValue obj = json_array_get(devices, i);
        const char *type, *id;

        if (obj.type != JSON_OBJ) {
            vm_error("device: object expected\n");
            return false;
        }
        if (vm_get_str(obj, "type", &type) < 0 ||
            vm_get_str_opt(obj, "id", &id) < 0) {
            return false;
        }
        const DeviceClass *cls = DeviceRoster::Default().Find(type);
        if (cls == nullptr) {
            vm_error("unsupported device type: %s\n", type);
            return false;
        }

        DeviceConfig cfg(obj, type, ctx->params->cfg_filename);
        std::unique_ptr<Device> owned(cls->Create(cfg, ctx));
        Device *dev = owned.get();
        if (dev == nullptr || !AddDevice(std::move(owned))) {
            return false;
        }

        JSONValue bus = cfg.Get("bus");
        if (!json_is_undefined(bus)) {
            std::string owner = std::string(dev->Name()) + ": device type '" +
                type + "'";
            if (!bus_config_attach(bus, owner.c_str(),
                                   [dev](const char *bus_type) {
                                       return dev->ResolveBus(bus_type);
                                   }, ctx)) {
                return false;
            }
        }
    }
    return true;
}


bool bus_config_attach(JSONValue bus_obj, const char *owner,
                       const std::function<Bus *(const char *)> &resolve,
                       DeviceContext *ctx)
{
    const char *type;

    if (bus_obj.type != JSON_OBJ) {
        vm_error("bus: object expected\n");
        return false;
    }
    if (vm_get_str(bus_obj, "type", &type) < 0) {
        return false;
    }
    Bus *bus = resolve(type);
    if (bus == nullptr) {
        vm_error("%s has no '%s' bus\n", owner, type);
        return false;
    }
    JSONValue devices = json_object_get(bus_obj, "devices");
    if (json_is_undefined(devices)) {
        return true;
    }
    if (devices.type != JSON_ARRAY) {
        vm_error("%s bus: 'devices' must be an array\n", type);
        return false;
    }
    return bus->Attach(devices, ctx);
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
        Device *dev = fDevices[i].get();
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
        Device *dev = fDevices[i].get();
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
    fPortBased(true),
    fIrqTable(irqs),
    fIrqCount(irq_count)
{
    /* Every line is a real one here, line 0 included: on a PC that is the
       timer. Which of them are spoken for is the machine's to claim. */
    fIrqAlloc.SetWindow(0, fIrqCount);
}


PhysMemoryMap *SystemBus::PortMap()
{
    if (fPortMap == nullptr) {
        fOwnedPortMap = std::make_unique<PhysMemoryMap>();
        fPortMap = fOwnedPortMap.get();
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
