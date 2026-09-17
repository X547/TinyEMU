/*
 * RISC-V Advanced Platform-Level Interrupt Controller
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
#include <assert.h>
#include <stdlib.h>
#include <string.h>

#include "bits.h"
#include "cutils.h"
#include "fdt.h"
#include "aplic.h"

/* register layout of a domain's control region */
#define APLIC_DOMAINCFG       0x0000
#define APLIC_SOURCECFG_BASE  0x0000 /* sourcecfg[1] is at 0x0004 */
#define APLIC_MMSIADDRCFG     0x1bc0
#define APLIC_MMSIADDRCFGH    0x1bc4
#define APLIC_SMSIADDRCFG     0x1bc8
#define APLIC_SMSIADDRCFGH    0x1bcc
#define APLIC_SETIP_BASE      0x1c00
#define APLIC_SETIPNUM        0x1cdc
#define APLIC_IN_CLRIP_BASE   0x1d00
#define APLIC_CLRIPNUM        0x1ddc
#define APLIC_SETIE_BASE      0x1e00
#define APLIC_SETIENUM        0x1edc
#define APLIC_CLRIE_BASE      0x1f00
#define APLIC_CLRIENUM        0x1fdc
#define APLIC_SETIPNUM_LE     0x2000
#define APLIC_SETIPNUM_BE     0x2004
#define APLIC_GENMSI          0x3000
#define APLIC_TARGET_BASE     0x3000 /* target[1] is at 0x3004 */
#define APLIC_IDC_BASE        0x4000
#define APLIC_IDC_SIZE        32

#define APLIC_BITMAP_WORDS    32 /* setip, in_clrip, setie, clrie */

/* IDC structure */
#define APLIC_IDC_IDELIVERY   0x00
#define APLIC_IDC_IFORCE      0x04
#define APLIC_IDC_ITHRESHOLD  0x08
#define APLIC_IDC_TOPI        0x18
#define APLIC_IDC_CLAIMI      0x1c

#define APLIC_DOMAINCFG_RO    0x80000000
#define APLIC_DOMAINCFG_IE    bit_at(8)
#define APLIC_DOMAINCFG_DM    bit_at(2)

#define APLIC_SOURCECFG_D     bit_at(10)
#define APLIC_SOURCECFG_SM    bit_mask(3)

/* source modes */
#define APLIC_SM_INACTIVE     0
#define APLIC_SM_DETACHED     1
#define APLIC_SM_EDGE1        4
#define APLIC_SM_EDGE0        5
#define APLIC_SM_LEVEL1       6
#define APLIC_SM_LEVEL0       7

#define APLIC_TARGET_HART_SHIFT 18
#define APLIC_TARGET_HART_BITS  14
#define APLIC_TARGET_IPRIO_BITS 8
#define APLIC_TARGET_EIID_BITS  11

#define APLIC_GENMSI_MASK \
    (field_mask(APLIC_TARGET_HART_SHIFT, APLIC_TARGET_HART_BITS) | \
     bit_mask(APLIC_TARGET_EIID_BITS))

#define APLIC_MSIADDRCFGH_L          bit_at(31)
#define APLIC_MSIADDRCFGH_LHXW_SHIFT 12

/* external interrupt causes at the harts' interrupt controllers */
#define IRQ_M_EXT 11
#define IRQ_S_EXT 9


static inline bool bitmap_get(const uint32_t *map, uint32_t n)
{
    return get_bit(map[n / 32], n % 32);
}

static inline void bitmap_put(uint32_t *map, uint32_t n, bool val)
{
    map[n / 32] = set_bit(map[n / 32], n % 32, val);
}

static bool mode_is_level(uint32_t mode)
{
    return mode == APLIC_SM_LEVEL1 || mode == APLIC_SM_LEVEL0;
}


