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
#pragma once

#include "intc.h"

/* Indirect register numbers of an interrupt file (*iselect). */
#define ISEL_EIDELIVERY  0x70
#define ISEL_EITHRESHOLD 0x72
#define ISEL_EIP0        0x80
#define ISEL_EIE0        0xc0

/* The boot hart's interrupt file at the privilege the image runs at, by the
   RISC-V AIA specification: reached through the *iselect, *ireg and *topei
   CSRs, and written to by MSIs at a page per hart. */
class Imsic final : public IntController {
public:
    /* nullptr when the machine has no interrupt files at this privilege */
    static Imsic *Mine();

    uint32_t IdCount() const {return fIds;}
    /* The MSI page of a hart's file at this privilege, or 0. */
    uint64_t PageOf(uint32_t hart) const;
    uint64_t MyPage() const {return PageOf(gBoot.boot_cpu);}

    unsigned long ReadReg(unsigned sel) const;
    void WriteReg(unsigned sel, unsigned long value) const;
    /* *topei, and the write to it that claims */
    unsigned long Topei() const;
    void ClaimTop() const;

    bool Pending(uint32_t id) const;
    void SetPending(uint32_t id, bool on) const;
    bool Enabled(uint32_t id) const;
    void SetEnabled(uint32_t id, bool on) const;
    /* The register and bit an identity is in. */
    static unsigned SelectFor(unsigned base, uint32_t id);
    static unsigned BitFor(uint32_t id);

    /* IntController: 'line' is an identity */
    const char *Kind() const override {return "imsic";}
    bool Route(uint32_t line, IrqHandler fn, void *ctx) override;
    void Unroute(uint32_t line) override;
    void Reset() override;

private:
    Imsic(const FdtNode *node);
    static void Dispatch(void *self);

    const FdtNode *fNode;
    uint32_t fIds = 0;
    struct Slot {
        IrqHandler fn;
        void *ctx;
    } fRoutes[2048] {};
};
