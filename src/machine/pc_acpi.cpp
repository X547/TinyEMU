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

typedef std::vector<uint8_t> Bytes;

static void put8(Bytes &b, uint8_t v)
{
    b.push_back(v);
}

static void put16(Bytes &b, uint16_t v)
{
    put8(b, v);
    put8(b, v >> 8);
}

static void put32(Bytes &b, uint32_t v)
{
    put16(b, v);
    put16(b, v >> 16);
}

static void put64(Bytes &b, uint64_t v)
{
    put32(b, v);
    put32(b, v >> 32);
}

static void put_str(Bytes &b, const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        put8(b, s[i]);
}

static void append(Bytes &b, const Bytes &more)
{
    b.insert(b.end(), more.begin(), more.end());
}

static uint8_t checksum(const uint8_t *p, size_t len)
{
    uint8_t sum = 0;

    for (size_t i = 0; i < len; i++)
        sum += p[i];
    return -sum;
}

#define OEM_ID "TINYEM"
#define OEM_TABLE_ID "TINYEMU "

static Bytes table_header(const char *sig, uint8_t revision)
{
    Bytes t;

    put_str(t, sig, 4);
    put32(t, 0); /* length */
    put8(t, revision);
    put8(t, 0); /* checksum */
    put_str(t, OEM_ID, 6);
    put_str(t, OEM_TABLE_ID, 8);
    put32(t, 1); /* OEM revision */
    put_str(t, "TEMU", 4);
    put32(t, 1); /* creator revision */
    return t;
}

static void table_finish(Bytes &t)
{
    put_le32(&t[4], t.size());
    t[9] = 0;
    t[9] = checksum(t.data(), t.size());
}


/* AML */

/* A PkgLength counts itself, so its size decides its value. */
static void aml_pkglength(Bytes &b, size_t body_len)
{
    if (body_len + 1 < 0x40) {
        put8(b, body_len + 1);
        return;
    }
    for (int n = 2; n <= 4; n++) {
        size_t len = body_len + n;
        if (len < ((size_t)1 << (4 + 8 * (n - 1)))) {
            put8(b, ((n - 1) << 6) | (len & 0xf));
            for (int i = 1; i < n; i++)
                put8(b, len >> (4 + 8 * (i - 1)));
            return;
        }
    }
    assert(0);
}

static Bytes aml_with_length(std::initializer_list<uint8_t> op,
                             const Bytes &body)
{
    Bytes b(op);

    aml_pkglength(b, body.size());
    append(b, body);
    return b;
}

static Bytes aml_int(uint32_t v)
{
    Bytes b;

    if (v <= 1) {
        put8(b, v); /* ZeroOp, OneOp */
    } else if (v <= 0xff) {
        put8(b, 0x0a);
        put8(b, v);
    } else if (v <= 0xffff) {
        put8(b, 0x0b);
        put16(b, v);
    } else {
        put8(b, 0x0c);
        put32(b, v);
    }
    return b;
}

static Bytes aml_name(const char *seg, const Bytes &value)
{
    Bytes b;

    put8(b, 0x08); /* NameOp */
    put_str(b, seg, 4);
    append(b, value);
    return b;
}

static Bytes aml_package(const std::vector<Bytes> &elements)
{
    Bytes body;

    put8(body, elements.size());
    for (const Bytes &e : elements)
        append(body, e);
    return aml_with_length({0x12}, body); /* PackageOp */
}

static Bytes aml_buffer(const Bytes &data)
{
    Bytes body = aml_int(data.size());

    append(body, data);
    return aml_with_length({0x11}, body); /* BufferOp */
}

static Bytes aml_device(const char *seg, const Bytes &contents)
{
    Bytes body;

    put_str(body, seg, 4);
    append(body, contents);
    return aml_with_length({0x5b, 0x82}, body); /* DeviceOp */
}

/* EISAID(): three letters in five bits each and four hex digits, stored
   big endian. */
static Bytes aml_eisaid(const char *id)
{
    uint32_t v = ((id[0] - 0x40) << 26) | ((id[1] - 0x40) << 21) |
        ((id[2] - 0x40) << 16);

    for (int i = 3; i < 7; i++) {
        int c = id[i];
        v |= (c <= '9' ? c - '0' : c - 'A' + 10) << ((6 - i) * 4);
    }
    return aml_int(__builtin_bswap32(v));
}

