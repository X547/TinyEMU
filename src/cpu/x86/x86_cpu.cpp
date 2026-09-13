/*
 * x86 CPU emulator
 *
 * Copyright (c) 2011-2017 Fabrice Bellard
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#include "x86_cpu_priv.h"

//#define DUMP_EXCEPTIONS

/* A Pentium II: family 6, model 5, stepping 2. */
#define CPUID_SIGNATURE 0x652

enum {
    CPUID_FPU = 0,
    CPUID_DE = 2,
    CPUID_PSE = 3,
    CPUID_TSC = 4,
    CPUID_MSR = 5,
    CPUID_CX8 = 8,
    CPUID_SEP = 11,
    CPUID_PGE = 13,
    CPUID_CMOV = 15,
};

enum {
    MSR_TSC = 0x10,
    MSR_PLATFORM_ID = 0x17,
    MSR_UCODE_REV = 0x8b,
    MSR_SYSENTER_CS = 0x174,
    MSR_SYSENTER_ESP = 0x175,
    MSR_SYSENTER_EIP = 0x176,
};

static const uint32_t CR0_VALID_MASK = bit_at(CR0_PE) | bit_at(CR0_MP) |
    bit_at(CR0_EM) | bit_at(CR0_TS) | bit_at(CR0_ET) | bit_at(CR0_NE) |
    bit_at(CR0_WP) | bit_at(CR0_AM) | bit_at(CR0_NW) | bit_at(CR0_CD) |
    bit_at(CR0_PG);

static const uint32_t CR4_VALID_MASK = bit_at(CR4_TSD) | bit_at(CR4_DE) |
    bit_at(CR4_PSE) | bit_at(CR4_PGE);


static void cpu_dump_state(X86CPUState *s)
{
    static const char *reg_names[8] = {
        "EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"
    };
    static const char *seg_names[SEG_COUNT] = {
        "ES", "CS", "SS", "DS", "FS", "GS"
    };

    for (int i = 0; i < 8; i++) {
        fprintf(stderr, "%s=%08x%s", reg_names[i], s->regs[i],
                i % 4 == 3 ? "\n" : " ");
    }
    fprintf(stderr, "EIP=%08x EFL=%08x CPL=%d CR0=%08x CR2=%08x CR3=%08x\n",
            s->eip, get_eflags(s), s->cpl, s->cr0, s->cr2, s->cr3);
    for (int i = 0; i < SEG_COUNT; i++) {
        fprintf(stderr, "%s=%04x %08x %08x %04x\n", seg_names[i],
                s->segs[i].sel, s->segs[i].base, s->segs[i].limit,
                s->segs[i].flags);
    }
}


//#pragma mark - physical memory

static uint32_t device_read(PhysMemoryRange *pr, uint32_t offset, int size)
{
    if (get_bit(pr->devio_flags, size)) {
        return trunc_size(pr->io->DeviceRead(offset, size), size);
    }
    /* compose from narrower accesses */
    if (size > SIZE8 && (pr->devio_flags & bit_mask(size)) != 0) {
        int half = size - 1;
        uint32_t low = device_read(pr, offset, half);
        uint32_t high = device_read(pr, offset + size_bytes(half), half);
        return set_bits(low, size_bits(half), size_bits(half), high);
    }
    /* extract from a wider access */
    for (int wide = size + 1; wide <= SIZE32; wide++) {
        if (get_bit(pr->devio_flags, wide)) {
            uint32_t val = pr->io->DeviceRead(set_bits(offset, 0, wide, 0),
                                              wide);
            return get_bits(val, get_bits(offset, 0, wide) * 8,
                            size_bits(size));
        }
    }
    return size_mask(size);
}

static void device_write(PhysMemoryRange *pr, uint32_t offset, uint32_t val,
                         int size)
{
    if (get_bit(pr->devio_flags, size)) {
        pr->io->DeviceWrite(offset, trunc_size(val, size), size);
        return;
    }
    if (size > SIZE8 && (pr->devio_flags & bit_mask(size)) != 0) {
        int half = size - 1;
        device_write(pr, offset, get_bits(val, 0, size_bits(half)), half);
        device_write(pr, offset + size_bytes(half),
                     get_bits(val, size_bits(half), size_bits(half)), half);
    }
    /* narrower than anything the device decodes: dropped */
}

