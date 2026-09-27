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

/* A Pentium M (Dothan): family 6, model 13, stepping 8. */
#define CPUID_SIGNATURE 0x6d8

enum {
    CPUID_FPU = 0,
    CPUID_DE = 2,
    CPUID_PSE = 3,
    CPUID_TSC = 4,
    CPUID_MSR = 5,
    CPUID_PAE = 6,
    CPUID_CX8 = 8,
    CPUID_SEP = 11,
    CPUID_PGE = 13,
    CPUID_CMOV = 15,
    CPUID_CLFSH = 19,
    CPUID_MMX = 23,
    CPUID_FXSR = 24,
    CPUID_SSE = 25,
    CPUID_SSE2 = 26,
};

/* CPUID 0x80000001 EDX */
enum {
    CPUID_EXT_NX = 20,
};

/* CPUID 1 EBX bits 8-15: the CLFLUSH line size in 8 byte units */
static const uint32_t CPUID_CLFLUSH_SIZE = 64 / 8;

enum {
    MSR_TSC = 0x10,
    MSR_PLATFORM_ID = 0x17,
    MSR_UCODE_REV = 0x8b,
    MSR_PERFCTR0 = 0xc1,
    MSR_PERFCTR1 = 0xc2,
    MSR_SYSENTER_CS = 0x174,
    MSR_SYSENTER_ESP = 0x175,
    MSR_SYSENTER_EIP = 0x176,
    MSR_EVNTSEL0 = 0x186,
    MSR_EVNTSEL1 = 0x187,
    MSR_MISC_ENABLE = 0x1a0,
    MSR_EFER = 0xc0000080,
    MSR_STAR = 0xc0000081,
    MSR_LSTAR = 0xc0000082,
    MSR_CSTAR = 0xc0000083,
    MSR_SFMASK = 0xc0000084,
    MSR_FS_BASE = 0xc0000100,
    MSR_GS_BASE = 0xc0000101,
    MSR_KERNEL_GS_BASE = 0xc0000102,
};

/* P6 counters are 40 bits wide */
static const int PMC_BITS = 40;

enum {
    MISC_ENABLE_FAST_STRING = 0,
    MISC_ENABLE_PERFMON = 7,      /* performance monitoring available */
    MISC_ENABLE_BTS_UNAVAIL = 11,
    MISC_ENABLE_PEBS_UNAVAIL = 12,
    MISC_ENABLE_LIMIT_CPUID = 22, /* no effect: the highest leaf is 1 */
};

static const uint32_t MISC_ENABLE_RESET = bit_at(MISC_ENABLE_FAST_STRING) |
    bit_at(MISC_ENABLE_PERFMON) | bit_at(MISC_ENABLE_BTS_UNAVAIL) |
    bit_at(MISC_ENABLE_PEBS_UNAVAIL);
static const uint32_t MISC_ENABLE_WRITABLE = bit_at(MISC_ENABLE_FAST_STRING) |
    bit_at(MISC_ENABLE_LIMIT_CPUID);

static const uint32_t CR0_VALID_MASK = bit_at(CR0_PE) | bit_at(CR0_MP) |
    bit_at(CR0_EM) | bit_at(CR0_TS) | bit_at(CR0_ET) | bit_at(CR0_NE) |
    bit_at(CR0_WP) | bit_at(CR0_AM) | bit_at(CR0_NW) | bit_at(CR0_CD) |
    bit_at(CR0_PG);

static const uint32_t CR4_VALID_MASK = bit_at(CR4_TSD) | bit_at(CR4_DE) |
    bit_at(CR4_PSE) | bit_at(CR4_PAE) | bit_at(CR4_PGE) | bit_at(CR4_PCE) |
    bit_at(CR4_OSFXSR) | bit_at(CR4_OSXMMEXCPT);

static const uint64_t EFER_VALID_MASK = bit_at(EFER_SCE) | bit_at(EFER_LME) |
    bit_at(EFER_LMA) | bit_at(EFER_NXE);


