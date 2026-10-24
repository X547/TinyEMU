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
#pragma once

#include "intc.h"

#define PLIC_PRIORITY(src)    (4 * (src))
#define PLIC_PENDING(word)    (0x1000 + 4 * (word))
#define PLIC_ENABLE(ctx, word) (0x2000 + 0x80 * (ctx) + 4 * (word))
#define PLIC_THRESHOLD(ctx)   (0x200000 + 0x1000 * (ctx))
#define PLIC_CLAIM(ctx)       (0x200000 + 0x1000 * (ctx) + 4)

/* The platform-level interrupt controller, by the RISC-V PLIC
   specification: a context per hart and privilege, in the order of the
   node's interrupts-extended. */
class Plic final : public IntController {
public:
    /* The driver of a PLIC node, or nullptr when it is no PLIC. */
    static Plic *Get(const FdtNode *node);
    /* The first PLIC in the tables, or nullptr. */
    static Plic *First();

    const HwDevice *Device() const {return fDevice;}
    uint64_t Base() const {return fBase;}
    /* Sources counted from 1: riscv,ndev. */
    uint32_t SourceCount() const {return fSources;}
    int ContextCount() const {return fContexts;}
    /* The context of 'hart' at a privilege, or -1. */
    int ContextFor(uint32_t hart, bool machine) const;
    /* The boot hart's, at the privilege the image runs at. */
    int MyContext() const {return fMyContext;}

    uint32_t Read(uint32_t offset) const;
    void Write(uint32_t offset, uint32_t value) const;
    uint32_t Claim(int ctx) const {return Read(PLIC_CLAIM(ctx));}
    void Complete(int ctx, uint32_t src) const
        {Write(PLIC_CLAIM(ctx), src);}
    void SetEnabled(int ctx, uint32_t src, bool on) const;

    /* IntController */
    const char *Kind() const override {return "plic";}
    bool Route(uint32_t line, IrqHandler fn, void *ctx,
               bool level = true) override;
    void Unroute(uint32_t line) override;
    void Reset() override;

private:
    Plic(const HwDevice *dev);
    static void Dispatch(void *self);

    const HwDevice *fDevice;
    uint64_t fBase = 0;
    uint32_t fSources = 0;
    int fContexts = 0;
    int fMyContext = -1;
    struct Route_ {
        IrqHandler fn;
        void *ctx;
    } fRoutes[1024] {};
};
