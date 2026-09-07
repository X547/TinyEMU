/*
 * Synopsys DesignWare PCI Express host controller
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
#include "pci_host_dw.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "cutils.h"
#include "fdt.h"
#include "machine.h"

/* PCI address space codes for the high cell of a PCI address triplet. */
#define PCI_RANGE_MMIO       0x02000000
#define PCI_RANGE_MMIO_64BIT 0x03000000

/* Port logic registers, all relative to the start of the DBI window. They
   begin here, which is also where the root port's own configuration space
   stops being visible: the two share the window, and the port logic wins. */
#define DW_PORT_LOGIC_BASE  0x700
#define DW_PORT_LINK_CTRL   0x710
#define DW_PORT_DEBUG0      0x728
#define DW_PORT_DEBUG1      0x72c
#define DW_GEN2_CTRL        0x80c
#define DW_MSI_ADDR_LO      0x820
#define DW_MSI_ADDR_HI      0x824
#define DW_MSI_GROUP_BASE   0x828
#define DW_MSI_GROUP_STRIDE 0x0c
#define DW_MISC_CONTROL_1   0x8bc

/* The legacy viewport register, from before the translation unit was given a
   block of its own. It is absent here, and reading all ones is how a driver
   is told so: that is the probe Linux uses to decide between the two
   layouts. */
#define DW_ATU_VIEWPORT     0x900

/* A driver waiting for the link polls PORT_DEBUG1 for this bit. The emulated
   link is always up, and never training. */
#define DW_PORT_DEBUG1_LINK_UP 0x10

/* Translation region registers, relative to the start of the region. */
#define DW_ATU_CTRL1     0x00
#define DW_ATU_CTRL2     0x04
#define DW_ATU_BASE_LO   0x08
#define DW_ATU_BASE_HI   0x0c
#define DW_ATU_LIMIT     0x10
#define DW_ATU_TARGET_LO 0x14
#define DW_ATU_TARGET_HI 0x18

#define DW_ATU_TYPE_MASK 0x1f
#define DW_ATU_TYPE_CFG0 0x04
#define DW_ATU_TYPE_CFG1 0x05
#define DW_ATU_ENABLE    0x80000000

/* The root port identifies itself as the Synopsys IP it models. */
#define DW_ROOT_PORT_VENDOR_ID 0x16c3
#define DW_ROOT_PORT_DEVICE_ID 0xabcd


/* The byte lanes an access of this size at this offset covers, so that a
   partial write can be applied without disturbing the rest of the register.
   Registers that clear on a written one depend on this: merging through a
   read-modify-write would clear whatever happened to be set. */
static uint32_t lane_mask(uint32_t offset, int size_log2)
{
    if (size_log2 >= 2) {
        return 0xffffffff;
    }
    uint32_t bits = (1u << (8 << size_log2)) - 1;
    return bits << ((offset & 3) * 8);
}


static uint32_t size_mask(int size_log2)
{
    if (size_log2 >= 2) {
        return 0xffffffff;
    }
    return (1u << (8 << size_log2)) - 1;
}


//#pragma mark - construction

PCIHostDWDevice::PCIHostDWDevice(const char *name, const char *compatible,
                                 uint64_t mmio_size, uint64_t mmio64_size,
                                 int bus_count):
    Device(name),
    fCompatible(compatible),
    fMmioSize(mmio_size),
    fMmio64Size(mmio64_size),
    fBusCount(bus_count)
{
    /* Bus 0 is the root port and bus 1 is what it forwards to, so there is
       nothing useful to describe below two. */
    if (fBusCount < 2) {
        fBusCount = 2;
    }
    if (fBusCount > 256) {
        fBusCount = 256;
    }
}


PCIHostDWDevice::~PCIHostDWDevice()
{
    delete fChildBus;
}