static uint32_t phys_read(X86CPUState *s, uint32_t phys, int size)
{
    PhysMemoryRange *pr = s->mem_map->FindRange(phys);
    if (pr == nullptr) {
        return size_mask(size);
    }
    if (pr->is_ram) {
        return host_load(pr->phys_mem + (phys - pr->addr), size);
    }
    return device_read(pr, phys - pr->addr, size);
}

static void phys_write(X86CPUState *s, uint32_t phys, uint32_t val, int size)
{
    PhysMemoryRange *pr = s->mem_map->FindRange(phys);
    if (pr == nullptr) {
        return;
    }
    if (pr->is_ram) {
        if (pr->devram_flags & DEVRAM_FLAG_ROM) {
            return;
        }
        pr->SetDirtyBit(phys - pr->addr);
        host_store(pr->phys_mem + (phys - pr->addr), val, size);
        return;
    }
    device_write(pr, phys - pr->addr, val, size);
}


//#pragma mark - TLB

void tlb_flush_all(X86CPUState *s)
{
    memset(s->tlb, 0xff, sizeof(s->tlb));
    s->tlb_large_pages = false;
    s->code_tag = TLB_INVALID;
}

void tlb_flush_page(X86CPUState *s, uint32_t lin)
{
    /* entries are per 4 KiB page, so a large page cannot be found from one
       address */
    if (s->tlb_large_pages) {
        tlb_flush_all(s);
        return;
    }
    uint32_t tag = page_base(lin);
    for (int mmu_idx = 0; mmu_idx < MMU_COUNT; mmu_idx++) {
        X86TLBEntry *e = tlb_entry(s, mmu_idx, lin);
        if (e->read == tag || e->write == tag || e->code == tag) {
            e->read = e->write = e->code = TLB_INVALID;
        }
    }
    if (s->code_tag == tag) {
        s->code_tag = TLB_INVALID;
    }
}

/* Map the page of 'lin' onto RAM range 'pr' and return the host pointer
   for 'lin'. */
static uint8_t *tlb_fill(X86CPUState *s, uint32_t lin, PhysMemoryRange *pr,
                         uint32_t phys, bool writable, int mmu_idx)
{
    uint32_t offset = phys - pr->addr;
    uint8_t *ptr = pr->phys_mem + offset;
    uint32_t tag = page_base(lin);
    X86TLBEntry *e = tlb_entry(s, mmu_idx, lin);

    e->addend = (uintptr_t)ptr - lin;
    e->read = tag;
    e->code = tag;
    /* A clean page of a dirty-tracked range takes the slow path once, so
       that the write is recorded. */
    if (writable && !(pr->devram_flags & DEVRAM_FLAG_ROM) &&
        pr->IsDirtyBit(offset)) {
        e->write = tag;
    } else {
        e->write = TLB_INVALID;
    }
    return ptr;
}


//#pragma mark - paging

[[noreturn]] static void page_fault(X86CPUState *s, uint32_t lin, int access,
                                    int mmu_idx, bool protection)
{
    uint32_t error_code = set_bit(0, 0, protection) |
        set_bit(0, 1, access == ACCESS_WRITE) |
        set_bit(0, 2, mmu_idx == MMU_USER);
    s->cr2 = lin;
    raise_exception(s, EXCP_PF, error_code);
}

/* Translate a linear address, updating the accessed and dirty bits.
   'writable' tells whether a write through the mapping needs no walk. */
static uint32_t page_walk(X86CPUState *s, uint32_t lin, int access,
                          int mmu_idx, bool *writable)
{
    if (!get_bit(s->cr0, CR0_PG)) {
        *writable = true;
        return lin;
    }

    bool is_user = mmu_idx == MMU_USER;
    bool is_write = access == ACCESS_WRITE;

    uint32_t pde_addr = page_base(s->cr3) + get_bits(lin, 22, 10) * 4;
    uint32_t pde = phys_read(s, pde_addr, SIZE32);
    if (!get_bit(pde, PTE_P)) {
        page_fault(s, lin, access, mmu_idx, false);
    }

    bool large = get_bit(pde, PTE_PS) && get_bit(s->cr4, CR4_PSE);
    uint32_t pte_addr, pte, perms, phys;
    if (large) {
        pte_addr = pde_addr;
        pte = pde;
        perms = pde;
        phys = set_bits(lin, 22, 10, get_bits(pde, 22, 10));
    } else {
        pte_addr = page_base(pde) + get_bits(lin, 12, 10) * 4;
        pte = phys_read(s, pte_addr, SIZE32);
        if (!get_bit(pte, PTE_P)) {
            page_fault(s, lin, access, mmu_idx, false);
        }
        perms = pde & pte;
        phys = page_base(pte) | page_offset(lin);
    }

    if (is_user && !get_bit(perms, PTE_US)) {
        page_fault(s, lin, access, mmu_idx, true);
    }
    bool rw = get_bit(perms, PTE_RW) ||
        (!is_user && !get_bit(s->cr0, CR0_WP));
    if (is_write && !rw) {
        page_fault(s, lin, access, mmu_idx, true);
    }

    if (!large && !get_bit(pde, PTE_A)) {
        phys_write(s, pde_addr, set_bit(pde, PTE_A, true), SIZE32);
    }
    uint32_t new_pte = set_bit(pte, PTE_A, true);
    if (is_write) {
        new_pte = set_bit(new_pte, PTE_D, true);
    }
    if (new_pte != pte) {
        phys_write(s, pte_addr, new_pte, SIZE32);
    }
    if (large) {
        s->tlb_large_pages = true;
    }
    *writable = rw && get_bit(new_pte, PTE_D);
    return phys;
}


