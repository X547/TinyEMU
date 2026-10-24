/*
 * The devices the firmware tables describe
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
#include "registry.h"

#include "format.h"
#include "heap.h"

#define MAX_DEVICES 256

namespace {

HwDevice *sDevices[MAX_DEVICES];
int sCount;

HwDevice *add_device()
{
    if (sCount == MAX_DEVICES)
        panic("more than %d devices", MAX_DEVICES);
    HwDevice *d = (HwDevice *)boot_alloc(sizeof(HwDevice));
    sDevices[sCount++] = d;
    return d;
}

char *dup(const char *s, size_t len)
{
    char *p = (char *)boot_alloc(len + 1, 1);
    memcpy(p, s, len);
    return p;
}


//#pragma mark - FDT

/* The interrupt parent, inherited down the tree. */
const FdtNode *irq_parent(const FdtNode *node)
{
    for (const FdtNode *n = node; n; n = n->parent) {
        uint32_t ph;
        if (fdt_prop_u32(n, "interrupt-parent", &ph))
            return fdt_find_phandle(ph);
    }
    return nullptr;
}

uint32_t irq_cells(const FdtNode *ctrl)
{
    uint32_t n = 1;
    if (ctrl)
        fdt_prop_u32(ctrl, "#interrupt-cells", &n);
    return n;
}

void add_irq(HwDevice *d, uint32_t parent, const uint8_t *cells, uint32_t n)
{
    if (d->nirqs == HW_MAX_IRQS)
        return;
    HwIrq &irq = d->irqs[d->nirqs++];
    irq.parent = parent;
    irq.ncells = min(n, (uint32_t)ARRAY_SIZE(irq.cells));
    for (int i = 0; i < irq.ncells; i++)
        irq.cells[i] = be32(cells + 4 * i);
    irq.line = irq.cells[0];
}

void fdt_irqs(HwDevice *d, const FdtNode *node)
{
    const FdtProp *ext = fdt_prop(node, "interrupts-extended");
    if (ext) {
        for (uint32_t off = 0; off + 4 <= ext->len;) {
            uint32_t ph = be32(ext->data + off);
            uint32_t n = irq_cells(fdt_find_phandle(ph));
            off += 4;
            if (off + 4 * n > ext->len)
                break;
            add_irq(d, ph, ext->data + off, n);
            off += 4 * n;
        }
        return;
    }
    const FdtProp *ints = fdt_prop(node, "interrupts");
    if (ints == nullptr)
        return;
    const FdtNode *parent = irq_parent(node);
    uint32_t n = irq_cells(parent);
    uint32_t ph = parent ? parent->phandle : 0;
    for (uint32_t off = 0; n && off + 4 * n <= ints->len; off += 4 * n)
        add_irq(d, ph, ints->data + off, n);
}

void build_fdt()
{
    for (const FdtNode *n = fdt_root(); n; n = fdt_next(n)) {
        const FdtProp *compat = fdt_prop(n, "compatible");
        if (compat == nullptr || !fdt_is_enabled(n))
            continue;
        HwDevice *d = add_device();
        d->name = n->name;
        d->fdt = n;
        for (uint32_t i = 0; i < compat->len && d->ncompat < HW_MAX_COMPAT;) {
            const char *s = (const char *)compat->data + i;
            d->compat[d->ncompat++] = s;
            i += strnlen(s, compat->len - i) + 1;
        }
        for (int i = 0; d->nregions < HW_MAX_REGIONS; i++) {
            uint64_t base, size;
            if (!fdt_reg(n, i, &base, &size))
                break;
            d->regions[d->nregions++] = {HW_MMIO, base, size};
        }
        fdt_irqs(d, n);
    }
}


//#pragma mark - ACPI

void add_id(HwDevice *d, const AmlValue &v)
{
    char buf[16];
    if (d->ncompat < HW_MAX_COMPAT && aml_id_string(v, buf, sizeof(buf)))
        d->compat[d->ncompat++] = dup(buf, strlen(buf));
}

void add_resource(void *ctx, const AcpiResource &r)
{
    HwDevice *d = (HwDevice *)ctx;
    if ((r.kind == ACPI_RES_MEMORY || r.kind == ACPI_RES_IO) &&
        d->nregions < HW_MAX_REGIONS) {
        d->regions[d->nregions++] = {
            r.kind == ACPI_RES_MEMORY ? HW_MMIO : HW_IO, r.base, r.length};
    } else if ((r.kind == ACPI_RES_IRQ || r.kind == ACPI_RES_INTERRUPT) &&
               d->nirqs < HW_MAX_IRQS) {
        HwIrq &irq = d->irqs[d->nirqs++];
        irq.line = r.base;
        irq.isa = r.kind == ACPI_RES_IRQ;
        irq.level = r.level;
        irq.active_low = r.active_low;
    }
}

void build_acpi()
{
    for (const AcpiNode *n = acpi_root(); n; n = acpi_next(n)) {
        if (n->kind != ACPI_DEVICE)
            continue;
        const AcpiNode *hid = acpi_child(n, "_HID");
        if (hid == nullptr || hid->kind != ACPI_NAME)
            continue;
        HwDevice *d = add_device();
        char path[64];
        acpi_path(n, path, sizeof(path));
        d->name = dup(path, strlen(path));
        d->acpi = n;
        add_id(d, hid->value);
        const AcpiNode *cid = acpi_child(n, "_CID");
        if (cid && cid->value.type == AML_PACKAGE) {
            AmlValue e;
            for (uint32_t i = 0; aml_package_element(cid->value, i, &e); i++)
                add_id(d, e);
        } else if (cid) {
            add_id(d, cid->value);
        }
        const AcpiNode *crs = acpi_child(n, "_CRS");
        if (crs && crs->kind == ACPI_NAME)
            acpi_parse_resources(crs->value, add_resource, d);
    }
}

}


void registry_build()
{
    if (fdt_root())
        build_fdt();
    if (acpi_present())
        build_acpi();
}


int registry_count()
{
    return sCount;
}


const HwDevice *registry_at(int index)
{
    return sDevices[index];
}


bool hw_is(const HwDevice *dev, const char *compat)
{
    for (int i = 0; i < dev->ncompat; i++) {
        if (strcmp(dev->compat[i], compat) == 0)
            return true;
    }
    return false;
}


const HwDevice *registry_find(const char *compat, int index)
{
    for (int i = 0; i < sCount; i++) {
        if (hw_is(sDevices[i], compat) && index-- == 0)
            return sDevices[i];
    }
    return nullptr;
}


const HwRegion *hw_region(const HwDevice *dev, HwRegionKind kind, int index)
{
    for (int i = 0; i < dev->nregions; i++) {
        if (dev->regions[i].kind == kind && index-- == 0)
            return &dev->regions[i];
    }
    return nullptr;
}
