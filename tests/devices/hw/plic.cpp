/*
 * RISC-V PLIC
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
#include "plic.h"

#include "heap.h"
#include "mmio.h"

#ifdef __riscv
#include "riscv.h"

namespace {

Plic *sPlics[4];
const FdtNode *sNodes[4];

}


Plic *Plic::Get(const FdtNode *node)
{
    if (!fdt_is_compatible(node, "riscv,plic0") &&
        !fdt_is_compatible(node, "sifive,plic-1.0.0"))
        return nullptr;
    for (int i = 0; i < (int)ARRAY_SIZE(sNodes); i++) {
        if (sNodes[i] == node)
            return sPlics[i];
        if (sNodes[i] == nullptr) {
            for (int j = 0; j < registry_count(); j++) {
                if (registry_at(j)->fdt != node)
                    continue;
                void *mem = boot_alloc(sizeof(Plic), alignof(Plic));
                sPlics[i] = new (mem) Plic(registry_at(j));
                sNodes[i] = node;
                /* reset between tests whether routed through or driven
                   directly */
                intc_register(sPlics[i]);
                return sPlics[i];
            }
            return nullptr;
        }
    }
    return nullptr;
}


Plic *Plic::First()
{
    for (int i = 0; i < registry_count(); i++) {
        const HwDevice *d = registry_at(i);
        if (d->fdt && (hw_is(d, "riscv,plic0") || hw_is(d, "sifive,plic-1.0.0")))
            return Get(d->fdt);
    }
    return nullptr;
}


Plic::Plic(const HwDevice *dev):
    fDevice(dev)
{
    if (dev->nregions > 0)
        fBase = dev->regions[0].base;
    uint32_t ndev = 0;
    fdt_prop_u32(dev->fdt, "riscv,ndev", &ndev);
    fSources = min(ndev, (uint32_t)ARRAY_SIZE(fRoutes) - 1);
    const FdtProp *ext = fdt_prop(dev->fdt, "interrupts-extended");
    fContexts = ext ? ext->len / 8 : 0;
    fMyContext = ContextFor(gBoot.boot_cpu, riscv_machine_mode());
}


int Plic::ContextFor(uint32_t hart, bool machine) const
{
    const FdtProp *ext = fdt_prop(fDevice->fdt, "interrupts-extended");
    uint32_t intc = hart_intc_phandle(hart);
    for (int ctx = 0; ext && ctx < fContexts; ctx++) {
        if (be32(ext->data + 8 * ctx) == intc &&
            be32(ext->data + 8 * ctx + 4) ==
                (uint32_t)(machine ? IRQ_M_EXT : IRQ_S_EXT))
            return ctx;
    }
    return -1;
}


uint32_t Plic::Read(uint32_t offset) const
{
    return mmio_read32(fBase + offset);
}


void Plic::Write(uint32_t offset, uint32_t value) const
{
    mmio_write32(fBase + offset, value);
}


void Plic::SetEnabled(int ctx, uint32_t src, bool on) const
{
    uint32_t w = Read(PLIC_ENABLE(ctx, src / 32));
    w = on ? w | 1u << (src % 32) : w & ~(1u << (src % 32));
    Write(PLIC_ENABLE(ctx, src / 32), w);
}


void Plic::Dispatch(void *self)
{
    Plic *p = (Plic *)self;
    for (;;) {
        uint32_t src = p->Claim(p->fMyContext);
        if (src == 0)
            break;
        if (src <= p->fSources && p->fRoutes[src].fn)
            p->fRoutes[src].fn(p->fRoutes[src].ctx);
        p->Complete(p->fMyContext, src);
    }
}


bool Plic::Route(uint32_t line, IrqHandler fn, void *ctx, bool level)
{
    (void)level; /* the gateways are level triggered */
    if (fMyContext < 0 || line == 0 || line > fSources)
        return false;
    fRoutes[line] = {fn, ctx};
    Write(PLIC_PRIORITY(line), 1);
    Write(PLIC_THRESHOLD(fMyContext), 0);
    SetEnabled(fMyContext, line, true);
    arch_set_irq_handler(riscv_ext_irq(), Dispatch, this);
    riscv_ie_set(1ul << riscv_ext_irq());
    return true;
}


void Plic::Unroute(uint32_t line)
{
    if (fMyContext < 0 || line == 0 || line > fSources)
        return;
    SetEnabled(fMyContext, line, false);
    Write(PLIC_PRIORITY(line), 0);
    fRoutes[line] = {};
}


void Plic::Reset()
{
    for (int ctx = 0; ctx < fContexts; ctx++) {
        for (uint32_t w = 0; w <= fSources / 32; w++)
            Write(PLIC_ENABLE(ctx, w), 0);
        Write(PLIC_THRESHOLD(ctx), 0);
    }
    /* completing a source that is not in service changes nothing */
    for (uint32_t src = 1; src <= fSources; src++) {
        Write(PLIC_PRIORITY(src), 0);
        if (fMyContext >= 0)
            Complete(fMyContext, src);
        fRoutes[src] = {};
    }
}

#else

Plic *Plic::Get(const FdtNode *)
{
    return nullptr;
}

Plic *Plic::First()
{
    return nullptr;
}

#endif
