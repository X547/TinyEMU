/*
 * The devices the firmware tables describe
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

#include "acpi.h"
#include "fdt.h"

#define HW_MAX_COMPAT  4
#define HW_MAX_REGIONS 8
#define HW_MAX_IRQS    8

enum HwRegionKind {
    HW_MMIO,
    HW_IO,
};

struct HwRegion {
    HwRegionKind kind;
    uint64_t base;
    uint64_t size;
};

/* An interrupt as the tables give it. Resolving it to a controller input
   is the interrupt layer's business. */
struct HwIrq {
    /* FDT: the controller and its specifier cells */
    uint32_t parent;
    uint32_t cells[4];
    int ncells;
    /* ACPI: an ISA IRQ or a GSI */
    uint32_t line;
    bool isa;
    bool level;
    bool active_low;
};

/* One device node with its identities and resources, the same shape
   whichever table it came from. */
struct HwDevice {
    const char *name;  /* the FDT node name, or the ACPI path */
    /* FDT "compatible" entries, or the ACPI _HID then _CIDs */
    const char *compat[HW_MAX_COMPAT];
    int ncompat;
    HwRegion regions[HW_MAX_REGIONS];
    int nregions;
    HwIrq irqs[HW_MAX_IRQS];
    int nirqs;
    const FdtNode *fdt;
    const AcpiNode *acpi;
};

/* From whichever tables were loaded. */
void registry_build();
int registry_count();
const HwDevice *registry_at(int index);

bool hw_is(const HwDevice *dev, const char *compat);
/* The 'index'th device that is 'compat', or nullptr. */
const HwDevice *registry_find(const char *compat, int index = 0);
/* The first region of a kind, or nullptr. */
const HwRegion *hw_region(const HwDevice *dev, HwRegionKind kind,
                          int index = 0);
