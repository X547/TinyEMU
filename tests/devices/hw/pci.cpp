/*
 * PCI host bridges, enumeration and resource assignment
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
#include "pci.h"

#include "aplic.h"
#include "format.h"
#include "heap.h"
#include "i8259.h"
#include "imsic.h"
#include "ioapic.h"
#include "lapic.h"
#include "mmio.h"
#include "plic.h"
#include "test.h"
#include "vectors.h"

/* DesignWare: the outbound translation region configuration cycles are
   sent through, and the receiver of messages, by the Synopsys databook as
   the Linux driver uses it. */
#define DW_ATU_OUT(i)       (0x300000 + 0x200 * (i))
#define DW_ATU_CTRL1        0x00
#define DW_ATU_CTRL2        0x04
#define DW_ATU_BASE_LO      0x08
#define DW_ATU_BASE_HI      0x0c
#define DW_ATU_LIMIT        0x10
#define DW_ATU_TARGET_LO    0x14
#define DW_ATU_TARGET_HI    0x18
#define DW_ATU_TYPE_CFG0    0x04
#define DW_ATU_TYPE_CFG1    0x05
#define DW_ATU_ENABLE       0x80000000u
#define DW_MSI_ADDR_LO      0x820
#define DW_MSI_ADDR_HI      0x824
#define DW_MSI_ENABLE(g)    (0x828 + 0xc * (g))
#define DW_MSI_MASK(g)      (0x82c + 0xc * (g))
#define DW_MSI_STATUS(g)    (0x830 + 0xc * (g))
#define DW_MSI_GROUPS       8

/* PLDA: the local interrupt registers of the bridge, as the Linux driver
   for the XpressRich core uses them. */
#define PLDA_IMASK_LOCAL    0x180
#define PLDA_ISTATUS_LOCAL  0x184
#define PLDA_IMSI_ADDR      0x190
#define PLDA_ISTATUS_MSI    0x194
#define PLDA_INTX_SHIFT     24
#define PLDA_INT_MSI        (1u << 28)

#define MAX_HOSTS 8
#define MAX_FUNCS 128

namespace {

PciHost *sHosts[MAX_HOSTS];
int sHostCount = -1;
PciFunc *sFuncs[MAX_FUNCS];
int sFuncCount;
bool sEnumerated;

/* MSI messages arrive here as numbers 0 to 31. */
struct MsiSlot {
    IrqHandler fn;
    void *ctx;
};


//#pragma mark - message receivers

#if defined(__riscv)

/* The boot hart's interrupt file: a message is a write of the identity to
   its page. The identities are the top 32, clear of those the APLIC
   uses for its sources. */
class ImsicSink final : public PciMsiSink {
public:
    ImsicSink(Imsic *imsic): fImsic(imsic) {}

    uint32_t Id(int n) const
    {
        uint32_t ids = fImsic->IdCount();
        return (ids >= 64 ? ids - 31 : 1) + n;
    }

    bool Message(int n, IrqHandler fn, void *ctx, uint64_t *addr,
                 uint32_t *data) override
    {
        if (n < 0 || n >= 32 || !fImsic->Route(Id(n), fn, ctx, false))
            return false;
        *addr = fImsic->MyPage();
        *data = Id(n);
        return true;
    }
    const char *Kind() const override {return "imsic";}
    bool Route(uint32_t line, IrqHandler fn, void *ctx, bool) override
        {return line < 32 && fImsic->Route(Id(line), fn, ctx, false);}
    void Unroute(uint32_t line) override
    {
        if (line < 32)
            fImsic->Unroute(Id(line));
    }
    /* the interrupt file is reset as a controller of its own */
    void Reset() override {}

private:
    Imsic *fImsic;
};

#else

/* The boot processor's local APIC, at the vectors from VEC_MSI. */
class LapicSink final : public PciMsiSink {
public:
    LapicSink(Lapic *lapic): fLapic(lapic) {}

    bool Message(int n, IrqHandler fn, void *ctx, uint64_t *addr,
                 uint32_t *data) override
    {
        if (!Route(n, fn, ctx, false))
            return false;
        *addr = 0xfee00000u | fLapic->Id() << 12;
        *data = VEC_MSI + n; /* fixed, edge */
        return true;
    }
    const char *Kind() const override {return "lapic";}
    bool Route(uint32_t line, IrqHandler fn, void *ctx, bool) override
    {
        if (line >= 32)
            return false;
        fLapic->Enable();
        fSlots[line] = {fn, ctx};
        arch_set_irq_handler(VEC_MSI + line, Dispatch, &fSlots[line]);
        return true;
    }
    void Unroute(uint32_t line) override
    {
        if (line < 32)
            fSlots[line] = {};
    }
    void Reset() override {}

private:
    static void Dispatch(void *p)
    {
        MsiSlot *s = (MsiSlot *)p;
        if (s->fn)
            s->fn(s->ctx);
        Lapic::Get()->Eoi();
    }

    Lapic *fLapic;
    MsiSlot fSlots[32] {};
};

#endif

/* The receiver in a DesignWare root complex: a doorbell address in RAM it
   intercepts, groups of 32 status bits, and one interrupt line. Messages
   use group 0. */
class DwSink final : public PciMsiSink {
public:
    DwSink(const PciHost *h): fHost(h)
    {
        fDoorbell = (uint64_t)(uintptr_t)boot_alloc(4, 4);
    }

    uint32_t Rd(uint32_t reg) const {return mmio_read32(fHost->dbi + reg);}
    void Wr(uint32_t reg, uint32_t v) const {mmio_write32(fHost->dbi + reg, v);}