bool PCIHostDWDevice::Prepare()
{
    SystemBus *sys = static_cast<SystemBus *>(ParentBus());
    if (sys == nullptr || ParentBus()->AsPCIBus() != nullptr) {
        vm_error("%s: must be attached to a system bus\n", Name());
        return false;
    }

    /* The DBI window is aligned to its own size so that the translation unit
       at 0x300000 keeps a tidy address. */
    fDbiRes = AddResource(RES_MMIO, PCIE_DW_DBI_SIZE, PCIE_DW_DBI_SIZE);
    fConfigRes = AddResource(RES_MMIO, PCIE_DW_CONFIG_SIZE,
                             PCIE_DW_CONFIG_SIZE);

    /* Reserved as one block, exactly as the ECAM bridge does: this is the
       range published in "ranges", so checking it against the static map is
       what stops the guest being told to place BARs over RAM. */
    fMmioRes = AddResource(RES_MMIO, fMmioSize, 0x1000000);

    if (fDbiRes == nullptr || fConfigRes == nullptr || fMmioRes == nullptr) {
        return false;
    }

    /* The 64 bit aperture, if one was asked for, comes out of the space above
       4 GB, allocated exactly as the ECAM bridge's is. */
    if (fMmio64Size != 0) {
        fMmio64Res = AddResource(RES_MMIO, fMmio64Size, 0x1000000, true);
        if (fMmio64Res == nullptr) {
            return false;
        }
    }

    /* The device tree lists the message signalled interrupt first, because
       that is the entry a driver reads to find this controller's own
       receiver. */
    fMsiIrqRes = AddResource(RES_IRQ, 1);
    if (fMsiIrqRes == nullptr) {
        return false;
    }
    for (int i = 0; i < 4; i++) {
        fIrqRes[i] = AddResource(RES_IRQ, 1);
        if (fIrqRes[i] == nullptr) {
            return false;
        }
    }

    fRootBus = pci_bus_init(sys->MemMap(), nullptr);
    pci_bus_set_pcie(fRootBus, true);

    /* Devices signal through this controller's receiver rather than by
       writing to memory, so they may advertise MSI-X. The buses behind the
       root port inherit it. */
    pci_bus_set_msi_target(fRootBus, this);

    /* A real root port, so that the type 1 configuration write mask, the
       capability list and the bus routing all come from the same code every
       other bridge uses. */
    fDevBus = pci_bridge_init(fRootBus, 0, "dw-root-port",
                              DW_ROOT_PORT_VENDOR_ID, DW_ROOT_PORT_DEVICE_ID,
                              PCI_EXP_TYPE_ROOT_PORT, &fRootPort);
    if (fDevBus == nullptr) {
        vm_error("%s: could not create the root port\n", Name());
        return false;
    }
    /* Numbered as a driver that never reprograms it would find it. Once one
       does, the routing follows what it wrote. */
    pci_device_set_config8(fRootPort, PCI_PRIMARY_BUS, 0);
    pci_device_set_config8(fRootPort, PCI_SECONDARY_BUS, 1);
    pci_device_set_config8(fRootPort, PCI_SUBORDINATE_BUS, fBusCount - 1);

    /* The bus behind the root port is one end of a link, so a configuration
       naming several devices gets the switch that has to sit between them. */
    fChildBus = pci_attach_bus_create(this, fDevBus);
    return fChildBus != nullptr;
}


