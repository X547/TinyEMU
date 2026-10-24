/*
 * PCI tests: the DesignWare root complex
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

/* The port logic, address translation unit and MSI receiver registers as
   the Linux driver for the Synopsys core (pcie-designware*.c) uses them. */

#define PORT_LINK_CTRL  0x710
#define PORT_DEBUG0     0x728
#define PORT_DEBUG1     0x72c
#define GEN2_CTRL       0x80c
#define MSI_ADDR_LO     0x820
#define MSI_ADDR_HI     0x824
#define MSI_ENABLE(g)   (0x828 + 0xc * (g))
#define MSI_MASK(g)     (0x82c + 0xc * (g))
#define MSI_STATUS(g)   (0x830 + 0xc * (g))
#define MISC_CONTROL_1  0x8bc
#define ATU_VIEWPORT    0x900
#define ATU(i, in)      (0x300000 + 0x200 * (i) + ((in) ? 0x100 : 0))
#define ATU_CTRL1       0x00
#define ATU_CTRL2       0x04
#define ATU_BASE_LO     0x08
#define ATU_BASE_HI     0x0c
#define ATU_LIMIT       0x10
#define ATU_TARGET_LO   0x14
#define ATU_TARGET_HI   0x18
#define ATU_CFG0        0x04
#define ATU_CFG1        0x05
#define ATU_ENABLE      0x80000000u

namespace {

PciHost *sHost;

/* Enumerated unless asked not to. */
PciHost *need_dw(bool enumerate = true)
{
    for (int i = 0; i < pci_host_count(); i++) {
        if (pci_host(i)->kind == PCI_HOST_DW) {
            sHost = pci_host(i);
            if (enumerate) {
                pci_enumerate();
                t_on_cleanup(pt_cleanup);
            }
            return sHost;
        }
    }
    t_skip("no DesignWare root complex");
}

uint32_t dbi(uint32_t reg)
{
    return mmio_read32(sHost->dbi + reg);
}

void set_dbi(uint32_t reg, uint32_t v)
{
    mmio_write32(sHost->dbi + reg, v);
}

/* Region 1, which the suite's own configuration cycles leave alone. */
void point(uint32_t type, uint32_t bus, uint32_t devfn, bool enable = true)
{
    uint32_t a = ATU(1, false);
    set_dbi(a + ATU_CTRL1, type);
    set_dbi(a + ATU_BASE_LO, (uint32_t)sHost->cfg + 0x1000);
    set_dbi(a + ATU_BASE_HI, sHost->cfg >> 32);
    set_dbi(a + ATU_LIMIT, (uint32_t)sHost->cfg + 0x1fff);
    set_dbi(a + ATU_TARGET_LO, bus << 24 | devfn << 16);
    set_dbi(a + ATU_TARGET_HI, 0);
    set_dbi(a + ATU_CTRL2, enable ? ATU_ENABLE : 0);
}

uint32_t window_read(uint32_t reg)
{
    return mmio_read32(sHost->cfg + 0x1000 + reg);
}

void clear_region1()
{
    for (uint32_t r = 0; r < 0x20; r += 4)
        set_dbi(ATU(1, false) + r, 0);
}

}


FRESH_TEST(designware, reset_state)
{
    need_dw(false);
    SNAP("root_port", "%08x %08x %08x", dbi(PCI_VENDOR_ID), dbi(PCI_REVISION),
         dbi(PCI_PRIMARY_BUS));
    SNAP("port_logic", "%08x %08x %08x %08x %08x %08x", dbi(PORT_LINK_CTRL),
         dbi(PORT_DEBUG0), dbi(PORT_DEBUG1), dbi(GEN2_CTRL),
         dbi(MISC_CONTROL_1), dbi(ATU_VIEWPORT));
    SNAP("msi", "%08x %08x %08x %08x %08x", dbi(MSI_ADDR_LO), dbi(MSI_ADDR_HI),
         dbi(MSI_ENABLE(0)), dbi(MSI_MASK(0)), dbi(MSI_STATUS(0)));
    SNAP("atu0", "%08x %08x %08x", dbi(ATU(0, false) + ATU_CTRL1),
         dbi(ATU(0, false) + ATU_CTRL2), dbi(ATU(0, true) + ATU_CTRL2));
}

