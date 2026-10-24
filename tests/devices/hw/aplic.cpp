/*
 * RISC-V APLIC
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
#include "aplic.h"

#include "heap.h"
#include "imsic.h"
#include "mmio.h"

#ifdef __riscv
#include "riscv.h"

namespace {

Aplic *sAplic;

/* Whichever node lists 'child' in its riscv,children. */
const FdtNode *parent_domain(const FdtNode *child)
{
    for (const FdtNode *n = fdt_root(); n; n = fdt_next(n)) {
        if (!fdt_is_compatible(n, "riscv,aplic"))
            continue;
        const FdtProp *c = fdt_prop(n, "riscv,children");
        for (uint32_t off = 0; c && off + 4 <= c->len; off += 4) {
            if (be32(c->data + off) == child->phandle)
                return n;
        }
    }
    return nullptr;
}

const FdtNode *child_domain(const FdtNode *root)
{
    uint32_t ph;
    return fdt_prop_u32(root, "riscv,children", &ph) ?
        fdt_find_phandle(ph) : nullptr;
}

}


Aplic *Aplic::Get(const FdtNode *node)
{
    if (!fdt_is_compatible(node, "riscv,aplic"))
        return nullptr;
    if (sAplic == nullptr) {
        const FdtNode *root = parent_domain(node);
        const FdtNode *child = node;
        if (root == nullptr) {
            root = node;
            child = child_domain(node);
        }
        void *mem = boot_alloc(sizeof(Aplic), alignof(Aplic));
        sAplic = new (mem) Aplic(root, child);
        intc_register(sAplic);
    }
    return sAplic;
}


Aplic *Aplic::First()
{
    const FdtNode *n = fdt_root() ? fdt_find_compatible("riscv,aplic")
                                  : nullptr;
    return n ? Get(n) : nullptr;
}


Aplic::Aplic(const FdtNode *root, const FdtNode *child)
{
    uint64_t size;
    /* the root is out of reach of a supervisor, whose tree disables it */
    if (root && riscv_machine_mode() && fdt_reg(root, 0, &fRoot.base, &size))
        fRoot.node = root;
    if (child && fdt_reg(child, 0, &fChild.base, &size))
        fChild.node = child;
    const FdtNode *any = fChild.node ? fChild.node : fRoot.node;
    if (any)
        fdt_prop_u32(any, "riscv,num-sources", &fSources);
    if (fSources >= ARRAY_SIZE(fRoutes))
        fSources = ARRAY_SIZE(fRoutes) - 1;
}


const Aplic::Domain &Aplic::Mine() const
{
    return riscv_machine_mode() ? fRoot : fChild;
}


bool Aplic::MsiMode() const
{
    const Domain &d = Mine();
    return d.node && (Read(d, APLIC_DOMAINCFG) & APLIC_DOMAINCFG_DM);
}


int Aplic::HartIndex(const Domain &d) const
{
    if (d.node == nullptr)
        return -1;
    uint32_t intc = hart_intc_phandle(gBoot.boot_cpu);
    const FdtProp *ext = fdt_prop(d.node, "interrupts-extended");
    if (ext == nullptr) {
        /* MSI delivery: hart indexes are the IMSIC's page numbers */
        uint32_t msi;
        const FdtNode *imsic = fdt_prop_u32(d.node, "msi-parent", &msi) ?
            fdt_find_phandle(msi) : nullptr;
        ext = imsic ? fdt_prop(imsic, "interrupts-extended") : nullptr;
    }
    for (uint32_t i = 0; ext && 8 * i + 8 <= ext->len; i++) {
        if (be32(ext->data + 8 * i) == intc)
            return (int)i;
    }
    return -1;
}


uint32_t Aplic::Read(const Domain &d, uint32_t offset) const
{
    return mmio_read32(d.base + offset);
}


void Aplic::Write(const Domain &d, uint32_t offset, uint32_t value) const
{
    mmio_write32(d.base + offset, value);
}


uint32_t Aplic::Idc(const Domain &d, uint32_t reg) const
{
    return Read(d, APLIC_IDC(HartIndex(d)) + reg);
}


void Aplic::SetIdc(const Domain &d, uint32_t reg, uint32_t value) const
{
    Write(d, APLIC_IDC(HartIndex(d)) + reg, value);
}


void Aplic::Dispatch(void *self)
{
    Aplic *a = (Aplic *)self;
    const Domain &d = a->Mine();
    for (;;) {
        uint32_t src = a->Idc(d, APLIC_CLAIMI) >> 16;
        if (src == 0)
            break;
        if (src <= a->fSources && a->fRoutes[src].fn)
            a->fRoutes[src].fn(a->fRoutes[src].ctx);
    }
}


bool Aplic::Route(uint32_t line, IrqHandler fn, void *ctx, bool level)
{
    const Domain &d = Mine();
    int hart = HartIndex(d);
    if (d.node == nullptr || hart < 0 || line == 0 || line > fSources)
        return false;
    Write(d, APLIC_SOURCECFG(line), level ? APLIC_SM_LEVEL1 : APLIC_SM_EDGE1);
    if (MsiMode()) {
        /* the source's own number as the identity */
        Imsic *imsic = Imsic::Mine();
        if (imsic == nullptr || !imsic->Route(line, fn, ctx))
            return false;
        Write(d, APLIC_TARGET(line), APLIC_TARGET_HART(hart) | line);
    } else {
        fRoutes[line] = {fn, ctx};
        Write(d, APLIC_TARGET(line), APLIC_TARGET_HART(hart) | 1);
        SetIdc(d, APLIC_ITHRESHOLD, 0);
        SetIdc(d, APLIC_IDELIVERY, 1);
        arch_set_irq_handler(riscv_ext_irq(), Dispatch, this);
        riscv_ie_set(1ul << riscv_ext_irq());
    }
    Write(d, APLIC_SETIENUM, line);
    Write(d, APLIC_DOMAINCFG, Read(d, APLIC_DOMAINCFG) | APLIC_DOMAINCFG_IE);
    return true;
}


void Aplic::Unroute(uint32_t line)
{
    const Domain &d = Mine();
    if (d.node == nullptr || line == 0 || line > fSources)
        return;
    Write(d, APLIC_CLRIENUM, line);
    fRoutes[line] = {};
    if (MsiMode() && Imsic::Mine())
        Imsic::Mine()->Unroute(line);
}


void Aplic::Reset()
{
    /* the child first: the root's sources going inactive would hide its
       registers */
    const Domain *domains[] = {&fChild, &fRoot};
    for (const Domain *d : domains) {
        if (d->node == nullptr)
            continue;
        Write(*d, APLIC_DOMAINCFG, 0);
        for (uint32_t src = 1; src <= fSources; src++) {
            Write(*d, APLIC_CLRIENUM, src);
            Write(*d, APLIC_SOURCECFG(src), 0);
        }
        if (!(Read(*d, APLIC_DOMAINCFG) & APLIC_DOMAINCFG_DM) &&
            HartIndex(*d) >= 0) {
            SetIdc(*d, APLIC_IDELIVERY, 0);
            SetIdc(*d, APLIC_IFORCE, 0);
            SetIdc(*d, APLIC_ITHRESHOLD, 0);
        }
    }
    for (auto &r : fRoutes)
        r = {};
}

#else

Aplic *Aplic::Get(const FdtNode *)
{
    return nullptr;
}

Aplic *Aplic::First()
{
    return nullptr;
}

#endif