//#pragma mark - virtual memory

static bool crosses_page(uint32_t lin, int size)
{
    return page_offset(lin) > (uint32_t)(PAGE_SIZE - size_bytes(size));
}

uint32_t mem_read_slow(X86CPUState *s, uint32_t lin, int size, int mmu_idx)
{
    if (crosses_page(lin, size)) {
        uint32_t val = 0;
        for (int i = 0; i < size_bytes(size); i++) {
            val = set_bits(val, 8 * i, 8,
                           mem_read_mmu(s, lin + i, SIZE8, mmu_idx));
        }
        return val;
    }

    bool writable;
    uint32_t phys = page_walk(s, lin, ACCESS_READ, mmu_idx, &writable);
    PhysMemoryRange *pr = s->mem_map->FindRange(phys);
    if (pr == nullptr) {
        return size_mask(size);
    }
    if (pr->is_ram) {
        return host_load(tlb_fill(s, lin, pr, phys, writable, mmu_idx), size);
    }
    return device_read(pr, phys - pr->addr, size);
}

/* Translate for writing and prepare the TLB, so that the write that follows
   cannot fault. */
static void probe_write(X86CPUState *s, uint32_t lin, int mmu_idx)
{
    bool writable;
    uint32_t phys = page_walk(s, lin, ACCESS_WRITE, mmu_idx, &writable);
    PhysMemoryRange *pr = s->mem_map->FindRange(phys);
    if (pr != nullptr && pr->is_ram &&
        !(pr->devram_flags & DEVRAM_FLAG_ROM)) {
        pr->SetDirtyBit(phys - pr->addr);
        tlb_fill(s, lin, pr, phys, writable, mmu_idx);
    }
}

void mem_probe_write(X86CPUState *s, uint32_t lin, int size)
{
    if (tlb_hit(tlb_entry(s, s->mmu_idx, lin)->write, lin, size)) {
        return;
    }
    probe_write(s, lin, s->mmu_idx);
    if (crosses_page(lin, size)) {
        probe_write(s, lin + size_bytes(size) - 1, s->mmu_idx);
    }
}

void mem_write_slow(X86CPUState *s, uint32_t lin, uint32_t val, int size,
                    int mmu_idx)
{
    if (crosses_page(lin, size)) {
        /* both pages must be writable before any byte is */
        bool writable;
        page_walk(s, lin, ACCESS_WRITE, mmu_idx, &writable);
        page_walk(s, lin + size_bytes(size) - 1, ACCESS_WRITE, mmu_idx,
                  &writable);
        for (int i = 0; i < size_bytes(size); i++) {
            mem_write_mmu(s, lin + i, get_bits(val, 8 * i, 8), SIZE8,
                          mmu_idx);
        }
        return;
    }

    bool writable;
    uint32_t phys = page_walk(s, lin, ACCESS_WRITE, mmu_idx, &writable);
    PhysMemoryRange *pr = s->mem_map->FindRange(phys);
    if (pr == nullptr) {
        return;
    }
    if (pr->is_ram) {
        if (pr->devram_flags & DEVRAM_FLAG_ROM) {
            return;
        }
        pr->SetDirtyBit(phys - pr->addr);
        host_store(tlb_fill(s, lin, pr, phys, writable, mmu_idx), val, size);
        return;
    }
    device_write(pr, phys - pr->addr, val, size);
}

