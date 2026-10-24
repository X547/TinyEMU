/*
 * x86 boot, traps and time
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

#include "acpi.h"
#include "mmio.h"
#include "x86_asm.h"

extern "C" char __image_start[], __image_end[];
extern "C" char x86_trap_stubs[];

void run_constructors();
[[noreturn]] void kmain();

/* Xen's arch/x86/hvm/start_info.h */
#define HVM_START_MAGIC 0x336ec578

struct HvmStartInfo {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t nr_modules;
    uint64_t modlist_paddr;
    uint64_t cmdline_paddr;
    uint64_t rsdp_paddr;
    uint64_t memmap_paddr;  /* version 1 */
    uint32_t memmap_entries;
    uint32_t reserved;
};

/* What traps.S saves, from the lowest address up. */
struct X86TrapFrame {
#ifdef __x86_64__
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rdi, rsi, rbp, rbx, rdx, rcx, rax;
    uint64_t vector, error;
    uint64_t ip, cs, flags, sp, ss;
#else
    uint32_t edi, esi, ebp, esp_unused, ebx, edx, ecx, eax;
    uint32_t vector, error;
    uint32_t ip, cs, flags;
#endif
};

namespace {

struct IdtGate {
    uint16_t offset_lo;
    uint16_t selector;
    uint8_t ist;
    uint8_t type;
    uint16_t offset_mid;
#ifdef __x86_64__
    uint32_t offset_hi;
    uint32_t reserved;
#endif
} __attribute__((packed));

struct __attribute__((packed)) DescriptorPointer {
    uint16_t limit;
    uintptr_t base;
};

IdtGate sIdt[256] __attribute__((aligned(16)));
uint64_t sTickHz;

struct HandlerSlot {
    IrqHandler fn;
    void *ctx;
};
HandlerSlot sHandlers[256];

#ifdef __x86_64__
constexpr uint16_t kCodeSelector = SEL_CODE64;
#else
constexpr uint16_t kCodeSelector = SEL_CODE32;
#endif

void set_gate(int vector, uintptr_t handler)
{
    IdtGate &g = sIdt[vector];
    g.offset_lo = handler & 0xffff;
    g.selector = kCodeSelector;
    g.ist = 0;
    g.type = 0x8e; /* present, ring 0, interrupt gate */
    g.offset_mid = (handler >> 16) & 0xffff;
#ifdef __x86_64__
    g.offset_hi = (uint64_t)handler >> 32;
    g.reserved = 0;
#endif
}

void load_idt()
{
    for (int v = 0; v < 256; v++)
        set_gate(v, (uintptr_t)x86_trap_stubs + 16 * v);
    DescriptorPointer p {sizeof(sIdt) - 1, (uintptr_t)sIdt};
    __asm__ volatile("lidt %0" : : "m"(p));
}

const char *vector_name(unsigned v)
{
    static const char *const names[] = {
        "divide error", "debug", "NMI", "breakpoint", "overflow",
        "bound range", "invalid opcode", "device not available",
        "double fault", "coprocessor segment overrun", "invalid TSS",
        "segment not present", "stack fault", "general protection",
        "page fault", nullptr, "x87 error", "alignment check",
        "machine check", "SIMD error", "virtualization", "control protection",
    };
    if (v < ARRAY_SIZE(names) && names[v])
        return names[v];
    return v < 32 ? "exception" : "interrupt";
}

uint64_t rdtsc()
{
    uint32_t lo, hi;
    __asm__ volatile("rdtsc" : "=a"(lo), "=d"(hi));
    return (uint64_t)hi << 32 | lo;
}

}


extern "C" [[noreturn]] void x86_boot(const HvmStartInfo *si)
{
    load_idt();
    if (si->magic != HVM_START_MAGIC)
        panic("no hvm_start_info");
    gBoot.rsdp = (const void *)(uintptr_t)si->rsdp_paddr;
    if (si->cmdline_paddr)
        gBoot.cmdline = (const char *)(uintptr_t)si->cmdline_paddr;
    if (si->version >= 1) {
        gBoot.memmap = (const void *)(uintptr_t)si->memmap_paddr;
        gBoot.memmap_entries = si->memmap_entries;
    }
    gBoot.image_start = (uintptr_t)__image_start;
    gBoot.image_end = (uintptr_t)__image_end;
    run_constructors();
    kmain();
}


extern "C" void x86_trap(X86TrapFrame *frame)
{
    HandlerSlot &h = sHandlers[frame->vector & 0xff];
    if (frame->vector >= 32 && h.fn) {
        h.fn(h.ctx);
        return;
    }
    TrapInfo info {};
    info.cause = frame->vector;
    info.pc = frame->ip;
    info.error = frame->error;
    info.name = vector_name(frame->vector);
    if (frame->vector == 14) {
        uintptr_t cr2;
        __asm__ volatile("mov %%cr2, %0" : "=r"(cr2));
        info.addr = cr2;
    }
    trap_unexpected(info);
    panic("unexpected trap: %s (vector %u, error %#llx) at %#llx",
          info.name, (unsigned)info.cause, (unsigned long long)info.error,
          (unsigned long long)info.pc);
}


const char *arch_target()
{
#ifdef __x86_64__
    return "x86_64";
#else
    return "i686";
#endif
}


const char *arch_mode()
{
    return "pvh";
}


/* The TSC against the ACPI PM timer, which runs at 3.579545 MHz on every
   machine that has one. */
void arch_init()
{
    uint16_t port;
    bool ext32;
    if (!acpi_pm_timer(&port, &ext32))
        panic("no ACPI PM timer to calibrate the TSC against");

    uint32_t mask = ext32 ? 0xffffffff : 0xffffff;
    const uint32_t wait = 3579545 / 50; /* 20 ms */
    uint32_t start = inl(port) & mask;
    uint64_t tsc_start = rdtsc();
    uint32_t elapsed;
    do {
        elapsed = ((inl(port) & mask) - start) & mask;
    } while (elapsed < wait);
    uint64_t tsc_end = rdtsc();
    sTickHz = (tsc_end - tsc_start) * 3579545 / elapsed;
    if (sTickHz == 0)
        panic("the TSC does not advance");
}


uint64_t arch_ticks()
{
    return rdtsc();
}


uint64_t arch_tick_hz()
{
    return sTickHz;
}


void arch_halt()
{
    for (;;)
        __asm__ volatile("cli; hlt");
}


void arch_cpu_relax()
{
    __asm__ volatile("pause");
}


void arch_illegal_instruction()
{
    __asm__ volatile("ud2");
}


void arch_set_irq_handler(unsigned num, IrqHandler fn, void *ctx)
{
    if (num >= 32 && num < 256)
        sHandlers[num] = {fn, ctx};
}


void arch_irq_enable()
{
    __asm__ volatile("sti" ::: "memory");
}


void arch_irq_disable()
{
    __asm__ volatile("cli" ::: "memory");
}


void arch_wait_irq()
{
    /* the instruction after sti runs before any interrupt is taken, so
       none can fall between the two */
    __asm__ volatile("sti; hlt; cli" ::: "memory");
}


void arch_irq_reset()
{
    arch_irq_disable();
    for (auto &h : sHandlers)
        h = {};
}