APLIC::APLIC(PhysMemoryMap *mem_map, HartIrqTarget *harts, int hart_count,
             int num_sources, const AplicMsiLayout *msi):
    fMemMap(mem_map),
    fHarts(harts),
    fHartCount(hart_count),
    fNumSources(num_sources),
    fWords((num_sources + 1 + 31) / 32),
    fMsiMode(msi != nullptr)
{
    assert(num_sources < 1024);
    assert(APLIC_IDC_BASE + hart_count * APLIC_IDC_SIZE <= APLIC_SIZE);

    if (msi != nullptr) {
        fMsi = *msi;
    }
    fLevel = new uint32_t[fWords] {};
    for (int i = 0; i < 2; i++) {
        Domain &d = fDomains[i];
        d.supervisor = i == 1;
        d.sourcecfg = new uint32_t[num_sources + 1] {};
        d.target = new uint32_t[num_sources + 1] {};
        d.pending = new uint32_t[fWords] {};
        d.enable = new uint32_t[fWords] {};
        d.idc = new Idc[hart_count] {};
    }
    fDomains[0].child = &fDomains[1];
    fDomains[1].parent = &fDomains[0];
}


APLIC::~APLIC()
{
    delete[] fLevel;
    for (Domain &d: fDomains) {
        delete[] d.sourcecfg;
        delete[] d.target;
        delete[] d.pending;
        delete[] d.enable;
        delete[] d.idc;
    }
}


/* A source a domain's parent has not delegated to it looks unimplemented. */
bool APLIC::Visible(const Domain &d, uint32_t irq) const
{
    if (irq == 0 || irq > (uint32_t)fNumSources)
        return false;
    if (d.parent == nullptr)
        return true;
    return Visible(*d.parent, irq) &&
        (d.parent->sourcecfg[irq] & APLIC_SOURCECFG_D) != 0;
}


/* The source mode, or inactive when the source is delegated further down. */
uint32_t APLIC::Mode(const Domain &d, uint32_t irq) const
{
    if (!Visible(d, irq) || (d.sourcecfg[irq] & APLIC_SOURCECFG_D))
        return APLIC_SM_INACTIVE;
    return d.sourcecfg[irq] & APLIC_SOURCECFG_SM;
}


bool APLIC::Rectified(const Domain &d, uint32_t irq) const
{
    uint32_t mode = Mode(d, irq);
    bool inverted = mode == APLIC_SM_EDGE0 || mode == APLIC_SM_LEVEL0;

    if (mode < APLIC_SM_EDGE1)
        return false;
    return bitmap_get(fLevel, irq) != inverted;
}


/* The pending and enable bits and the target of an inactive source are
   read-only zero. */
void APLIC::Deactivate(Domain &d, uint32_t irq)
{
    bitmap_put(d.pending, irq, false);
    bitmap_put(d.enable, irq, false);
    d.target[irq] = 0;
}


void APLIC::WriteSourcecfg(Domain &d, uint32_t irq, uint32_t val)
{
    uint32_t mode;

    if (!Visible(d, irq))
        return;

    if (val & APLIC_SOURCECFG_D) {
        /* the one child is index 0; a leaf domain cannot delegate */
        val = d.child != nullptr ? APLIC_SOURCECFG_D : 0;
    } else {
        val &= APLIC_SOURCECFG_SM;
        if (val == 2 || val == 3)
            val = APLIC_SM_INACTIVE; /* reserved */
    }
    d.sourcecfg[irq] = val;

    mode = Mode(d, irq);
    if (mode == APLIC_SM_INACTIVE) {
        Deactivate(d, irq);
    } else if (!fMsiMode &&
               get_bits(d.target[irq], 0, APLIC_TARGET_IPRIO_BITS) == 0) {
        d.target[irq] = 1; /* priority 0 is not a legal value */
    }

    /* A source no longer delegated vanishes from the child, which keeps
       reading zero until it is delegated and configured again. */
    if (d.child != nullptr && !(val & APLIC_SOURCECFG_D)) {
        d.child->sourcecfg[irq] = 0;
        Deactivate(*d.child, irq);
    }

    if (mode_is_level(mode)) {
        bitmap_put(d.pending, irq, Rectified(d, irq));
    }
}