uint8_t fetch_slow(X86CPUState *s, uint32_t lin)
{
    int mmu_idx = s->mmu_idx;
    X86TLBEntry *e = tlb_entry(s, mmu_idx, lin);
    if (e->code != page_base(lin)) {
        bool writable;
        uint32_t phys = page_walk(s, lin, ACCESS_CODE, mmu_idx, &writable);
        PhysMemoryRange *pr = s->mem_map->FindRange(phys);
        if (pr == nullptr) {
            return 0xff;
        }
        if (!pr->is_ram) {
            return device_read(pr, phys - pr->addr, SIZE8);
        }
        tlb_fill(s, lin, pr, phys, writable, mmu_idx);
    }
    s->code_tag = e->code;
    s->code_addend = e->addend;
    return *(uint8_t *)(e->addend + lin);
}


//#pragma mark - mode and control registers

void cpu_update_mode(X86CPUState *s)
{
    s->code32 = get_bit(s->segs[SEG_CS].flags, DESC_DB);
    s->ss32 = get_bit(s->segs[SEG_SS].flags, DESC_DB);
    int mmu_idx = s->cpl == 3 ? MMU_USER : MMU_SUPERVISOR;
    if (mmu_idx != s->mmu_idx) {
        s->mmu_idx = mmu_idx;
        s->code_tag = TLB_INVALID;
    }
}

void cpu_set_eflags(X86CPUState *s, uint32_t val, uint32_t mask)
{
    uint32_t eflags = (get_eflags(s) & ~mask) | (val & mask);
    s->eflags = set_bit(eflags & EFLAGS_VALID_MASK, 1, true);
    s->cc_op = CC_OP_EFLAGS;
}

void cpu_set_cr0(X86CPUState *s, uint32_t val)
{
    val = set_bit(val & CR0_VALID_MASK, CR0_ET, true);
    uint32_t changed = s->cr0 ^ val;
    if (changed & (bit_at(CR0_PG) | bit_at(CR0_WP) | bit_at(CR0_PE))) {
        tlb_flush_all(s);
    }
    s->cr0 = val;
    if (!get_bit(val, CR0_PE)) {
        s->cpl = 0;
    }
    cpu_update_mode(s);
}

void cpu_set_cr3(X86CPUState *s, uint32_t val)
{
    s->cr3 = val;
    tlb_flush_all(s);
}

void cpu_set_cr4(X86CPUState *s, uint32_t val)
{
    if (val & ~CR4_VALID_MASK) {
        raise_exception(s, EXCP_GP, 0);
    }
    if ((s->cr4 ^ val) & (bit_at(CR4_PSE) | bit_at(CR4_PGE))) {
        tlb_flush_all(s);
    }
    s->cr4 = val;
}

static void cpu_reset(X86CPUState *s)
{
    uint32_t data_flags = bit_at(DESC_P) | bit_at(DESC_S) | bit_at(DESC_RW) |
        bit_at(DESC_A);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[REG_EDX] = CPUID_SIGNATURE;
    s->eip = 0xfff0;
    s->eflags = bit_at(1);
    s->cc_op = CC_OP_EFLAGS;
    s->cr0 = bit_at(CR0_ET) | bit_at(CR0_NW) | bit_at(CR0_CD);
    s->cr2 = 0;
    s->cr3 = 0;
    s->cr4 = 0;
    memset(s->dr, 0, sizeof(s->dr));
    s->dr[6] = 0xffff0ff0;
    s->dr[7] = 0x400;
    s->sysenter_cs = 0;
    s->sysenter_esp = 0;
    s->sysenter_eip = 0;
    s->tsc_offset = 0;

    s->cpl = 0;
    for (int seg = 0; seg < SEG_COUNT; seg++) {
        load_seg_cache(s, seg, 0, 0, 0xffff, data_flags);
    }
    load_seg_cache(s, SEG_CS, 0xf000, 0xffff0000, 0xffff,
                   data_flags | bit_at(DESC_CODE));
    s->gdt = {0, 0, 0, 0xffff};
    s->idt = {0, 0, 0, 0xffff};
    s->ldt = {0, (uint16_t)(bit_at(DESC_P) | SYS_LDT), 0, 0xffff};
    s->tr = {0, (uint16_t)(bit_at(DESC_P) | SYS_TSS16_BUSY), 0, 0xffff};

    s->irq_inhibit = false;
    s->power_down = false;
    s->old_exception = -1;
    fpu_reset(s);
    tlb_flush_all(s);
    cpu_update_mode(s);
}


