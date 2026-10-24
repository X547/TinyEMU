/*
 * ACPI tables and namespace
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

#include "klib.h"

/* Panics on an RSDP it cannot read. */
void acpi_init(const void *rsdp);
bool acpi_present();

/* The 'index'th table with this signature, or nullptr. */
const uint8_t *acpi_table(const char *signature, int index = 0);
int acpi_table_count();
const uint8_t *acpi_table_at(int index);

/* From the FADT. */
bool acpi_pm_timer(uint16_t *port, bool *ext32);
bool acpi_pm1_control(uint16_t *pm1a, uint16_t *pm1b);

/* From the MADT. */
struct MadtIoApic {
    uint32_t id;
    uint64_t address;
    uint32_t gsi_base;
};
struct MadtOverride {
    uint8_t isa_irq;
    uint32_t gsi;
    uint16_t flags; /* MPS INTI: polarity bits 0-1, trigger bits 2-3 */
};
bool acpi_madt_present();
uint64_t acpi_madt_lapic_address();
/* Whether the 8259s are there too (PCAT_COMPAT). */
bool acpi_madt_pcat_compat();
int acpi_madt_cpu_count();
/* APIC ID of the index'th enabled processor. */
uint32_t acpi_madt_cpu_apic_id(int index);
int acpi_madt_ioapic_count();
MadtIoApic acpi_madt_ioapic(int index);
int acpi_madt_override_count();
MadtOverride acpi_madt_override(int index);
/* The GSI an ISA IRQ arrives at, through the overrides. */
uint32_t acpi_isa_irq_gsi(uint8_t irq, uint16_t *flags);


/* The namespace the DSDT and SSDTs declare, as far as it is made of named
   objects: Scope, Device and Name are read, a Method is recorded but not
   run. That is all firmware tables describing fixed hardware need. */

enum AmlType {
    AML_NONE,
    AML_INTEGER,
    AML_STRING,
    AML_BUFFER,
    AML_PACKAGE,
};

struct AmlValue {
    AmlType type = AML_NONE;
    uint64_t integer = 0;
    /* a string (terminated), a buffer's bytes, or a package's encoded
       elements */
    const uint8_t *data = nullptr;
    uint32_t len = 0;
    uint32_t count = 0; /* package elements */
};

enum AcpiNodeKind {
    ACPI_SCOPE,
    ACPI_DEVICE,
    ACPI_NAME,
    ACPI_METHOD,
};

struct AcpiNode {
    char seg[5];
    AcpiNodeKind kind;
    AmlValue value; /* ACPI_NAME */
    AcpiNode *parent;
    AcpiNode *child;
    AcpiNode *sibling;
};

const AcpiNode *acpi_root();
const AcpiNode *acpi_next(const AcpiNode *node);
/* "\\_SB_.PCI0", or relative to 'scope' when it does not start with '\\' */
const AcpiNode *acpi_find(const char *path, const AcpiNode *scope = nullptr);
const AcpiNode *acpi_child(const AcpiNode *node, const char *seg);
/* The full path, "\\_SB_.COM0". */
void acpi_path(const AcpiNode *node, char *buf, size_t size);

/* Element 'index' of a package. */
bool aml_package_element(const AmlValue &pkg, uint32_t index, AmlValue *out);

/* A _HID or _CID value as a string: an EISA ID integer is decoded. */
bool aml_id_string(const AmlValue &v, char *buf, size_t size);


/* Resource template descriptors, as _CRS gives them. */
enum AcpiResourceKind {
    ACPI_RES_IO,
    ACPI_RES_MEMORY,
    ACPI_RES_IRQ,      /* an ISA IRQ */
    ACPI_RES_INTERRUPT,/* a GSI */
    ACPI_RES_BUS,
    ACPI_RES_OTHER,
};

struct AcpiResource {
    AcpiResourceKind kind;
    uint64_t base;
    uint64_t length;
    uint8_t type;    /* the descriptor's tag */
    bool level;      /* interrupts */
    bool active_low;
    /* the whole descriptor */
    const uint8_t *raw;
    uint32_t raw_len;
};

/* Calls 'fn' for each descriptor; false on a malformed template. An IRQ
   descriptor with several lines yields one resource per line. */
bool acpi_parse_resources(const AmlValue &buffer,
                          void (*fn)(void *ctx, const AcpiResource &res),
                          void *ctx);
