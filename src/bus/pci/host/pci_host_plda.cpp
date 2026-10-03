/*
 * PLDA XpressRich PCI Express host controller (StarFive JH7110)
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
#include "pci_host_plda.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "bits.h"
#include "cutils.h"
#include "device_class.h"
#include "fdt.h"
#include "machine.h"
#include "pci_host_ecam.h"

/* PCI address space codes for the high cell of a PCI address triplet. The
   64 bit window is prefetchable: everything here sits behind the root port,
   and a bridge forwards memory above 4 GB only through that kind of window. */
#define PCI_RANGE_MMIO            0x02000000
#define PCI_RANGE_MMIO_64BIT_PREF 0x43000000

/* The root port identifies itself as the PLDA reference design. */
#define PLDA_ROOT_PORT_VENDOR_ID 0x1556
#define PLDA_ROOT_PORT_DEVICE_ID 0x1111

/* Bridge registers. */
#define PLDA_PCIE_PCI_IDS_DW1   0x09c /* class code and revision */
#define PLDA_IMASK_LOCAL        0x180
#define PLDA_ISTATUS_LOCAL      0x184
#define PLDA_IMSI_ADDR          0x190
#define PLDA_ISTATUS_MSI        0x194
#define PLDA_PMSG_SUPPORT_RX    0x3f0

#define PLDA_PMSG_LTR_SUPPORT   bit_at(2)

/* The local interrupt status: the four INTx pins, and one bit summing up the
   message signalled interrupts. The other events it can report -- address
   translation errors, doorbells, AER -- never happen here. */
#define PLDA_INT_INTX_SHIFT     24
#define PLDA_INT_MSI            bit_at(28)

#define PLDA_MSI_COUNT          32

/* System registers. The driver finds the ones for its controller at one of
   two bases, chosen by its PCI domain number. */
#define STG_SYSCON_PCIE0_BASE   0x048
#define STG_SYSCON_PCIE1_BASE   0x1f8
#define STG_SYSCON_LNKSTA       0x170
#define STG_SYSCON_DATA_LINK_ACTIVE bit_at(5)

/* What the resets are called in the binding, in the order it lists them. */
static const char *const kResetNames[] = {
    "mst0", "slv0", "slv", "brg", "core", "apb",
};


//#pragma mark - construction

PCIHostPLDADevice::PCIHostPLDADevice(const char *name, uint64_t mmio_size,
                                     uint64_t mmio64_size, int bus_count):
    Device(name),
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

    /* The root port is a PCI to PCI bridge from reset, and this is where it
       says so; the driver rewrites the class code and keeps the revision. */
    fRegs[PLDA_PCIE_PCI_IDS_DW1 / 4] = (uint32_t)PCI_CLASS_BRIDGE_PCI << 16;
    /* Latency tolerance messages are accepted by default. */
    fRegs[PLDA_PMSG_SUPPORT_RX / 4] = PLDA_PMSG_LTR_SUPPORT;
}


bool PCIHostPLDADevice::Prepare()
{
    SystemBus *sys = static_cast<SystemBus *>(ParentBus());
    if (sys == nullptr || ParentBus()->AsPCIBus() != nullptr) {
        vm_error("%s: must be attached to a system bus\n", Name());
        return false;
    }

    /* The ECAM window is aligned to its own size so that the bus number
       lands on bit 20 and up. */
    uint64_t cfg_size = (uint64_t)fBusCount << PCIE_ECAM_BUS_SHIFT;
    fCfgRes = AddResource(RES_MMIO, cfg_size, cfg_size);
    fApbRes = AddResource(RES_MMIO, PCIE_PLDA_APB_SIZE, PCIE_PLDA_APB_SIZE);
    fSysconRes = AddResource(RES_MMIO, PCIE_PLDA_SYSCON_SIZE,
                             PCIE_PLDA_SYSCON_SIZE);
    fResetRes = AddResource(RES_MMIO, PCIE_PLDA_RESET_SIZE,
                            PCIE_PLDA_RESET_SIZE);

    /* Reserved as one block, as with the other host bridges: this is the
       range published in "ranges". */
    fMmioRes = AddResource(RES_MMIO, fMmioSize, 0x1000000);
    fIrqRes = AddResource(RES_IRQ, 1);

    if (fCfgRes == nullptr || fApbRes == nullptr || fSysconRes == nullptr ||
        fResetRes == nullptr || fMmioRes == nullptr || fIrqRes == nullptr) {
        return false;
    }

    if (fMmio64Size != 0) {
        fMmio64Res = AddResource(RES_MMIO, fMmio64Size, 0x1000000, true);
        if (fMmio64Res == nullptr) {
            return false;
        }
    }

    fRootBus = pci_bus_init(sys->MemMap(), nullptr);
    pci_bus_set_pcie(fRootBus.get(), true);

    /* Messages are collected by the bridge itself, so devices below may
       advertise MSI-X. The buses behind the root port inherit it. */
    pci_bus_set_msi_target(fRootBus.get(), this);

    /* A real root port, so that the type 1 configuration write mask, the
       capability list and the bus routing all come from the same code every
       other bridge uses. Its bus numbers are the guest's to program. */
    fDevBus = pci_bridge_init(fRootBus.get(), 0, "plda-root-port",
                              PLDA_ROOT_PORT_VENDOR_ID,
                              PLDA_ROOT_PORT_DEVICE_ID,
                              PCI_EXP_TYPE_ROOT_PORT, nullptr);
    if (fDevBus == nullptr) {
        vm_error("%s: could not create the root port\n", Name());
        return false;
    }

    fChildBus = pci_attach_bus_create(this, fDevBus);
    return fChildBus != nullptr;
}