//#pragma mark - identification and model specific registers

static uint32_t cpuid_chars(const char *str)
{
    return get_le32((const uint8_t *)str);
}

void cpu_cpuid(X86CPUState *s)
{
    static const char brand[48] = "TinyEMU i686 CPU";
    uint32_t leaf = s->regs[REG_EAX];
    uint32_t a = 0, b = 0, c = 0, d = 0;

    switch (leaf) {
    case 0:
        a = 1;
        b = cpuid_chars("Genu");
        d = cpuid_chars("ineI");
        c = cpuid_chars("ntel");
        break;
    case 1:
        a = CPUID_SIGNATURE;
        d = bit_at(CPUID_FPU) | bit_at(CPUID_DE) | bit_at(CPUID_PSE) |
            bit_at(CPUID_TSC) | bit_at(CPUID_MSR) | bit_at(CPUID_CX8) |
            bit_at(CPUID_SEP) | bit_at(CPUID_PGE) | bit_at(CPUID_CMOV);
        break;
    case 0x80000000:
        a = 0x80000004;
        break;
    case 0x80000002:
    case 0x80000003:
    case 0x80000004: {
        const char *str = brand + (leaf - 0x80000002) * 12;
        a = cpuid_chars(str);
        b = cpuid_chars(str + 4);
        c = cpuid_chars(str + 8);
        break;
    }
    }
    s->regs[REG_EAX] = a;
    s->regs[REG_EBX] = b;
    s->regs[REG_ECX] = c;
    s->regs[REG_EDX] = d;
}

uint64_t cpu_get_tsc(X86CPUState *s)
{
    uint64_t tsc = s->tsc_source != nullptr ? s->tsc_source->Tsc() :
        (uint64_t)s->cycles;
    return tsc + s->tsc_offset;
}

void cpu_rdmsr(X86CPUState *s)
{
    uint64_t val;

    if (s->cpl != 0) {
        raise_exception(s, EXCP_GP, 0);
    }
    switch (s->regs[REG_ECX]) {
    case MSR_TSC:
        val = cpu_get_tsc(s);
        break;
    case MSR_PLATFORM_ID:
    case MSR_UCODE_REV:
        val = 0;
        break;
    case MSR_SYSENTER_CS:
        val = s->sysenter_cs;
        break;
    case MSR_SYSENTER_ESP:
        val = s->sysenter_esp;
        break;
    case MSR_SYSENTER_EIP:
        val = s->sysenter_eip;
        break;
    default:
        raise_exception(s, EXCP_GP, 0);
    }
    set_edx_eax(s, val);
}

void cpu_wrmsr(X86CPUState *s)
{
    uint64_t val = get_edx_eax(s);

    if (s->cpl != 0) {
        raise_exception(s, EXCP_GP, 0);
    }
    switch (s->regs[REG_ECX]) {
    case MSR_TSC:
        s->tsc_offset += val - cpu_get_tsc(s);
        break;
    case MSR_UCODE_REV:
        break;
    case MSR_SYSENTER_CS:
        s->sysenter_cs = get_bits(val, 0, 16);
        break;
    case MSR_SYSENTER_ESP:
        s->sysenter_esp = val;
        break;
    case MSR_SYSENTER_EIP:
        s->sysenter_eip = val;
        break;
    default:
        raise_exception(s, EXCP_GP, 0);
    }
}


//#pragma mark - exceptions

static bool exception_is_contributory(int intno)
{
    return intno == EXCP_DE || (intno >= EXCP_TS && intno <= EXCP_GP);
}

/* Deliver a fault or trap and resume at the instruction loop. The return
   address is s->eip: the faulting instruction, or the next one for a trap
   raised once the instruction is complete. */
void raise_exception(X86CPUState *s, int intno, int error_code)
{
    int old = s->old_exception;

#ifdef DUMP_EXCEPTIONS
    fprintf(stderr, "x86: exception %d error=%04x at %04x:%08x cr2=%08x\n",
            intno, error_code, s->segs[SEG_CS].sel, s->eip, s->cr2);
#endif
    if (old == EXCP_DF) {
        fprintf(stderr, "x86: triple fault, resetting\n");
        cpu_dump_state(s);
        cpu_reset(s);
        longjmp(s->jmp_env, 1);
    }
    if ((exception_is_contributory(old) && exception_is_contributory(intno)) ||
        (old == EXCP_PF &&
         (intno == EXCP_PF || exception_is_contributory(intno)))) {
        intno = EXCP_DF;
        error_code = 0;
    }
    s->old_exception = intno;
    do_interrupt(s, intno, false, error_code, s->eip, false);
    s->old_exception = -1;
    longjmp(s->jmp_env, 1);
}