void APLIC::WriteTarget(Domain &d, uint32_t irq, uint32_t val)
{
    uint32_t hart = get_bits(val, APLIC_TARGET_HART_SHIFT,
                             APLIC_TARGET_HART_BITS);

    if (Mode(d, irq) == APLIC_SM_INACTIVE)
        return;
    if (fMsiMode) {
        /* no guest interrupt files, so the guest index is read-only zero */
        d.target[irq] = (hart << APLIC_TARGET_HART_SHIFT) |
            get_bits(val, 0, APLIC_TARGET_EIID_BITS);
    } else {
        uint32_t prio = get_bits(val, 0, APLIC_TARGET_IPRIO_BITS);
        d.target[irq] = (hart << APLIC_TARGET_HART_SHIFT) | (prio ? prio : 1);
    }
}


/* setip, setipnum */
void APLIC::SetPendingByWrite(Domain &d, uint32_t irq)
{
    uint32_t mode = Mode(d, irq);

    if (mode == APLIC_SM_INACTIVE)
        return;
    if (mode_is_level(mode)) {
        /* a level source follows its wire in direct delivery mode, and can
           only be made pending while the wire is asserted in MSI mode */
        if (!fMsiMode || !Rectified(d, irq))
            return;
    }
    bitmap_put(d.pending, irq, true);
}


/* in_clrip, clripnum, and a claim */
void APLIC::ClearPendingByWrite(Domain &d, uint32_t irq)
{
    uint32_t mode = Mode(d, irq);

    if (mode == APLIC_SM_INACTIVE || (mode_is_level(mode) && !fMsiMode))
        return;
    bitmap_put(d.pending, irq, false);
}


void APLIC::SetEnable(Domain &d, uint32_t irq, bool enable)
{
    if (Mode(d, irq) == APLIC_SM_INACTIVE)
        return;
    bitmap_put(d.enable, irq, enable);
}


/* The top pending and enabled source directed at 'hart' under its threshold,
   in topi format, or 0. Lower priority numbers win, then lower sources. */
uint32_t APLIC::Topi(const Domain &d, int hart) const
{
    uint32_t threshold = d.idc[hart].ithreshold;
    uint32_t best = 0, best_prio = 0;

    for (uint32_t irq = 1; irq <= (uint32_t)fNumSources; irq++) {
        uint32_t target = d.target[irq];
        uint32_t prio = get_bits(target, 0, APLIC_TARGET_IPRIO_BITS);

        if (!bitmap_get(d.pending, irq) || !bitmap_get(d.enable, irq))
            continue;
        if (get_bits(target, APLIC_TARGET_HART_SHIFT,
                     APLIC_TARGET_HART_BITS) != (uint32_t)hart)
            continue;
        if (threshold != 0 && prio >= threshold)
            continue;
        if (best == 0 || prio < best_prio) {
            best = irq;
            best_prio = prio;
        }
    }
    return best != 0 ? (best << 16) | best_prio : 0;
}


/* The MSI addresses are fixed by the machine, so the configuration registers
   are locked from reset and show that layout. */
uint32_t APLIC::MsiAddrCfg(uint32_t offset) const
{
    uint64_t m_ppn = fMsi.m_base >> 12;
    uint64_t s_ppn = fMsi.s_base >> 12;

    switch (offset) {
    case APLIC_MMSIADDRCFG:
        return m_ppn;
    case APLIC_MMSIADDRCFGH:
        return APLIC_MSIADDRCFGH_L |
            (fMsi.hart_index_bits << APLIC_MSIADDRCFGH_LHXW_SHIFT) |
            get_bits(m_ppn, 32, 12);
    case APLIC_SMSIADDRCFG:
        return s_ppn;
    case APLIC_SMSIADDRCFGH:
        return get_bits(s_ppn, 32, 12);
    default:
        return 0;
    }
}


/* The address formula of the MSI address configuration registers, with
   every shift and the group index width zero. A supervisor level domain uses
   the same hart numbering as the machine level one. */
void APLIC::SendMsi(const Domain &d, uint32_t hart, uint32_t eiid)
{
    uint64_t base = d.supervisor ? fMsi.s_base : fMsi.m_base;
    uint32_t h = get_bits(hart, 0, fMsi.hart_index_bits);

    fMemMap->IoWrite(base | ((uint64_t)h << 12), eiid, 2);
}