static void cpu_dump_state(X86CPUState *s)
{
    static const char *reg_names[8] = {
        "EAX", "ECX", "EDX", "EBX", "ESP", "EBP", "ESI", "EDI"
    };
    static const char *seg_names[SEG_COUNT] = {
        "ES", "CS", "SS", "DS", "FS", "GS"
    };

    if (get_bit(s->efer, EFER_LMA)) {
        for (int i = 0; i < GPR_COUNT; i++) {
            fprintf(stderr, "R%-2d=%016" PRIx64 "%s", i, s->regs[i],
                    i % 4 == 3 ? "\n" : " ");
        }
        fprintf(stderr, "RIP=%016" PRIx64 " EFL=%08x CPL=%d CR0=%08x "
                "CR2=%016" PRIx64 " CR3=%016" PRIx64 "\n", s->rip,
                get_eflags(s), s->cpl, s->cr0, s->cr2, s->cr3);
    } else {
        for (int i = 0; i < 8; i++) {
            fprintf(stderr, "%s=%08x%s", reg_names[i], (uint32_t)s->regs[i],
                    i % 4 == 3 ? "\n" : " ");
        }
        fprintf(stderr, "EIP=%08x EFL=%08x CPL=%d CR0=%08x CR2=%08x "
                "CR3=%08x\n", (uint32_t)s->rip, get_eflags(s), s->cpl, s->cr0,
                (uint32_t)s->cr2, (uint32_t)s->cr3);
    }
    for (int i = 0; i < SEG_COUNT; i++) {
        fprintf(stderr, "%s=%04x %016" PRIx64 " %08x %04x\n", seg_names[i],
                s->segs[i].sel, s->segs[i].base, s->segs[i].limit,
                s->segs[i].flags);
    }
}


//#pragma mark - physical memory