    bool Message(int n, IrqHandler fn, void *ctx, uint64_t *addr,
                 uint32_t *data) override
    {
        if (!Route(n, fn, ctx, false))
            return false;
        *addr = fDoorbell;
        *data = n;
        return true;
    }
    const char *Kind() const override {return "dw-msi";}
    bool Route(uint32_t line, IrqHandler fn, void *ctx, bool) override
    {
        if (line >= 32 || !irq_route(fHost->dev, 0, Dispatch, this))
            return false;
        fSlots[line] = {fn, ctx};
        Wr(DW_MSI_ADDR_LO, (uint32_t)fDoorbell);
        Wr(DW_MSI_ADDR_HI, fDoorbell >> 32);
        Wr(DW_MSI_MASK(0), Rd(DW_MSI_MASK(0)) & ~(1u << line));
        Wr(DW_MSI_ENABLE(0), Rd(DW_MSI_ENABLE(0)) | 1u << line);
        return true;
    }
    void Unroute(uint32_t line) override
    {
        if (line >= 32)
            return;
        Wr(DW_MSI_ENABLE(0), Rd(DW_MSI_ENABLE(0)) & ~(1u << line));
        fSlots[line] = {};
    }
    /* back to the values from reset */
    void Reset() override
    {
        for (int g = 0; g < DW_MSI_GROUPS; g++) {
            Wr(DW_MSI_ENABLE(g), 0);
            Wr(DW_MSI_MASK(g), 0);
            Wr(DW_MSI_STATUS(g), ~0u);
        }
        Wr(DW_MSI_ADDR_LO, 0);
        Wr(DW_MSI_ADDR_HI, 0);
        for (MsiSlot &s : fSlots)
            s = {};
    }

private:
    static void Dispatch(void *p)
    {
        DwSink *s = (DwSink *)p;
        uint32_t st = s->Rd(DW_MSI_STATUS(0));
        for (int n = 0; n < 32; n++) {
            if (!((st >> n) & 1))
                continue;
            s->Wr(DW_MSI_STATUS(0), 1u << n);
            if (s->fSlots[n].fn)
                s->fSlots[n].fn(s->fSlots[n].ctx);
        }
    }

    const PciHost *fHost;
    uint64_t fDoorbell;
    MsiSlot fSlots[32] {};
};


/* The PLDA bridge's local interrupts, all on one line: INTA to INTD are
   lines 0 to 3 and messages are lines 32 up. */
class PldaSink final : public PciMsiSink {
public:
    PldaSink(const PciHost *h): fHost(h) {}

    uint32_t Rd(uint32_t reg) const {return mmio_read32(fHost->apb + reg);}
    void Wr(uint32_t reg, uint32_t v) const {mmio_write32(fHost->apb + reg, v);}

    bool Message(int n, IrqHandler fn, void *ctx, uint64_t *addr,
                 uint32_t *data) override
    {
        if (n < 0 || n >= 32 || !Route(32 + n, fn, ctx, false))
            return false;
        *addr = PLDA_IMSI_ADDR;
        *data = n;
        return true;
    }
    const char *Kind() const override {return "plda";}
    bool Route(uint32_t line, IrqHandler fn, void *ctx, bool) override
    {
        if ((line >= 4 && line < 32) || line >= 64 ||
            !irq_route(fHost->dev, 0, Dispatch, this))
            return false;
        fSlots[line] = {fn, ctx};
        uint32_t bit = line < 4 ? 1u << (PLDA_INTX_SHIFT + line)
                                : PLDA_INT_MSI;
        Wr(PLDA_IMASK_LOCAL, Rd(PLDA_IMASK_LOCAL) | bit);
        return true;
    }
    void Unroute(uint32_t line) override
    {
        if (line < 4) {
            Wr(PLDA_IMASK_LOCAL,
               Rd(PLDA_IMASK_LOCAL) & ~(1u << (PLDA_INTX_SHIFT + line)));
        }
        if (line < 64)
            fSlots[line] = {};
    }
    void Reset() override
    {
        Wr(PLDA_IMASK_LOCAL, 0);
        Wr(PLDA_ISTATUS_MSI, ~0u);
        for (MsiSlot &s : fSlots)
            s = {};
    }

private:
    static void Dispatch(void *p)
    {
        PldaSink *s = (PldaSink *)p;
        uint32_t st = s->Rd(PLDA_ISTATUS_LOCAL) & s->Rd(PLDA_IMASK_LOCAL);
        for (int pin = 0; pin < 4; pin++) {
            if ((st >> (PLDA_INTX_SHIFT + pin)) & 1 && s->fSlots[pin].fn)
                s->fSlots[pin].fn(s->fSlots[pin].ctx);
        }
        if (!(st & PLDA_INT_MSI))
            return;
        uint32_t msi = s->Rd(PLDA_ISTATUS_MSI);
        for (int n = 0; n < 32; n++) {
            if (!((msi >> n) & 1))
                continue;
            s->Wr(PLDA_ISTATUS_MSI, 1u << n);
            if (s->fSlots[32 + n].fn)
                s->fSlots[32 + n].fn(s->fSlots[32 + n].ctx);
        }
    }

