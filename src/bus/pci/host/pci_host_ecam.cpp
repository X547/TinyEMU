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
#include "pci_host_ecam.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "cutils.h"
#include "fdt.h"
#include "machine.h"

/* PCI address space codes for the high cell of a PCI address triplet. */
#define PCI_RANGE_CONFIG      0x00000000
#define PCI_RANGE_IO          0x01000000
#define PCI_RANGE_MMIO        0x02000000
#define PCI_RANGE_MMIO_64BIT  0x03000000


//#pragma mark - PCIHostECAMDevice

PCIHostECAMDevice::PCIHostECAMDevice(const char *name, int bus_count,
                                     uint64_t mmio_size, uint64_t mmio64_size,
                                     uint64_t io_size):
    Device(name),
    fBusCount(bus_count),
    fMmioSize(mmio_size),
    fMmio64Size(mmio64_size),
    fIoSize(io_size)
{
    if (fBusCount < 1) {
        fBusCount = 1;
    }
    if (fBusCount > 256) {
        fBusCount = 256;
    }
}


PCIHostECAMDevice::~PCIHostECAMDevice()
{
    delete fChildBus;
}


bool PCIHostECAMDevice::Prepare()
{
    SystemBus *sys = static_cast<SystemBus *>(ParentBus());
    if (sys == nullptr || ParentBus()->AsPCIBus() != nullptr) {
        vm_error("%s: must be attached to a system bus\n", Name());
        return false;
    }

    /* The ECAM window must be aligned to its own size so that the bus number
       lands on bit 20 and up. */
    uint64_t ecam_size = (uint64_t)fBusCount << PCIE_ECAM_BUS_SHIFT;
    fEcamRes = AddResource(RES_MMIO, ecam_size, ecam_size);

    /* The aperture is reserved as one block: this is the range published in
       the FDT "ranges" property, so checking it against the static map is
       exactly what keeps the guest from being told to place BARs on top of
       RAM or another device. */
    fMmioRes = AddResource(RES_MMIO, fMmioSize, 0x1000000);

    if (fEcamRes == nullptr || fMmioRes == nullptr) {
        return false;
    }

    /* The 64 bit aperture, if one was asked for, comes out of the space above
       4 GB. It is allocated like everything else, so several host bridges may
       each have one. */
    if (fMmio64Size != 0) {
        fMmio64Res = AddResource(RES_MMIO, fMmio64Size, 0x1000000, true);
        if (fMmio64Res == nullptr) {
            return false;
        }
    }

    /* The I/O aperture is two reservations: the ports themselves, out of the
       machine's port space, and the memory window they are reached through,
       out of the same MMIO space everything else comes from. A processor with
       no port instructions can only get at a port through that window. */
    if (fIoSize != 0) {
        fIoRes = AddResource(RES_IO, fIoSize, fIoSize);
        fIoWindowRes = AddResource(RES_MMIO, fIoSize, fIoSize);
        if (fIoRes == nullptr || fIoWindowRes == nullptr) {
            return false;
        }
    }

    /* One PLIC line per INTx pin. */
    for (int i = 0; i < 4; i++) {
        fIrqRes[i] = AddResource(RES_IRQ, 1);
        if (fIrqRes[i] == nullptr) {
            return false;
        }
    }

    /* Devices may register I/O BARs only on a bridge that has somewhere to
       put them, so a bridge configured without an aperture keeps the port
       space it never reserved out of reach. */
    fPciBus = pci_bus_init(sys->MemMap(),
                           fIoRes != nullptr ? sys->PortMap() : nullptr);
    pci_bus_set_pcie(fPciBus, true);
    /* The bridge has no MSI receiver, but the machine may have an MSI
       controller the messages can be written to directly. */
    if (sys->MsiTarget() != nullptr) {
        pci_bus_set_msi_target(fPciBus, sys->MsiTarget());
    }
    fChildBus = pci_attach_bus_create(this, fPciBus);
    return fChildBus != nullptr;
}


bool PCIHostECAMDevice::Realize()
{
    SystemBus *sys = static_cast<SystemBus *>(ParentBus());

    for (int i = 0; i < 4; i++) {
        IRQSignal *sig = sys->IrqSignalFor(fIrqRes[i]->base);
        if (sig == nullptr) {
            vm_error("%s: bad INTx line %d\n", Name(), (int)fIrqRes[i]->base);
            return false;
        }
        pci_bus_set_irq(fPciBus, i, sig);
    }

    sys->MemMap()->RegisterDevice(fEcamRes->base, fEcamRes->size, &fEcamIo,
                                  DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32);

    if (fIoRes != nullptr) {
        fIoWindow.Init(sys->PortMap(), fIoRes->base, fIoRes->size);
        sys->MemMap()->RegisterDevice(fIoWindowRes->base, fIoWindowRes->size,
                                      &fIoWindow,
                                      DEVIO_SIZE8 | DEVIO_SIZE16 |
                                      DEVIO_SIZE32);
    }
    return true;
}


