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
#include "intc.h"
#include "mmio.h"
#include "riscv.h"

extern "C" char __image_start[], __image_end[];
extern "C" void riscv_trap_entry_m();
extern "C" void riscv_trap_entry_s();

void run_constructors();
[[noreturn]] void kmain();

namespace {

bool sMachineMode;
uint64_t sTickHz;

#define MAX_IRQ 64
#define MSTATUS_MIE 0x8
#define SSTATUS_SIE 0x2

struct HandlerSlot {
    IrqHandler fn;
    void *ctx;
};
HandlerSlot sHandlers[MAX_IRQ];

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
    unsigned long cause = frame[FRAME_CAUSE];
    if ((long)cause < 0) {
        unsigned num = cause & (MAX_IRQ - 1);
        if (sHandlers[num].fn) {
            sHandlers[num].fn(sHandlers[num].ctx);
            return;
        }
    }
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


unsigned long riscv_ip()
{
    return sMachineMode ? CSR_READ(mip) : CSR_READ(sip);
}


void riscv_ie_set(unsigned long bits)
{
    if (sMachineMode)
        CSR_SET(mie, bits);
    else
        CSR_SET(sie, bits);
}


void riscv_ie_clear(unsigned long bits)
{
    if (sMachineMode)
        CSR_CLEAR(mie, bits);
    else
        CSR_CLEAR(sie, bits);
}


unsigned riscv_ext_irq()
{
    return sMachineMode ? IRQ_M_EXT : IRQ_S_EXT;
}


unsigned riscv_timer_irq()
{
    return sMachineMode ? IRQ_M_TIMER : IRQ_S_TIMER;
}


unsigned riscv_soft_irq()
{
    return sMachineMode ? IRQ_M_SOFT : IRQ_S_SOFT;
}


void arch_set_irq_handler(unsigned num, IrqHandler fn, void *ctx)
{
    if (num < MAX_IRQ)
        sHandlers[num] = {fn, ctx};
}


void arch_irq_enable()
{
    if (sMachineMode)
        CSR_SET(mstatus, MSTATUS_MIE);
    else
        CSR_SET(sstatus, SSTATUS_SIE);
}


void arch_irq_disable()
{
    if (sMachineMode)
        CSR_CLEAR(mstatus, MSTATUS_MIE);
    else
        CSR_CLEAR(sstatus, SSTATUS_SIE);
}


void arch_wait_irq()
{
    /* wfi with interrupts off returns once one is pending, which is then
       taken as soon as they are on; no wakeup can fall between the two */
    arch_irq_disable();
    __asm__ volatile("wfi");
    arch_irq_enable();
    arch_irq_disable();
}


void arch_irq_reset()
{
    arch_irq_disable();
    if (sMachineMode) {
        CSR_WRITE(mie, 0);
        /* the M-mode software interrupt bit is the CLINT's; the supervisor
           ones M-mode may clear itself */
        CSR_CLEAR(mip, (1ul << IRQ_S_SOFT) | (1ul << IRQ_S_TIMER) |
                       (1ul << IRQ_S_EXT));
    } else {
        CSR_WRITE(sie, 0);
        CSR_CLEAR(sip, 1ul << IRQ_S_SOFT);
    }
    for (auto &h : sHandlers)
        h = {};
}


//#pragma mark - other harts

extern "C" uintptr_t riscv_hart_entry[MAX_HARTS];
extern "C" uintptr_t riscv_hart_stack[MAX_HARTS];
extern "C" uintptr_t riscv_hart_entry_s;
extern "C" void riscv_hart_start_s();

#define SBI_EXT_HSM 0x48534d

namespace {

/* the cpu nodes, in order */
const FdtNode *cpu_node(int index)
{
    const FdtNode *cpus = fdt_find_path("/cpus");
    int n = 0;
    for (const FdtNode *c = cpus ? cpus->child : nullptr; c; c = c->sibling) {
        const char *type = fdt_prop_str(c, "device_type");
        if (type == nullptr || strcmp(type, "cpu") != 0 || !fdt_is_enabled(c))
            continue;
        if (n++ == index)
            return c;
    }
    return nullptr;
}

/* A hart's msip register in the CLINT, or 0. */
uint64_t msip_of(uint32_t hart)
{
    const FdtNode *clint = fdt_find_compatible("riscv,clint0");
    uint64_t base, size;
    if (clint == nullptr || !fdt_reg(clint, 0, &base, &size))
        return 0;
    const FdtProp *ext = fdt_prop(clint, "interrupts-extended");
    uint32_t intc = hart_intc_phandle(hart);
    for (uint32_t i = 0; ext && 16 * i + 16 <= ext->len; i++) {
        if (be32(ext->data + 16 * i) == intc)
            return base + 4 * i;
    }
    return 0;
}

}


int arch_cpu_count()
{
    int n = 0;
    while (cpu_node(n))
        n++;
    return n ? n : 1;
}


uint32_t arch_cpu_hw_id(int index)
{
    const FdtNode *c = cpu_node(index);
    uint32_t reg = gBoot.boot_cpu;
    if (c)
        fdt_prop_u32(c, "reg", &reg);
    return reg;
}


uint32_t arch_this_cpu_hw_id()
{
    /* the entry code keeps the hart ID in tp */
    unsigned long tp;
    __asm__ volatile("mv %0, tp" : "=r"(tp));
    return tp;
}


bool arch_cpu_start(int index, void (*entry)(), uintptr_t stack_top)
{
    uint32_t hart = arch_cpu_hw_id(index);
    if (sMachineMode) {
        /* parked in the entry code: released by its slot and its software
           interrupt */
        uint64_t msip = msip_of(hart);
        if (hart >= MAX_HARTS || msip == 0)
            return false;
        riscv_hart_stack[hart] = stack_top;
        __atomic_store_n(&riscv_hart_entry[hart], (uintptr_t)entry,
                         __ATOMIC_RELEASE);
        mmio_write32(msip, 1);
        return true;
    }
    riscv_hart_entry_s = (uintptr_t)entry;
    __atomic_thread_fence(__ATOMIC_RELEASE);
    SbiRet r = sbi_call(SBI_EXT_HSM, 0, hart, (uintptr_t)riscv_hart_start_s,
                        stack_top);
    return r.error == 0;
}


void arch_ap_init()
{
    if (sMachineMode) {
        CSR_WRITE(mtvec, (uintptr_t)riscv_trap_entry_m);
        CSR_WRITE(mcounteren, 7);
        if (uint64_t msip = msip_of(arch_this_cpu_hw_id()))
            mmio_write32(msip, 0);
    } else {
        CSR_WRITE(stvec, (uintptr_t)riscv_trap_entry_s);
    }
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