    const PciHost *fHost;
    MsiSlot fSlots[64] {};
};


template <typename T, typename... Args>
T *make(Args... args)
{
    void *mem = boot_alloc(sizeof(T), alignof(T));
    T *obj = new (mem) T(args...);
    intc_register(obj);
    return obj;
}


//#pragma mark - discovery

PciHost *add_host(PciHostKind kind, const HwDevice *dev)
{
    if (sHostCount == MAX_HOSTS)
        return nullptr;
    PciHost *h = (PciHost *)boot_alloc(sizeof(PciHost), alignof(PciHost));
    h->kind = kind;
    h->dev = dev;
    h->index = sHostCount;
    h->bus_last = 255;
    sHosts[sHostCount++] = h;
    return h;
}

/* "ranges": the I/O window and the memory windows, by the PCI bus binding:
   phys.hi says which space, prefetchable in bit 30. */
void fdt_apertures(PciHost *h, const FdtNode *node)
{
    const FdtProp *ranges = fdt_prop(node, "ranges");
    int pac = fdt_address_cells(node->parent);
    int sc = fdt_size_cells(node);
    uint32_t entry = 4 * (3 + pac + sc);
    for (uint32_t off = 0; ranges && off + entry <= ranges->len;
         off += entry) {
        const uint8_t *p = ranges->data + off;
        uint32_t hi = be32(p);
        PciAperture a;
        a.pci = fdt_read_cells(p + 4, 2);
        a.cpu = fdt_read_cells(p + 12, pac);
        a.size = fdt_read_cells(p + 12 + 4 * pac, sc);
        a.prefetch = (hi >> 30) & 1;
        switch ((hi >> 24) & 3) {
        case 1: h->io = a; break;
        case 2: h->mem = a; break;
        case 3: h->mem64 = a; break;
        }
    }
    const FdtProp *range = fdt_prop(node, "bus-range");
    if (range && range->len >= 8) {
        h->bus_first = be32(range->data);
        h->bus_last = be32(range->data + 4);
    }
}

void add_fdt_hosts()
{
    for (int i = 0; i < registry_count(); i++) {
        const HwDevice *d = registry_at(i);
        PciHostKind kind;
        if (d->fdt == nullptr)
            continue;
        if (hw_is(d, "pci-host-ecam-generic"))
            kind = PCI_HOST_ECAM;
        else if (hw_is(d, "snps,dw-pcie"))
            kind = PCI_HOST_DW;
        else if (hw_is(d, "starfive,jh7110-pcie"))
            kind = PCI_HOST_PLDA;
        else
            continue;
        PciHost *h = add_host(kind, d);
        if (h == nullptr)
            return;
        h->ext_config = true;
        fdt_apertures(h, d->fdt);
        if (kind == PCI_HOST_DW) {
            h->dbi = d->nregions > 0 ? d->regions[0].base : 0;
            h->cfg = d->nregions > 1 ? d->regions[1].base : 0;
        } else if (kind == PCI_HOST_PLDA) {
            h->cfg = d->nregions > 0 ? d->regions[0].base : 0;
            h->apb = d->nregions > 1 ? d->regions[1].base : 0;
        } else {
            h->cfg = d->nregions > 0 ? d->regions[0].base : 0;
        }
        uint32_t ph;
        const FdtNode *parent = fdt_prop_u32(d->fdt, "msi-parent", &ph)
            ? fdt_find_phandle(ph) : nullptr;
        if (parent && fdt_is_compatible(parent, "riscv,imsics")) {
#if defined(__riscv)
            if (Imsic *imsic = Imsic::Mine())
                h->msi = make<ImsicSink>(imsic);
#endif
        } else if (kind == PCI_HOST_DW) {
            h->msi = make<DwSink>(h);
        } else if (kind == PCI_HOST_PLDA) {
            h->msi = make<PldaSink>(h);
        }
    }
}

struct CrsScan {
    PciHost *host;
    uint64_t io_best;
};

/* The windows of a root bridge's _CRS: the largest I/O range, the memory
   range below 4 GB (not the legacy VGA hole), and the one above. */
void crs_resource(void *ctx, const AcpiResource &r)
{
    CrsScan *s = (CrsScan *)ctx;
    PciHost *h = s->host;
    /* only the address space descriptors are windows */
    bool window = r.type == 0x07 || r.type == 0x08 || r.type == 0x0a;
    if (r.kind == ACPI_RES_BUS) {
        h->bus_first = r.base;
        h->bus_last = r.base + r.length - 1;
    } else if (r.kind == ACPI_RES_IO && window && r.length > s->io_best) {
        s->io_best = r.length;
        h->io.pci = h->io.cpu = r.base;
        h->io.size = r.length;
    } else if (r.kind == ACPI_RES_MEMORY && window && r.base >= 0x100000) {
        PciAperture &a = r.base + r.length > 0x100000000ull ? h->mem64
                                                           : h->mem;
        a.pci = a.cpu = r.base;
        a.size = r.length;
    }
}

/* The MCFG entry of a PCI Express root bridge's segment: the ECAM window
   of its buses, from bus 0. */
bool mcfg_window(const HwDevice *d, uint64_t *base)
{
    const uint8_t *t = acpi_table("MCFG");
    if (t == nullptr || !hw_is(d, "PNP0A08"))
        return false;
    const AcpiNode *seg = acpi_child(d->acpi, "_SEG");
    uint64_t segment = seg && seg->kind == ACPI_NAME ? seg->value.integer : 0;
    for (uint32_t off = 44; off + 16 <= le32(t + 4); off += 16) {
        if (le16(t + off + 8) == segment) {
            *base = le64(t + off);
            return true;
        }
    }
    return false;
}

void add_acpi_hosts()
{
    for (int i = 0; i < registry_count(); i++) {
        const HwDevice *d = registry_at(i);
        if (d->acpi == nullptr || !hw_is(d, "PNP0A03"))
            continue;
        uint64_t ecam;
        bool is_ecam = mcfg_window(d, &ecam);
        PciHost *h = add_host(is_ecam ? PCI_HOST_ECAM : PCI_HOST_CF8, d);
        if (h == nullptr)
            return;
        h->io_ports = true;
        const AcpiNode *crs = acpi_child(d->acpi, "_CRS");
        CrsScan scan = {h, 0};
        if (crs && crs->kind == ACPI_NAME)
            acpi_parse_resources(crs->value, crs_resource, &scan);
        if (is_ecam) {
            h->ext_config = true;
            h->cfg = ecam + ((uint64_t)h->bus_first << 20);
        }
#if !defined(__riscv)
        if (Lapic *lapic = Lapic::Get())
            h->msi = make<LapicSink>(lapic);
#endif
    }
}

void discover()
{
    if (sHostCount >= 0)
        return;
    sHostCount = 0;
    add_fdt_hosts();
    add_acpi_hosts();
}


//#pragma mark - configuration cycles

uint32_t ones(int size_log2)
{
    return size_log2 == 2 ? 0xffffffffu : (1u << (8 << size_log2)) - 1;
}

uint32_t mmio_read(uint64_t a, int size_log2)
{
    switch (size_log2) {
    case 0: return mmio_read8(a);
    case 1: return mmio_read16(a);
    default: return mmio_read32(a);
    }
}

void mmio_write(uint64_t a, uint32_t v, int size_log2)
{
    switch (size_log2) {
    case 0: mmio_write8(a, v); break;
    case 1: mmio_write16(a, v); break;
    default: mmio_write32(a, v); break;
    }
}

#if defined(__i386__) || defined(__x86_64__)

uint32_t port_read(uint16_t port, int size_log2)
{
    switch (size_log2) {
    case 0: return inb(port);
    case 1: return inw(port);
    default: return inl(port);
    }
}

void port_write(uint16_t port, uint32_t v, int size_log2)
{
    switch (size_log2) {
    case 0: outb(port, v); break;
    case 1: outw(port, v); break;
    default: outl(port, v); break;
    }
}

#else

uint32_t port_read(uint16_t, int size_log2)
{
    return ones(size_log2);
}

void port_write(uint16_t, uint32_t, int)
{
}

#endif

/* DesignWare: point outbound region 0 at the function, as a type 0 cycle
   on the link below the root port and a type 1 cycle further down. */
uint64_t dw_target(const PciHost *h, uint32_t bus, uint32_t devfn)
{
    uint64_t atu = h->dbi + DW_ATU_OUT(0);
    uint32_t link = mmio_read8(h->dbi + PCI_SECONDARY_BUS);
    mmio_write32(atu + DW_ATU_CTRL1,
                 bus == link ? DW_ATU_TYPE_CFG0 : DW_ATU_TYPE_CFG1);
    mmio_write32(atu + DW_ATU_BASE_LO, (uint32_t)h->cfg);
    mmio_write32(atu + DW_ATU_BASE_HI, h->cfg >> 32);
    mmio_write32(atu + DW_ATU_LIMIT, (uint32_t)h->cfg + 0xfff);
    mmio_write32(atu + DW_ATU_TARGET_LO, bus << 24 | devfn << 16);
    mmio_write32(atu + DW_ATU_TARGET_HI, 0);
    mmio_write32(atu + DW_ATU_CTRL2, DW_ATU_ENABLE);
    return h->cfg;
}

/* The address a cycle goes to, or 0 when it reaches nothing. */
uint64_t cfg_address(const PciHost *h, uint32_t bus, uint32_t devfn,
                     uint32_t reg)
{
    if (bus < h->bus_first || bus > h->bus_last)
        return 0;
    switch (h->kind) {
    case PCI_HOST_ECAM:
    case PCI_HOST_PLDA:
        return h->cfg + ((uint64_t)(bus - h->bus_first) << 20 |
                         devfn << 12 | reg);
    case PCI_HOST_DW:
        /* the root port is the bottom of DBI */
        if (bus == h->bus_first)
            return devfn == 0 ? h->dbi + reg : 0;
        return dw_target(h, bus, devfn) + reg;
    default:
        return 0;
    }
}

}


