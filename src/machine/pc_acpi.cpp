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
/* The bits the ACPI specification defines: in the status register the
   timer, bus master, global, power and sleep button, RTC, PCI Express wake
   and wake status, which a written one clears; in the enable register
   their enables; in the control register the bus master reload and the
   sleep type, GBL_RLS and SLP_EN being write only. */
#define PM1_STS_DEFINED 0xc731
#define PM1_EN_DEFINED  0x4721
#define PM1_CNT_KEPT    0x1c03

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
        fStatus &= ~((val << shift) & PM1_STS_DEFINED);
        break;
    case 2: case 3:
        fEnable = ((fEnable & ~(0xff << shift)) | (val << shift)) &
            PM1_EN_DEFINED;
        break;
    case 4: case 5:
        fControl = (fControl & ~(0xff << shift)) | (val << shift);
        fControl |= PM1_CNT_SCI_EN;
        break;
    }
}


uint64_t AcpiPmBlock::DeviceRead(uint32_t offset, int size_log2)
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


void AcpiPmBlock::DeviceWrite(uint32_t offset, uint64_t val, int size_log2)
{
    for (int i = 0; i < (1 << size_log2); i++)
        WriteByte(offset + i, val >> (i * 8));
    if (fControl & PM1_CNT_SLP_EN) {
        fControl &= ~PM1_CNT_SLP_EN;
        if (get_bits(fControl, 10, 3) == SLP_TYP_S5)
            fMachine.RequestShutdown(0);
    }
    fControl &= PM1_CNT_KEPT;
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


/* A device of fixed resources, by its EISA ID. */
static AmlBytes fixed_device(const std::string &name, const char *hid,
                             const AmlBytes &r)
{
    AmlBytes dev = aml_name("_HID", aml_eisaid(hid));

    aml_append(dev, aml_name("_CRS", aml_resource_template(r)));
    return aml_device(name.c_str(), dev);
}

void pc_acpi_chipset(AcpiContext &ctx)
{
    AmlBytes r;

    aml_res_io(r, 0x20, 2);
    aml_res_io(r, 0xa0, 2);
    aml_res_irq(r, 2);
    aml_append(*ctx.aml, fixed_device(ctx.NewName("PIC_"), "PNP0000", r));

    r.clear();
    aml_res_io(r, 0x40, 4);
    aml_res_irq(r, 0);
    aml_append(*ctx.aml, fixed_device(ctx.NewName("TMR_"), "PNP0100", r));

    r.clear();
    aml_res_io(r, 0x61, 1);
    aml_append(*ctx.aml, fixed_device(ctx.NewName("SPKR"), "PNP0800", r));

    r.clear();
    aml_res_io(r, 0x70, 2);
    aml_res_irq(r, 8);
    aml_append(*ctx.aml, fixed_device(ctx.NewName("RTC_"), "PNP0B00", r));

    /* the ports no device above names: the POST code, the fast reset, the
       ELCR and the PM block */
    r.clear();
    aml_res_io(r, 0x80, 2);
    aml_res_io(r, 0x92, 2);
    aml_res_io(r, 0x4d0, 2);
    aml_res_io(r, ACPI_PM_BASE, ACPI_PM_SIZE);
    aml_append(*ctx.aml, fixed_device(ctx.NewName("MBRS"), "PNP0C02", r));
}

/* An OS may start only the processors the namespace declares too, by the
   UIDs of their MADT entries. */
void pc_acpi_processors(AcpiContext &ctx, int cpu_count)
{
    for (int i = 0; i < cpu_count; i++) {
        char name[8];
        snprintf(name, sizeof(name), "C%03X", i);
        AmlBytes dev = aml_name("_HID", aml_string("ACPI0007"));
        aml_append(dev, aml_name("_UID", aml_int(i)));
        aml_append(*ctx.aml, aml_device(ctx.NewName(name).c_str(), dev));
    }
}

static AmlBytes build_dsdt(const PcAcpiConfig &config)
{
    AmlBytes t = table_header("DSDT", 2);

    aml_append(t, aml_scope("\\_SB_", config.sb));

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

static AmlBytes build_mcfg(const PcAcpiConfig &config)
{
    AmlBytes t = table_header("MCFG", 1);

    aml_put64(t, 0); /* reserved */
    for (const AcpiEcam &e : config.ecams) {
        aml_put64(t, e.base);
        aml_put16(t, e.segment);
        aml_put8(t, 0); /* first bus */
        aml_put8(t, e.bus_last);
        aml_put32(t, 0); /* reserved */
    }
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
    if (!config.ecams.empty())
        tables.push_back(place(build_mcfg(config), 16));
    uint32_t rsdt = place(build_rsdt(tables), 16);
    uint32_t xsdt = place(build_xsdt(tables), 16);

    AmlBytes rsdp = build_rsdp(rsdt, xsdt);
    memcpy(mem, rsdp.data(), rsdp.size());
}
