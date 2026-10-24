/*
 * x86 IOAPIC
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

#define IOREGSEL 0x00
#define IOWIN    0x10

#define IOAPIC_REG_ID     0x00
#define IOAPIC_REG_VER    0x01
#define IOAPIC_REG_ARB    0x02
#define IOAPIC_REG_RTE(n) (0x10 + 2 * (n))

#define RTE_LOGICAL     (1ull << 11)
#define RTE_PENDING     (1ull << 12)
#define RTE_ACTIVE_LOW  (1ull << 13)
#define RTE_REMOTE_IRR  (1ull << 14)
#define RTE_LEVEL       (1ull << 15)
#define RTE_MASKED      (1ull << 16)
#define RTE_DEST(id)    ((uint64_t)(id) << 56)

/* An IOAPIC of the MADT: GSIs from its base, delivered to the boot
   processor's local APIC at VEC_IOAPIC plus the GSI. */
class IoApic final : public IntController {
public:
    static int Count();
    static IoApic *At(int index);
    /* the one whose inputs include 'gsi' */
    static IoApic *ForGsi(uint32_t gsi);

    uint64_t Base() const {return fBase;}
    uint32_t GsiBase() const {return fGsiBase;}
    uint32_t Pins() const;
    uint32_t Read(uint32_t reg) const;
    void Write(uint32_t reg, uint32_t value) const;
    uint64_t Rte(uint32_t pin) const;
    void SetRte(uint32_t pin, uint64_t value) const;

    /* IntController: 'line' is a GSI */
    const char *Kind() const override {return "ioapic";}
    bool Route(uint32_t line, IrqHandler fn, void *ctx,
               bool level = true) override;
    void Unroute(uint32_t line) override;
    void Reset() override;

private:
    IoApic(const MadtIoApic &m);
    static void Dispatch(void *slot);

    uint64_t fBase;
    uint32_t fGsiBase;
    uint32_t fId; /* as the MADT gives it */
    struct Slot {
        IrqHandler fn;
        void *ctx;
    } fRoutes[64] {};
};
