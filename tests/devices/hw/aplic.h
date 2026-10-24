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
#pragma once

#include "intc.h"

/* A domain's control region, by the RISC-V AIA specification */
#define APLIC_DOMAINCFG     0x0000
#define APLIC_SOURCECFG(i)  (4 * (i))
#define APLIC_MMSIADDRCFG   0x1bc0
#define APLIC_MMSIADDRCFGH  0x1bc4
#define APLIC_SMSIADDRCFG   0x1bc8
#define APLIC_SMSIADDRCFGH  0x1bcc
#define APLIC_SETIP(w)      (0x1c00 + 4 * (w))
#define APLIC_SETIPNUM      0x1cdc
#define APLIC_IN_CLRIP(w)   (0x1d00 + 4 * (w))
#define APLIC_CLRIPNUM      0x1ddc
#define APLIC_SETIE(w)      (0x1e00 + 4 * (w))
#define APLIC_SETIENUM      0x1edc
#define APLIC_CLRIE(w)      (0x1f00 + 4 * (w))
#define APLIC_CLRIENUM      0x1fdc
#define APLIC_SETIPNUM_LE   0x2000
#define APLIC_SETIPNUM_BE   0x2004
#define APLIC_GENMSI        0x3000
#define APLIC_TARGET(i)     (0x3000 + 4 * (i))
#define APLIC_IDC(h)        (0x4000 + 32 * (h))
#define APLIC_IDELIVERY     0x00
#define APLIC_IFORCE        0x04
#define APLIC_ITHRESHOLD    0x08
#define APLIC_TOPI          0x18
#define APLIC_CLAIMI        0x1c

#define APLIC_DOMAINCFG_BE  (1u << 0)
#define APLIC_DOMAINCFG_DM  (1u << 2)
#define APLIC_DOMAINCFG_IE  (1u << 8)
#define APLIC_SOURCECFG_D   (1u << 10)
#define APLIC_TARGET_HART(h) ((uint32_t)(h) << 18)

#define APLIC_SM_INACTIVE 0
#define APLIC_SM_DETACHED 1
#define APLIC_SM_EDGE1    4
#define APLIC_SM_EDGE0    5
#define APLIC_SM_LEVEL1   6
#define APLIC_SM_LEVEL0   7

/* An APLIC with a machine level root domain and a supervisor level child,
   as temu builds it. The image drives the root in M-mode and the child in
   S-mode, whose sources the root delegated before the supervisor started. */
class Aplic final : public IntController {
public:
    struct Domain {
        const FdtNode *node;
        uint64_t base;
    };

    /* The APLIC a domain node belongs to, or nullptr. */
    static Aplic *Get(const FdtNode *node);
    static Aplic *First();

    const Domain &Root() const {return fRoot;}
    const Domain &Child() const {return fChild;}
    /* the domain of the privilege the image runs at */
    const Domain &Mine() const;
    bool HasRoot() const {return fRoot.node != nullptr;}
    bool MsiMode() const;
    uint32_t SourceCount() const {return fSources;}
    /* The boot hart's IDC, or in MSI mode its hart index; -1 if none. */
    int HartIndex(const Domain &d) const;

    uint32_t Read(const Domain &d, uint32_t offset) const;
    void Write(const Domain &d, uint32_t offset, uint32_t value) const;
    uint32_t Idc(const Domain &d, uint32_t reg) const;
    void SetIdc(const Domain &d, uint32_t reg, uint32_t value) const;

    /* IntController: active high, level or rising edge */
    const char *Kind() const override {return "aplic";}
    bool Route(uint32_t line, IrqHandler fn, void *ctx,
               bool level = true) override;
    void Unroute(uint32_t line) override;
    void Reset() override;

private:
    Aplic(const FdtNode *root, const FdtNode *child);
    static void Dispatch(void *self);
    static void MsiDispatch(void *slot);

    Domain fRoot {};
    Domain fChild {};
    uint32_t fSources = 0;
    struct Slot {
        IrqHandler fn;
        void *ctx;
        Aplic *aplic;   /* MSI mode */
        uint32_t line;
        bool level;
    } fRoutes[1024] {};
};
