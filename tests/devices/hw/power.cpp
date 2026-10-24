/*
 * Stopping the machine
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
#include "power.h"

#include "arch.h"
#include "console.h"
#include "mmio.h"
#include "test_control.h"

#ifdef __riscv
#include "riscv.h"
#endif

#define SIFIVE_TEST_PASS 0x5555
#define SIFIVE_TEST_FAIL 0x3333
#define PM1_CNT_SLP_EN   (1 << 13)

namespace {

/* Gives the write a moment to stop the machine before the next method is
   tried. */
void settle()
{
    uint64_t hz = arch_tick_hz();
    if (hz == 0) {
        for (int i = 0; i < 1000000; i++)
            __asm__ volatile("");
        return;
    }
    uint64_t end = arch_ticks() + hz / 10;
    while (arch_ticks() < end)
        arch_cpu_relax();
}

void try_test_control(int code)
{
    const HwDevice *d = test_control_device();
    const HwRegion *r = d ? hw_region(d, HW_MMIO) : nullptr;
    if (r == nullptr ||
        mmio_read32(r->base + TEST_CONTROL_ID) != TEST_CONTROL_MAGIC)
        return;
    mmio_write32(r->base + TEST_CONTROL_EXIT, code);
    settle();
}

void try_htif(int code)
{
    const HwDevice *d = registry_find("ucb,htif0");
    if (d == nullptr || d->nregions < 2)
        return;
    uint64_t tohost = d->regions[1].base;
    mmio_write32(tohost, (uint32_t)code << 1 | 1);
    mmio_write32(tohost + 4, 0);
    settle();
}

void try_syscon(int code)
{
    const HwDevice *d = registry_find("syscon-poweroff");
    if (d == nullptr || d->fdt == nullptr)
        return;
    uint32_t regmap, offset = 0, value = 0;
    if (!fdt_prop_u32(d->fdt, "regmap", &regmap) ||
        !fdt_prop_u32(d->fdt, "value", &value))
        return;
    fdt_prop_u32(d->fdt, "offset", &offset);
    const FdtNode *syscon = fdt_find_phandle(regmap);
    uint64_t base, size;
    if (syscon == nullptr || !fdt_reg(syscon, 0, &base, &size))
        return;
    /* the SiFive test device carries a status; anything else just stops */
    if (code != 0 && fdt_is_compatible(syscon, "sifive,test0"))
        value = (uint32_t)code << 16 | SIFIVE_TEST_FAIL;
    mmio_write32(base + offset, value);
    settle();
}

void try_sbi(int code)
{
#ifdef __riscv
    if (riscv_machine_mode() || !sbi_probe(SBI_EXT_SRST))
        return;
    sbi_call(SBI_EXT_SRST, 0, SBI_SRST_SHUTDOWN,
             code ? SBI_SRST_REASON_FAILURE : SBI_SRST_REASON_NONE);
    settle();
#else
    (void)code;
#endif
}

void try_acpi()
{
#if defined(__i386__) || defined(__x86_64__)
    uint16_t pm1a, pm1b;
    const AcpiNode *s5 = acpi_present() ? acpi_find("\\_S5_") : nullptr;
    AmlValue typa, typb;
    if (s5 == nullptr || !acpi_pm1_control(&pm1a, &pm1b) ||
        !aml_package_element(s5->value, 0, &typa) ||
        typa.type != AML_INTEGER)
        return;
    if (!aml_package_element(s5->value, 1, &typb) || typb.type != AML_INTEGER)
        typb = typa;
    outw(pm1a, (typa.integer & 7) << 10 | PM1_CNT_SLP_EN);
    if (pm1b)
        outw(pm1b, (typb.integer & 7) << 10 | PM1_CNT_SLP_EN);
    settle();
#endif
}

}


void power_off(int code)
{
    try_test_control(code);
    try_htif(code);
    try_syscon(code);
    try_sbi(code);
    try_acpi();
    if (console_ready())
        report("PANIC no way to power off");
    arch_halt();
}