bool PCIHostPLDADevice::Realize()
{
    SystemBus *sys = static_cast<SystemBus *>(ParentBus());

    fIrq = sys->IrqSignalFor(fIrqRes->base);
    if (fIrq == nullptr) {
        vm_error("%s: bad interrupt line %d\n", Name(), (int)fIrqRes->base);
        return false;
    }

    for (int i = 0; i < 4; i++) {
        fIntx[i].Init(this, i);
        /* The pins of the bus behind the root port land on the bridge's
           status bits directly, because the device tree's "interrupt-map"
           keys on the pin alone; see the same note in the DesignWare host. */
        pci_bus_set_irq(fDevBus, i, &fIntx[i]);
        pci_bus_set_irq(fRootBus.get(), i, &fIntx[i]);
    }

    PhysMemoryMap *map = sys->MemMap();
    map->RegisterDevice(fCfgRes->base, fCfgRes->size, &fCfgIo,
                        DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32);
    map->RegisterDevice(fApbRes->base, fApbRes->size, &fApbIo,
                        DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32);
    map->RegisterDevice(fSysconRes->base, fSysconRes->size, &fSysconIo,
                        DEVIO_SIZE32);
    map->RegisterDevice(fResetRes->base, fResetRes->size, &fResetIo,
                        DEVIO_SIZE32);
    return true;
}


//#pragma mark - interrupts

uint32_t PCIHostPLDADevice::LocalStatus() const
{
    /* Both kinds are level: a pin reads as set for as long as a device
       drives it, and the summary bit for as long as any vector is pending.
       Neither is cleared by writing it back, which is harmless to a driver
       that does, and means nothing is lost to one that clears the source
       first. */
    uint32_t status = fIntxLevel << PLDA_INT_INTX_SHIFT;
    if (fMsiStatus != 0) {
        status |= PLDA_INT_MSI;
    }
    return status;
}


void PCIHostPLDADevice::IrqUpdate()
{
    bool level = (LocalStatus() & fRegs[PLDA_IMASK_LOCAL / 4]) != 0;
    if (level != fLevel) {
        fLevel = level;
        fIrq->Set(level);
    }
}


void PCIHostPLDADevice::SetIRQ(int irq_num, int level)
{
    fIntxLevel = set_bit(fIntxLevel, irq_num, level != 0);
    IrqUpdate();
}


void PCIHostPLDADevice::SendMsi(uint64_t addr, uint32_t data)
{
    /* The root port's first base address register maps the bridge
       registers into PCI memory space. The driver sets it to zero and hides
       it, and it is fixed there here, so the doorbell is the register's own
       offset -- which is the address handed out to every device. */
    if (addr != PLDA_IMSI_ADDR) {
        /* Not aimed at this receiver, so it is an ordinary posted write and
           is performed as one. */
        SystemBus *sys = static_cast<SystemBus *>(ParentBus());
        uint8_t *ptr = sys->MemMap()->GetRamPtr(addr, true);
        if (ptr != nullptr) {
            put_le32(ptr, data);
        } else {
            sys->MemMap()->IoWrite(addr, data, 2);
        }
        return;
    }

    fMsiStatus |= 1u << (data & (PLDA_MSI_COUNT - 1));
    IrqUpdate();
}


//#pragma mark - bridge registers

