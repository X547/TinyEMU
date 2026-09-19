/*
 * Synopsys DesignWare Mobile Storage Host controller
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

/* The register file and the data FIFO window above it, in one page. */
#define DW_MMC_REG_SIZE 0x1000

/* The card interface clock the node describes; the guest divides it down. */
#define DW_MMC_DEFAULT_CLOCK_HZ 50000000

#define DW_MMC_DEFAULT_COMPATIBLE "snps,dw-mshc"

/* 'dma' false reports a controller built without the internal DMA
   controller, so data moves through the FIFO only. */
Device *dw_mmc_node_create(const char *name, const char *compatible,
                           uint32_t clock_hz, bool dma);
