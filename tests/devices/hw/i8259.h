/*
 * The PC's 8259 pair
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

/* Two cascaded 8259As, the second on input 2 of the first, with the PIIX
   edge/level control registers. The command ports come from the DSDT; the
   ELCRs have no description of their own and are at their PIIX ports. */
class I8259 final : public IntController {
public:
    static I8259 *Get();

    uint16_t CommandPort(int pic) const {return fPort[pic];}
    uint16_t DataPort(int pic) const {return fPort[pic] + 1;}
    uint16_t ElcrPort(int pic) const {return 0x4d0 + pic;}

    /* ICW1 to ICW4, which leaves every input unmasked */
    void Init(uint8_t base0, uint8_t base1, bool auto_eoi = false) const;
    uint8_t Irr(int pic) const;
    uint8_t Isr(int pic) const;
    uint8_t Imr(int pic) const;
    void SetImr(int pic, uint8_t mask) const;
    uint8_t Elcr(int pic) const;
    void SetElcr(int pic, uint8_t value) const;
    /* a non-specific EOI for 'line', to the slave and the master */
    void Eoi(uint32_t line) const;
    void SetMasked(uint32_t line, bool masked) const;

    /* IntController: lines 0-15, vectors VEC_PIC_MASTER on */
    const char *Kind() const override {return "i8259";}
    bool Route(uint32_t line, IrqHandler fn, void *ctx,
               bool level = true) override;
    void Unroute(uint32_t line) override;
    void Reset() override;

private:
    I8259(uint16_t master, uint16_t slave);
    static void Dispatch(void *slot);
    static void Spurious(void *slot);

    uint16_t fPort[2];
    bool fReady = false; /* initialized by Reset() */
    struct Slot {
        I8259 *pic;
        uint32_t line;
        IrqHandler fn;
        void *ctx;
    } fRoutes[16] {};
};
