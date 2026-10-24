/*
 * PCI tests: the PLDA root complex
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
#if defined(__riscv)

#include "fixture.h"
#include "mmio.h"
#include "plic.h"

/* The bridge registers as the Linux drivers for the PLDA XpressRich core
   (pcie-plda*.c) and the StarFive JH7110 (pcie-starfive.c) use them. */

#define PCI_IDS_DW1     0x09c
#define IMASK_LOCAL     0x180
#define ISTATUS_LOCAL   0x184
#define IMSI_ADDR       0x190
#define ISTATUS_MSI     0x194
#define PMSG_SUPPORT_RX 0x3f0
#define RC_CONFIG       0x1000
#define INTX_SHIFT      24
#define INT_MSI         (1u << 28)
#define SYSCON_LNKSTA0  (0x048 + 0x170)
#define SYSCON_LNKSTA1  (0x1f8 + 0x170)

namespace {

PciHost *sHost;

/* Enumerated unless asked not to. */
PciHost *need_plda(bool enumerate = true)
{
    for (int i = 0; i < pci_host_count(); i++) {
        if (pci_host(i)->kind == PCI_HOST_PLDA) {
            sHost = pci_host(i);
            if (enumerate) {
                pci_enumerate();
                t_on_cleanup(pt_cleanup);
            }
            return sHost;
        }
    }
    t_skip("no PLDA root complex");
}

uint32_t apb(uint32_t reg)
{
    return mmio_read32(sHost->apb + reg);
}

void set_apb(uint32_t reg, uint32_t v)
{
    mmio_write32(sHost->apb + reg, v);
}

/* The node a phandle property of the bridge names, by its first reg. */
uint64_t phandle_reg(const char *prop)
{
    const FdtProp *p = fdt_prop(sHost->dev->fdt, prop);
    if (p == nullptr || p->len < 4)
        return 0;
    const FdtNode *n = fdt_find_phandle(be32(p->data));
    uint64_t base, size;
    return n && fdt_reg(n, 0, &base, &size) ? base : 0;
}

PciFunc *function_behind()
{
    for (int i = 0; PciFunc *f = pt_get(i); i++) {
        if (f->host == sHost)
            return f;
    }
    t_skip("no test function behind it");
}

}


FRESH_TEST(plda, reset_state)
{
    need_plda(false);
    SNAP("ids", "%08x pmsg %08x", apb(PCI_IDS_DW1), apb(PMSG_SUPPORT_RX));
    SNAP("interrupts", "%08x %08x %08x %08x", apb(IMASK_LOCAL),
         apb(ISTATUS_LOCAL), apb(IMSI_ADDR), apb(ISTATUS_MSI));
}

/* Bridge registers keep what is written, a word at a time only; the root
   port's configuration space is mirrored above them. */
TEST(plda, registers)
{
    PciHost *h = need_plda();
    uint32_t saved = apb(0x200);
    set_apb(0x200, 0xdeadbeef);
    EXPECT_EQ(apb(0x200), 0xdeadbeefu);
    mmio_write8(h->apb + 0x200, 0x11);
    mmio_write16(h->apb + 0x202, 0x2222);
    SNAP("narrow", "%08x %02x %04x", apb(0x200), mmio_read8(h->apb + 0x200),
         mmio_read16(h->apb + 0x202));
    set_apb(0x200, saved);
    /* the mirror is the root port at bus 0 */
    EXPECT_EQ(apb(RC_CONFIG + PCI_VENDOR_ID),
              h->Read(h->bus_first, 0, PCI_VENDOR_ID, 2));
    EXPECT_EQ(mmio_read16(h->apb + RC_CONFIG + PCI_DEVICE_ID),
              h->Read(h->bus_first, 0, PCI_DEVICE_ID, 1));
    uint8_t line = mmio_read8(h->apb + RC_CONFIG + PCI_INTERRUPT_LINE);
    mmio_write8(h->apb + RC_CONFIG + PCI_INTERRUPT_LINE, 0x77);
    EXPECT_EQ(h->Read(h->bus_first, 0, PCI_INTERRUPT_LINE, 0), 0x77u);
    mmio_write8(h->apb + RC_CONFIG + PCI_INTERRUPT_LINE, line);
    /* the rest of the window */
    set_apb(0x8000, 0x1234);
    SNAP("unused", "%08x", apb(0x8000));
}

