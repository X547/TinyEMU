/*
 * Generic ECAM PCI Express host controller
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

/* One ECAM function window is 4 KB, so one bus is 256 * 4 KB = 1 MB. */
#define PCIE_ECAM_BUS_SHIFT 20
#define PCIE_ECAM_BUS_SIZE (1 << PCIE_ECAM_BUS_SHIFT)

/* devfn is allocated one slot at a time (function 0 only), so 32 slots
   covers every device the bus can hold. */
#define PCIE_ECAM_SLOT_COUNT 32

#define PCIE_ECAM_DEFAULT_BUS_COUNT 16
#define PCIE_ECAM_DEFAULT_MMIO_SIZE 0x10000000 /* 256 MB */


/* Wraps a PCIBus so devices can be attached through the generic Bus
   interface. BAR placement is left to the guest, which is why this bus
   assigns no resources of its own. */
class PCIBusWrapper final: public Bus {
private:
    PCIBus *fBus;

public:
    PCIBusWrapper(Device *owner, PCIBus *bus): Bus(owner), fBus(bus) {}

    const char *Type() const override {return "pci";}
    PCIBus *AsPCIBus() override {return fBus;}

    bool AssignResources(Device *dev) override;
};


/* An "pci-host-ecam-generic" bridge: an ECAM configuration window plus a
   memory aperture, both taken from the parent bus's MMIO space so that they
   are checked against every other static mapping. The FDT "interrupt-map" is
   generated from pci_bus_map_irq(), the same function that routes INTx at run
   time, so the table and the emulation cannot disagree. */
class PCIHostECAMDevice final: public Device {
private:
    PCIBus *fPciBus = nullptr;
    PCIBusWrapper *fChildBus = nullptr;
    Resource *fEcamRes = nullptr;
    Resource *fMmioRes = nullptr;
    Resource *fIrqRes[4] {};
    int fBusCount;
    uint64_t fMmioSize;

    uint32_t EcamRead(uint32_t offset, int size_log2);
    void EcamWrite(uint32_t offset, uint32_t val, int size_log2);

public:
    PCIHostECAMDevice(const char *name, int bus_count, uint64_t mmio_size);
    ~PCIHostECAMDevice() override;

    bool Prepare() override;
    bool Realize() override;
    void BuildFDT(FDTContext &ctx) override;
    Bus *ChildBus() override {return fChildBus;}

    DeviceIOAdapter<PCIHostECAMDevice, &PCIHostECAMDevice::EcamRead,
                    &PCIHostECAMDevice::EcamWrite> fEcamIo {*this};
};
