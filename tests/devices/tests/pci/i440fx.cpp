/*
 * PCI tests: the i440FX and the PIIX3's interrupt routing
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
#if defined(__i386__) || defined(__x86_64__)

#include "fixture.h"
#include "i8259.h"
#include "mmio.h"

/* Configuration mechanism #1 by the PCI Local Bus specification 3.0,
   3.2.2.3.2; the PIRQ route control registers by the PIIX3 datasheet
   (Intel 290550), 2.2.9. */

#define PIRQ_ROUTE(n) (0x60 + (n))
#define PIRQ_DISABLE  0x80

namespace {

PciHost *need_cf8()
{
    for (int i = 0; i < pci_host_count(); i++) {
        if (pci_host(i)->kind == PCI_HOST_CF8)
            return pci_host(i);
    }
    t_skip("no i440FX");
}

/* The PIIX3: the ISA bridge, function 0 of its slot. */
PciFunc *need_piix3()
{
    PciFunc *f = pci_find(0x8086, 0x7000);
    if (f == nullptr)
        t_skip("no PIIX3");
    return f;
}

volatile int sIrqs;

}

extern uint8_t gPicResetState[8];

namespace {

void handler(void *ctx)
{
    sIrqs = sIrqs + 1;
    pt_write((const PciFunc *)ctx, TEST_PCI_INTX, 0);
}

}


/* The address register reads back what was written; with bit 31 clear the
   data port reads nothing. */
TEST(i440fx, config_ports)
{
    need_cf8();
    uint32_t saved = inl(0xcf8);
    outl(0xcf8, 0x12345678);
    SNAP("address", "%08x", inl(0xcf8));
    outl(0xcf8, 0x00000000);
    SNAP("disabled", "%08x %04x %02x", inl(0xcfc), inw(0xcfe), inb(0xcfd));
    outl(0xcfc, 0xffffffff); /* goes nowhere */
    outl(0xcf8, 0x80000000);
    SNAP("host_bridge", "%08x", inl(0xcfc));
    /* the byte lanes of the data port select the register's bytes */
    EXPECT_EQ(inb(0xcfd), 0x80u);
    SNAP("device_word", "%04x", inw(0xcfe));
    outl(0xcf8, 0x80000008);
    SNAP("class", "%08x", inl(0xcfc));
    outl(0xcf8, 0x8000002c);
    SNAP("subsystem", "%08x", inl(0xcfc));
    outl(0xcf8, saved);
}

TEST(i440fx, piix3)
{
    PciFunc *f = need_piix3();
    SNAP("ids", "%08x %08x header %02x", pci_read32(f, PCI_VENDOR_ID),
         pci_read32(f, PCI_REVISION), pci_read8(f, PCI_HEADER_TYPE));
    SNAP("pirq", "%08x", pci_read32(f, PIRQ_ROUTE(0)));
}

/* With no firmware temu routed PIRQA-D itself and set the lines to level
   in the ELCRs; as the boot found them. */
FRESH_TEST(i440fx, loader_routing)
{
    PciFunc *f = need_piix3();
    SNAP("pirq", "%08x", pci_read32(f, PIRQ_ROUTE(0)));
    /* the ELCRs as the boot found them, before it reset the 8259s */
    if (I8259::Get())
        SNAP("elcr", "%02x %02x", gPicResetState[3], gPicResetState[7]);
}

/* A test function's PIRQ sent to another line, or turned off. */
TEST(i440fx, pirq_reroute)
{
    PciFunc *piix = need_piix3();
    PciFunc *f = pt_require();
    I8259 *pic = I8259::Get();
    const PciFunc *root;
    uint8_t pin = pci_root_pin(f, &root);
    if (pic == nullptr || pin == 0 || f->host->msi)
        t_skip("needs the 8259s alone");
    /* which PIRQ the root bus swizzle puts it on, as temu's _PRT has it */
    int pirq = (pin - 1 + PCI_SLOT(root->devfn) - 1) & 3;
    uint8_t saved = pci_read8(piix, PIRQ_ROUTE(pirq));
    t_on_cleanup([] {
        PciFunc *p = pci_find(0x8086, 0x7000);
        pci_write32(p, PIRQ_ROUTE(0), 0x0b0a0b0a);
    });
    /* 9 is the SCI's, which nothing raises here */
    const uint32_t line = 9;
    sIrqs = 0;
    ASSERT_TRUE(pic->Route(line, handler, f, true));
    pci_write8(piix, PIRQ_ROUTE(pirq), line);
    arch_irq_enable();
    pt_write(f, TEST_PCI_INTX, 1);
    EXPECT_TRUE(t_wait_until([] {return sIrqs > 0;}));

    /* disabled; the route register is looked at as the pin changes */
    sIrqs = 0;
    pci_write8(piix, PIRQ_ROUTE(pirq), line | PIRQ_DISABLE);
    pt_write(f, TEST_PCI_INTX, 1);
    t_delay_us(5000);
    int disabled = sIrqs;
    pt_write(f, TEST_PCI_INTX, 0);
    SNAP("disabled", "irqs %d route %02x", disabled,
         pci_read8(piix, PIRQ_ROUTE(pirq)));

    /* a line past the 8259s' sixteen */
    sIrqs = 0;
    pci_write8(piix, PIRQ_ROUTE(pirq), 0x10);
    pt_write(f, TEST_PCI_INTX, 1);
    t_delay_us(5000);
    pt_write(f, TEST_PCI_INTX, 0);
    arch_irq_disable();
    EXPECT_EQ(sIrqs, 0);
    pic->Unroute(line);
    SNAP("saved", "%02x", saved);
}

#endif
