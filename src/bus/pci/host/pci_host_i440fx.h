/*
 * i440FX PCI host bridge
 *
 * Copyright (c) 2017 Fabrice Bellard
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

#include "device.h"
#include "pci.h"

typedef struct I440FXState I440FXState;

/* Memory ranges a host bridge forwards to its PCI devices, [base, end) below
   4 GB and above RAM. */
struct PcPciApertures {
    uint64_t mmio_base = 0;
    uint64_t mmio_end = 0;
    uint64_t mmio64_base = 0;
    uint64_t mmio64_end = 0;
};

enum I440FXVariant {
    I440FX_PC,
    /* the host bridge of Cloud Hypervisor, whose EDK2 build fixes the 32
       bit aperture and takes the 64 bit one above RAM */
    I440FX_CLOUD_HYPERVISOR,
};

/* The bridge a realized "pci-host-i440fx" or "pci-host-cloudhv" built, or
   null for any other device. The machine's no-BIOS path needs it: with no
   firmware to program the PIRQ registers it has to route the INTx lines
   itself. */
I440FXState *i440fx_node_state(Device *dev);

/* What a realized node decodes, for the machine to describe. */
PcPciApertures i440fx_node_apertures(Device *dev);

/* In case no BIOS is used, map the interrupts. */
void i440fx_map_interrupts(I440FXState *s, uint8_t *elcr,
                           const uint8_t *pci_irqs);