void APLIC::Update()
{
    for (Domain &d: fDomains) {
        if (fMsiMode) {
            if (!d.ie)
                continue;
            for (uint32_t irq = 1; irq <= (uint32_t)fNumSources; irq++) {
                if (!bitmap_get(d.pending, irq) || !bitmap_get(d.enable, irq))
                    continue;
                bitmap_put(d.pending, irq, false);
                SendMsi(d, d.target[irq] >> APLIC_TARGET_HART_SHIFT,
                        get_bits(d.target[irq], 0, APLIC_TARGET_EIID_BITS));
            }
        } else {
            for (int hart = 0; hart < fHartCount; hart++) {
                const Idc &idc = d.idc[hart];
                bool level = d.ie && idc.idelivery &&
                    (idc.iforce || Topi(d, hart) != 0);
                fHarts->SetExternalIrq(hart, d.supervisor, level);
            }
        }
    }
}


uint32_t APLIC::Read(Domain &d, uint32_t offset)
{
    uint32_t irq, k;

    if (offset == APLIC_DOMAINCFG) {
        return APLIC_DOMAINCFG_RO | (d.ie ? APLIC_DOMAINCFG_IE : 0) |
            (fMsiMode ? APLIC_DOMAINCFG_DM : 0);
    }
    if (offset < APLIC_MMSIADDRCFG) {
        irq = (offset - APLIC_SOURCECFG_BASE) / 4;
        return Visible(d, irq) ? d.sourcecfg[irq] : 0;
    }
    if (offset <= APLIC_SMSIADDRCFGH) {
        return fMsiMode && d.parent == nullptr ? MsiAddrCfg(offset) : 0;
    }
    if (offset >= APLIC_SETIP_BASE &&
        offset < APLIC_SETIP_BASE + APLIC_BITMAP_WORDS * 4) {
        k = (offset - APLIC_SETIP_BASE) / 4;
        return k < (uint32_t)fWords ? d.pending[k] : 0;
    }
    if (offset >= APLIC_IN_CLRIP_BASE &&
        offset < APLIC_IN_CLRIP_BASE + APLIC_BITMAP_WORDS * 4) {
        uint32_t val = 0;
        k = (offset - APLIC_IN_CLRIP_BASE) / 4;
        for (int bit = 0; bit < 32; bit++) {
            if (Rectified(d, k * 32 + bit))
                val = set_bit(val, bit, true);
        }
        return val;
    }
    if (offset >= APLIC_SETIE_BASE &&
        offset < APLIC_SETIE_BASE + APLIC_BITMAP_WORDS * 4) {
        k = (offset - APLIC_SETIE_BASE) / 4;
        return k < (uint32_t)fWords ? d.enable[k] : 0;
    }
    if (offset == APLIC_GENMSI) {
        return fMsiMode ? d.genmsi : 0;
    }
    if (offset > APLIC_TARGET_BASE && offset < APLIC_IDC_BASE) {
        irq = (offset - APLIC_TARGET_BASE) / 4;
        return Mode(d, irq) != APLIC_SM_INACTIVE ? d.target[irq] : 0;
    }
    if (offset >= APLIC_IDC_BASE && !fMsiMode) {
        int hart = (offset - APLIC_IDC_BASE) / APLIC_IDC_SIZE;
        Idc *idc;
        uint32_t topi;

        if (hart >= fHartCount)
            return 0;
        idc = &d.idc[hart];
        switch ((offset - APLIC_IDC_BASE) % APLIC_IDC_SIZE) {
        case APLIC_IDC_IDELIVERY:
            return idc->idelivery;
        case APLIC_IDC_IFORCE:
            return idc->iforce;
        case APLIC_IDC_ITHRESHOLD:
            return idc->ithreshold;
        case APLIC_IDC_TOPI:
            return Topi(d, hart);
        case APLIC_IDC_CLAIMI:
            topi = Topi(d, hart);
            if (topi != 0) {
                ClearPendingByWrite(d, topi >> 16);
            } else {
                idc->iforce = 0;
            }
            Update();
            return topi;
        }
    }
    /* setipnum, clripnum, setienum, clrienum, clrie and reserved */
    return 0;
}


