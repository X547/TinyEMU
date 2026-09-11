/*
 * SD Host Controller (SDHCI)
 *
 * Copyright (c) 2016-2018 Fabrice Bellard
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

#include <stdint.h>

#include "device.h"

/* The register file the specification defines is 256 bytes, but a page is the
   smallest thing worth mapping and is what the PCI binding gives a slot. */
#define SDHCI_REG_SIZE 0x1000

/* The base clock the controller reports, and the one the device tree node
   describes. It is only a number the guest divides down: nothing here is
   timed, so what it is chosen to be decides only what a driver prints. */
#define SDHCI_DEFAULT_CLOCK_HZ 50000000

/* Which controller a device tree node claims to be, and so which driver binds
   to it. The Arasan one is a plain SDHCI part with no platform glue beyond
   the two clocks this emits alongside it, which is why it is the default;
   "compatible" in the configuration overrides it for a guest whose driver
   probes for something else. */
#define SDHCI_DEFAULT_COMPATIBLE "arasan,sdhci-8.9a"

Device *sdhci_node_create(const char *name, const char *compatible,
                          uint32_t clock_hz);
