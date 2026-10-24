/*
 * RISC-V IMSIC
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
#include "imsic.h"

#include "heap.h"

#ifdef __riscv
#include "riscv.h"

namespace {

Imsic *sMine;
bool sLooked;

}


Imsic *Imsic::Mine()
{
    if (sLooked)
        return sMine;
    sLooked = true;
    uint32_t intc = hart_intc_phandle(gBoot.boot_cpu);
    uint32_t cause = riscv_ext_irq();
    for (const FdtNode *n = fdt_find_compatible("riscv,imsics"); n;
         n = fdt_find_compatible("riscv,imsics", n)) {
        const FdtProp *ext = fdt_prop(n, "interrupts-extended");
        for (uint32_t off = 0; ext && off + 8 <= ext->len; off += 8) {
            if (be32(ext->data + off) == intc &&
                be32(ext->data + off + 4) == cause) {
                void *mem = boot_alloc(sizeof(Imsic), alignof(Imsic));
                sMine = new (mem) Imsic(n);
                intc_register(sMine);
                return sMine;
            }
        }
    }
    return nullptr;
}


Imsic::Imsic(const FdtNode *node):
    fNode(node)
{
    fdt_prop_u32(node, "riscv,num-ids", &fIds);
    if (fIds >= ARRAY_SIZE(fRoutes))
        fIds = ARRAY_SIZE(fRoutes) - 1;
}


uint64_t Imsic::PageOf(uint32_t hart) const
{
    /* a page per hart, in the order the harts are listed */
    uint64_t base, size;
    if (!fdt_reg(fNode, 0, &base, &size))
        return 0;
    const FdtProp *ext = fdt_prop(fNode, "interrupts-extended");
    uint32_t intc = hart_intc_phandle(hart);
    for (uint32_t i = 0; ext && 8 * i + 8 <= ext->len; i++) {
        if (be32(ext->data + 8 * i) == intc)
            return base + 0x1000ull * i;
    }
    return 0;
}


/* The CSRs by number, since the assembler knows their names only with the
   AIA extensions in -march: miselect 0x350, mireg 0x351, mtopei 0x35c, and
   the supervisor's at 0x150, 0x151 and 0x15c. */
unsigned long Imsic::ReadReg(unsigned sel) const
{
    if (riscv_machine_mode()) {
        CSR_WRITE(0x350, sel);
        return CSR_READ(0x351);
    }
    CSR_WRITE(0x150, sel);
    return CSR_READ(0x151);
}


void Imsic::WriteReg(unsigned sel, unsigned long value) const
{
    if (riscv_machine_mode()) {
        CSR_WRITE(0x350, sel);
        CSR_WRITE(0x351, value);
    } else {
        CSR_WRITE(0x150, sel);
        CSR_WRITE(0x151, value);
    }
}


unsigned long Imsic::Topei() const
{
    return riscv_machine_mode() ? CSR_READ(0x35c) : CSR_READ(0x15c);
}


void Imsic::ClaimTop() const
{
    if (riscv_machine_mode())
        CSR_WRITE(0x35c, 0);
    else
        CSR_WRITE(0x15c, 0);
}


unsigned Imsic::SelectFor(unsigned base, uint32_t id)
{
    /* XLEN identities a register; RV64 numbers only the even ones */
    unsigned reg = id / __riscv_xlen;
    return base + (__riscv_xlen == 64 ? 2 * reg : reg);
}


unsigned Imsic::BitFor(uint32_t id)
{
    return id % __riscv_xlen;
}


bool Imsic::Pending(uint32_t id) const
{
    return (ReadReg(SelectFor(ISEL_EIP0, id)) >> BitFor(id)) & 1;
}


void Imsic::SetPending(uint32_t id, bool on) const
{
    unsigned sel = SelectFor(ISEL_EIP0, id);
    unsigned long v = ReadReg(sel);
    v = on ? v | 1ul << BitFor(id) : v & ~(1ul << BitFor(id));
    WriteReg(sel, v);
}


bool Imsic::Enabled(uint32_t id) const
{
    return (ReadReg(SelectFor(ISEL_EIE0, id)) >> BitFor(id)) & 1;
}


void Imsic::SetEnabled(uint32_t id, bool on) const
{
    unsigned sel = SelectFor(ISEL_EIE0, id);
    unsigned long v = ReadReg(sel);
    v = on ? v | 1ul << BitFor(id) : v & ~(1ul << BitFor(id));
    WriteReg(sel, v);
}


void Imsic::Dispatch(void *self)
{
    Imsic *f = (Imsic *)self;
    for (;;) {
        uint32_t id = f->Topei() >> 16;
        if (id == 0)
            break;
        f->ClaimTop();
        if (id <= f->fIds && f->fRoutes[id].fn)
            f->fRoutes[id].fn(f->fRoutes[id].ctx);
    }
}


bool Imsic::Route(uint32_t line, IrqHandler fn, void *ctx)
{
    if (line == 0 || line > fIds)
        return false;
    fRoutes[line] = {fn, ctx};
    SetEnabled(line, true);
    WriteReg(ISEL_EITHRESHOLD, 0);
    WriteReg(ISEL_EIDELIVERY, 1);
    arch_set_irq_handler(riscv_ext_irq(), Dispatch, this);
    riscv_ie_set(1ul << riscv_ext_irq());
    return true;
}


void Imsic::Unroute(uint32_t line)
{
    if (line == 0 || line > fIds)
        return;
    SetEnabled(line, false);
    fRoutes[line] = {};
}


void Imsic::Reset()
{
    WriteReg(ISEL_EIDELIVERY, 0);
    WriteReg(ISEL_EITHRESHOLD, 0);
    for (uint32_t id = 0; id <= fIds; id += __riscv_xlen) {
        WriteReg(SelectFor(ISEL_EIE0, id), 0);
        WriteReg(SelectFor(ISEL_EIP0, id), 0);
    }
    for (auto &r : fRoutes)
        r = {};
}

#else

Imsic *Imsic::Mine()
{
    return nullptr;
}

#endif