bool PCIHostDWDevice::Realize()
{
    SystemBus *sys = static_cast<SystemBus *>(ParentBus());

    fMsiIrq = sys->IrqSignalFor(fMsiIrqRes->base);
    if (fMsiIrq == nullptr) {
        vm_error("%s: bad MSI line %d\n", Name(), (int)fMsiIrqRes->base);
        return false;
    }

    for (int i = 0; i < 4; i++) {
        IRQSignal *sig = sys->IrqSignalFor(fIrqRes[i]->base);
        if (sig == nullptr) {
            vm_error("%s: bad INTx line %d\n", Name(), (int)fIrqRes[i]->base);
            return false;
        }
        /* The four pins of the bus behind the root port go straight to the
           controller's lines rather than through the root port's own. That
           is what the device tree describes: its "interrupt-map" keys on the
           pin alone, and a guest reaches that pin by swizzling from the
           device up to the root bus, so the last tier must not swizzle
           again. */
        pci_bus_set_irq(fDevBus, i, sig);
        /* The root port raises nothing itself, but leaving its pins without
           a target would turn a modelling slip into a null dereference. */
        pci_bus_set_irq(fRootBus, i, sig);
    }

    sys->MemMap()->RegisterDevice(fDbiRes->base, fDbiRes->size, &fDbiIo,
                                  DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32);
    sys->MemMap()->RegisterDevice(fConfigRes->base, fConfigRes->size,
                                  &fConfigIo,
                                  DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32);
    return true;
}


//#pragma mark - message signalled interrupts

void PCIHostDWDevice::MsiUpdate()
{
    bool level = false;

    for (int i = 0; i < PCIE_DW_MSI_GROUP_COUNT; i++) {
        if ((fMsi[i].status & fMsi[i].enable & ~fMsi[i].mask) != 0) {
            level = true;
            break;
        }
    }
    if (level != fMsiLevel) {
        fMsiLevel = level;
        fMsiIrq->Set(level);
    }
}


void PCIHostDWDevice::SendMsi(uint64_t addr, uint32_t data)
{
    uint64_t doorbell = ((uint64_t)fMsiAddrHi << 32) | fMsiAddrLo;

    if (addr != doorbell) {
        /* Not aimed at this receiver, so it is an ordinary posted write and
           is performed as one. */
        SystemBus *sys = static_cast<SystemBus *>(ParentBus());
        uint8_t *ptr = sys->MemMap()->GetRamPtr(addr, true);
        if (ptr != nullptr) {
            put_le32(ptr, data);
        }
        return;
    }

    /* The message payload is the vector number. The bit latches whatever the
       mask says; the mask only decides whether the group drives the line, so
       that a driver can unmask a vector and collect what arrived meanwhile. */
    uint32_t vector = data & (PCIE_DW_MSI_COUNT - 1);
    fMsi[vector >> 5].status |= 1u << (vector & 31);
    MsiUpdate();
}


//#pragma mark - DBI window

PCIeDWAtuRegion *PCIHostDWDevice::AtuAt(uint32_t offset, uint32_t *reg_out)
{
    uint32_t rel = offset - PCIE_DW_ATU_OFFSET;
    uint32_t index = rel >> 9;
    uint32_t inbound = (rel >> 8) & 1;

    if (index >= PCIE_DW_ATU_REGION_COUNT) {
        return nullptr;
    }
    *reg_out = rel & 0xff;
    return inbound ? &fAtuIn[index] : &fAtuOut[index];
}