const char *PciHost::Kind() const
{
    switch (kind) {
    case PCI_HOST_ECAM: return "ecam";
    case PCI_HOST_DW: return "designware";
    case PCI_HOST_PLDA: return "plda";
    case PCI_HOST_CF8: return "cf8";
    }
    return "?";
}


uint32_t PciHost::Read(uint32_t bus, uint32_t devfn, uint32_t reg,
                       int size_log2) const
{
    if (reg >= (ext_config ? 0x1000u : 0x100u))
        return ones(size_log2);
    if (kind == PCI_HOST_CF8) {
        port_write(0xcf8, 0x80000000u | bus << 16 | devfn << 8 | (reg & 0xfc),
                   2);
        return port_read(0xcfc + (reg & 3), size_log2);
    }
    uint64_t a = cfg_address(this, bus, devfn, reg);
    return a ? mmio_read(a, size_log2) : ones(size_log2);
}


void PciHost::Write(uint32_t bus, uint32_t devfn, uint32_t reg,
                    uint32_t value, int size_log2) const
{
    if (reg >= (ext_config ? 0x1000u : 0x100u))
        return;
    if (kind == PCI_HOST_CF8) {
        port_write(0xcf8, 0x80000000u | bus << 16 | devfn << 8 | (reg & 0xfc),
                   2);
        port_write(0xcfc + (reg & 3), value, size_log2);
        return;
    }
    uint64_t a = cfg_address(this, bus, devfn, reg);
    if (a)
        mmio_write(a, value, size_log2);
}