void APLIC::Write(Domain &d, uint32_t offset, uint32_t val)
{
    uint32_t k;

    if (offset == APLIC_DOMAINCFG) {
        /* DM is fixed and BE is read-only zero */
        d.ie = (val & APLIC_DOMAINCFG_IE) != 0;
    } else if (offset < APLIC_MMSIADDRCFG) {
        WriteSourcecfg(d, (offset - APLIC_SOURCECFG_BASE) / 4, val);
    } else if (offset <= APLIC_SMSIADDRCFGH) {
        return; /* locked */
    } else if (offset >= APLIC_SETIP_BASE &&
               offset < APLIC_SETIP_BASE + APLIC_BITMAP_WORDS * 4) {
        k = (offset - APLIC_SETIP_BASE) / 4;
        for (int bit = 0; bit < 32; bit++) {
            if (get_bit(val, bit))
                SetPendingByWrite(d, k * 32 + bit);
        }
    } else if (offset == APLIC_SETIPNUM || offset == APLIC_SETIPNUM_LE) {
        SetPendingByWrite(d, val);
    } else if (offset >= APLIC_IN_CLRIP_BASE &&
               offset < APLIC_IN_CLRIP_BASE + APLIC_BITMAP_WORDS * 4) {
        k = (offset - APLIC_IN_CLRIP_BASE) / 4;
        for (int bit = 0; bit < 32; bit++) {
            if (get_bit(val, bit))
                ClearPendingByWrite(d, k * 32 + bit);
        }
    } else if (offset == APLIC_CLRIPNUM) {
        ClearPendingByWrite(d, val);
    } else if (offset >= APLIC_SETIE_BASE &&
               offset < APLIC_SETIE_BASE + APLIC_BITMAP_WORDS * 4) {
        k = (offset - APLIC_SETIE_BASE) / 4;
        for (int bit = 0; bit < 32; bit++) {
            if (get_bit(val, bit))
                SetEnable(d, k * 32 + bit, true);
        }
    } else if (offset == APLIC_SETIENUM) {
        SetEnable(d, val, true);
    } else if (offset >= APLIC_CLRIE_BASE &&
               offset < APLIC_CLRIE_BASE + APLIC_BITMAP_WORDS * 4) {
        k = (offset - APLIC_CLRIE_BASE) / 4;
        for (int bit = 0; bit < 32; bit++) {
            if (get_bit(val, bit))
                SetEnable(d, k * 32 + bit, false);
        }
    } else if (offset == APLIC_CLRIENUM) {
        SetEnable(d, val, false);
    } else if (offset == APLIC_GENMSI) {
        if (!fMsiMode)
            return;
        /* sent at once, so it is never busy; IE does not apply */
        d.genmsi = val & APLIC_GENMSI_MASK;
        SendMsi(d, d.genmsi >> APLIC_TARGET_HART_SHIFT,
                get_bits(d.genmsi, 0, APLIC_TARGET_EIID_BITS));
        return;
    } else if (offset > APLIC_TARGET_BASE && offset < APLIC_IDC_BASE) {
        WriteTarget(d, (offset - APLIC_TARGET_BASE) / 4, val);
    } else if (offset >= APLIC_IDC_BASE && !fMsiMode) {
        int hart = (offset - APLIC_IDC_BASE) / APLIC_IDC_SIZE;
        if (hart >= fHartCount)
            return;
        Idc *idc = &d.idc[hart];
        switch ((offset - APLIC_IDC_BASE) % APLIC_IDC_SIZE) {
        case APLIC_IDC_IDELIVERY:
            idc->idelivery = get_bit(val, 0);
            break;
        case APLIC_IDC_IFORCE:
            idc->iforce = get_bit(val, 0);
            break;
        case APLIC_IDC_ITHRESHOLD:
            idc->ithreshold = get_bits(val, 0, APLIC_TARGET_IPRIO_BITS);
            break;
        default:
            return;
        }
    } else {
        return;
    }
    Update();
}