/* Registers kept only to read back, and the link that is always up. */
TEST(designware, port_logic)
{
    need_dw();
    EXPECT_TRUE(dbi(PORT_DEBUG1) & 0x10);
    EXPECT_EQ(dbi(ATU_VIEWPORT), 0xffffffffu);
    static const uint32_t regs[] = {PORT_LINK_CTRL, GEN2_CTRL, MISC_CONTROL_1,
                                    MSI_ADDR_LO, MSI_ADDR_HI};
    for (uint32_t reg : regs) {
        uint32_t saved = dbi(reg);
        set_dbi(reg, 0xa5a5a5a5);
        EXPECT_EQ(dbi(reg), 0xa5a5a5a5u);
        /* a byte at a time, the other lanes untouched */
        mmio_write8(sHost->dbi + reg + 2, 0x3c);
        mmio_write16(sHost->dbi + reg, 0x1234);
        EXPECT_EQ(dbi(reg), 0xa53c1234u);
        EXPECT_EQ(mmio_read8(sHost->dbi + reg + 1), 0x12u);
        EXPECT_EQ(mmio_read16(sHost->dbi + reg + 2), 0xa53cu);
        set_dbi(reg, saved);
    }
    /* unknown port logic reads 0 and takes nothing */
    set_dbi(0x800, 0xffffffff);
    EXPECT_EQ(dbi(0x800), 0u);
}

/* The translation regions, inbound and outbound, as registers. */
TEST(designware, atu_registers)
{
    need_dw();
    t_on_cleanup(clear_region1);
    for (int in = 0; in < 2; in++) {
        uint32_t a = ATU(7, in);
        for (uint32_t r = 0; r <= ATU_TARGET_HI; r += 4)
            set_dbi(a + r, 0x01020304u * (r + 1));
        mmio_write8(sHost->dbi + a + ATU_LIMIT + 3, 0xee);
        SNAP(in ? "inbound7" : "outbound7", "%08x %08x %08x %08x %08x %08x "
             "%08x", dbi(a), dbi(a + 4), dbi(a + 8), dbi(a + 12), dbi(a + 16),
             dbi(a + 20), dbi(a + 24));
        for (uint32_t r = 0; r <= ATU_TARGET_HI; r += 4)
            set_dbi(a + r, 0);
    }
    /* past the eighth region, and a register no region has */
    set_dbi(ATU(8, false), 0x1234);
    SNAP("past_regions", "%08x %08x", dbi(ATU(8, false)),
         dbi(ATU(0, false) + 0x1c));
    set_dbi(ATU(0, false) + 0x1c, 0x5678);
}

/* Configuration cycles through the window: type 0 reaches the link below
   the root port only, type 1 the buses further down only. */