int pci_host_count()
{
    discover();
    return sHostCount;
}


PciHost *pci_host(int index)
{
    discover();
    return index < sHostCount ? sHosts[index] : nullptr;
}


uint32_t pci_read(const PciFunc *f, uint32_t reg, int size_log2)
{
    return f->host->Read(f->bus, f->devfn, reg, size_log2);
}


void pci_write(const PciFunc *f, uint32_t reg, uint32_t value, int size_log2)
{
    f->host->Write(f->bus, f->devfn, reg, value, size_log2);
}


//#pragma mark - enumeration

namespace {

/* Where the next of each kind goes. */
struct Cursor {
    uint64_t io, io_end;
    uint64_t mem, mem_end;
    uint64_t mem64, mem64_end; /* empty when the processor cannot reach it */
    uint32_t next_bus;
};

uint32_t rom_reg(const PciFunc *f)
{
    return f->bridge ? PCI_ROM1 : PCI_ROM0;
}

/* Write all ones and see which bits stick, putting the BAR back. */
void size_bars(PciFunc *f)
{
    int count = f->bridge ? 2 : 6;
    for (int i = 0; i < count; i++) {
        uint32_t reg = PCI_BAR(i);
        uint32_t orig = pci_read32(f, reg);
        pci_write32(f, reg, ~0u);
        uint32_t lo = pci_read32(f, reg);
        pci_write32(f, reg, orig);
        if (lo == 0)
            continue;
        PciBar &b = f->bars[i];
        b.io = lo & PCI_BAR_IO;
        if (b.io) {
            b.size = (uint32_t)(~(lo & ~3u) + 1) & 0xffff;
            continue;
        }
        b.prefetch = lo & PCI_BAR_PREFETCH;
        uint64_t mask = lo & ~0xfu;
        if ((lo & 6) == PCI_BAR_MEM64 && i + 1 < count) {
            b.is64 = true;
            uint32_t orig_hi = pci_read32(f, reg + 4);
            pci_write32(f, reg + 4, ~0u);
            mask |= (uint64_t)pci_read32(f, reg + 4) << 32;
            pci_write32(f, reg + 4, orig_hi);
            i++;
        } else {
            mask |= 0xffffffff00000000ull;
        }
        b.size = ~mask + 1;
    }
    uint32_t reg = rom_reg(f);
    uint32_t orig = pci_read32(f, reg);
    pci_write32(f, reg, ~PCI_ROM_ENABLE);
    uint32_t v = pci_read32(f, reg) & ~0x7ffu;
    pci_write32(f, reg, orig);
    if (v)
        f->bars[6].size = (uint32_t)(~v + 1);
}

bool take(uint64_t *cursor, uint64_t end, uint64_t size, uint64_t *out)
{
    uint64_t a = align_up(*cursor, size);
    if (a < *cursor || a + size > end || a + size < a)
        return false;
    *out = a;
    *cursor = a + size;
    return true;
}

void write_bar(const PciFunc *f, int i)
{
    const PciBar &b = f->bars[i];
    if (i == 6) {
        pci_write32(f, rom_reg(f), (uint32_t)b.addr);
        return;
    }
    pci_write32(f, PCI_BAR(i), (uint32_t)b.addr);
    if (b.is64)
        pci_write32(f, PCI_BAR(i + 1), b.addr >> 32);
}

void assign_bars(PciFunc *f, Cursor &c)
{
    char name[24];
    for (int i = 0; i < 7; i++) {
        PciBar &b = f->bars[i];
        if (b.size == 0)
            continue;
        bool ok;
        if (b.io)
            ok = take(&c.io, c.io_end, b.size, &b.addr);
        else if (b.is64 && b.prefetch &&
                 take(&c.mem64, c.mem64_end, b.size, &b.addr))
            ok = true;
        else
            ok = take(&c.mem, c.mem_end, b.size, &b.addr);
        if (!ok) {
            pci_name(f, name, sizeof(name));
            t_log("pci: %s: no room for BAR %d of %#llx bytes", name, i,
                  (unsigned long long)b.size);
            b.addr = 0;
        }
        write_bar(f, i);
    }
}

void write_windows(const PciFunc *f)
{
    const PciAperture &io = f->win_io, &mem = f->win_mem,
        &pref = f->win_pref;
    if (io.size) {
        uint64_t last = io.pci + io.size - 1;
        pci_write8(f, PCI_IO_BASE, (io.pci >> 8) & 0xf0);
        pci_write8(f, PCI_IO_LIMIT, (last >> 8) & 0xf0);
        pci_write16(f, PCI_IO_BASE_HI, io.pci >> 16);
        pci_write16(f, PCI_IO_LIMIT_HI, last >> 16);
    } else {
        pci_write8(f, PCI_IO_BASE, 0xf0);
        pci_write8(f, PCI_IO_LIMIT, 0);
        pci_write16(f, PCI_IO_BASE_HI, 0);
        pci_write16(f, PCI_IO_LIMIT_HI, 0);
    }
    if (mem.size) {
        pci_write16(f, PCI_MEM_BASE, (mem.pci >> 16) & 0xfff0);
        pci_write16(f, PCI_MEM_LIMIT,
                    ((mem.pci + mem.size - 1) >> 16) & 0xfff0);
    } else {
        pci_write16(f, PCI_MEM_BASE, 0xfff0);
        pci_write16(f, PCI_MEM_LIMIT, 0);
    }
    if (pref.size) {
        uint64_t last = pref.pci + pref.size - 1;
        pci_write16(f, PCI_PREF_BASE, (pref.pci >> 16) & 0xfff0);
        pci_write16(f, PCI_PREF_LIMIT, (last >> 16) & 0xfff0);
        pci_write32(f, PCI_PREF_BASE_HI, pref.pci >> 32);
        pci_write32(f, PCI_PREF_LIMIT_HI, last >> 32);
    } else {
        pci_write16(f, PCI_PREF_BASE, 0xfff0);
        pci_write16(f, PCI_PREF_LIMIT, 0);
        pci_write32(f, PCI_PREF_BASE_HI, 0);
        pci_write32(f, PCI_PREF_LIMIT_HI, 0);
    }
}

void write_buses(const PciFunc *f)
{
    pci_write8(f, PCI_PRIMARY_BUS, f->bus);
    pci_write8(f, PCI_SECONDARY_BUS, f->secondary);
    pci_write8(f, PCI_SUBORDINATE_BUS, f->subordinate);
}

/* A window from 'start' to where the cursor got, at this granularity. */
PciAperture window(uint64_t start, uint64_t *cursor, uint64_t gran)
{
    PciAperture w;
    *cursor = align_up(*cursor, gran);
    if (*cursor > start) {
        w.pci = w.cpu = start;
        w.size = *cursor - start;
    }
    return w;
}

void scan_bus(PciHost *h, uint32_t bus, PciFunc *parent, Cursor &c);

void add_function(PciHost *h, uint32_t bus, uint32_t devfn, PciFunc *parent,
                  Cursor &c)
{
    if (sFuncCount == MAX_FUNCS)
        return;
    PciFunc *f = (PciFunc *)boot_alloc(sizeof(PciFunc), alignof(PciFunc));
    sFuncs[sFuncCount++] = f;
    f->host = h;
    f->parent = parent;
    f->bus = bus;
    f->devfn = devfn;
    f->vendor = pci_read16(f, PCI_VENDOR_ID);
    f->device = pci_read16(f, PCI_DEVICE_ID);
    f->class_rev = pci_read32(f, PCI_REVISION);
    f->header = pci_read8(f, PCI_HEADER_TYPE);
    f->bridge = (f->header & 0x7f) == 1;
    f->pin = pci_read8(f, PCI_INTERRUPT_PIN);
    for (uint32_t w = 0; w < ARRAY_SIZE(f->dev_config); w++)
        f->dev_config[w] = pci_read32(f, 0x40 + 4 * w);

    pci_write16(f, PCI_COMMAND, 0);
    size_bars(f);
    assign_bars(f, c);

    if (f->bridge) {
        f->secondary = c.next_bus++;
        f->subordinate = h->bus_last;
        write_buses(f);
        uint64_t io = c.io = align_up(c.io, 0x1000);
        uint64_t mem = c.mem = align_up(c.mem, 0x100000);
        uint64_t mem64 = c.mem64 = align_up(c.mem64, 0x100000);
        scan_bus(h, f->secondary, f, c);
        f->subordinate = c.next_bus - 1;
        pci_write8(f, PCI_SUBORDINATE_BUS, f->subordinate);
        f->win_io = window(io, &c.io, 0x1000);
        f->win_mem = window(mem, &c.mem, 0x100000);
        f->win_pref = window(mem64, &c.mem64, 0x100000);
        f->win_pref.prefetch = true;
        write_windows(f);
    }
    f->command = PCI_CMD_IO | PCI_CMD_MEMORY | PCI_CMD_MASTER;
    pci_write16(f, PCI_COMMAND, f->command);
}

void scan_bus(PciHost *h, uint32_t bus, PciFunc *parent, Cursor &c)
{
    for (uint32_t slot = 0; slot < 32; slot++) {
        uint32_t fns = 1;
        for (uint32_t fn = 0; fn < fns; fn++) {
            uint32_t devfn = PCI_DEVFN(slot, fn);
            if (h->Read(bus, devfn, PCI_VENDOR_ID, 1) == 0xffff)
                continue;
            if (fn == 0 && h->Read(bus, devfn, PCI_HEADER_TYPE, 0) & 0x80)
                fns = 8;
            add_function(h, bus, devfn, parent, c);
        }
    }
}

/* PCI addresses the processor reaches, from the bottom of a window. Port
   0 and the first 4 KB of ports are left alone where there is room, as
   firmware leaves them. */
void cursor_for(const PciHost *h, Cursor &c)
{
    c = {};
    if (h->io.size) {
        c.io = h->io.pci;
        c.io_end = h->io.pci + h->io.size;
        if (c.io < 0x1000 && c.io_end > 0x2000)
            c.io = 0x1000;
    }
    c.mem = h->mem.pci;
    c.mem_end = h->mem.pci + h->mem.size;
    if (h->mem64.size && h->mem64.cpu + h->mem64.size <= ARCH_ADDRESS_LIMIT) {
        c.mem64 = h->mem64.pci;
        c.mem64_end = h->mem64.pci + h->mem64.size;
    }
    c.next_bus = h->bus_first + 1;
}

}