/* An ECAM offset is already exactly the address the bus takes: the bus number
   at bit 20, then devfn, then twelve bits of register. */
uint32_t PCIHostECAMDevice::EcamRead(uint32_t offset, int size_log2)
{
    return pci_bus_config_read(fPciBus, offset, size_log2);
}


void PCIHostECAMDevice::EcamWrite(uint32_t offset, uint32_t val, int size_log2)
{
    pci_bus_config_write(fPciBus, offset, val, size_log2);
}


void PCIHostECAMDevice::BuildFDT(FDTContext &ctx)
{
    FDTBuilder *fdt = ctx.fdt;
    /* an interrupt-map entry: unit address, pin, phandle and specifier */
    uint32_t tab[PCIE_ECAM_SLOT_COUNT * 4 * (5 + FDT_IRQ_SPEC_MAX)];
    int n;

    fdt->BeginNodeNum("pci", fEcamRes->base);
    fdt->PropStr("compatible", "pci-host-ecam-generic");
    fdt->PropStr("device_type", "pci");
    fdt->PropU32("#address-cells", 3);
    fdt->PropU32("#size-cells", 2);
    fdt->PropU32("#interrupt-cells", 1);
    fdt->PropU64Range("reg", fEcamRes->base, fEcamRes->size);

    /* Each host bridge is a segment of its own, so that a machine with more
       than one of them names its devices unambiguously. */
    fdt->PropU32("linux,pci-domain", ctx.pci_domain++);

    tab[0] = 0;
    tab[1] = fBusCount - 1;
    fdt->PropTabU32("bus-range", tab, 2);

    /* An I/O window first, when the configuration asked for one. This is the
       only range that is not identity mapped: the child address is the port
       number and the parent address is the memory window a processor with no
       port instructions reaches it through.

       One non-prefetchable 32 bit memory window follows, identity mapped: the
       PCI side address equals the CPU side address. A second, 64 bit window
       comes after that when the configuration asked for one; that is where a
       guest can put a 64 bit BAR that does not have to live below 4 GB. */
    n = 0;
    if (fIoRes != nullptr) {
        tab[n++] = PCI_RANGE_IO;
        tab[n++] = fIoRes->base >> 32;
        tab[n++] = fIoRes->base;
        tab[n++] = fIoWindowRes->base >> 32;
        tab[n++] = fIoWindowRes->base;
        tab[n++] = fIoRes->size >> 32;
        tab[n++] = fIoRes->size;
    }
    tab[n++] = PCI_RANGE_MMIO;          /* child phys.hi */
    tab[n++] = fMmioRes->base >> 32;    /* child phys.mid */
    tab[n++] = fMmioRes->base;          /* child phys.lo */
    tab[n++] = fMmioRes->base >> 32;    /* parent address */
    tab[n++] = fMmioRes->base;
    tab[n++] = fMmioRes->size >> 32;    /* size */
    tab[n++] = fMmioRes->size;
    if (fMmio64Res != nullptr) {
        tab[n++] = PCI_RANGE_MMIO_64BIT;
        tab[n++] = fMmio64Res->base >> 32;
        tab[n++] = fMmio64Res->base;
        tab[n++] = fMmio64Res->base >> 32;
        tab[n++] = fMmio64Res->base;
        tab[n++] = fMmio64Res->size >> 32;
        tab[n++] = fMmio64Res->size;
    }
    fdt->PropTabU32("ranges", tab, n);

    /* Only the device number and the pin select an entry. */
    n = 0;
    tab[n++] = 0xf800;
    tab[n++] = 0;
    tab[n++] = 0;
    tab[n++] = 7;
    fdt->PropTabU32("interrupt-map-mask", tab, n);

    /* Derive the table from the routing function itself: whatever swizzle
       pci_bus_map_irq() implements is what the guest is told. */
    n = 0;
    for (int slot = 0; slot < PCIE_ECAM_SLOT_COUNT; slot++) {
        for (int pin = 1; pin <= 4; pin++) {
            int intx = pci_bus_map_irq(slot << 3, pin - 1);
            tab[n++] = slot << 11;  /* child unit address, phys.hi */
            tab[n++] = 0;
            tab[n++] = 0;
            tab[n++] = pin;         /* child interrupt specifier */
            tab[n++] = ctx.irq_phandle;
            n += fdt_irq_spec(ctx, tab + n, fIrqRes[intx]->base);
        }
    }
    assert(n <= (int)countof(tab));
    fdt->PropTabU32("interrupt-map", tab, n);

    if (ctx.msi_phandle != 0) {
        fdt->PropU32("msi-parent", ctx.msi_phandle);
    }

    fdt->EndNode();
}
