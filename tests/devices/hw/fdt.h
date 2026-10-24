/*
 * Device tree reader
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

/* The flattened device tree, parsed once into nodes that point into the
   blob. Read only: the blob is never changed. */

struct FdtProp {
    const char *name;
    const uint8_t *data;
    uint32_t len;
    FdtProp *next;
};

struct FdtNode {
    const char *name;      /* with the unit address: "serial@10000000" */
    FdtNode *parent;
    FdtNode *child;
    FdtNode *sibling;
    FdtProp *props;
    uint32_t phandle;
};

/* Panics on a blob it cannot read. */
void fdt_load(const void *blob);
uint32_t fdt_blob_size();

/* The memory reservation block. */
int fdt_reservation_count();
void fdt_reservation(int index, uint64_t *addr, uint64_t *size);

const FdtNode *fdt_root();
/* Depth first, the root first; nullptr after the last node. */
const FdtNode *fdt_next(const FdtNode *node);
/* An absolute path, whose components match a node's name with or without
   its unit address, or an alias. */
const FdtNode *fdt_find_path(const char *path);
const FdtNode *fdt_find_phandle(uint32_t phandle);
const FdtNode *fdt_find_compatible(const char *compat,
                                   const FdtNode *after = nullptr);

const FdtProp *fdt_prop(const FdtNode *node, const char *name);
bool fdt_prop_u32(const FdtNode *node, const char *name, uint32_t *out);
/* A string property, or nullptr. */
const char *fdt_prop_str(const FdtNode *node, const char *name);
bool fdt_is_compatible(const FdtNode *node, const char *compat);
/* "okay" or absent */
bool fdt_is_enabled(const FdtNode *node);

/* The cells the children of 'node' use. */
int fdt_address_cells(const FdtNode *node);
int fdt_size_cells(const FdtNode *node);

/* Entry 'index' of "reg", translated through the "ranges" of every bus
   above it to a processor address. False when there is no such entry or
   it does not map. */
bool fdt_reg(const FdtNode *node, int index, uint64_t *addr, uint64_t *size);
int fdt_reg_count(const FdtNode *node);

/* Read 'cells' big endian cells as one number. */
uint64_t fdt_read_cells(const uint8_t *p, int cells);