/* A resource template of fixed ports and an ISA IRQ. */
static Bytes aml_isa_resources(std::initializer_list<uint16_t> ports,
                               int irq)
{
    Bytes r;

    for (uint16_t port : ports) {
        put8(r, 0x47); /* I/O port descriptor */
        put8(r, 1); /* 16 bit decode */
        put16(r, port);
        put16(r, port);
        put8(r, 1); /* alignment */
        put8(r, 1); /* length */
    }
    put8(r, 0x22); /* IRQ descriptor */
    put16(r, 1 << irq);
    put8(r, 0x79); /* end tag */
    put8(r, 0);
    return aml_buffer(r);
}

/* A resource template of one fixed read/write memory range. */
static Bytes aml_memory_resources(uint32_t base, uint32_t size)
{
    Bytes r;

    put8(r, 0x86); /* 32 bit fixed memory range descriptor */
    put16(r, 9);
    put8(r, 1); /* read/write */
    put32(r, base);
    put32(r, size);
    put8(r, 0x79); /* end tag */
    put8(r, 0);
    return aml_buffer(r);
}

/* Address space descriptors, flagged fixed at both ends. */
static void word_space(Bytes &r, int type, int type_flags, uint16_t min,
                       uint16_t max)
{
    put8(r, 0x88); /* word address space descriptor */
    put16(r, 13);
    put8(r, type);
    put8(r, 0x0c);
    put8(r, type_flags);
    put16(r, 0); /* granularity */
    put16(r, min);
    put16(r, max);
    put16(r, 0); /* translation */
    put16(r, max - min + 1);
}

static void dword_memory(Bytes &r, uint32_t min, uint32_t max)
{
    put8(r, 0x87); /* dword address space descriptor */
    put16(r, 23);
    put8(r, 0); /* memory */
    put8(r, 0x0c);
    put8(r, 1); /* read/write, not cacheable */
    put32(r, 0); /* granularity */
    put32(r, min);
    put32(r, max);
    put32(r, 0); /* translation */
    put32(r, max - min + 1);
}

static void qword_memory(Bytes &r, uint64_t min, uint64_t max)
{
    put8(r, 0x8a); /* qword address space descriptor */
    put16(r, 43);
    put8(r, 0); /* memory */
    put8(r, 0x0c);
    put8(r, 1); /* read/write, not cacheable */
    put64(r, 0); /* granularity */
    put64(r, min);
    put64(r, max);
    put64(r, 0); /* translation */
    put64(r, max - min + 1);
}

/* What the host bridge decodes: every bus, the port space but for the
   configuration ports, the VGA hole and its apertures. */
static Bytes pci0_resources(const PcAcpiConfig &config)
{
    Bytes r;

    word_space(r, 2, 0, 0x00, 0xff);
    put8(r, 0x47); /* the configuration ports, which it consumes */
    put8(r, 1);
    put16(r, 0xcf8);
    put16(r, 0xcf8);
    put8(r, 1);
    put8(r, 8);
    word_space(r, 1, 3, 0x0000, 0x0cf7);
    word_space(r, 1, 3, 0x0d00, 0xffff);
    const PcPciApertures &a = config.pci_apertures;
    dword_memory(r, 0xa0000, 0xbffff);
    if (a.mmio_end > a.mmio_base)
        dword_memory(r, a.mmio_base, a.mmio_end - 1);
    if (a.mmio64_end > a.mmio64_base)
        qword_memory(r, a.mmio64_base, a.mmio64_end - 1);
    put8(r, 0x79); /* end tag */
    put8(r, 0);
    return aml_buffer(r);
}

