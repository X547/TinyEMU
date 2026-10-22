/*
 * ACPI tables and fixed hardware of the PC machine
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
#include "pc_acpi.h"

#include <assert.h>
#include <string.h>

#include <vector>

#include "acpi.h"
#include "bits.h"
#include "cutils.h"
#include "host_time.h"
#include "hpet.h"
#include "ioapic.h"
#include "machine.h"


/* PM1 control */
#define PM1_CNT_SCI_EN bit_at(0)
#define PM1_CNT_SLP_EN bit_at(13)

/* the sleep type \_S5 names */
#define SLP_TYP_S5 5

#define PM_TIMER_FREQ 3579545


AcpiPmBlock::AcpiPmBlock(VirtMachine &machine):
    fMachine(machine),
    /* there is no SMI command port, so ACPI mode is always on */
    fControl(PM1_CNT_SCI_EN)
{
}


uint32_t AcpiPmBlock::ReadByte(uint32_t offset)
{
    switch (offset) {
    case 0: case 1:
        return get_bits(fStatus, (offset & 1) * 8, 8);
    case 2: case 3:
        return get_bits(fEnable, (offset & 1) * 8, 8);
    case 4: case 5:
        return get_bits(fControl, (offset & 1) * 8, 8);
    default:
        return 0;
    }
}


void AcpiPmBlock::WriteByte(uint32_t offset, uint8_t val)
{
    int shift = (offset & 1) * 8;

    switch (offset) {
    case 0: case 1:
        fStatus &= ~(val << shift);
        break;
    case 2: case 3:
        fEnable = (fEnable & ~(0xff << shift)) | (val << shift);
        break;
    case 4: case 5:
        fControl = (fControl & ~(0xff << shift)) | (val << shift);
        fControl |= PM1_CNT_SCI_EN;
        break;
    }
}


uint32_t AcpiPmBlock::DeviceRead(uint32_t offset, int size_log2)
{
    uint32_t val = 0;

    /* the 32 bit PM timer, read whole */
    if (offset >= 8) {
        uint64_t us = host_monotonic_us();
        uint32_t ticks = (us / 1000000) * PM_TIMER_FREQ +
            (us % 1000000) * PM_TIMER_FREQ / 1000000;
        return ticks >> ((offset - 8) * 8);
    }
    for (int i = 0; i < (1 << size_log2); i++)
        val |= ReadByte(offset + i) << (i * 8);
    return val;
}


void AcpiPmBlock::DeviceWrite(uint32_t offset, uint32_t val, int size_log2)
{
    for (int i = 0; i < (1 << size_log2); i++)
        WriteByte(offset + i, val >> (i * 8));
    if (fControl & PM1_CNT_SLP_EN) {
        fControl &= ~PM1_CNT_SLP_EN;
        if (get_bits(fControl, 10, 3) == SLP_TYP_S5)
            fMachine.RequestShutdown(0);
    }
}


/* Tables */

static uint8_t checksum(const uint8_t *p, size_t len)
{
    uint8_t sum = 0;

    for (size_t i = 0; i < len; i++)
        sum += p[i];
    return -sum;
}

#define OEM_ID "TINYEM"
#define OEM_TABLE_ID "TINYEMU "

static AmlBytes table_header(const char *sig, uint8_t revision)
{
    AmlBytes t;

    aml_put_str(t, sig, 4);
    aml_put32(t, 0); /* length */
    aml_put8(t, revision);
    aml_put8(t, 0); /* checksum */
    aml_put_str(t, OEM_ID, 6);
    aml_put_str(t, OEM_TABLE_ID, 8);
    aml_put32(t, 1); /* OEM revision */
    aml_put_str(t, "TEMU", 4);
    aml_put32(t, 1); /* creator revision */
    return t;
}

static void table_finish(AmlBytes &t)
{
    put_le32(&t[4], t.size());
    t[9] = 0;
    t[9] = checksum(t.data(), t.size());
}


/* What the host bridge decodes: every bus, the port space but for the
   configuration ports, the VGA hole and its apertures. */