uint32_t PCIHostDWDevice::DbiRead(uint32_t offset, int size_log2)
{
    /* The root complex's own configuration space is the bottom of DBI, all
       4 KB of it save for the tail the port logic registers take over. */
    if (offset < DW_PORT_LOGIC_BASE) {
        return pci_bus_config_read(fRootBus, offset, size_log2);
    }

    uint32_t reg = offset & ~3u;
    uint32_t val = 0;

    if (reg >= PCIE_DW_ATU_OFFSET) {
        uint32_t sub;
        PCIeDWAtuRegion *atu = AtuAt(reg, &sub);
        if (atu != nullptr) {
            switch (sub) {
            case DW_ATU_CTRL1: val = atu->ctrl1; break;
            case DW_ATU_CTRL2: val = atu->ctrl2; break;
            case DW_ATU_BASE_LO: val = atu->base_lo; break;
            case DW_ATU_BASE_HI: val = atu->base_hi; break;
            case DW_ATU_LIMIT: val = atu->limit; break;
            case DW_ATU_TARGET_LO: val = atu->target_lo; break;
            case DW_ATU_TARGET_HI: val = atu->target_hi; break;
            }
        }
    } else if (reg >= DW_MSI_GROUP_BASE &&
               reg < DW_MSI_GROUP_BASE +
                     PCIE_DW_MSI_GROUP_COUNT * DW_MSI_GROUP_STRIDE) {
        uint32_t rel = reg - DW_MSI_GROUP_BASE;
        PCIeDWMsiGroup *grp = &fMsi[rel / DW_MSI_GROUP_STRIDE];
        switch (rel % DW_MSI_GROUP_STRIDE) {
        case 0: val = grp->enable; break;
        case 4: val = grp->mask; break;
        case 8: val = grp->status; break;
        }
    } else {
        switch (reg) {
        case DW_PORT_LINK_CTRL: val = fPortLinkCtrl; break;
        case DW_PORT_DEBUG1: val = DW_PORT_DEBUG1_LINK_UP; break;
        case DW_ATU_VIEWPORT: val = 0xffffffff; break;
        case DW_GEN2_CTRL: val = fGen2Ctrl; break;
        case DW_MSI_ADDR_LO: val = fMsiAddrLo; break;
        case DW_MSI_ADDR_HI: val = fMsiAddrHi; break;
        case DW_MISC_CONTROL_1: val = fMiscControl1; break;
        }
    }

    return (val >> ((offset & 3) * 8)) & size_mask(size_log2);
}


void PCIHostDWDevice::DbiWrite(uint32_t offset, uint32_t val, int size_log2)
{
    if (offset < DW_PORT_LOGIC_BASE) {
        pci_bus_config_write(fRootBus, offset, val, size_log2);
        return;
    }

    uint32_t reg = offset & ~3u;
    uint32_t mask = lane_mask(offset, size_log2);
    uint32_t data = val << ((offset & 3) * 8);

    if (reg >= PCIE_DW_ATU_OFFSET) {
        uint32_t sub;
        PCIeDWAtuRegion *atu = AtuAt(reg, &sub);
        if (atu == nullptr) {
            return;
        }
        uint32_t *slot = nullptr;
        switch (sub) {
        case DW_ATU_CTRL1: slot = &atu->ctrl1; break;
        case DW_ATU_CTRL2: slot = &atu->ctrl2; break;
        case DW_ATU_BASE_LO: slot = &atu->base_lo; break;
        case DW_ATU_BASE_HI: slot = &atu->base_hi; break;
        case DW_ATU_LIMIT: slot = &atu->limit; break;
        case DW_ATU_TARGET_LO: slot = &atu->target_lo; break;
        case DW_ATU_TARGET_HI: slot = &atu->target_hi; break;
        }
        /* Stored verbatim, the enable bit included: a driver that spins
           waiting for it to read back gets out on its first read. */
        if (slot != nullptr) {
            *slot = (*slot & ~mask) | (data & mask);
        }
        return;
    }

    if (reg >= DW_MSI_GROUP_BASE &&
        reg < DW_MSI_GROUP_BASE +
              PCIE_DW_MSI_GROUP_COUNT * DW_MSI_GROUP_STRIDE) {
        uint32_t rel = reg - DW_MSI_GROUP_BASE;
        PCIeDWMsiGroup *grp = &fMsi[rel / DW_MSI_GROUP_STRIDE];
        switch (rel % DW_MSI_GROUP_STRIDE) {
        case 0:
            grp->enable = (grp->enable & ~mask) | (data & mask);
            break;
        case 4:
            grp->mask = (grp->mask & ~mask) | (data & mask);
            break;
        case 8:
            /* Written ones clear. */
            grp->status &= ~(data & mask);
            break;
        }
        MsiUpdate();
        return;
    }

    switch (reg) {
    case DW_PORT_LINK_CTRL:
        fPortLinkCtrl = (fPortLinkCtrl & ~mask) | (data & mask);
        break;
    case DW_GEN2_CTRL:
        fGen2Ctrl = (fGen2Ctrl & ~mask) | (data & mask);
        break;
    case DW_MSI_ADDR_LO:
        fMsiAddrLo = (fMsiAddrLo & ~mask) | (data & mask);
        break;
    case DW_MSI_ADDR_HI:
        fMsiAddrHi = (fMsiAddrHi & ~mask) | (data & mask);
        break;
    case DW_MISC_CONTROL_1:
        /* Accepted and read back, but not acted on: the read only registers
           it would unlock are not write protected here in the first place. */
        fMiscControl1 = (fMiscControl1 & ~mask) | (data & mask);
        break;
    }
}


