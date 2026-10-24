/*
 * RISC-V boot, traps and time
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
#include "arch.h"

#include "asm.h"
#include "fdt.h"
#include "riscv.h"

extern "C" char __image_start[], __image_end[];
extern "C" void riscv_trap_entry_m();
extern "C" void riscv_trap_entry_s();

void run_constructors();
[[noreturn]] void kmain();

namespace {

bool sMachineMode;
uint64_t sTickHz;

const char *cause_name(unsigned long cause)
{
    static const char *const names[] = {
        "instruction address misaligned",
        "instruction access fault",
        "illegal instruction",
        "breakpoint",
        "load address misaligned",
        "load access fault",
        "store address misaligned",
        "store access fault",
        "user ecall",
        "supervisor ecall",
        "hypervisor ecall",
        "machine ecall",
        "instruction page fault",
        "load page fault",
        nullptr,
        "store page fault",
    };
    if ((long)cause < 0)
        return "interrupt";
    if (cause < ARRAY_SIZE(names) && names[cause])
        return names[cause];
    return "exception";
}

}


bool riscv_machine_mode()
{
    return sMachineMode;
}


extern "C" [[noreturn]] void riscv_boot(unsigned long hart, const void *fdt,
                                        unsigned long machine_mode)
{
    sMachineMode = machine_mode != 0;
    if (sMachineMode) {
        CSR_WRITE(mtvec, (uintptr_t)riscv_trap_entry_m);
        /* time, from S-mode too, for when a test drops to it */
        CSR_WRITE(mcounteren, 7);
    } else {
        CSR_WRITE(stvec, (uintptr_t)riscv_trap_entry_s);
    }
    gBoot.fdt = fdt;
    gBoot.boot_cpu = hart;
    gBoot.image_start = (uintptr_t)__image_start;
    gBoot.image_end = (uintptr_t)__image_end;
    run_constructors();
    kmain();
}


extern "C" void riscv_trap(unsigned long *frame, unsigned long is_m)
{
    (void)is_m;
    TrapInfo info {};
    info.cause = frame[FRAME_CAUSE];
    info.pc = frame[FRAME_EPC];
    info.addr = frame[FRAME_TVAL];
    info.name = cause_name(frame[FRAME_CAUSE]);
    trap_unexpected(info);
    panic("unexpected trap: %s (cause %#lx) at pc %#lx, tval %#lx",
          info.name, frame[FRAME_CAUSE], frame[FRAME_EPC], frame[FRAME_TVAL]);
}


const char *arch_target()
{
    return __riscv_xlen == 64 ? "riscv64" : "riscv32";
}


const char *arch_mode()
{
    return sMachineMode ? "M" : "S";
}


void arch_init()
{
    const FdtNode *cpus = fdt_find_path("/cpus");
    uint32_t hz = 0;
    if (cpus == nullptr || !fdt_prop_u32(cpus, "timebase-frequency", &hz) ||
        hz == 0)
        panic("no /cpus/timebase-frequency in the device tree");
    sTickHz = hz;
}


uint64_t arch_ticks()
{
#if __riscv_xlen == 64
    return CSR_READ(time);
#else
    for (;;) {
        uint32_t hi = CSR_READ(timeh);
        uint32_t lo = CSR_READ(time);
        if (CSR_READ(timeh) == hi)
            return (uint64_t)hi << 32 | lo;
    }
#endif
}


uint64_t arch_tick_hz()
{
    return sTickHz;
}


void arch_halt()
{
    for (;;) {
        if (sMachineMode)
            CSR_WRITE(mie, 0);
        else
            CSR_WRITE(sie, 0);
        __asm__ volatile("wfi");
    }
}


void arch_cpu_relax()
{
}


void arch_illegal_instruction()
{
    /* all zeros is reserved as illegal in every encoding */
    __asm__ volatile(".4byte 0");
}


SbiRet sbi_call(unsigned long ext, unsigned long fid, unsigned long a0,
                unsigned long a1, unsigned long a2)
{
    register unsigned long r_a0 __asm__("a0") = a0;
    register unsigned long r_a1 __asm__("a1") = a1;
    register unsigned long r_a2 __asm__("a2") = a2;
    register unsigned long r_a6 __asm__("a6") = fid;
    register unsigned long r_a7 __asm__("a7") = ext;
    __asm__ volatile("ecall"
                     : "+r"(r_a0), "+r"(r_a1)
                     : "r"(r_a2), "r"(r_a6), "r"(r_a7)
                     : "memory");
    return {(long)r_a0, (long)r_a1};
}


bool sbi_probe(unsigned long ext)
{
    SbiRet r = sbi_call(SBI_EXT_BASE, 3, ext);
    return r.error == 0 && r.value != 0;
}
