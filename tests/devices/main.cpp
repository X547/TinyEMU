/*
 * Device test suite
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
#include "acpi.h"
#include "arch.h"
#include "console.h"
#include "fdt.h"
#include "heap.h"
#include "memory.h"
#include "registry.h"
#include "test.h"

/* The protocol version the HELLO line announces; run.py checks it. */
#define PROTOCOL_VERSION 1

BootInfo gBoot;

[[noreturn]] void kmain()
{
    if (gBoot.fdt)
        fdt_load(gBoot.fdt);
    if (gBoot.rsdp)
        acpi_init(gBoot.rsdp);
    registry_build();
    console_init();
    memory_init();
    arch_init();

    const char *cmdline = gBoot.cmdline;
    if (gBoot.fdt) {
        const FdtNode *chosen = fdt_find_path("/chosen");
        cmdline = chosen ? fdt_prop_str(chosen, "bootargs") : nullptr;
    }
    if (cmdline == nullptr)
        cmdline = "";

    report("HELLO %d target=%s mode=%s out=%s devices=%d ram=%llu free=%llu "
           "tick_hz=%llu", PROTOCOL_VERSION, arch_target(), arch_mode(),
           console_kind(), registry_count(),
           (unsigned long long)memory_total(),
           (unsigned long long)heap_free_bytes(),
           (unsigned long long)arch_tick_hz());
    run_tests(cmdline);
}