//#pragma mark - configuration window

/* Translate an offset in the configuration window into the PCI address the
   outbound translation unit currently points it at. Only regions carrying a
   configuration TLP type are considered: memory regions, and every inbound
   region, are stored so they read back but are not applied, because the
   aperture and DMA are both identity mapped. */
bool PCIHostDWDevice::ConfigTarget(uint32_t offset, uint32_t *addr_out)
{
    uint64_t cpu_addr = fConfigRes->base + offset;

    for (int i = 0; i < PCIE_DW_ATU_REGION_COUNT; i++) {
        PCIeDWAtuRegion *atu = &fAtuOut[i];
        if ((atu->ctrl2 & DW_ATU_ENABLE) == 0) {
            continue;
        }
        uint32_t type = atu->ctrl1 & DW_ATU_TYPE_MASK;
        if (type != DW_ATU_TYPE_CFG0 && type != DW_ATU_TYPE_CFG1) {
            continue;
        }

        uint64_t base = ((uint64_t)atu->base_hi << 32) | atu->base_lo;
        /* Drivers commonly leave the high half of the limit alone, so it is
           taken from the base. Every window this machine hands out lives
           below 4 GB, so the two always share a high half anyway. */
        uint64_t limit = (base & ~(uint64_t)0xffffffff) | atu->limit;
        if (cpu_addr < base || cpu_addr > limit) {
            continue;
        }

        uint64_t target = ((uint64_t)atu->target_hi << 32) | atu->target_lo;
        *addr_out = (uint32_t)(target + (cpu_addr - base));
        return true;
    }
    return false;
}


/* Split a translated configuration address into a bus address this machine's
   PCI bus understands, or report that nothing answers there. */
bool PCIHostDWDevice::ConfigDecode(uint32_t offset, uint32_t *bus_addr_out)
{
    uint32_t addr;
    if (!ConfigTarget(offset, &addr)) {
        return false;
    }

    uint32_t bus = (addr >> 24) & 0xff;
    uint32_t devfn = (addr >> 16) & 0xff;
    uint32_t reg = addr & 0xfff;

    /* This window reaches what is behind the root port. The root port itself
       is the bottom of the DBI window instead, so letting a cycle for bus 0
       through here would show it to a driver twice. Everything below is left
       to the bus, which follows the numbers the guest programmed into the
       bridges it found. */
    if (bus == (uint32_t)pci_bus_get_bus_num(fRootBus)) {
        return false;
    }

    *bus_addr_out = PCI_CONFIG_ADDR(bus, devfn, reg);
    return true;
}


uint32_t PCIHostDWDevice::ConfigRead(uint32_t offset, int size_log2)
{
    uint32_t bus_addr;
    if (!ConfigDecode(offset, &bus_addr)) {
        return size_mask(size_log2);
    }
    return pci_bus_config_read(fDevBus, bus_addr, size_log2);
}


void PCIHostDWDevice::ConfigWrite(uint32_t offset, uint32_t val, int size_log2)
{
    uint32_t bus_addr;
    if (!ConfigDecode(offset, &bus_addr)) {
        return;
    }
    pci_bus_config_write(fDevBus, bus_addr, val, size_log2);
}


//#pragma mark - device tree

