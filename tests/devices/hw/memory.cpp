/*
 * RAM discovery
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
#include "memory.h"

#include "arch.h"
#include "fdt.h"
#include "heap.h"

#define E820_RAM 1

namespace {

uint64_t sTotal;

struct E820Entry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
    uint32_t reserved;
};

void from_fdt()
{
    for (const FdtNode *n = fdt_root(); n; n = fdt_next(n)) {
        const char *type = fdt_prop_str(n, "device_type");
        if (type == nullptr || strcmp(type, "memory") != 0 ||
            !fdt_is_enabled(n))
            continue;
        uint64_t base, size;
        for (int i = 0; fdt_reg(n, i, &base, &size); i++) {
            heap_add(base, size);
            sTotal += size;
        }
    }

    heap_reserve((uintptr_t)gBoot.fdt, fdt_blob_size());
    for (int i = 0; i < fdt_reservation_count(); i++) {
        uint64_t base, size;
        fdt_reservation(i, &base, &size);
        heap_reserve(base, size);
    }
    const FdtNode *rm = fdt_find_path("/reserved-memory");
    for (const FdtNode *c = rm ? rm->child : nullptr; c; c = c->sibling) {
        uint64_t base, size;
        for (int i = 0; fdt_reg(c, i, &base, &size); i++)
            heap_reserve(base, size);
    }
}

void from_e820()
{
    const E820Entry *e = (const E820Entry *)gBoot.memmap;
    for (uint32_t i = 0; i < gBoot.memmap_entries; i++) {
        if (e[i].type != E820_RAM)
            continue;
        heap_add(e[i].addr, e[i].size);
        sTotal += e[i].size;
    }
    /* left for real mode trampolines and whatever the BIOS area holds */
    heap_reserve(0, 0x100000);
    /* the hvm_start_info, command line and memory map live in RAM the map
       does not mark, below 1 MB on temu; reserved above all the same */
    if (gBoot.cmdline)
        heap_reserve((uintptr_t)gBoot.cmdline, strlen(gBoot.cmdline) + 1);
    heap_reserve((uintptr_t)gBoot.memmap,
                 gBoot.memmap_entries * sizeof(E820Entry));
}

}


void memory_init()
{
    if (gBoot.fdt)
        from_fdt();
    else
        from_e820();
    heap_reserve(gBoot.image_start, gBoot.image_end - gBoot.image_start);
    if (heap_free_bytes() == 0)
        panic("no free RAM");
}


uint64_t memory_total()
{
    return sTotal;
}
