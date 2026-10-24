/*
 * ACPI tables and the PM block
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

#include "acpi.h"
#include "format.h"
#include "mmio.h"
#include "test.h"

/* The tables the PC machine builds for a kernel it loads, and the PM
   registers its FADT describes. */

namespace {

const uint8_t *need_fadt()
{
    const uint8_t *f = acpi_table("FACP");
    if (f == nullptr)
        t_skip("no FADT");
    return f;
}

uint8_t sum(const uint8_t *p, uint32_t len)
{
    uint8_t s = 0;
    for (uint32_t i = 0; i < len; i++)
        s += p[i];
    return s;
}

}


TEST(acpi, checksums)
{
    if (!acpi_present())
        t_skip("no ACPI tables");
    const uint8_t *rsdp = (const uint8_t *)gBoot.rsdp;
    EXPECT_EQ(sum(rsdp, 20), 0);
    if (rsdp[15] >= 2)
        EXPECT_EQ(sum(rsdp, le32(rsdp + 20)), 0);
    for (int i = 0; i < acpi_table_count(); i++) {
        const uint8_t *t = acpi_table_at(i);
        if (sum(t, le32(t + 4)) != 0)
            t_fail(__FILE__, __LINE__, "%.4s checksum", (const char *)t);
    }
}

TEST(acpi, tables)
{
    if (!acpi_present())
        t_skip("no ACPI tables");
    const uint8_t *rsdp = (const uint8_t *)gBoot.rsdp;
    SNAP("rsdp", "rev %u oem %.6s", rsdp[15], (const char *)rsdp + 9);
    char buf[256];
    size_t n = 0;
    for (int i = 0; i < acpi_table_count(); i++) {
        const uint8_t *t = acpi_table_at(i);
        n += snprintf(buf + n, sizeof(buf) - n, "%s%.4s/%u/%.6s", n ? " " : "",
                      (const char *)t, t[8], (const char *)t + 10);
    }
    SNAP("list", "%s", buf);
}

TEST(acpi, fadt)
{
    const uint8_t *f = need_fadt();
    SNAP("revision", "%u", f[8]);
    SNAP("sci_int", "%u", le16(f + 46));
    SNAP("smi_cmd", "%#x", le32(f + 48));
    SNAP("pm1a_evt", "%#x len %u", le32(f + 56), f[88]);
    SNAP("pm1a_cnt", "%#x len %u", le32(f + 64), f[89]);
    SNAP("pm_tmr", "%#x len %u", le32(f + 76), f[91]);
    SNAP("gpe0", "%#x len %u", le32(f + 80), f[92]);
    SNAP("iapc_boot_arch", "%#x", le16(f + 109));
    SNAP("flags", "%#x", le32(f + 112));
    if (le32(f + 4) >= 244)
        SNAP("x_pm_tmr", "%u %u %#llx", f[208], f[209],
             T_FMT_VAL(le64(f + 212)));
}

TEST(acpi, dsdt_devices)
{
    if (!acpi_present())
        t_skip("no ACPI tables");
    char buf[512];
    size_t n = 0;
    for (int i = 0; i < registry_count(); i++) {
        const HwDevice *d = registry_at(i);
        if (d->acpi == nullptr || d->ncompat == 0)
            continue;
        n += snprintf(buf + n, sizeof(buf) - n, "%s%s=%s", n ? " " : "",
                      d->name, d->compat[0]);
    }
    SNAP("devices", "%s", buf);
    const AcpiNode *s5 = acpi_find("\\_S5_");
    AmlValue typ;
    EXPECT_TRUE(s5 && aml_package_element(s5->value, 0, &typ));
}

TEST(acpi, pm_timer)
{
    const uint8_t *f = need_fadt();
    uint16_t port = le32(f + 76);
    bool ext32 = (le32(f + 112) >> 8) & 1;
    uint32_t mask = ext32 ? 0xffffffff : 0xffffff;
    uint32_t a = inl(port) & mask;
    t_delay_us(10000);
    uint32_t b = inl(port) & mask;
    uint32_t delta = (b - a) & mask;
    /* 3.579545 MHz: about 35795 in 10 ms */
    EXPECT_GT(delta, 30000u);
    EXPECT_LT(delta, 60000u);
    /* the bytes of one value read separately, high to low, carry over at
       most once */
    uint32_t whole = inl(port);
    uint32_t b3 = inb(port + 3), b2 = inb(port + 2), b1 = inb(port + 1);
    EXPECT_GE((b3 << 24 | b2 << 16 | b1 << 8), whole & 0xffffff00);
    /* writes do nothing */
    outl(port, 0);
    EXPECT_GE(inl(port) & mask, whole & mask);
}

TEST(acpi, pm1_registers)
{
    const uint8_t *f = need_fadt();
    uint16_t evt = le32(f + 56), cnt = le32(f + 64);
    /* status: write one to clear; enable: plain */
    SNAP("status", "%#x", inw(evt));
    outw(evt, 0xffff);
    EXPECT_EQ(inw(evt), 0);
    outw(evt + 2, 0xffff);
    SNAP("enable_all_ones", "%#x", inw(evt + 2));
    /* the enables the ACPI specification defines: timer, global, power
       and sleep button, RTC and PCI Express wake */
    EXPECT_EQ(inw(evt + 2), 0x4721);
    outw(evt + 2, 0);
    /* control: SCI_EN stays on, there being no SMI port to leave ACPI by */
    uint16_t c = inw(cnt);
    SNAP("control", "%#x", c);
    outw(cnt, 0);
    EXPECT_EQ(inw(cnt) & 1, 1);
    /* reserved bits and the write only GBL_RLS read 0 */
    outw(cnt, 0xc000 | 1 << 2);
    EXPECT_EQ(inw(cnt), 1);
    /* a sleep state other than S5 is not entered, and SLP_EN reads 0 */
    outw(cnt, 1 << 10 | 1 << 13);
    SNAP("after_s1", "%#x", inw(cnt));
    outb(cnt + 1, 0);
    SNAP("byte_write", "%#x", inw(cnt));
    outw(cnt, c);
    /* past the registers */
    SNAP("offset_6", "%#x", inw(evt + 6));
}

#endif