uint32_t APLIC::ReadM(uint32_t offset, int size_log2)
{
    assert(size_log2 == 2);
    return Read(fDomains[0], offset);
}


void APLIC::WriteM(uint32_t offset, uint32_t val, int size_log2)
{
    assert(size_log2 == 2);
    Write(fDomains[0], offset, val);
}


uint32_t APLIC::ReadS(uint32_t offset, int size_log2)
{
    assert(size_log2 == 2);
    return Read(fDomains[1], offset);
}


void APLIC::WriteS(uint32_t offset, uint32_t val, int size_log2)
{
    assert(size_log2 == 2);
    Write(fDomains[1], offset, val);
}


void APLIC::SetIRQ(int irq_num, int level)
{
    uint32_t irq = irq_num;

    if (irq == 0 || irq > (uint32_t)fNumSources)
        return;

    for (Domain &d: fDomains) {
        uint32_t mode = Mode(d, irq);
        bool was, now;

        if (mode < APLIC_SM_EDGE1)
            continue;
        was = Rectified(d, irq);
        bitmap_put(fLevel, irq, level != 0);
        now = Rectified(d, irq);

        if (mode_is_level(mode) && !fMsiMode) {
            bitmap_put(d.pending, irq, now);
        } else if (!was && now) {
            bitmap_put(d.pending, irq, true);
        } else if (mode_is_level(mode) && !now) {
            bitmap_put(d.pending, irq, false);
        }
        Update();
        return;
    }
    /* active nowhere: the wire is still recorded for when it becomes so */
    bitmap_put(fLevel, irq, level != 0);
}


void APLIC::BuildDomainFDT(FDTBuilder &fdt, const Domain &d, uint64_t base,
                           uint32_t phandle, uint32_t child_phandle,
                           const uint32_t *intc_phandle, uint32_t msi_phandle)
{
    fdt.BeginNodeNum("interrupt-controller", base);
    fdt.PropStr("compatible", "riscv,aplic");
    fdt.PropU64Range("reg", base, APLIC_SIZE);
    fdt.PropEmpty("interrupt-controller");
    fdt.PropU32("#interrupt-cells", 2);
    /* so that an "interrupt-map" naming this controller has an unambiguous
       parent specifier length */
    fdt.PropU32("#address-cells", 0);
    fdt.PropU32("riscv,num-sources", fNumSources);

    if (fMsiMode) {
        fdt.PropU32("msi-parent", msi_phandle);
    } else {
        uint32_t *tab = new uint32_t[2 * fHartCount];
        /* IDC n is hart n */
        for (int hart = 0; hart < fHartCount; hart++) {
            tab[2 * hart] = intc_phandle[hart];
            tab[2 * hart + 1] = d.supervisor ? IRQ_S_EXT : IRQ_M_EXT;
        }
        fdt.PropTabU32("interrupts-extended", tab, 2 * fHartCount);
        delete[] tab;
    }

    if (d.child != nullptr) {
        uint32_t tab[3];
        fdt.PropU32("riscv,children", child_phandle);
        tab[0] = child_phandle;
        tab[1] = 1;
        tab[2] = fNumSources;
        fdt.PropTabU32("riscv,delegation", tab, 3);
    }

    fdt.PropU32("phandle", phandle);
    fdt.EndNode();
}


uint32_t APLIC::BuildFDT(FDTBuilder &fdt, uint64_t m_base, uint64_t s_base,
                         const uint32_t *intc_phandle,
                         uint32_t imsic_m_phandle, uint32_t imsic_s_phandle)
{
    uint32_t m_phandle = fdt.AllocPhandle();
    uint32_t s_phandle = fdt.AllocPhandle();

    BuildDomainFDT(fdt, fDomains[0], m_base, m_phandle, s_phandle,
                   intc_phandle, imsic_m_phandle);
    BuildDomainFDT(fdt, fDomains[1], s_base, s_phandle, 0,
                   intc_phandle, imsic_s_phandle);
    return s_phandle;
}
