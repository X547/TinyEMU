/*
 * PCI tests: what the resets leave of a function
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
#include "fixture.h"
#include "testctl.h"

/* The PCI Express capability's device capabilities and control, by the
   PCI Express Base Specification 7.5.3.3 and 7.5.3.4. */
#define EXP_DEVCAP        0x04
#define EXP_DEVCAP_FLR    0x10000000
#define EXP_DEVCTL        0x08
#define EXP_DEVCTL_FLR    0x8000u
/* Secondary Bus Reset, by the PCI-to-PCI Bridge Architecture 3.2.5.18. */
#define BRIDGE_CTL_SBR    0x40
/* the mask bit of an MSI-X table entry's vector control */
#define MSIX_ENTRY_CTRL   12
#define MSIX_ENTRY_MASKED 0x1u

namespace {

bool has_msix(const PciFunc *f)
{
    return pci_find_cap(f, PCI_CAP_MSIX) != 0;
}

uint32_t msix_ctrl(const PciFunc *f)
{
    return pci_bar_read(f, TEST_PCI_BAR_MSIX,
                        TEST_PCI_MSIX_TABLE + MSIX_ENTRY_CTRL, 2);
}

/* Whether 'br' is one of the bridges above 'f'. */
bool behind(const PciFunc *f, const PciFunc *br)
{
    for (const PciFunc *p = f->parent; p != nullptr; p = p->parent) {
        if (p == br)
            return true;
    }
    return false;
}

}


/* A reset of the machine puts every function back as it was before the
   guest first wrote to it, the device's own registers with it. Read before
   enumeration programs them again: what is on each root bus. */
FRESH_TEST(pci, machine_reset)
{
    if (!t_after_reset()) {
        pt_require();
        for (int i = 0; PciFunc *f = pt_get(i); i++) {
            pt_write(f, TEST_PCI_SCRATCH, 0x5a5a0000 | i);
            if (has_msix(f))
                pci_bar_write(f, TEST_PCI_BAR_MSIX,
                              TEST_PCI_MSIX_TABLE + MSIX_ENTRY_CTRL, 0, 2);
        }
        t_reboot();
    }

    for (int h = 0; h < pci_host_count(); h++) {
        const PciHost *host = pci_host(h);
        uint32_t bus = host->bus_first;
        for (uint32_t devfn = 0; devfn < 256; devfn++) {
            uint32_t id = host->Read(bus, devfn, PCI_VENDOR_ID, 2);
            if (id == 0xffffffff)
                continue;
            EXPECT_EQ(host->Read(bus, devfn, PCI_COMMAND, 1), 0u);
            uint8_t header = host->Read(bus, devfn, PCI_HEADER_TYPE, 0);
            if ((header & 0x7f) == 1) {
                EXPECT_EQ(host->Read(bus, devfn, PCI_SECONDARY_BUS, 0), 0u);
                EXPECT_EQ(host->Read(bus, devfn, PCI_SUBORDINATE_BUS, 0),
                          0u);
            } else if (id == (TEST_PCI_DEVICE_ID << 16 | TEST_PCI_VENDOR_ID)) {
                EXPECT_EQ(host->Read(bus, devfn, PCI_BAR(TEST_PCI_BAR_REGS),
                                     2), 0u);
            }
        }
    }

    pt_require();
    for (int i = 0; PciFunc *f = pt_get(i); i++) {
        EXPECT_EQ(pt_read(f, TEST_PCI_SCRATCH), 0u);
        if (has_msix(f))
            EXPECT_EQ(msix_ctrl(f) & MSIX_ENTRY_MASKED, MSIX_ENTRY_MASKED);
    }
}


/* A function on a PCI Express bus offers a function level reset, which puts
   the function alone back: its configuration space and its registers. */
TEST(pci, function_level_reset)
{
    pt_require();
    PciFunc *f = nullptr;
    uint32_t cap = 0;
    for (int i = 0; PciFunc *g = pt_get(i); i++) {
        if ((cap = pci_find_cap(g, PCI_CAP_EXP)) != 0) {
            f = g;
            break;
        }
    }
    if (f == nullptr)
        t_skip("no test function on PCI Express");
    ASSERT_TRUE((pci_read32(f, cap + EXP_DEVCAP) & EXP_DEVCAP_FLR) != 0);

    PciFunc *other = nullptr;
    for (int i = 0; PciFunc *g = pt_get(i); i++) {
        pt_write(g, TEST_PCI_SCRATCH, 0x1234 + i);
        if (g != f && other == nullptr)
            other = g;
    }
    pci_write16(f, cap + EXP_DEVCTL,
                pci_read16(f, cap + EXP_DEVCTL) | EXP_DEVCTL_FLR);
    /* the bit reads as 0, and the function no longer decodes */
    EXPECT_EQ(pci_read16(f, cap + EXP_DEVCTL) & EXP_DEVCTL_FLR, 0u);
    EXPECT_EQ(pci_read16(f, PCI_COMMAND), 0u);
    EXPECT_EQ(pci_read32(f, PCI_BAR(TEST_PCI_BAR_REGS)), 0u);

    pci_restore_all();
    EXPECT_EQ(pt_read(f, TEST_PCI_SCRATCH), 0u);
    if (other != nullptr)
        EXPECT_NE(pt_read(other, TEST_PCI_SCRATCH), 0u);
}


/* Setting a bridge's Secondary Bus Reset resets everything behind it and
   nothing else. */
TEST(pci, secondary_bus_reset)
{
    pt_require();
    PciFunc *f = nullptr;
    for (int i = 0; PciFunc *g = pt_get(i); i++) {
        if (g->parent != nullptr) {
            f = g;
            break;
        }
    }
    if (f == nullptr)
        t_skip("no test function behind a bridge");
    PciFunc *br = f->parent;
    for (int i = 0; PciFunc *g = pt_get(i); i++)
        pt_write(g, TEST_PCI_SCRATCH, 0x4321 + i);

    uint16_t ctl = pci_read16(br, PCI_BRIDGE_CONTROL);
    pci_write16(br, PCI_BRIDGE_CONTROL, ctl | BRIDGE_CTL_SBR);
    t_delay_us(1000);
    pci_write16(br, PCI_BRIDGE_CONTROL, ctl);
    /* the bridge itself keeps its configuration */
    EXPECT_EQ(pci_read8(br, PCI_SECONDARY_BUS), br->secondary);

    pci_restore_all();
    for (int i = 0; PciFunc *g = pt_get(i); i++) {
        if (behind(g, br))
            EXPECT_EQ(pt_read(g, TEST_PCI_SCRATCH), 0u);
        else
            EXPECT_EQ(pt_read(g, TEST_PCI_SCRATCH), 0x4321u + i);
    }
}