static Bytes build_dsdt(const PcAcpiConfig &config)
{
    Bytes t = table_header("DSDT", 2);
    Bytes sb;

    /* The host bridge. Every slot's INTx pins are swizzled onto PIRQA-D
       the way the root bus does it. */
    std::vector<Bytes> prt;
    for (int slot = 0; slot < 32; slot++) {
        for (int pin = 0; pin < 4; pin++) {
            prt.push_back(aml_package({
                aml_int((slot << 16) | 0xffff), aml_int(pin), aml_int(0),
                aml_int(config.pci_gsis[(pin + slot - 1) & 3]),
            }));
        }
    }
    Bytes pci0 = aml_name("_HID", aml_eisaid("PNP0A03"));
    append(pci0, aml_name("_ADR", aml_int(0)));
    append(pci0, aml_name("_CRS", pci0_resources(config)));
    append(pci0, aml_name("_PRT", aml_package(prt)));
    append(sb, aml_device("PCI0", pci0));

    /* Linux finds the i8042 only through these once ACPI is on. */
    if (config.i8042) {
        Bytes kbd = aml_name("_HID", aml_eisaid("PNP0303"));
        append(kbd, aml_name("_CRS", aml_isa_resources({0x60, 0x64}, 1)));
        append(sb, aml_device("KBD_", kbd));
        Bytes mou = aml_name("_HID", aml_eisaid("PNP0F13"));
        append(mou, aml_name("_CRS", aml_isa_resources({}, 12)));
        append(sb, aml_device("MOU_", mou));
    }

    /* what reserves the HPET's registers */
    if (config.hpet_block_id != 0) {
        Bytes hpet = aml_name("_HID", aml_eisaid("PNP0103"));
        append(hpet, aml_name("_UID", aml_int(0)));
        append(hpet, aml_name("_CRS", aml_memory_resources(HPET_ADDR,
                                                           HPET_SIZE)));
        append(sb, aml_device("HPET", hpet));
    }

    Bytes scope;
    put_str(scope, "\\_SB_", 5);
    append(scope, sb);
    append(t, aml_with_length({0x10}, scope)); /* ScopeOp */

    append(t, aml_name("_S5_", aml_package({
        aml_int(SLP_TYP_S5), aml_int(SLP_TYP_S5), aml_int(0), aml_int(0),
    })));
    table_finish(t);
    return t;
}

/* Where the 8259 inputs land on the IOAPIC differs from pin = IRQ for the
   PIT only; the SCI is the level triggered, active high line ACPI says it
   is. */
static void madt_override(Bytes &t, int irq, int gsi, uint16_t flags)
{
    put8(t, 2); /* interrupt source override */
    put8(t, 10);
    put8(t, 0); /* ISA */
    put8(t, irq);
    put32(t, gsi);
    put16(t, flags);
}

static Bytes build_madt(const PcAcpiConfig &config)
{
    Bytes t = table_header("APIC", 3);

    put32(t, 0xfee00000); /* local APIC address */
    put32(t, 1); /* PCAT_COMPAT: there are 8259s */
    for (int i = 0; i < config.cpu_count; i++) {
        put8(t, 0); /* processor local APIC */
        put8(t, 8);
        put8(t, i); /* processor UID */
        put8(t, i); /* APIC ID */
        put32(t, 1); /* enabled */
    }
    put8(t, 1); /* I/O APIC */
    put8(t, 12);
    put8(t, 0); /* ID */
    put8(t, 0);
    put32(t, IOAPIC_ADDR);
    put32(t, 0); /* first GSI */
    madt_override(t, 0, 2, 0);
    madt_override(t, ACPI_SCI_IRQ, ACPI_SCI_IRQ,
                  1 | (3 << 2)); /* active high, level */
    /* NMI on LINT1 of every processor */
    put8(t, 4);
    put8(t, 6);
    put8(t, 0xff);
    put16(t, 0);
    put8(t, 1);
    table_finish(t);
    return t;
}

static Bytes build_hpet(const PcAcpiConfig &config)
{
    Bytes t = table_header("HPET", 1);

    put32(t, config.hpet_block_id);
    /* the registers, as a generic address */
    put8(t, 0); /* system memory */
    put8(t, 64); /* bit width */
    put8(t, 0); /* bit offset */
    put8(t, 0); /* access size: undefined */
    put64(t, HPET_ADDR);
    put8(t, 0); /* HPET number */
    put16(t, 0); /* minimum periodic tick: no minimum */
    put8(t, 0); /* page protection: none */
    table_finish(t);
    return t;
}

static Bytes build_facs()
{
    Bytes t;

    put_str(t, "FACS", 4);
    put32(t, 64);
    t.resize(32);
    put8(t, 1); /* version */
    t.resize(64);
    return t;
}

/* An ACPI 3.0 (revision 4) FADT; the X_ blocks are left for the OS to
   derive from the 32 bit ones. */