uint32_t PCIHostPLDADevice::ApbRead(uint32_t offset, int size_log2)
{
    if (offset >= PCIE_PLDA_RC_CONFIG_OFFSET &&
        offset < PCIE_PLDA_RC_CONFIG_OFFSET + PCI_EXT_CONFIG_SIZE) {
        return pci_bus_config_read(fRootBus.get(),
                                   offset - PCIE_PLDA_RC_CONFIG_OFFSET,
                                   size_log2);
    }
    /* The register block sits on a 32 bit bus. */
    if (offset >= PCIE_PLDA_REG_SIZE || size_log2 != 2) {
        return 0;
    }

    switch (offset) {
    case PLDA_ISTATUS_LOCAL:
        return LocalStatus();
    case PLDA_ISTATUS_MSI:
        return fMsiStatus;
    }
    return fRegs[offset / 4];
}


void PCIHostPLDADevice::ApbWrite(uint32_t offset, uint32_t val, int size_log2)
{
    if (offset >= PCIE_PLDA_RC_CONFIG_OFFSET &&
        offset < PCIE_PLDA_RC_CONFIG_OFFSET + PCI_EXT_CONFIG_SIZE) {
        pci_bus_config_write(fRootBus.get(),
                             offset - PCIE_PLDA_RC_CONFIG_OFFSET, val,
                             size_log2);
        return;
    }
    if (offset >= PCIE_PLDA_REG_SIZE || size_log2 != 2) {
        return;
    }

    switch (offset) {
    case PLDA_ISTATUS_LOCAL:
        /* Every bit in it follows its source; see LocalStatus(). */
        return;
    case PLDA_ISTATUS_MSI:
        fMsiStatus &= ~val;
        IrqUpdate();
        return;
    case PLDA_IMASK_LOCAL:
        fRegs[offset / 4] = val;
        IrqUpdate();
        return;
    }
    fRegs[offset / 4] = val;
}


//#pragma mark - configuration window

uint32_t PCIHostPLDADevice::CfgRead(uint32_t offset, int size_log2)
{
    return pci_bus_config_read(fRootBus.get(), offset, size_log2);
}


void PCIHostPLDADevice::CfgWrite(uint32_t offset, uint32_t val, int size_log2)
{
    pci_bus_config_write(fRootBus.get(), offset, val, size_log2);
}


//#pragma mark - system registers and resets

uint32_t PCIHostPLDADevice::SysconRead(uint32_t offset, int size_log2)
{
    (void)size_log2;
    uint32_t val = fSyscon[offset / 4];
    /* The link is always up. The JH7110 shares one block between its two
       controllers and each finds its own registers by domain number; here
       each controller has a block to itself, so both words report it. */
    if (offset == STG_SYSCON_PCIE0_BASE + STG_SYSCON_LNKSTA ||
        offset == STG_SYSCON_PCIE1_BASE + STG_SYSCON_LNKSTA) {
        val |= STG_SYSCON_DATA_LINK_ACTIVE;
    }
    return val;
}


void PCIHostPLDADevice::SysconWrite(uint32_t offset, uint32_t val,
                                    int size_log2)
{
    (void)size_log2;
    fSyscon[offset / 4] = val;
}


/* The reset lines are stored so they read back, but nothing is held in
   reset by them. */
uint32_t PCIHostPLDADevice::ResetRead(uint32_t offset, int size_log2)
{
    (void)size_log2;
    return offset == 0 ? fResets : 0;
}


void PCIHostPLDADevice::ResetWrite(uint32_t offset, uint32_t val,
                                   int size_log2)
{
    (void)size_log2;
    if (offset == 0) {
        fResets = val;
    }
}


//#pragma mark - device tree

