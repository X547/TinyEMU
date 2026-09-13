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
#pragma once

#include "iomem.h"
#include "hart_irq.h"

class FDTBuilder;

/* The control region of each interrupt domain: the registers of the first
   16 KB and an IDC structure for up to 512 harts. */
#define APLIC_SIZE 0x8000


/* Where the IMSIC interrupt files an APLIC in MSI delivery mode forwards to
   are: one page per hart from each base, hart 0 first. */
struct AplicMsiLayout {
    uint64_t m_base;
    uint64_t s_base;
    int hart_index_bits;
};


/* A machine level root domain and one supervisor level child domain, both
   spanning every hart. Each delivers either directly, driving the harts'
   external interrupt signals, or by MSIs to their IMSICs; which one is fixed
   when the APLIC is created. */
class APLIC final: public IRQTarget {
private:
    struct Idc {
        uint32_t idelivery;
        uint32_t iforce;
        uint32_t ithreshold;
    };

    struct Domain {
        bool supervisor;
        Domain *parent;
        Domain *child;
        bool ie; /* domaincfg.IE */
        uint32_t genmsi;
        /* indexed by source number */
        uint32_t *sourcecfg;
        uint32_t *target;
        /* bitmaps, bit n being source n */
        uint32_t *pending;
        uint32_t *enable;
        Idc *idc;
    };

    PhysMemoryMap *fMemMap;
    HartIrqTarget *fHarts;
    int fHartCount;
    int fNumSources;
    int fWords; /* in each bitmap */
    bool fMsiMode;
    AplicMsiLayout fMsi {};
    uint32_t *fLevel; /* input wires */
    Domain fDomains[2] {}; /* machine level root, supervisor level child */

    uint32_t ReadM(uint32_t offset, int size_log2);
    void WriteM(uint32_t offset, uint32_t val, int size_log2);
    uint32_t ReadS(uint32_t offset, int size_log2);
    void WriteS(uint32_t offset, uint32_t val, int size_log2);

    DeviceIOAdapter<APLIC, &APLIC::ReadM, &APLIC::WriteM> fIoM {*this};
    DeviceIOAdapter<APLIC, &APLIC::ReadS, &APLIC::WriteS> fIoS {*this};

    uint32_t Read(Domain &d, uint32_t offset);
    void Write(Domain &d, uint32_t offset, uint32_t val);

    bool Visible(const Domain &d, uint32_t irq) const;
    uint32_t Mode(const Domain &d, uint32_t irq) const;
    bool Rectified(const Domain &d, uint32_t irq) const;
    void Deactivate(Domain &d, uint32_t irq);

    void WriteSourcecfg(Domain &d, uint32_t irq, uint32_t val);
    void WriteTarget(Domain &d, uint32_t irq, uint32_t val);
    void SetPendingByWrite(Domain &d, uint32_t irq);
    void ClearPendingByWrite(Domain &d, uint32_t irq);
    void SetEnable(Domain &d, uint32_t irq, bool enable);
    uint32_t Topi(const Domain &d, int hart) const;
    uint32_t MsiAddrCfg(uint32_t offset) const;
    void SendMsi(const Domain &d, uint32_t hart, uint32_t eiid);
    void Update();
    void BuildDomainFDT(FDTBuilder &fdt, const Domain &d, uint64_t base,
                        uint32_t phandle, uint32_t child_phandle,
                        const uint32_t *intc_phandle, uint32_t msi_phandle);

public:
    /* 'msi' is null for direct delivery. */
    APLIC(PhysMemoryMap *mem_map, HartIrqTarget *harts, int hart_count,
          int num_sources, const AplicMsiLayout *msi);
    ~APLIC() override;

    DeviceIO *DomainIO(bool supervisor)
        {return supervisor ? static_cast<DeviceIO *>(&fIoS) : &fIoM;}

    /* IRQTarget */
    void SetIRQ(int irq_num, int level) override;

    /* Emit both domains' nodes and return the supervisor level one's phandle,
       which is what devices name as their interrupt parent. The IMSIC
       phandles are used in MSI delivery mode only. */
    uint32_t BuildFDT(FDTBuilder &fdt, uint64_t m_base, uint64_t s_base,
                      const uint32_t *intc_phandle,
                      uint32_t imsic_m_phandle, uint32_t imsic_s_phandle);
};