TEST(designware, config_window)
{
    PciHost *h = need_dw();
    t_on_cleanup(clear_region1);
    uint32_t buses = dbi(PCI_PRIMARY_BUS);
    uint32_t link = (buses >> 8) & 0xff;
    uint32_t sub = (buses >> 16) & 0xff;
    point(ATU_CFG0, link, 0);
    uint32_t id0 = window_read(0);
    point(ATU_CFG1, link, 0);
    uint32_t id0_type1 = window_read(0);
    EXPECT_NE(id0, 0xffffffffu);
    EXPECT_EQ(id0_type1, 0xffffffffu);
    if (sub > link) {
        point(ATU_CFG1, link + 1, 0);
        EXPECT_NE(window_read(0), 0xffffffffu);
        point(ATU_CFG0, link + 1, 0);
        EXPECT_EQ(window_read(0), 0xffffffffu);
    }
    /* bus 0 is the root port, which only DBI shows */
    point(ATU_CFG0, h->bus_first, 0);
    EXPECT_EQ(window_read(0), 0xffffffffu);
    /* a disabled region, and a memory region, translate nothing */
    point(ATU_CFG0, link, 0, false);
    EXPECT_EQ(window_read(0), 0xffffffffu);
    point(0, link, 0);
    EXPECT_EQ(window_read(0), 0xffffffffu);
    /* outside the region */
    point(ATU_CFG0, link, 0);
    EXPECT_EQ(mmio_read32(h->cfg + 0x2000), 0xffffffffu);
    mmio_write32(h->cfg + 0x2000, 0);
    /* narrow reads, and writes through it */
    EXPECT_EQ(mmio_read16(h->cfg + 0x1000 + 2), id0 >> 16);
    EXPECT_EQ(mmio_read8(h->cfg + 0x1000 + 1), (id0 >> 8) & 0xff);
    mmio_write8(h->cfg + 0x1000 + PCI_INTERRUPT_LINE, 0x42);
    EXPECT_EQ(mmio_read8(h->cfg + 0x1000 + PCI_INTERRUPT_LINE), 0x42u);
    SNAP("ids", "%08x %08x", id0, id0_type1);
}

/* The receiver: a message to the doorbell sets its bit whatever the mask
   says; enabled and unmasked bits raise the line; written ones clear. */
TEST(designware, msi_receiver)
{
    PciHost *h = need_dw();
    PciFunc *f = nullptr;
    for (int i = 0; (f = pt_get(i)); i++) {
        if (f->host == h)
            break;
    }
    if (f == nullptr || !pci_find_cap(f, PCI_CAP_MSIX))
        t_skip("no test function with MSI-X behind it");
    uint32_t cap = pci_find_cap(f, PCI_CAP_MSIX);
    Plic *plic = Plic::Get(fdt_find_phandle(h->dev->irqs[0].parent));
    ASSERT_TRUE(plic != nullptr);
    uint32_t line = h->dev->irqs[0].cells[0];
    auto pending = [&] {
        return (plic->Read(PLIC_PENDING(line / 32)) >> (line % 32)) & 1;
    };
    const uint64_t doorbell = 0x1000;
    set_dbi(MSI_ADDR_LO, doorbell);
    set_dbi(MSI_ADDR_HI, 0);
    for (int n = 0; n < 2; n++) {
        uint32_t e = TEST_PCI_MSIX_TABLE + 16 * n;
        pci_bar_write(f, TEST_PCI_BAR_MSIX, e, doorbell, 2);
        pci_bar_write(f, TEST_PCI_BAR_MSIX, e + 4, 0, 2);
        pci_bar_write(f, TEST_PCI_BAR_MSIX, e + 8, n == 0 ? 3 : 0x123, 2);
        pci_bar_write(f, TEST_PCI_BAR_MSIX, e + 12, 0, 2);
    }
    pci_write16(f, cap + 2, pci_read16(f, cap + 2) | 0x8000);
    pt_write(f, TEST_PCI_MSI_SEND, 0);
    pt_write(f, TEST_PCI_MSI_SEND, 1);
    /* 0x123 is vector 35, in the second group */
    EXPECT_EQ(dbi(MSI_STATUS(0)), 0x8u);
    EXPECT_EQ(dbi(MSI_STATUS(1)), 0x8u);
    uint32_t idle = pending();
    set_dbi(MSI_ENABLE(0), 0x8);
    uint32_t enabled = pending();
    set_dbi(MSI_MASK(0), 0x8);
    uint32_t masked = pending();
    set_dbi(MSI_MASK(0), 0);
    mmio_write8(h->dbi + MSI_STATUS(0), 0x8);
    EXPECT_EQ(dbi(MSI_STATUS(0)), 0u);
    SNAP("line", "idle %u enabled %u masked %u cleared %u", idle, enabled,
         masked, pending());
}

#endif