void pci_enumerate()
{
    if (sEnumerated)
        return;
    sEnumerated = true;
    discover();
    for (int i = 0; i < sHostCount; i++) {
        PciHost *h = sHosts[i];
        Cursor c;
        cursor_for(h, c);
        scan_bus(h, h->bus_first, nullptr, c);
    }
}


int pci_func_count()
{
    pci_enumerate();
    return sFuncCount;
}


PciFunc *pci_func(int index)
{
    pci_enumerate();
    return index < sFuncCount ? sFuncs[index] : nullptr;
}


PciFunc *pci_find(uint16_t vendor, uint16_t device, int index)
{
    pci_enumerate();
    for (int i = 0; i < sFuncCount; i++) {
        if (sFuncs[i]->vendor == vendor && sFuncs[i]->device == device &&
            index-- == 0)
            return sFuncs[i];
    }
    return nullptr;
}


PciFunc *pci_require(uint16_t vendor, uint16_t device, int index)
{
    PciFunc *f = pci_find(vendor, device, index);
    if (f == nullptr)
        t_skip("no PCI function %04x:%04x #%d", vendor, device, index);
    return f;
}


void pci_name(const PciFunc *f, char *buf, size_t size)
{
    int n = snprintf(buf, size, "%x:%02x.%x", f->bus, PCI_SLOT(f->devfn),
                     PCI_FN(f->devfn));
    if (f->host->index > 0 && n >= 0 && (size_t)n < size)
        snprintf(buf + n, size - n, "@%d", f->host->index);
}