/* The link is up in both of the JH7110's syscon words; the reset lines
   only read back. */
TEST(plda, syscon_and_resets)
{
    need_plda();
    uint64_t syscon = phandle_reg("starfive,stg-syscon");
    uint64_t resets = phandle_reg("resets");
    ASSERT_NE(syscon, 0u);
    ASSERT_NE(resets, 0u);
    SNAP("lnksta", "%08x %08x", mmio_read32(syscon + SYSCON_LNKSTA0),
         mmio_read32(syscon + SYSCON_LNKSTA1));
    mmio_write32(syscon + 0x10, 0xabcd);
    EXPECT_EQ(mmio_read32(syscon + 0x10), 0xabcdu);
    mmio_write32(syscon + SYSCON_LNKSTA0, 0);
    EXPECT_TRUE(mmio_read32(syscon + SYSCON_LNKSTA0) & 0x20);
    uint32_t saved = mmio_read32(resets);
    mmio_write32(resets, 0x3f);
    mmio_write32(resets + 4, 0x3f);
    SNAP("resets", "%08x %08x", mmio_read32(resets), mmio_read32(resets + 4));
    mmio_write32(resets, saved);
}

/* INTx and messages in the local status; the mask gates the line. */
TEST(plda, local_interrupts)
{
    PciHost *h = need_plda();
    PciFunc *f = function_behind();
    Plic *plic = Plic::Get(fdt_find_phandle(h->dev->irqs[0].parent));
    ASSERT_TRUE(plic != nullptr);
    uint32_t line = h->dev->irqs[0].cells[0];
    auto pending = [&] {
        return (plic->Read(PLIC_PENDING(line / 32)) >> (line % 32)) & 1;
    };
    const PciFunc *root;
    uint8_t pin = pci_root_pin(f, &root);
    ASSERT_NE(pin, 0);
    uint32_t intx = 1u << (INTX_SHIFT + pin - 1);
    set_apb(IMASK_LOCAL, 0);
    pt_write(f, TEST_PCI_INTX, 1);
    EXPECT_EQ(apb(ISTATUS_LOCAL), intx);
    uint32_t unmasked_before = pending();
    set_apb(IMASK_LOCAL, intx);
    uint32_t unmasked = pending();
    /* written back, the status stays while the pin does */
    set_apb(ISTATUS_LOCAL, ~0u);
    EXPECT_EQ(apb(ISTATUS_LOCAL), intx);
    pt_write(f, TEST_PCI_INTX, 0);
    EXPECT_EQ(apb(ISTATUS_LOCAL), 0u);
    SNAP("intx", "masked %u unmasked %u released %u", unmasked_before,
         unmasked, pending());

    uint32_t cap = pci_find_cap(f, PCI_CAP_MSIX);
    if (cap == 0)
        return;
    pci_bar_write(f, TEST_PCI_BAR_MSIX, TEST_PCI_MSIX_TABLE, IMSI_ADDR, 2);
    pci_bar_write(f, TEST_PCI_BAR_MSIX, TEST_PCI_MSIX_TABLE + 4, 0, 2);
    pci_bar_write(f, TEST_PCI_BAR_MSIX, TEST_PCI_MSIX_TABLE + 8, 37, 2);
    pci_bar_write(f, TEST_PCI_BAR_MSIX, TEST_PCI_MSIX_TABLE + 12, 0, 2);
    pci_write16(f, cap + 2, pci_read16(f, cap + 2) | 0x8000);
    pt_write(f, TEST_PCI_MSI_SEND, 0);
    /* the vector is taken modulo the 32 there are */
    EXPECT_EQ(apb(ISTATUS_MSI), 1u << 5);
    EXPECT_EQ(apb(ISTATUS_LOCAL), INT_MSI);
    set_apb(IMASK_LOCAL, INT_MSI);
    uint32_t msi = pending();
    set_apb(ISTATUS_MSI, 1u << 5);
    EXPECT_EQ(apb(ISTATUS_MSI), 0u);
    SNAP("msi", "raised %u cleared %u", msi, pending());
    set_apb(IMASK_LOCAL, 0);
}

#endif