static AmlBytes pci0_resources(const PcAcpiConfig &config)
{
    AmlBytes r;

    aml_res_word_space(r, 2, 0, 0x00, 0xff);
    /* the configuration ports, which it consumes */
    aml_res_io(r, 0xcf8, 8);
    aml_res_word_space(r, 1, 3, 0x0000, 0x0cf7);
    aml_res_word_space(r, 1, 3, 0x0d00, 0xffff);
    const PcPciApertures &a = config.pci_apertures;
    aml_res_dword_memory(r, 0xa0000, 0xbffff);
    if (a.mmio_end > a.mmio_base)
        aml_res_dword_memory(r, a.mmio_base, a.mmio_end - 1);
    if (a.mmio64_end > a.mmio64_base)
        aml_res_qword_memory(r, a.mmio64_base, a.mmio64_end - 1);
    return aml_resource_template(r);
}

static AmlBytes build_dsdt(const PcAcpiConfig &config)
{
    AmlBytes t = table_header("DSDT", 2);
    AmlBytes sb;

    /* The host bridge. Every slot's INTx pins are swizzled onto PIRQA-D
       the way the root bus does it. */
    std::vector<AmlBytes> prt;
    for (int slot = 0; slot < 32; slot++) {
        for (int pin = 0; pin < 4; pin++) {
            prt.push_back(aml_package({
                aml_int((slot << 16) | 0xffff), aml_int(pin), aml_int(0),
                aml_int(config.pci_gsis[(pin + slot - 1) & 3]),
            }));
        }
    }
    AmlBytes pci0 = aml_name("_HID", aml_eisaid("PNP0A03"));
    aml_append(pci0, aml_name("_ADR", aml_int(0)));
    aml_append(pci0, aml_name("_CRS", pci0_resources(config)));
    aml_append(pci0, aml_name("_PRT", aml_package(prt)));
    aml_append(sb, aml_device("PCI0", pci0));

    /* Linux finds the i8042 only through these once ACPI is on. */
    if (config.i8042) {
        AmlBytes r;
        aml_res_io(r, 0x60, 1);
        aml_res_io(r, 0x64, 1);
        aml_res_irq(r, 1);
        AmlBytes kbd = aml_name("_HID", aml_eisaid("PNP0303"));
        aml_append(kbd, aml_name("_CRS", aml_resource_template(r)));
        aml_append(sb, aml_device("KBD_", kbd));
        r.clear();
        aml_res_irq(r, 12);
        AmlBytes mou = aml_name("_HID", aml_eisaid("PNP0F13"));
        aml_append(mou, aml_name("_CRS", aml_resource_template(r)));
        aml_append(sb, aml_device("MOU_", mou));
    }

    /* what reserves the HPET's registers */
    if (config.hpet_block_id != 0) {
        AmlBytes hpet = aml_name("_HID", aml_eisaid("PNP0103"));
        aml_append(hpet, aml_name("_UID", aml_int(0)));
        AmlBytes r;
        aml_res_memory32_fixed(r, HPET_ADDR, HPET_SIZE);
        aml_append(hpet, aml_name("_CRS", aml_resource_template(r)));
        aml_append(sb, aml_device("HPET", hpet));
    }

    aml_append(t, aml_scope("\\_SB_", sb));

    aml_append(t, aml_name("_S5_", aml_package({
        aml_int(SLP_TYP_S5), aml_int(SLP_TYP_S5), aml_int(0), aml_int(0),
    })));
    table_finish(t);
    return t;
}

/* Where the 8259 inputs land on the IOAPIC differs from pin = IRQ for the
   PIT only; the SCI is the level triggered, active high line ACPI says it
   is. */
static void madt_override(AmlBytes &t, int irq, int gsi, uint16_t flags)
{
    aml_put8(t, 2); /* interrupt source override */
    aml_put8(t, 10);
    aml_put8(t, 0); /* ISA */
    aml_put8(t, irq);
    aml_put32(t, gsi);
    aml_put16(t, flags);
}