void pci_restore_all()
{
    for (int i = 0; i < sFuncCount; i++) {
        const PciFunc *f = sFuncs[i];
        pci_write16(f, PCI_COMMAND, 0);
        if (f->bridge) {
            write_buses(f);
            write_windows(f);
        }
        for (int b = 0; b < 7; b++) {
            if (f->bars[b].size)
                write_bar(f, b);
        }
        for (uint32_t w = 0; w < ARRAY_SIZE(f->dev_config); w++) {
            uint32_t reg = 0x40 + 4 * w;
            if (pci_read32(f, reg) != f->dev_config[w])
                pci_write32(f, reg, f->dev_config[w]);
        }
        pci_write16(f, PCI_COMMAND, f->command);
    }
}


uint32_t pci_find_cap(const PciFunc *f, uint8_t id)
{
    if (!(pci_read16(f, PCI_STATUS) & PCI_STATUS_CAPS))
        return 0;
    uint32_t off = pci_read8(f, PCI_CAP_PTR) & ~3u;
    for (int guard = 0; off >= 0x40 && guard < 48; guard++) {
        if (pci_read8(f, off) == id)
            return off;
        off = pci_read8(f, off + 1) & ~3u;
    }
    return 0;
}


uint32_t pci_find_ext_cap(const PciFunc *f, uint16_t id)
{
    if (!f->host->ext_config)
        return 0;
    uint32_t off = PCI_EXT_CAP_START;
    for (int guard = 0; off >= PCI_EXT_CAP_START && guard < 64; guard++) {
        uint32_t hdr = pci_read32(f, off);
        if (hdr == 0 || hdr == 0xffffffff)
            return 0;
        if ((hdr & 0xffff) == id)
            return off;
        off = (hdr >> 20) & ~3u;
    }
    return 0;
}


//#pragma mark - access

namespace {

/* The processor address of a PCI address, through the window holding it;
   memory outside every window is taken as identity mapped. */
bool cpu_address(const PciHost *h, bool io, uint64_t addr, uint64_t size,
                 uint64_t *cpu)
{
    const PciAperture *list[3] = {&h->io, &h->mem, &h->mem64};
    for (int i = io ? 0 : 1; i < (io ? 1 : 3); i++) {
        const PciAperture &a = *list[i];
        if (a.size && addr >= a.pci && addr + size <= a.pci + a.size) {
            *cpu = a.cpu + (addr - a.pci);
            return true;
        }
    }
    if (io)
        return false;
    *cpu = addr;
    return true;
}

}


bool pci_reachable(const PciHost *h, uint64_t addr, uint64_t size, bool io)
{
    if (io && h->io_ports)
        return addr + size <= 0x10000;
    uint64_t cpu;
    return cpu_address(h, io, addr, size, &cpu) &&
           cpu + size <= ARCH_ADDRESS_LIMIT;
}


uint32_t pci_space_read(const PciHost *h, bool io, uint64_t addr,
                        int size_log2)
{
    if (io && h->io_ports)
        return port_read(addr, size_log2);
    uint64_t cpu;
    if (!pci_reachable(h, addr, 1u << size_log2, io) ||
        !cpu_address(h, io, addr, 1u << size_log2, &cpu))
        return ones(size_log2);
    return mmio_read(cpu, size_log2);
}


void pci_space_write(const PciHost *h, bool io, uint64_t addr, uint32_t value,
                     int size_log2)
{
    if (io && h->io_ports) {
        port_write(addr, value, size_log2);
        return;
    }
    uint64_t cpu;
    if (pci_reachable(h, addr, 1u << size_log2, io) &&
        cpu_address(h, io, addr, 1u << size_log2, &cpu))
        mmio_write(cpu, value, size_log2);
}


namespace {

struct Probe {
    const PciHost *h;
    bool io;
    uint64_t addr;
    int size_log2;
    uint32_t value;
};

}


bool pci_space_probe(const PciHost *h, bool io, uint64_t addr, int size_log2,
                     uint32_t *value)
{
    Probe p = {h, io, addr, size_log2, ones(size_log2)};
    bool trapped = t_trap([](void *ctx) {
        Probe *p = (Probe *)ctx;
        p->value = pci_space_read(p->h, p->io, p->addr, p->size_log2);
    }, &p);
    *value = p.value;
    return !trapped;
}