static Bytes build_fadt(const PcAcpiConfig &config, uint32_t facs,
                        uint32_t dsdt)
{
    Bytes t = table_header("FACP", 4);

    put32(t, facs);
    put32(t, dsdt);
    put8(t, 0); /* reserved */
    put8(t, 0); /* preferred PM profile: unspecified */
    put16(t, ACPI_SCI_IRQ);
    put32(t, 0); /* SMI_CMD: always in ACPI mode */
    put8(t, 0); /* ACPI_ENABLE */
    put8(t, 0); /* ACPI_DISABLE */
    put8(t, 0); /* S4BIOS_REQ */
    put8(t, 0); /* PSTATE_CNT */
    put32(t, ACPI_PM_BASE); /* PM1a_EVT_BLK */
    put32(t, 0); /* PM1b_EVT_BLK */
    put32(t, ACPI_PM_BASE + 4); /* PM1a_CNT_BLK */
    put32(t, 0); /* PM1b_CNT_BLK */
    put32(t, 0); /* PM2_CNT_BLK */
    put32(t, ACPI_PM_BASE + 8); /* PM_TMR_BLK */
    put32(t, 0); /* GPE0_BLK */
    put32(t, 0); /* GPE1_BLK */
    put8(t, 4); /* PM1_EVT_LEN */
    put8(t, 2); /* PM1_CNT_LEN */
    put8(t, 0); /* PM2_CNT_LEN */
    put8(t, 4); /* PM_TMR_LEN */
    put8(t, 0); /* GPE0_BLK_LEN */
    put8(t, 0); /* GPE1_BLK_LEN */
    put8(t, 0); /* GPE1_BASE */
    put8(t, 0); /* CST_CNT */
    put16(t, 101); /* P_LVL2_LAT: no C2 */
    put16(t, 1001); /* P_LVL3_LAT: no C3 */
    put16(t, 0); /* FLUSH_SIZE */
    put16(t, 0); /* FLUSH_STRIDE */
    put8(t, 0); /* DUTY_OFFSET */
    put8(t, 0); /* DUTY_WIDTH */
    put8(t, 0); /* DAY_ALRM */
    put8(t, 0); /* MON_ALRM */
    put8(t, 0x32); /* CENTURY, in CMOS */
    /* IAPC_BOOT_ARCH: LEGACY_DEVICES, and 8042 when there is one */
    put16(t, bit_at(0) | (config.i8042 ? bit_at(1) : 0));
    put8(t, 0); /* reserved */
    /* WBINVD, PROC_C1, PWR_BUTTON and SLP_BUTTON (neither is a fixed
       feature here), TMR_VAL_EXT, RESET_REG_SUP */
    put32(t, bit_at(0) | bit_at(2) | bit_at(4) | bit_at(5) | bit_at(8) |
          bit_at(10));
    /* RESET_REG: the fast reset bit of port 0x92 */
    put8(t, 1); /* system I/O */
    put8(t, 8); /* bit width */
    put8(t, 0); /* bit offset */
    put8(t, 1); /* byte access */
    put64(t, 0x92);
    put8(t, 1); /* RESET_VALUE */
    put16(t, 0); /* ARM_BOOT_ARCH */
    put8(t, 0); /* FADT minor version */
    put64(t, 0); /* X_FIRMWARE_CTRL: FIRMWARE_CTRL holds it */
    put64(t, dsdt); /* X_DSDT */
    t.resize(244); /* X_PM1a_EVT_BLK to X_GPE1_BLK */
    table_finish(t);
    return t;
}

static Bytes build_rsdt(const std::vector<uint32_t> &tables)
{
    Bytes t = table_header("RSDT", 1);

    for (uint32_t addr : tables)
        put32(t, addr);
    table_finish(t);
    return t;
}

static Bytes build_xsdt(const std::vector<uint32_t> &tables)
{
    Bytes t = table_header("XSDT", 1);

    for (uint32_t addr : tables)
        put64(t, addr);
    table_finish(t);
    return t;
}

static Bytes build_rsdp(uint32_t rsdt, uint32_t xsdt)
{
    Bytes t;

    put_str(t, "RSD PTR ", 8);
    put8(t, 0); /* checksum of the first 20 bytes */
    put_str(t, OEM_ID, 6);
    put8(t, 2); /* revision */
    put32(t, rsdt);
    put32(t, 36); /* length */
    put64(t, xsdt);
    put8(t, 0); /* extended checksum */
    put8(t, 0);
    put16(t, 0);
    t[8] = checksum(t.data(), 20);
    t[32] = checksum(t.data(), t.size());
    return t;
}


void pc_acpi_build(uint8_t *mem, const PcAcpiConfig &config)
{
    size_t pos = 64; /* after the RSDP */

    /* Places a table, aligned as the FACS must be and the rest may be. */
    auto place = [mem, &pos](const Bytes &t, size_t align) {
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

    Bytes rsdp = build_rsdp(rsdt, xsdt);
    memcpy(mem, rsdp.data(), rsdp.size());
}
