/*
 * CFI parallel NOR flash
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

#include <memory>

#include "device.h"
#include "host_block.h"

/* the erase block size QEMU's virt machines give their flash */
#define CFI_FLASH_DEFAULT_BLOCK_SIZE (256 << 10)

/* The "cfi-flash" configuration node: a flash bank on the FDT bus, read in
   place and programmed with the Intel command set. It holds the image file,
   rounded up to a power of two with erased blocks. 'base' of -1 lets the bus
   place it; 'read_only' refuses every program and erase. */
Device *cfi_flash_node_create(const char *name,
                              std::unique_ptr<HostBlockDevice> bs,
                              int64_t base, uint32_t block_size,
                              bool read_only);