uint64_t pci_bar_cpu(const PciFunc *f, int bar)
{
    const PciBar &b = f->bars[bar];
    uint64_t cpu;
    if (b.io || b.size == 0 || !pci_reachable(f->host, b.addr, b.size, false) ||
        !cpu_address(f->host, false, b.addr, b.size, &cpu))
        return 0;
    return cpu;
}


uint32_t pci_bar_read(const PciFunc *f, int bar, uint32_t offset,
                      int size_log2)
{
    const PciBar &b = f->bars[bar];
    return pci_space_read(f->host, b.io, b.addr + offset, size_log2);
}


void pci_bar_write(const PciFunc *f, int bar, uint32_t offset, uint32_t value,
                   int size_log2)
{
    const PciBar &b = f->bars[bar];
    pci_space_write(f->host, b.io, b.addr + offset, value, size_log2);
}


//#pragma mark - INTx

uint8_t pci_root_pin(const PciFunc *f, const PciFunc **root)
{
    if (f->pin == 0 || f->pin > 4)
        return 0;
    uint32_t pin = f->pin - 1;
    for (; f->parent; f = f->parent)
        pin = (pin + PCI_SLOT(f->devfn)) % 4;
    *root = f;
    return pin + 1;
}


namespace {

/* "interrupt-map", keyed on the root bus function's address and the pin,
   by the PCI bus binding and the interrupt mapping recommended practice. */
bool fdt_intx(const PciFunc *root, uint8_t pin, IntController **ctrl,
              uint32_t *line)
{
    const PciHost *h = root->host;
    const FdtNode *node = h->dev->fdt;
    const FdtProp *map = fdt_prop(node, "interrupt-map");
    const FdtProp *mask_p = fdt_prop(node, "interrupt-map-mask");
    uint32_t key[4] = {root->bus << 16 | root->devfn << 8, 0, 0, pin};
    uint32_t mask[4] = {~0u, ~0u, ~0u, ~0u};
    for (int i = 0; mask_p && i < 4 && 4u * i + 4 <= mask_p->len; i++)
        mask[i] = be32(mask_p->data + 4 * i);
    for (uint32_t off = 0; map && off + 20 <= map->len;) {
        const uint8_t *p = map->data + off;
        bool match = true;
        for (int i = 0; i < 4; i++)
            match &= ((be32(p + 4 * i) ^ key[i]) & mask[i]) == 0;
        const FdtNode *parent = fdt_find_phandle(be32(p + 16));
        if (parent == nullptr)
            return false;
        uint32_t pac = 0, pic = 1;
        fdt_prop_u32(parent, "#address-cells", &pac);
        fdt_prop_u32(parent, "#interrupt-cells", &pic);
        const uint8_t *spec = p + 20 + 4 * pac;
        off += 20 + 4 * (pac + pic);
        if (!match || off > map->len)
            continue;
        *line = be32(spec);
        if (IntController *c = Plic::Get(parent)) {
            *ctrl = c;
            return true;
        }
        if (IntController *c = Aplic::Get(parent)) {
            *ctrl = c;
            return true;
        }
        /* the INTx domain nested in a PLDA bridge */
        if (parent->parent == node && h->msi) {
            *ctrl = h->msi;
            return true;
        }
        return false;
    }
    return false;
}

/* _PRT: (slot << 16 | 0xffff, pin from 0, source, index), where a source
   of 0 makes the index a GSI. */
bool acpi_intx(const PciFunc *root, uint8_t pin, IntController **ctrl,
               uint32_t *line)
{
    const AcpiNode *prt = acpi_child(root->host->dev->acpi, "_PRT");
    if (prt == nullptr || prt->kind != ACPI_NAME)
        return false;
    AmlValue entry;
    for (uint32_t i = 0; aml_package_element(prt->value, i, &entry); i++) {
        AmlValue addr, epin, src, index;
        if (!aml_package_element(entry, 0, &addr) ||
            !aml_package_element(entry, 1, &epin) ||
            !aml_package_element(entry, 2, &src) ||
            !aml_package_element(entry, 3, &index))
            continue;
        if (addr.integer >> 16 != PCI_SLOT(root->devfn) ||
            epin.integer != (uint64_t)pin - 1 || src.type != AML_INTEGER)
            continue;
        uint32_t gsi = index.integer;
        if (IoApic *io = IoApic::ForGsi(gsi)) {
            *ctrl = io;
            *line = gsi;
            return true;
        }
        if (I8259 *pic = I8259::Get(); pic && gsi < 16) {
            *ctrl = pic;
            *line = gsi;
            return true;
        }
        return false;
    }
    return false;
}

}


bool pci_intx_resolve(const PciFunc *f, IntController **ctrl, uint32_t *line)
{
    const PciFunc *root;
    uint8_t pin = pci_root_pin(f, &root);
    if (pin == 0)
        return false;
    if (f->host->dev->fdt)
        return fdt_intx(root, pin, ctrl, line);
    return acpi_intx(root, pin, ctrl, line);
}


bool pci_intx_route(const PciFunc *f, IrqHandler fn, void *ctx)
{
    IntController *c;
    uint32_t line;
    /* PCI interrupts are level triggered */
    return pci_intx_resolve(f, &c, &line) && c->Route(line, fn, ctx, true);
}


void pci_intx_unroute(const PciFunc *f)
{
    IntController *c;
    uint32_t line;
    if (pci_intx_resolve(f, &c, &line))
        c->Unroute(line);
}
