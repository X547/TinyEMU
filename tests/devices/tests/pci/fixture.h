/*
 * The PCI test function, for the PCI tests
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

#include "pci.h"
#include "pci_test.h"
#include "test.h"

/* The test functions are temu's "pci-test" fixture, whose registers are in
   BAR 0. */

inline uint32_t pt_read(const PciFunc *f, uint32_t reg)
{
    return pci_bar_read(f, TEST_PCI_BAR_REGS, reg, 2);
}

inline void pt_write(const PciFunc *f, uint32_t reg, uint32_t value)
{
    pci_bar_write(f, TEST_PCI_BAR_REGS, reg, value, 2);
}

inline int pt_count()
{
    int n = 0;
    while (pci_find(TEST_PCI_VENDOR_ID, TEST_PCI_DEVICE_ID, n))
        n++;
    return n;
}

inline PciFunc *pt_get(int index)
{
    return pci_find(TEST_PCI_VENDOR_ID, TEST_PCI_DEVICE_ID, index);
}

/* Put the PCI functions and the test functions back as enumeration left
   them, whatever the test did. */
inline void pt_cleanup()
{
    pci_restore_all();
    for (int i = 0; PciFunc *f = pt_get(i); i++) {
        if (pci_reachable(f->host, f->bars[TEST_PCI_BAR_REGS].addr, 4, false))
            pt_write(f, TEST_PCI_INTX, 0);
    }
}

/* The first test function, or skip; the cleanup is registered. */
inline PciFunc *pt_require(int index = 0)
{
    PciFunc *f = pci_require(TEST_PCI_VENDOR_ID, TEST_PCI_DEVICE_ID, index);
    t_on_cleanup(pt_cleanup);
    return f;
}
