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

/* Interrupts. A handler is per interrupt number: on RISC-V the cause (1
   supervisor software, 3 machine software, 5 and 7 the timers, 9 and 11
   external), on x86 the vector (32 to 255). An interrupt nobody handles is
   an unexpected trap. */
typedef void (*IrqHandler)(void *ctx);
void arch_set_irq_handler(unsigned num, IrqHandler fn, void *ctx);
void arch_irq_enable();
void arch_irq_disable();
/* Wait for an interrupt with interrupts enabled; returns after one ran. */
void arch_wait_irq();
/* Between tests: interrupts off, every handler gone, and on RISC-V every
   interrupt source the processor has masked. */
void arch_irq_reset();

/* The processors the tables list, in their order: index 0 up. The boot
   processor is one of them. */
int arch_cpu_count();
uint32_t arch_cpu_hw_id(int index); /* APIC ID or hart ID */
uint32_t arch_this_cpu_hw_id();
/* Start processor 'index', which calls 'entry' on 'stack_top' with
   interrupts off and never returns. False if it cannot be started. */
bool arch_cpu_start(int index, void (*entry)(), uintptr_t stack_top);
/* On a started processor, entered from 'entry': the trap handling the
   boot processor has. */
void arch_ap_init();
