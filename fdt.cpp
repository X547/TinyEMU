/*
 * Flattened Device Tree builder
 *
 * Copyright (c) 2016-2017 Fabrice Bellard
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
#include "fdt.h"

#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include "cutils.h"


#define FDT_MAGIC   0xd00dfeed
#define FDT_VERSION 17

struct fdt_header {
    uint32_t magic;
    uint32_t totalsize;
    uint32_t off_dt_struct;
    uint32_t off_dt_strings;
    uint32_t off_mem_rsvmap;
    uint32_t version;
    uint32_t last_comp_version; /* <= 17 */
    uint32_t boot_cpuid_phys;
    uint32_t size_dt_strings;
    uint32_t size_dt_struct;
};

struct fdt_reserve_entry {
    uint64_t address;
    uint64_t size;
};

#define FDT_BEGIN_NODE 1
#define FDT_END_NODE   2
#define FDT_PROP       3
#define FDT_NOP        4
#define FDT_END        9


FDTBuilder::~FDTBuilder()
{
    free(fTab);
    free(fStringTable);
}


void FDTBuilder::AllocLen(int len)
{
    if (unlikely(len > fTabSize)) {
        int new_size = max_int(len, fTabSize * 3 / 2);
        fTab = static_cast<uint32_t *>(realloc(fTab,
                                               new_size * sizeof(uint32_t)));
        fTabSize = new_size;
    }
}


void FDTBuilder::Put32(uint32_t v)
{
    AllocLen(fTabLen + 1);
    fTab[fTabLen++] = cpu_to_be32(v);
}


/* the data is zero padded */
void FDTBuilder::PutData(const uint8_t *data, int len)
{
    int len1 = (len + 3) / 4;
    AllocLen(fTabLen + len1);
    memcpy(fTab + fTabLen, data, len);
    memset((uint8_t *)(fTab + fTabLen) + len, 0, -len & 3);
    fTabLen += len1;
}


void FDTBuilder::BeginNode(const char *name)
{
    Put32(FDT_BEGIN_NODE);
    PutData((const uint8_t *)name, strlen(name) + 1);
    fOpenNodeCount++;
}


void FDTBuilder::BeginNodeNum(const char *name, uint64_t n)
{
    char buf[256];
    snprintf(buf, sizeof(buf), "%s@%" PRIx64, name, n);
    BeginNode(buf);
}


void FDTBuilder::EndNode()
{
    Put32(FDT_END_NODE);
    fOpenNodeCount--;
}


int FDTBuilder::StringOffset(const char *name)
{
    int pos = 0;
    while (pos < fStringTableLen) {
        if (strcmp(fStringTable + pos, name) == 0) {
            return pos;
        }
        pos += strlen(fStringTable + pos) + 1;
    }
    /* add a new string */
    int name_size = strlen(name) + 1;
    int new_len = fStringTableLen + name_size;
    if (new_len > fStringTableSize) {
        int new_size = max_int(new_len, fStringTableSize * 3 / 2);
        fStringTable = static_cast<char *>(realloc(fStringTable, new_size));
        fStringTableSize = new_size;
    }
    pos = fStringTableLen;
    memcpy(fStringTable + pos, name, name_size);
    fStringTableLen = new_len;
    return pos;
}


void FDTBuilder::Prop(const char *prop_name, const void *data, int data_len)
{
    Put32(FDT_PROP);
    Put32(data_len);
    Put32(StringOffset(prop_name));
    PutData(static_cast<const uint8_t *>(data), data_len);
}


void FDTBuilder::PropTabU32(const char *prop_name, const uint32_t *tab,
                            int tab_len)
{
    Put32(FDT_PROP);
    Put32(tab_len * sizeof(uint32_t));
    Put32(StringOffset(prop_name));
    for (int i = 0; i < tab_len; i++) {
        Put32(tab[i]);
    }
}


void FDTBuilder::PropU32(const char *prop_name, uint32_t val)
{
    PropTabU32(prop_name, &val, 1);
}


void FDTBuilder::PropU64(const char *prop_name, uint64_t v0)
{
    uint32_t tab[2];
    tab[0] = v0 >> 32;
    tab[1] = v0;
    PropTabU32(prop_name, tab, 2);
}


void FDTBuilder::PropU64Range(const char *prop_name, uint64_t addr,
                              uint64_t size)
{
    uint32_t tab[4];
    tab[0] = addr >> 32;
    tab[1] = addr;
    tab[2] = size >> 32;
    tab[3] = size;
    PropTabU32(prop_name, tab, 4);
}


void FDTBuilder::PropStr(const char *prop_name, const char *str)
{
    Prop(prop_name, str, strlen(str) + 1);
}


void FDTBuilder::PropStrList(const char *prop_name, ...)
{
    va_list ap;
    int size, str_size;
    char *ptr, *tab;

    va_start(ap, prop_name);
    size = 0;
    for (;;) {
        ptr = va_arg(ap, char *);
        if (ptr == nullptr) {
            break;
        }
        size += strlen(ptr) + 1;
    }
    va_end(ap);

    tab = static_cast<char *>(malloc(size));
    va_start(ap, prop_name);
    size = 0;
    for (;;) {
        ptr = va_arg(ap, char *);
        if (ptr == nullptr) {
            break;
        }
        str_size = strlen(ptr) + 1;
        memcpy(tab + size, ptr, str_size);
        size += str_size;
    }
    va_end(ap);

    Prop(prop_name, tab, size);
    free(tab);
}


int FDTBuilder::Output(uint8_t *dst)
{
    struct fdt_header *h;
    struct fdt_reserve_entry *re;
    int dt_struct_size;
    int dt_strings_size;
    int pos;

    assert(fOpenNodeCount == 0);

    Put32(FDT_END);

    dt_struct_size = fTabLen * sizeof(uint32_t);
    dt_strings_size = fStringTableLen;

    h = (struct fdt_header *)dst;
    h->magic = cpu_to_be32(FDT_MAGIC);
    h->version = cpu_to_be32(FDT_VERSION);
    h->last_comp_version = cpu_to_be32(16);
    h->boot_cpuid_phys = cpu_to_be32(0);
    h->size_dt_strings = cpu_to_be32(dt_strings_size);
    h->size_dt_struct = cpu_to_be32(dt_struct_size);

    pos = sizeof(struct fdt_header);

    h->off_dt_struct = cpu_to_be32(pos);
    memcpy(dst + pos, fTab, dt_struct_size);
    pos += dt_struct_size;

    /* align to 8 */
    while ((pos & 7) != 0) {
        dst[pos++] = 0;
    }
    h->off_mem_rsvmap = cpu_to_be32(pos);
    re = (struct fdt_reserve_entry *)(dst + pos);
    re->address = 0; /* no reserved entry */
    re->size = 0;
    pos += sizeof(struct fdt_reserve_entry);

    h->off_dt_strings = cpu_to_be32(pos);
    memcpy(dst + pos, fStringTable, dt_strings_size);
    pos += dt_strings_size;

    /* align to 8, just in case */
    while ((pos & 7) != 0) {
        dst[pos++] = 0;
    }

    h->totalsize = cpu_to_be32(pos);
    return pos;
}
