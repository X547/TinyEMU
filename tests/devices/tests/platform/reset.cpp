/*
 * Resets of the machine, by every way a guest has of asking for one
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
#include "mmio.h"
#include "pci.h"
#include "test.h"
#include "testctl.h"

#ifdef __riscv
#include "riscv.h"
#endif

/* Each test resets the machine, so it gets one of its own: nothing after
   it runs on what the reset left. */

/* The reset test-control offers the suite: the machine starts over, and
   test-control counts it and keeps nothing else. */
FRESH_TEST(reset, test_control)
{
    if (!t_after_reset()) {
        EXPECT_EQ(tc_resets(), 0u);
        if (tc_irq_count() > 0)
            tc_irq_raise(0);
        t_reboot();
    }
    EXPECT_EQ(tc_resets(), 1u);
    EXPECT_EQ(tc_irq_levels(), 0u);
}

#ifdef __riscv

static void sbi_reboot()
{
    sbi_call(SBI_EXT_SRST, 0, SBI_SRST_COLD_REBOOT, SBI_SRST_REASON_NONE);
}

/* A supervisor asks the SBI, which temu answers without firmware. */
FRESH_TEST(reset, sbi_srst)
{
    if (!t_after_reset()) {
        t_require_mode("S");
        if (!sbi_probe(SBI_EXT_SRST))
            t_skip("no SBI system reset");
        t_reboot(sbi_reboot);
    }
    EXPECT_EQ(tc_resets(), 1u);
}

#endif

#if defined(__i386__) || defined(__x86_64__)

static void i8042_pulse()
{
    outb(0x64, 0xfe);
}

static void port92_reset()
{
    outb(0x92, inb(0x92) | 1);
}

/* The PIIX3's reset control register: a rising RCPU, with SRST for a
   hard reset. */
static void cf9_reset()
{
    outb(0xcf9, 0x02);
    outb(0xcf9, 0x06);
}

/* With an empty IDT every exception is a fault delivering another, and the
   third is a shutdown, which resets a PC. */
static void triple_fault()
{
    struct __attribute__((packed)) {
        uint16_t limit;
        uintptr_t base;
    } idtr = {0, 0};
    asm volatile("lidt %0; int3" : : "m"(idtr));
}

/* The keyboard controller's reset line, which every PC has had. */
FRESH_TEST(reset, i8042)
{
    if (!t_after_reset()) {
        t_require_device("PNP0303");
        t_reboot(i8042_pulse);
    }
    EXPECT_EQ(tc_resets(), 1u);
}

/* Port 0x92's fast reset, which the FADT also names. */
FRESH_TEST(reset, port92)
{
    if (!t_after_reset())
        t_reboot(port92_reset);
    EXPECT_EQ(tc_resets(), 1u);
}

FRESH_TEST(reset, piix3_cf9)
{
    if (!t_after_reset()) {
        if (pci_find(0x8086, 0x7000) == nullptr)
            t_skip("no PIIX3");
        t_reboot(cf9_reset);
    }
    EXPECT_EQ(tc_resets(), 1u);
    /* the register comes out of reset clear */
    EXPECT_EQ(inb(0xcf9), 0u);
}

FRESH_TEST(reset, triple_fault)
{
    if (!t_after_reset())
        t_reboot(triple_fault);
    EXPECT_EQ(tc_resets(), 1u);
}

#endif
