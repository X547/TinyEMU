/*
 * What each target provides
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

/* The first address the image cannot reach: physical addresses with
   paging off, or the boot identity map. */
#if defined(__riscv) && __riscv_xlen == 64
#define ARCH_ADDRESS_LIMIT UINT64_MAX
#else
#define ARCH_ADDRESS_LIMIT 0x100000000ull
#endif

/* What the entry code found, for main(). */
struct BootInfo {
    const void *fdt = nullptr;     /* RISC-V */
    const void *rsdp = nullptr;    /* x86 */
    const char *cmdline = nullptr; /* x86; RISC-V takes /chosen/bootargs */
    /* x86: the PVH memory map, in E820 types */
    const void *memmap = nullptr;
    uint32_t memmap_entries = 0;
    uint64_t image_start = 0;
    uint64_t image_end = 0;
    uint32_t boot_cpu = 0;         /* hart ID */
};

extern BootInfo gBoot;

/* "riscv64", "riscv32", "i686" or "x86_64", and the privilege the image
   runs in: "M" or "S" on RISC-V, "pvh" on x86. */
const char *arch_target();
const char *arch_mode();

/* After discovery: whatever the target needs from the tables. */
void arch_init();

/* A counter that only goes up, and its rate. */
uint64_t arch_ticks();
uint64_t arch_tick_hz();

[[noreturn]] void arch_halt();
void arch_cpu_relax();

/* A trap nobody asked for. The arch code fills this in and hands it to
   trap_unexpected(), which does not return when it can recover. */
struct TrapInfo {
    uint64_t cause;     /* scause/mcause, or the x86 vector */
    uint64_t pc;
    uint64_t addr;      /* tval, or CR2 for a page fault */
    uint64_t error;     /* x86 error code */
    const char *name;
};
void trap_unexpected(const TrapInfo &info);

/* The callee saved registers, the stack pointer and the return address. */
struct JmpBuf {
    uintptr_t regs[16];
};
extern "C" int arch_setjmp(JmpBuf *buf) __attribute__((returns_twice));
extern "C" [[noreturn]] void arch_longjmp(JmpBuf *buf, int value);

/* An instruction that no processor here executes. */
void arch_illegal_instruction();