static AmlBytes build_madt(const PcAcpiConfig &config)
{
    AmlBytes t = table_header("APIC", 3);

    aml_put32(t, 0xfee00000); /* local APIC address */
    aml_put32(t, 1); /* PCAT_COMPAT: there are 8259s */
    for (int i = 0; i < config.cpu_count; i++) {
        aml_put8(t, 0); /* processor local APIC */
        aml_put8(t, 8);
        aml_put8(t, i); /* processor UID */
        aml_put8(t, i); /* APIC ID */
        aml_put32(t, 1); /* enabled */
    }
    aml_put8(t, 1); /* I/O APIC */
    aml_put8(t, 12);
    aml_put8(t, 0); /* ID */
    aml_put8(t, 0);
    aml_put32(t, IOAPIC_ADDR);
    aml_put32(t, 0); /* first GSI */
    madt_override(t, 0, 2, 0);
    madt_override(t, ACPI_SCI_IRQ, ACPI_SCI_IRQ,
                  1 | (3 << 2)); /* active high, level */
    /* NMI on LINT1 of every processor */
    aml_put8(t, 4);
    aml_put8(t, 6);
    aml_put8(t, 0xff);
    aml_put16(t, 0);
    aml_put8(t, 1);
    table_finish(t);
    return t;
}

static AmlBytes build_hpet(const PcAcpiConfig &config)
{
    AmlBytes t = table_header("HPET", 1);

    aml_put32(t, config.hpet_block_id);
    /* the registers, as a generic address */
    aml_put8(t, 0); /* system memory */
    aml_put8(t, 64); /* bit width */
    aml_put8(t, 0); /* bit offset */
    aml_put8(t, 0); /* access size: undefined */
    aml_put64(t, HPET_ADDR);
    aml_put8(t, 0); /* HPET number */
    aml_put16(t, 0); /* minimum periodic tick: no minimum */
    aml_put8(t, 0); /* page protection: none */
    table_finish(t);
    return t;
}

static AmlBytes build_facs()
{
    AmlBytes t;

    aml_put_str(t, "FACS", 4);
    aml_put32(t, 64);
    t.resize(32);
    aml_put8(t, 1); /* version */
    t.resize(64);
    return t;
}

/* An ACPI 3.0 (revision 4) FADT; the X_ blocks are left for the OS to
   derive from the 32 bit ones. */
static AmlBytes build_fadt(const PcAcpiConfig &config, uint32_t facs,
                        uint32_t dsdt)
{
    AmlBytes t = table_header("FACP", 4);

    aml_put32(t, facs);
    aml_put32(t, dsdt);
    aml_put8(t, 0); /* reserved */
    aml_put8(t, 0); /* preferred PM profile: unspecified */
    aml_put16(t, ACPI_SCI_IRQ);
    aml_put32(t, 0); /* SMI_CMD: always in ACPI mode */
    aml_put8(t, 0); /* ACPI_ENABLE */
    aml_put8(t, 0); /* ACPI_DISABLE */
    aml_put8(t, 0); /* S4BIOS_REQ */
    aml_put8(t, 0); /* PSTATE_CNT */
    aml_put32(t, ACPI_PM_BASE); /* PM1a_EVT_BLK */
    aml_put32(t, 0); /* PM1b_EVT_BLK */
    aml_put32(t, ACPI_PM_BASE + 4); /* PM1a_CNT_BLK */
    aml_put32(t, 0); /* PM1b_CNT_BLK */
    aml_put32(t, 0); /* PM2_CNT_BLK */
    aml_put32(t, ACPI_PM_BASE + 8); /* PM_TMR_BLK */
    aml_put32(t, 0); /* GPE0_BLK */
    aml_put32(t, 0); /* GPE1_BLK */
    aml_put8(t, 4); /* PM1_EVT_LEN */
    aml_put8(t, 2); /* PM1_CNT_LEN */
    aml_put8(t, 0); /* PM2_CNT_LEN */
    aml_put8(t, 4); /* PM_TMR_LEN */
    aml_put8(t, 0); /* GPE0_BLK_LEN */
    aml_put8(t, 0); /* GPE1_BLK_LEN */
    aml_put8(t, 0); /* GPE1_BASE */
    aml_put8(t, 0); /* CST_CNT */
    aml_put16(t, 101); /* P_LVL2_LAT: no C2 */
    aml_put16(t, 1001); /* P_LVL3_LAT: no C3 */
    aml_put16(t, 0); /* FLUSH_SIZE */
    aml_put16(t, 0); /* FLUSH_STRIDE */
    aml_put8(t, 0); /* DUTY_OFFSET */
    aml_put8(t, 0); /* DUTY_WIDTH */
    aml_put8(t, 0); /* DAY_ALRM */
    aml_put8(t, 0); /* MON_ALRM */
    aml_put8(t, 0x32); /* CENTURY, in CMOS */
    /* IAPC_BOOT_ARCH: LEGACY_DEVICES, and 8042 when there is one */
    aml_put16(t, bit_at(0) | (config.i8042 ? bit_at(1) : 0));
    aml_put8(t, 0); /* reserved */
    /* WBINVD, PROC_C1, PWR_BUTTON and SLP_BUTTON (neither is a fixed
       feature here), TMR_VAL_EXT, RESET_REG_SUP */
    aml_put32(t, bit_at(0) | bit_at(2) | bit_at(4) | bit_at(5) | bit_at(8) |
          bit_at(10));
    /* RESET_REG: the fast reset bit of port 0x92 */
    aml_put8(t, 1); /* system I/O */
    aml_put8(t, 8); /* bit width */
    aml_put8(t, 0); /* bit offset */
    aml_put8(t, 1); /* byte access */
    aml_put64(t, 0x92);
    aml_put8(t, 1); /* RESET_VALUE */
    aml_put16(t, 0); /* ARM_BOOT_ARCH */
    aml_put8(t, 0); /* FADT minor version */
    aml_put64(t, 0); /* X_FIRMWARE_CTRL: FIRMWARE_CTRL holds it */
    aml_put64(t, dsdt); /* X_DSDT */
    t.resize(244); /* X_PM1a_EVT_BLK to X_GPE1_BLK */
    table_finish(t);
    return t;
}