static uint64_t device_read(PhysMemoryRange *pr, uint32_t offset, int size)
{
    if (get_bit(pr->devio_flags, size)) {
        return trunc_size(pr->io->DeviceRead(offset, size), size);
    }
    /* compose from narrower accesses */
    if (size > SIZE8 && (pr->devio_flags & bit_mask(size)) != 0) {
        int half = size - 1;
        uint64_t low = device_read(pr, offset, half);
        uint64_t high = device_read(pr, offset + size_bytes(half), half);
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

static void device_write(PhysMemoryRange *pr, uint32_t offset, uint64_t val,
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

/* The entries into device code, which hold the device lock. Nothing in them
   raises an exception, so no longjmp() leaves the lock held. */
static uint64_t locked_device_read(X86CPUState *s, PhysMemoryRange *pr,
                                   uint32_t offset, int size)
{
    DeviceLocker locker(*s->device_lock);
    return device_read(pr, offset, size);
}

static void locked_device_write(X86CPUState *s, PhysMemoryRange *pr,
                                uint32_t offset, uint64_t val, int size)
{
    DeviceLocker locker(*s->device_lock);
    device_write(pr, offset, val, size);
}

static uint64_t phys_read(X86CPUState *s, uint64_t phys, int size)
{
    PhysMemoryRange *pr = s->mem_map->FindRange(phys);
    if (pr == nullptr) {
        return size_mask(size);
    }
    if (pr->is_ram) {
        return host_load(pr->phys_mem + (phys - pr->addr), size);
    }
    return locked_device_read(s, pr, phys - pr->addr, size);
}

static void phys_write(X86CPUState *s, uint64_t phys, uint64_t val, int size)
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
    locked_device_write(s, pr, phys - pr->addr, val, size);
}


//#pragma mark - TLB

void tlb_flush_all(X86CPUState *s)
{
    memset(s->tlb, 0xff, sizeof(s->tlb));
    s->tlb_large_pages = false;
    s->code_tag = TLB_INVALID;
}

void tlb_flush_page(X86CPUState *s, uint64_t lin)
{
    lin &= s->lin_mask;
    /* entries are per 4 KiB page, so a large page cannot be found from one
       address */
    if (s->tlb_large_pages) {
        tlb_flush_all(s);
        return;
    }
    uint64_t tag = page_base(lin);
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

/* Map the page of 'lin', within the linear address width, onto RAM range
   'pr' and return the host pointer for 'lin'. */
static uint8_t *tlb_fill(X86CPUState *s, uint64_t lin, PhysMemoryRange *pr,
                         uint64_t phys, bool writable, bool executable,
                         int mmu_idx)
{
    uint64_t offset = phys - pr->addr;
    uint8_t *ptr = pr->phys_mem + offset;
    uint64_t tag = page_base(lin);
    X86TLBEntry *e = tlb_entry(s, mmu_idx, lin);

    e->addend = (uintptr_t)ptr - lin;
    e->read = tag;
    e->code = executable ? tag : TLB_INVALID;
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

/* page fault error code bits */
enum {
    PF_P = 0,     /* a protection or reserved bit fault, not a missing page */
    PF_W = 1,
    PF_U = 2,
    PF_RSVD = 3,
    PF_ID = 4,
};

/* the frame address bits of a PAE entry */
static const uint64_t PAE_ADDR_MASK =
    field_mask<uint64_t>(PAGE_BITS, X86_CPU_PHYS_ADDRESS_BITS - PAGE_BITS);
static const uint64_t PAE_HIGH_RSVD_MASK =
    field_mask<uint64_t>(X86_CPU_PHYS_ADDRESS_BITS,
                         64 - X86_CPU_PHYS_ADDRESS_BITS);
static const uint64_t PDPTE_RSVD_MASK = PAE_HIGH_RSVD_MASK |
    field_mask<uint64_t>(1, 2) | field_mask<uint64_t>(5, 4);
/* 4-level entries leave bits 52-62 to software */
static const uint64_t LONG_RSVD_MASK =
    field_mask<uint64_t>(X86_CPU_PHYS_ADDRESS_BITS,
                         52 - X86_CPU_PHYS_ADDRESS_BITS) |
    bit_at<uint64_t>(PTE_XD);
/* below the frame of a 2 MB page, above PAT */
static const uint64_t PAE_LARGE_RSVD_MASK = field_mask<uint64_t>(13, 8);

[[noreturn]] static void page_fault(X86CPUState *s, uint64_t lin, int access,
                                    int mmu_idx, uint32_t cause)
{
    bool nx = get_bit(s->cr4, CR4_PAE) && get_bit(s->efer, EFER_NXE);
    uint32_t error_code = cause | set_bit(0, PF_W, access == ACCESS_WRITE) |
        set_bit(0, PF_U, mmu_idx == MMU_USER) |
        set_bit(0, PF_ID, access == ACCESS_CODE && nx);
    s->cr2 = lin;
    raise_exception(s, EXCP_PF, error_code);
}

/* Translate a linear address, within the linear address width, updating
   the accessed and dirty bits. 'writable' tells whether a write through the
   mapping needs no walk, 'executable' whether a fetch does. */
static uint64_t page_walk(X86CPUState *s, uint64_t lin, int access,
                          int mmu_idx, bool *writable, bool *executable)
{
    if (!get_bit(s->cr0, CR0_PG)) {
        *writable = true;
        *executable = true;
        return lin;
    }

    bool is_user = mmu_idx == MMU_USER;
    bool is_write = access == ACCESS_WRITE;
    bool pae = get_bit(s->cr4, CR4_PAE);
    bool lma = get_bit(s->efer, EFER_LMA);
    int entry_size = pae ? SIZE64 : SIZE32;
    int index_bits = pae ? 9 : 10;
    uint64_t addr_mask = pae ? PAE_ADDR_MASK :
        field_mask<uint64_t>(PAGE_BITS, 32 - PAGE_BITS);
    /* XD is reserved unless NX is enabled */
    uint64_t rsvd = 0;
    if (pae) {
        rsvd = lma ? LONG_RSVD_MASK : PAE_HIGH_RSVD_MASK;
        if (get_bit(s->efer, EFER_NXE)) {
            rsvd = set_bit(rsvd, PTE_XD, false);
        }
    }

    /* Level 0 holds the page table entries; PAE starts from its page
       directory pointer registers rather than from memory. */
    int level;
    uint64_t table;
    if (lma) {
        level = 3;
        table = s->cr3 & PAE_ADDR_MASK;
    } else if (pae) {
        uint64_t pdpte = s->pdpte[get_bits(lin, 30, 2)];
        if (!get_bit(pdpte, PTE_P)) {
            page_fault(s, lin, access, mmu_idx, 0);
        }
        level = 1;
        table = pdpte & PAE_ADDR_MASK;
    } else {
        level = 1;
        table = page_base(get_bits(s->cr3, 0, 32));
    }

    uint64_t entry_addr[4], entry[4];
    uint64_t perms = UINT64_MAX;
    int top = level, shift;
    for (;; level--) {
        shift = PAGE_BITS + level * index_bits;
        entry_addr[level] = table +
            (get_bits(lin, shift, index_bits) << entry_size);
        uint64_t e = phys_read(s, entry_addr[level], entry_size);
        entry[level] = e;
        if (!get_bit(e, PTE_P)) {
            page_fault(s, lin, access, mmu_idx, 0);
        }
        /* large pages are 2 MB with PAE, 4 MB without; there are no 1 GB
           ones */
        bool large = level == 1 && get_bit(e, PTE_PS) &&
            (pae || get_bit(s->cr4, CR4_PSE));
        uint64_t e_rsvd = rsvd;
        if (pae && level == 1 && large) {
            e_rsvd |= PAE_LARGE_RSVD_MASK;
        } else if (pae && level >= 2) {
            e_rsvd |= bit_at<uint64_t>(PTE_PS);
        }
        if (e & e_rsvd) {
            page_fault(s, lin, access, mmu_idx,
                       bit_at(PF_P) | bit_at(PF_RSVD));
        }
        perms &= e;
        if (level == 0 || large) {
            break;
        }
        table = e & addr_mask;
    }
    uint64_t leaf = entry[level];
    uint64_t phys = set_bits(leaf & addr_mask, 0, shift, lin);

    if (is_user && !get_bit(perms, PTE_US)) {
        page_fault(s, lin, access, mmu_idx, bit_at(PF_P));
    }
    bool rw = get_bit(perms, PTE_RW) ||
        (!is_user && !get_bit(s->cr0, CR0_WP));
    if (is_write && !rw) {
        page_fault(s, lin, access, mmu_idx, bit_at(PF_P));
    }
    bool nx = false;
    for (int i = level; i <= top; i++) {
        nx = nx || get_bit(entry[i], PTE_XD);
    }
    if (access == ACCESS_CODE && nx) {
        page_fault(s, lin, access, mmu_idx, bit_at(PF_P));
    }

    for (int i = top; i > level; i--) {
        if (!get_bit(entry[i], PTE_A)) {
            phys_write(s, entry_addr[i], set_bit(entry[i], PTE_A, true),
                       entry_size);
        }
    }
    uint64_t new_leaf = set_bit(leaf, PTE_A, true);
    if (is_write) {
        new_leaf = set_bit(new_leaf, PTE_D, true);
    }
    if (new_leaf != leaf) {
        phys_write(s, entry_addr[level], new_leaf, entry_size);
    }
    if (level != 0) {
        s->tlb_large_pages = true;
    }
    *writable = rw && get_bit(new_leaf, PTE_D);
    *executable = !nx;
    return phys;
}

/* Load the page directory pointers CR3 names, as PAE paging does when it is
   enabled and on each write of CR3. Nothing changes if one is invalid. */
static void load_pdptes(X86CPUState *s, uint64_t cr3)
{
    uint64_t pdpte[4];
    uint64_t base = set_bits(get_bits(cr3, 0, 32), 0, 5, 0);

    for (int i = 0; i < 4; i++) {
        pdpte[i] = phys_read(s, base + 8 * i, SIZE64);
        if (get_bit(pdpte[i], PTE_P) && (pdpte[i] & PDPTE_RSVD_MASK)) {
            raise_exception(s, EXCP_GP, 0);
        }
    }
    memcpy(s->pdpte, pdpte, sizeof(pdpte));
}


//#pragma mark - virtual memory

static bool crosses_page(uint64_t lin, int size)
{
    return page_offset(lin) > (uint32_t)(PAGE_SIZE - size_bytes(size));
}

/* The slow paths wrap the linear address at 4 GB, or refuse a non-canonical
   one in IA-32e mode, which the fast ones leave to a TLB miss. Compatibility
   mode does not wrap: only a segment base that carries an offset past 4 GB
   would tell. */
static uint64_t wrap_linear(X86CPUState *s, uint64_t lin)
{
    if (s->lin_mask != UINT64_MAX) {
        return lin & s->lin_mask;
    }
    if (!is_canonical(lin)) {
        raise_exception(s, EXCP_GP, 0);
    }
    return lin;
}

uint64_t mem_read_slow(X86CPUState *s, uint64_t lin, int size, int mmu_idx)
{
    lin = wrap_linear(s, lin);
    if (crosses_page(lin, size)) {
        uint64_t val = 0;
        for (int i = 0; i < size_bytes(size); i++) {
            val = set_bits(val, 8 * i, 8,
                           mem_read_mmu(s, lin + i, SIZE8, mmu_idx));
        }
        return val;
    }

    bool writable, executable;
    uint64_t phys = page_walk(s, lin, ACCESS_READ, mmu_idx, &writable,
                              &executable);
    PhysMemoryRange *pr = s->mem_map->FindRange(phys);
    if (pr == nullptr) {
        return size_mask(size);
    }
    if (pr->is_ram) {
        return host_load(tlb_fill(s, lin, pr, phys, writable, executable,
                                  mmu_idx), size);
    }
    return locked_device_read(s, pr, phys - pr->addr, size);
}

/* Translate for writing and prepare the TLB, so that the write that follows
   cannot fault. */
static void probe_write(X86CPUState *s, uint64_t lin, int mmu_idx)
{
    bool writable, executable;
    lin = wrap_linear(s, lin);
    uint64_t phys = page_walk(s, lin, ACCESS_WRITE, mmu_idx, &writable,
                              &executable);
    PhysMemoryRange *pr = s->mem_map->FindRange(phys);
    if (pr != nullptr && pr->is_ram &&
        !(pr->devram_flags & DEVRAM_FLAG_ROM)) {
        pr->SetDirtyBit(phys - pr->addr);
        tlb_fill(s, lin, pr, phys, writable, executable, mmu_idx);
    }
}

void mem_probe_write(X86CPUState *s, uint64_t lin, int size)
{
    lin = wrap_linear(s, lin);
    if (tlb_hit(tlb_entry(s, s->mmu_idx, lin)->write, lin, size)) {
        return;
    }
    probe_write(s, lin, s->mmu_idx);
    if (crosses_page(lin, size)) {
        probe_write(s, lin + size_bytes(size) - 1, s->mmu_idx);
    }
}

void mem_write_slow(X86CPUState *s, uint64_t lin, uint64_t val, int size,
                    int mmu_idx)
{
    lin = wrap_linear(s, lin);
    if (crosses_page(lin, size)) {
        /* both pages must be writable before any byte is */
        bool writable, executable;
        page_walk(s, lin, ACCESS_WRITE, mmu_idx, &writable, &executable);
        page_walk(s, wrap_linear(s, lin + size_bytes(size) - 1),
                  ACCESS_WRITE, mmu_idx, &writable, &executable);
        for (int i = 0; i < size_bytes(size); i++) {
            mem_write_mmu(s, lin + i, get_bits(val, 8 * i, 8), SIZE8,
                          mmu_idx);
        }
        return;
    }

    bool writable, executable;
    uint64_t phys = page_walk(s, lin, ACCESS_WRITE, mmu_idx, &writable,
                              &executable);
    PhysMemoryRange *pr = s->mem_map->FindRange(phys);
    if (pr == nullptr) {
        return;
    }
    if (pr->is_ram) {
        if (pr->devram_flags & DEVRAM_FLAG_ROM) {
            return;
        }
        pr->SetDirtyBit(phys - pr->addr);
        host_store(tlb_fill(s, lin, pr, phys, writable, executable, mmu_idx),
                   val, size);
        return;
    }
    locked_device_write(s, pr, phys - pr->addr, val, size);
}

uint8_t fetch_slow(X86CPUState *s, uint64_t lin)
{
    int mmu_idx = s->mmu_idx;
    lin = wrap_linear(s, lin);
    X86TLBEntry *e = tlb_entry(s, mmu_idx, lin);
    if (e->code != page_base(lin)) {
        bool writable, executable;
        uint64_t phys = page_walk(s, lin, ACCESS_CODE, mmu_idx, &writable,
                                  &executable);
        PhysMemoryRange *pr = s->mem_map->FindRange(phys);
        if (pr == nullptr) {
            return 0xff;
        }
        if (!pr->is_ram) {
            return locked_device_read(s, pr, phys - pr->addr, SIZE8);
        }
        tlb_fill(s, lin, pr, phys, writable, executable, mmu_idx);
    }
    s->code_tag = e->code;
    s->code_addend = e->addend;
    return *(uint8_t *)(e->addend + lin);
}


//#pragma mark - mode and control registers

void cpu_update_mode(X86CPUState *s)
{
    bool lma = get_bit(s->efer, EFER_LMA);
    bool code64 = lma && get_bit(s->segs[SEG_CS].flags, DESC_L);
    if (code64 != s->code64) {
        s->code64 = code64;
        /* 64 bit mode uses the bases of FS and GS only */
        if (code64) {
            s->segs[SEG_ES].base = 0;
            s->segs[SEG_SS].base = 0;
            s->segs[SEG_DS].base = 0;
        }
        for (int seg = 0; seg < SEG_COUNT; seg++) {
            seg_update_fast(s, seg);
        }
    }
    if (code64) {
        s->segs[SEG_CS].base = 0;
    }
    s->code32 = get_bit(s->segs[SEG_CS].flags, DESC_DB);
    s->code_opsize = s->code32 || code64 ? SIZE32 : SIZE16;
    s->code_addr_size = code64 ? SIZE64 : s->code_opsize;
    s->eip_mask = size_mask(s->code_addr_size);
    s->sp_mask = code64 ? UINT64_MAX :
        get_bit(s->segs[SEG_SS].flags, DESC_DB) ? UINT32_MAX : 0xffff;
    s->lin_mask = lma ? UINT64_MAX : UINT32_MAX;
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
    bool pg = get_bit(val, CR0_PG);
    bool lme = get_bit(s->efer, EFER_LME);
    if (pg && !get_bit(val, CR0_PE)) {
        raise_exception(s, EXCP_GP, 0);
    }
    /* IA-32e mode starts with paging, from a 32 bit code segment with PAE
       on, and 64 bit mode cannot turn paging off */
    if (get_bit(changed, CR0_PG)) {
        if (pg && lme && (!get_bit(s->cr4, CR4_PAE) ||
                          get_bit(s->segs[SEG_CS].flags, DESC_L))) {
            raise_exception(s, EXCP_GP, 0);
        }
        if (!pg && s->code64) {
            raise_exception(s, EXCP_GP, 0);
        }
    }
    if (pg && !lme && get_bit(s->cr4, CR4_PAE) &&
        (changed & (bit_at(CR0_PG) | bit_at(CR0_CD) | bit_at(CR0_NW)))) {
        load_pdptes(s, s->cr3);
    }
    if (changed & (bit_at(CR0_PG) | bit_at(CR0_WP) | bit_at(CR0_PE))) {
        tlb_flush_all(s);
    }
    s->cr0 = val;
    s->efer = set_bit(s->efer, EFER_LMA, pg && lme);
    if (!get_bit(val, CR0_PE)) {
        s->cpl = 0;
    }
    cpu_update_mode(s);
}

void cpu_set_cr3(X86CPUState *s, uint64_t val)
{
    if (get_bit(s->efer, EFER_LMA)) {
        if (val & ~bit_mask<uint64_t>(X86_CPU_PHYS_ADDRESS_BITS)) {
            raise_exception(s, EXCP_GP, 0);
        }
    } else if (get_bit(s->cr0, CR0_PG) && get_bit(s->cr4, CR4_PAE)) {
        load_pdptes(s, val);
    }
    s->cr3 = val;
    tlb_flush_all(s);
}

void cpu_set_cr4(X86CPUState *s, uint32_t val)
{
    uint32_t paging = bit_at(CR4_PSE) | bit_at(CR4_PAE) | bit_at(CR4_PGE);
    bool lma = get_bit(s->efer, EFER_LMA);
    if ((val & ~CR4_VALID_MASK) || (lma && !get_bit(val, CR4_PAE))) {
        raise_exception(s, EXCP_GP, 0);
    }
    if ((s->cr4 ^ val) & paging) {
        if (get_bit(s->cr0, CR0_PG) && get_bit(val, CR4_PAE) && !lma) {
            load_pdptes(s, s->cr3);
        }
        tlb_flush_all(s);
    }
    s->cr4 = val;
}

static void cpu_set_efer(X86CPUState *s, uint64_t val)
{
    if ((val & ~EFER_VALID_MASK) ||
        (get_bit(s->cr0, CR0_PG) &&
         get_bit(val ^ s->efer, EFER_LME))) {
        raise_exception(s, EXCP_GP, 0);
    }
    /* LMA follows LME and paging; NXE decides which PAE entry bits are
       reserved */
    val = set_bit(val, EFER_LMA, get_bit(s->efer, EFER_LMA));
    if (val != s->efer) {
        tlb_flush_all(s);
    }
    s->efer = val;
}

static void cpu_reset(X86CPUState *s)
{
    uint32_t data_flags = bit_at(DESC_P) | bit_at(DESC_S) | bit_at(DESC_RW) |
        bit_at(DESC_A);

    memset(s->regs, 0, sizeof(s->regs));
    s->regs[REG_EDX] = CPUID_SIGNATURE;
    s->rip = 0xfff0;
    s->eflags = bit_at(1);
    s->cc_op = CC_OP_EFLAGS;
    s->cr0 = bit_at(CR0_ET) | bit_at(CR0_NW) | bit_at(CR0_CD);
    s->cr2 = 0;
    s->cr3 = 0;
    s->cr4 = 0;
    s->cr8 = 0;
    s->efer = 0;
    memset(s->pdpte, 0, sizeof(s->pdpte));
    memset(s->dr, 0, sizeof(s->dr));
    s->dr[6] = 0xffff0ff0;
    s->dr[7] = 0x400;
    s->sysenter_cs = 0;
    s->sysenter_esp = 0;
    s->sysenter_eip = 0;
    s->star = 0;
    s->lstar = 0;
    s->cstar = 0;
    s->sfmask = 0;
    s->kernel_gs_base = 0;
    s->tsc_offset = 0;
    memset(s->pmc_evtsel, 0, sizeof(s->pmc_evtsel));
    memset(s->pmc_ctr, 0, sizeof(s->pmc_ctr));
    s->misc_enable = MISC_ENABLE_RESET;

    s->cpl = 0;
    s->code64 = false;
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
    simd_reset(s);
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
        b = set_bits(0, 8, 8, CPUID_CLFLUSH_SIZE);
        d = bit_at(CPUID_FPU) | bit_at(CPUID_DE) | bit_at(CPUID_PSE) |
            bit_at(CPUID_TSC) | bit_at(CPUID_MSR) | bit_at(CPUID_PAE) |
            bit_at(CPUID_CX8) | bit_at(CPUID_SEP) | bit_at(CPUID_PGE) |
            bit_at(CPUID_CMOV) | bit_at(CPUID_CLFSH) | bit_at(CPUID_MMX) |
            bit_at(CPUID_FXSR) | bit_at(CPUID_SSE) | bit_at(CPUID_SSE2);
        break;
    case 0x80000000:
        a = 0x80000004;
        break;
    case 0x80000001:
        d = bit_at(CPUID_EXT_NX);
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
    case MSR_PERFCTR0:
    case MSR_PERFCTR1:
        val = s->pmc_ctr[s->regs[REG_ECX] - MSR_PERFCTR0];
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
    case MSR_EVNTSEL0:
    case MSR_EVNTSEL1:
        val = s->pmc_evtsel[s->regs[REG_ECX] - MSR_EVNTSEL0];
        break;
    case MSR_MISC_ENABLE:
        val = s->misc_enable;
        break;
    case MSR_EFER:
        val = s->efer;
        break;
    case MSR_STAR:
        val = s->star;
        break;
    case MSR_LSTAR:
        val = s->lstar;
        break;
    case MSR_CSTAR:
        val = s->cstar;
        break;
    case MSR_SFMASK:
        val = s->sfmask;
        break;
    case MSR_FS_BASE:
        val = s->segs[SEG_FS].base;
        break;
    case MSR_GS_BASE:
        val = s->segs[SEG_GS].base;
        break;
    case MSR_KERNEL_GS_BASE:
        val = s->kernel_gs_base;
        break;
    default:
        raise_exception(s, EXCP_GP, 0);
    }
    set_edx_eax(s, val);
}

/* An MSR that holds an address takes only a canonical one. */
static uint64_t canonical_msr(X86CPUState *s, uint64_t val)
{
    if (!is_canonical(val)) {
        raise_exception(s, EXCP_GP, 0);
    }
    return val;
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
    case MSR_PERFCTR0:
    case MSR_PERFCTR1:
        /* the upper bits come from bit 31, not from EDX */
        s->pmc_ctr[s->regs[REG_ECX] - MSR_PERFCTR0] =
            get_bits(sign_extend(val, 32), 0, PMC_BITS);
        break;
    case MSR_SYSENTER_CS:
        s->sysenter_cs = get_bits(val, 0, 16);
        break;
    case MSR_SYSENTER_ESP:
        s->sysenter_esp = canonical_msr(s, val);
        break;
    case MSR_SYSENTER_EIP:
        s->sysenter_eip = canonical_msr(s, val);
        break;
    case MSR_EVNTSEL0:
    case MSR_EVNTSEL1:
        if (get_bits(val, 32, 32) != 0) {
            raise_exception(s, EXCP_GP, 0);
        }
        s->pmc_evtsel[s->regs[REG_ECX] - MSR_EVNTSEL0] = val;
        break;
    case MSR_MISC_ENABLE:
        /* the other bits report what the part has and keep their value */
        s->misc_enable = (s->misc_enable & ~MISC_ENABLE_WRITABLE) |
            (val & MISC_ENABLE_WRITABLE);
        break;
    case MSR_EFER:
        cpu_set_efer(s, val);
        break;
    case MSR_STAR:
        s->star = val;
        break;
    case MSR_LSTAR:
        s->lstar = canonical_msr(s, val);
        break;
    case MSR_CSTAR:
        s->cstar = canonical_msr(s, val);
        break;
    case MSR_SFMASK:
        if (get_bits(val, 32, 32) != 0) {
            raise_exception(s, EXCP_GP, 0);
        }
        s->sfmask = val;
        break;
    case MSR_FS_BASE:
        s->segs[SEG_FS].base = canonical_msr(s, val);
        break;
    case MSR_GS_BASE:
        s->segs[SEG_GS].base = canonical_msr(s, val);
        break;
    case MSR_KERNEL_GS_BASE:
        s->kernel_gs_base = canonical_msr(s, val);
        break;
    default:
        raise_exception(s, EXCP_GP, 0);
    }
}

void cpu_rdpmc(X86CPUState *s)
{
    uint32_t idx = s->regs[REG_ECX];

    if ((s->cpl != 0 && !get_bit(s->cr4, CR4_PCE)) || idx > 1) {
        raise_exception(s, EXCP_GP, 0);
    }
    set_edx_eax(s, s->pmc_ctr[idx]);
}


//#pragma mark - exceptions

static bool exception_is_contributory(int intno)
{
    return intno == EXCP_DE || (intno >= EXCP_TS && intno <= EXCP_GP);
}

/* Deliver a fault or trap and resume at the instruction loop. The return
   address is s->rip: the faulting instruction, or the next one for a trap
   raised once the instruction is complete. */
void raise_exception(X86CPUState *s, int intno, int error_code)
{
    int old = s->old_exception;

#ifdef DUMP_EXCEPTIONS
    fprintf(stderr, "x86: exception %d error=%04x at %04x:%08" PRIx64
            " cr2=%08" PRIx64 "\n", intno, error_code, s->segs[SEG_CS].sel,
            s->rip, s->cr2);
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
    do_interrupt(s, intno, false, error_code, s->rip, false);
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
        if (!s->irq_level.load() || !get_bit(s->eflags, EFLAGS_IF)) {
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
    s->irq_level.store(set);
}

void x86_cpu_set_reg(X86CPUState *s, int reg, uint32_t val)
{
    switch (reg) {
    case X86_CPU_REG_EIP:
        s->rip = val;
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
        return s->rip;
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

void x86_cpu_set_device_lock(X86CPUState *s, DeviceLock *lock)
{
    s->device_lock = lock;
}

int64_t x86_cpu_get_cycles(X86CPUState *s)
{
    return s->cycles;
}

bool x86_cpu_get_power_down(X86CPUState *s)
{
    return s->power_down &&
        !(s->irq_level.load() && get_bit(s->eflags, EFLAGS_IF));
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