void PCIHostDWDevice::BuildFDT(FDTContext &ctx)
{
    FDTBuilder *fdt = ctx.fdt;
    uint32_t tab[32];
    int n;

    fdt->BeginNodeNum("pcie", fDbiRes->base);
    /* The first entry is what a driver matching on a single string sees, so
       the configured name leads and the generic one follows it. */
    if (strcmp(fCompatible, "snps,dw-pcie") == 0) {
        fdt->PropStrList("compatible", fCompatible, nullptr);
    } else {
        fdt->PropStrList("compatible", fCompatible, "snps,dw-pcie", nullptr);
    }
    fdt->PropStr("device_type", "pci");
    fdt->PropU32("#address-cells", 3);
    fdt->PropU32("#size-cells", 2);
    fdt->PropU32("#interrupt-cells", 1);

    /* The order is load bearing: drivers read these by index, not by name. */
    n = 0;
    tab[n++] = fDbiRes->base >> 32;
    tab[n++] = fDbiRes->base;
    tab[n++] = fDbiRes->size >> 32;
    tab[n++] = fDbiRes->size;
    tab[n++] = fConfigRes->base >> 32;
    tab[n++] = fConfigRes->base;
    tab[n++] = fConfigRes->size >> 32;
    tab[n++] = fConfigRes->size;
    fdt->PropTabU32("reg", tab, n);
    fdt->PropStrList("reg-names", "dbi", "config", nullptr);

    /* Each host bridge is a segment of its own, so that a machine with more
       than one of them names its devices unambiguously. */
    fdt->PropU32("linux,pci-domain", ctx.pci_domain++);

    /* Bus 0 carries the root port and the rest are behind it. */
    n = 0;
    tab[n++] = 0;
    tab[n++] = fBusCount - 1;
    fdt->PropTabU32("bus-range", tab, n);

    fdt->PropU32("num-lanes", 1);
    fdt->PropEmpty("dma-coherent");

    /* One non-prefetchable 32 bit memory window, identity mapped, and a 64
       bit one after it when the configuration asked for one. */
    n = 0;
    tab[n++] = PCI_RANGE_MMIO;       /* child phys.hi */
    tab[n++] = fMmioRes->base >> 32; /* child phys.mid */
    tab[n++] = fMmioRes->base;       /* child phys.lo */
    tab[n++] = fMmioRes->base >> 32; /* parent address */
    tab[n++] = fMmioRes->base;
    tab[n++] = fMmioRes->size >> 32; /* size */
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

    /* The message signalled interrupt comes first: that is the entry a driver
       reads to find this controller's own receiver. */
    fdt->PropU32("interrupt-parent", ctx.plic_phandle);
    n = 0;
    tab[n++] = fMsiIrqRes->base;
    for (int i = 0; i < 4; i++) {
        tab[n++] = fIrqRes[i]->base;
    }
    fdt->PropTabU32("interrupts", tab, n);
    fdt->PropStrList("interrupt-names", "msi", "inta", "intb", "intc", "intd",
                     nullptr);

    /* Only the pin selects an entry. The per slot swizzle is applied by
       pci_bus_map_irq() before the interrupt ever reaches one of the four
       pins, so the table below describes the pins themselves. */
    n = 0;
    tab[n++] = 0;
    tab[n++] = 0;
    tab[n++] = 0;
    tab[n++] = 7;
    fdt->PropTabU32("interrupt-map-mask", tab, n);

    n = 0;
    for (int pin = 1; pin <= 4; pin++) {
        tab[n++] = 0; /* child unit address, phys.hi */
        tab[n++] = 0;
        tab[n++] = 0;
        tab[n++] = pin; /* child interrupt specifier */
        tab[n++] = ctx.plic_phandle;
        tab[n++] = fIrqRes[pin - 1]->base;
    }
    assert(n <= (int)countof(tab));
    fdt->PropTabU32("interrupt-map", tab, n);

    /* Deliberately no "msi-parent": a driver takes the absence of one as the
       cue to use the receiver built into this controller, which is the one
       modelled here. */

    fdt->EndNode();
}