static AmlBytes build_rsdt(const std::vector<uint32_t> &tables)
{
    AmlBytes t = table_header("RSDT", 1);

    for (uint32_t addr : tables)
        aml_put32(t, addr);
    table_finish(t);
    return t;
}

static AmlBytes build_xsdt(const std::vector<uint32_t> &tables)
{
    AmlBytes t = table_header("XSDT", 1);

    for (uint32_t addr : tables)
        aml_put64(t, addr);
    table_finish(t);
    return t;
}

static AmlBytes build_rsdp(uint32_t rsdt, uint32_t xsdt)
{
    AmlBytes t;

    aml_put_str(t, "RSD PTR ", 8);
    aml_put8(t, 0); /* checksum of the first 20 bytes */
    aml_put_str(t, OEM_ID, 6);
    aml_put8(t, 2); /* revision */
    aml_put32(t, rsdt);
    aml_put32(t, 36); /* length */
    aml_put64(t, xsdt);
    aml_put8(t, 0); /* extended checksum */
    aml_put8(t, 0);
    aml_put16(t, 0);
    t[8] = checksum(t.data(), 20);
    t[32] = checksum(t.data(), t.size());
    return t;
}


void pc_acpi_build(uint8_t *mem, const PcAcpiConfig &config)
{
    size_t pos = 64; /* after the RSDP */

    /* Places a table, aligned as the FACS must be and the rest may be. */
    auto place = [mem, &pos](const AmlBytes &t, size_t align) {
        pos = (pos + align - 1) & ~(align - 1);
        assert(pos + t.size() <= PC_ACPI_SIZE);
        memcpy(mem + pos, t.data(), t.size());
        uint32_t addr = PC_ACPI_ADDR + pos;
        pos += t.size();
        return addr;
    };

    uint32_t facs = place(build_facs(), 64);
    uint32_t dsdt = place(build_dsdt(config), 16);
    std::vector<uint32_t> tables;
    tables.push_back(place(build_fadt(config, facs, dsdt), 16));
    if (config.apic)
        tables.push_back(place(build_madt(config), 16));
    if (config.hpet_block_id != 0)
        tables.push_back(place(build_hpet(config), 16));
    uint32_t rsdt = place(build_rsdt(tables), 16);
    uint32_t xsdt = place(build_xsdt(tables), 16);

    AmlBytes rsdp = build_rsdp(rsdt, xsdt);
    memcpy(mem, rsdp.data(), rsdp.size());
}
