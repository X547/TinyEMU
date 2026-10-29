/*
 * PCI test function
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

/* Identifiers made up for the fixture, which no real part uses. */
#define TEST_PCI_VENDOR_ID 0x7e57
#define TEST_PCI_DEVICE_ID 0x0001
#define TEST_PCI_REVISION  1
#define TEST_PCI_CLASS     0xff00

/* Base address registers. BAR 1 is there only on a bus with port space,
   BAR 5 only with the "probe" property. */
#define TEST_PCI_BAR_REGS  0 /* 32 bit memory: the registers */
#define TEST_PCI_BAR_IO    1 /* I/O: the registers again */
#define TEST_PCI_BAR_MEM64 2 /* 64 bit prefetchable memory, with BAR 3:
                                the registers, repeated every 4 KB */
#define TEST_PCI_BAR_MSIX  4 /* 32 bit prefetchable memory: the MSI-X table
                                and pending bit array */

#define TEST_PCI_REGS_SIZE  0x1000
#define TEST_PCI_IO_SIZE    0x40
#define TEST_PCI_BAR_PROBE 5 /* 32 bit memory: a window registered with the
                                widths "probe" lists, which logs the
                                accesses that reach it */

#define TEST_PCI_MSIX_SIZE  0x1000
#define TEST_PCI_MSIX_TABLE 0x000
#define TEST_PCI_MSIX_PBA   0x800
#define TEST_PCI_ROM_SIZE   0x800 /* 0x55 0xaa, then each byte its offset */

/* Registers, 32 bits wide. */
#define TEST_PCI_ID       0x00 /* RO: TEST_PCI_MAGIC */
#define TEST_PCI_VERSION  0x04 /* RO */
#define TEST_PCI_INTX     0x08 /* RW: bit 0 is the level of the pin */
#define TEST_PCI_MSI_SEND 0x0c /* WO: sends this vector */
#define TEST_PCI_MSI_INFO 0x10 /* RO: below */
#define TEST_PCI_SCRATCH  0x14 /* RW */
#define TEST_PCI_DMA_LO   0x18 /* RW: the address DMA_DATA reaches */
#define TEST_PCI_DMA_HI   0x1c /* RW */
#define TEST_PCI_DMA_DATA 0x20 /* RW: the 32 bits of RAM at that address */
#define TEST_PCI_DMA_STATUS 0x24 /* RO: 1 when the last access found no RAM */
#define TEST_PCI_PROBE_COUNT  0x28 /* RW: the accesses logged; a write clears
                                      the log and refills the window */
#define TEST_PCI_PROBE_SELECT 0x2c /* RW: the entry the next three read */
#define TEST_PCI_PROBE_ACCESS 0x30 /* RO: below */
#define TEST_PCI_PROBE_VAL_LO 0x34 /* RO: the value read or written */
#define TEST_PCI_PROBE_VAL_HI 0x38 /* RO */
#define TEST_PCI_PROBE_INFO   0x3c /* RO: how the window is registered, 0
                                      for no window */

#define TEST_PCI_PROBE_SIZE 0x1000
#define TEST_PCI_PROBE_LOG  16    /* entries kept; COUNT goes on counting */
/* what a byte of the window holds when it is filled */
#define TEST_PCI_PROBE_BYTE(offset) ((uint8_t)~(offset))
/* PROBE_ACCESS */
#define TEST_PCI_PROBE_OFFSET_MASK 0xffff
#define TEST_PCI_PROBE_SIZE_SHIFT  16 /* log2 of the width */
#define TEST_PCI_PROBE_WRITE       0x1000000
/* PROBE_INFO: bit n for a width of 1 << n bytes, and */
#define TEST_PCI_PROBE_MERGE     0x10 /* "probe_merge" */
#define TEST_PCI_PROBE_UNALIGNED 0x20 /* "probe_unaligned" */

#define TEST_PCI_MSI_PRESENT 0x1    /* MSI or MSI-X was offered */
#define TEST_PCI_MSI_ENABLED 0x2    /* and the guest turned one on */
#define TEST_PCI_MSI_COUNT_SHIFT 16    /* the vectors it may name now */

#define TEST_PCI_MAGIC    0x50434954 /* "PCIT" */
#define TEST_PCI_VERSION_VALUE 1
