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
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "iomem.h"

class VirtMachine;


/* The PM registers the FADT points at, in the port space: the PM1a event
   block (status, then enable), the PM1a control block and the PM timer. */
#define ACPI_PM_BASE 0x600
#define ACPI_PM_SIZE 12
/* The SCI line the FADT names. Nothing raises it, since the PM block has no
   event that can be enabled. */
#define ACPI_SCI_IRQ 9


/* The fixed hardware of a system that is always in ACPI mode: enough to read
   the PM timer and to power off through the \_S5 sleep type. */
class AcpiPmBlock final: public DeviceIO {
private:
    VirtMachine &fMachine;
    uint16_t fStatus = 0;
    uint16_t fEnable = 0;
    uint16_t fControl;

    uint32_t ReadByte(uint32_t offset);
    void WriteByte(uint32_t offset, uint8_t val);

public:
    AcpiPmBlock(VirtMachine &machine);

    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;
};


struct PcAcpiConfig {
    /* a MADT: the local APIC of each processor and the IOAPIC */
    bool apic = false;
    int cpu_count = 1;
    /* an i8042, which the FADT and the DSDT then report */
    bool i8042 = false;
    /* an HPET, described by its capabilities' low half; 0 when there is
       none */
    uint32_t hpet_block_id = 0;
    /* the GSI each of PIRQA-D is routed to */
    const uint8_t *pci_gsis = nullptr;
    /* where the OS may place memory BARs: [base, end) */
    uint32_t pci_mmio_base = 0;
    uint32_t pci_mmio_end = 0;
};


/* Where the tables go: the RSDP first, where the legacy search of
   0xe0000-0xfffff finds it. */
#define PC_ACPI_ADDR 0xe0000
#define PC_ACPI_SIZE 0x10000

/* Writes the RSDP, RSDT, XSDT, FACS, FADT, DSDT, with APICs the MADT and
   with an HPET its table to 'mem', which the guest sees at PC_ACPI_ADDR. */
void pc_acpi_build(uint8_t *mem, const PcAcpiConfig &config);