void PCIHostPLDADevice::BuildFDT(FDTContext &ctx)
{
    FDTBuilder *fdt = ctx.fdt;
    uint32_t syscon_phandle = fdt->AllocPhandle();
    uint32_t reset_phandle = fdt->AllocPhandle();
    uint32_t intc_phandle = fdt->AllocPhandle();

    /* The driver insists on a system register block and on its reset
       lines, and this machine has neither a JH7110 system controller nor its
       clock and reset generator. So each controller brings its own: a plain
       syscon, and a register bank the generic reset driver binds to. */
    fdt->BeginNodeNum("syscon", fSysconRes->base);
    fdt->PropStr("compatible", "syscon");
    fdt->PropU64Range("reg", fSysconRes->base, fSysconRes->size);
    fdt->PropU32("phandle", syscon_phandle);
    fdt->EndNode();

    fdt->BeginNodeNum("reset-controller", fResetRes->base);
    fdt->PropStr("compatible", "snps,dw-high-reset");
    fdt->PropU64Range("reg", fResetRes->base, fResetRes->size);
    fdt->PropU32("#reset-cells", 1);
    fdt->PropU32("phandle", reset_phandle);
    fdt->EndNode();

    fdt->BeginNodeNum("pcie", fCfgRes->base);
    fdt->PropStr("compatible", "starfive,jh7110-pcie");
    fdt->AddCellU64(fCfgRes->base);
    fdt->AddCellU64(fCfgRes->size);
    fdt->AddCellU64(fApbRes->base);
    fdt->AddCellU64(fApbRes->size);
    fdt->PropCells("reg");
    fdt->PropStrList("reg-names", "cfg", "apb", nullptr);
    fdt->PropStr("device_type", "pci");
    fdt->PropU32("#address-cells", 3);
    fdt->PropU32("#size-cells", 2);
    fdt->PropU32("#interrupt-cells", 1);

    fdt->AddCellU32(0);
    fdt->AddCellU32(fBusCount - 1);
    fdt->PropCells("bus-range");

    /* The driver tells the two controllers of a JH7110 apart by this, and
       refuses any number but 0 or 1. */
    fdt->PropU32("linux,pci-domain", ctx.pci_domain++);

    /* Both windows are identity mapped. */
    fdt->AddCellU32(PCI_RANGE_MMIO);
    fdt->AddCellU64(fMmioRes->base);
    fdt->AddCellU64(fMmioRes->base);
    fdt->AddCellU64(fMmioRes->size);
    if (fMmio64Res != nullptr) {
        fdt->AddCellU32(PCI_RANGE_MMIO_64BIT_PREF);
        fdt->AddCellU64(fMmio64Res->base);
        fdt->AddCellU64(fMmio64Res->base);
        fdt->AddCellU64(fMmio64Res->size);
    }
    fdt->PropCells("ranges");

    fdt->PropU32("starfive,stg-syscon", syscon_phandle);
    fdt->PropEmpty("msi-controller");
    fdt_prop_irq(ctx, fIrqRes->base);

    /* Only the pin selects an entry, and it goes to the INTx controller
       nested below, which is the bridge's own status register. That
       controller numbers its inputs by status bit, INTA being 0. The JH7110's
       own device tree numbers them from 1, which the driver it shares with
       the Microchip part does not translate, so INTA would arrive nowhere. */
    fdt->AddCellU32(0);
    fdt->AddCellU32(0);
    fdt->AddCellU32(0);
    fdt->AddCellU32(7);
    fdt->PropCells("interrupt-map-mask");
    for (int pin = 1; pin <= 4; pin++) {
        fdt->AddCellU32(0);
        fdt->AddCellU32(0);
        fdt->AddCellU32(0);
        fdt->AddCellU32(pin);
        fdt->AddCellU32(intc_phandle);
        fdt->AddCellU32(pin - 1);
    }
    fdt->PropCells("interrupt-map");

    for (size_t i = 0; i < countof(kResetNames); i++) {
        fdt->AddCellU32(reset_phandle);
        fdt->AddCellU32(i);
    }
    fdt->PropCells("resets");
    for (size_t i = 0; i < countof(kResetNames); i++) {
        fdt->AddCellString(kResetNames[i]);
    }
    fdt->PropCells("reset-names");

    /* The driver takes the first child node as its INTx domain. */
    fdt->BeginNode("interrupt-controller");
    fdt->PropU32("#address-cells", 0);
    fdt->PropU32("#interrupt-cells", 1);
    fdt->PropEmpty("interrupt-controller");
    fdt->PropU32("phandle", intc_phandle);
    fdt->EndNode();

    fdt->EndNode();
}


//#pragma mark - class

class PCIHostPLDAClass final: public DeviceClass {
public:
    PCIHostPLDAClass(): DeviceClass("pci-host-plda") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        int mmio_size_mb, mmio64_size_mb, bus_count;

        (void)ctx;
        if (!cfg.GetInt("mmio_size", &mmio_size_mb,
                        PCIE_PLDA_DEFAULT_MMIO_SIZE >> 20) ||
            !cfg.GetInt("mmio64_size", &mmio64_size_mb,
                        PCIE_PLDA_DEFAULT_MMIO64_SIZE >> 20) ||
            !cfg.GetInt("bus_count", &bus_count,
                        PCIE_PLDA_DEFAULT_BUS_COUNT)) {
            return nullptr;
        }
        return new PCIHostPLDADevice(cfg.IdOr("pcie"),
                                     (uint64_t)mmio_size_mb << 20,
                                     (uint64_t)mmio64_size_mb << 20,
                                     bus_count);
    }
};

static const PCIHostPLDAClass sPCIHostPLDAClass;