//#pragma mark - API

X86CPUState *x86_cpu_init(PhysMemoryMap *mem_map)
{
    X86CPUState *s = new X86CPUState();
    s->mem_map = mem_map;
    cpu_reset(s);
    return s;
}

void x86_cpu_end(X86CPUState *s)
{
    delete s;
}

void x86_cpu_interp(X86CPUState *s, int max_cycles)
{
    s->cycles_end = s->cycles + max_cycles;
    if (s->power_down) {
        if (!s->irq_level || !get_bit(s->eflags, EFLAGS_IF)) {
            return;
        }
        s->power_down = false;
    }
    /* raise_exception() comes back here once the exception is delivered */
    setjmp(s->jmp_env);
    x86_exec(s);
}

void x86_cpu_set_irq(X86CPUState *s, bool set)
{
    s->irq_level = set;
}

void x86_cpu_set_reg(X86CPUState *s, int reg, uint32_t val)
{
    switch (reg) {
    case X86_CPU_REG_EIP:
        s->eip = val;
        break;
    case X86_CPU_REG_CR0:
        cpu_set_cr0(s, val);
        break;
    case X86_CPU_REG_CR2:
        s->cr2 = val;
        break;
    default:
        if (reg >= 0 && reg < 8) {
            s->regs[reg] = val;
        }
        break;
    }
}

uint32_t x86_cpu_get_reg(X86CPUState *s, int reg)
{
    switch (reg) {
    case X86_CPU_REG_EIP:
        return s->eip;
    case X86_CPU_REG_CR0:
        return s->cr0;
    case X86_CPU_REG_CR2:
        return s->cr2;
    default:
        if (reg >= 0 && reg < 8) {
            return s->regs[reg];
        }
        return 0;
    }
}

void x86_cpu_set_seg(X86CPUState *s, int seg, const X86CPUSeg *sd)
{
    switch (seg) {
    case X86_CPU_SEG_LDT:
        s->ldt = *sd;
        break;
    case X86_CPU_SEG_TR:
        s->tr = *sd;
        break;
    case X86_CPU_SEG_GDT:
        s->gdt.base = sd->base;
        s->gdt.limit = sd->limit;
        break;
    case X86_CPU_SEG_IDT:
        s->idt.base = sd->base;
        s->idt.limit = sd->limit;
        break;
    default:
        if (seg == SEG_CS) {
            s->cpl = get_bit(s->cr0, CR0_PE) ? desc_dpl(sd->flags) : 0;
        }
        load_seg_cache(s, seg, sd->sel, sd->base, sd->limit, sd->flags);
        break;
    }
}

void x86_cpu_set_hard_intno_source(X86CPUState *s, X86HardIntnoSource *source)
{
    s->hard_intno_source = source;
}

void x86_cpu_set_tsc_source(X86CPUState *s, X86TscSource *source)
{
    s->tsc_source = source;
}

void x86_cpu_set_port_io(X86CPUState *s, DeviceIO *port_io)
{
    s->port_io = port_io;
}

int64_t x86_cpu_get_cycles(X86CPUState *s)
{
    return s->cycles;
}

bool x86_cpu_get_power_down(X86CPUState *s)
{
    return s->power_down;
}

/* A RAM mapping moved or its dirty bits were reset: drop the entries that
   point into it. */
void x86_cpu_flush_tlb_write_range_ram(X86CPUState *s,
                                       uint8_t *ram_ptr, size_t ram_size)
{
    uint8_t *ram_end = ram_ptr + ram_size;

    for (int mmu_idx = 0; mmu_idx < MMU_COUNT; mmu_idx++) {
        for (int i = 0; i < TLB_SIZE; i++) {
            X86TLBEntry *e = &s->tlb[mmu_idx][i];
            if (e->read == TLB_INVALID) {
                continue;
            }
            uint8_t *ptr = (uint8_t *)(e->addend + e->read);
            if (ptr >= ram_ptr && ptr < ram_end) {
                e->read = e->write = e->code = TLB_INVALID;
            }
        }
    }
    s->code_tag = TLB_INVALID;
}
