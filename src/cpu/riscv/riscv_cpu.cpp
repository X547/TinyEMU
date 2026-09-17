/*
 * RISCV CPU emulator
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
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <atomic>

#include "bits.h"
#include "cutils.h"
#include "host_time.h"
#include "iomem.h"
#include "riscv_cpu.h"

#ifndef MAX_XLEN
#error MAX_XLEN must be defined
#endif
#ifndef CONFIG_RISCV_MAX_XLEN
#error CONFIG_RISCV_MAX_XLEN must be defined
#endif

#define DUMP_INVALID_MEM_ACCESS
//#define DUMP_MMU_EXCEPTIONS
//#define DUMP_INTERRUPTS
//#define DUMP_INVALID_CSR
//#define DUMP_EXCEPTIONS
//#define DUMP_CSR
//#define CONFIG_LOGFILE

/* softfp is shared with the other XLEN builds, so it stays at global scope. */
#include "softfp.h"

/* Each XLEN build defines its own RISCVCPUState. Giving them internal linkage
   keeps three differently-shaped, polymorphic types with the same name from
   emitting three conflicting vtables under one symbol. */
namespace {

#include "riscv_cpu_priv.h"

#ifdef CONFIG_LOGFILE
static FILE *log_file;

static void log_vprintf(const char *fmt, va_list ap)
{
    if (!log_file)
        log_file = fopen("/tmp/riscemu.log", "wb");
    vfprintf(log_file, fmt, ap);
}
#else
static void log_vprintf(const char *fmt, va_list ap)
{
    vprintf(fmt, ap);
}
#endif

static void __attribute__((format(printf, 1, 2), unused)) log_printf(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    log_vprintf(fmt, ap);
    va_end(ap);
}

#if MAX_XLEN == 128
static void fprint_target_ulong(FILE *f, target_ulong a)
{
    fprintf(f, "%016" PRIx64 "%016" PRIx64, (uint64_t)(a >> 64), (uint64_t)a);
}
#else
static void fprint_target_ulong(FILE *f, target_ulong a)
{
    fprintf(f, "%" PR_target_ulong, a);
}
#endif

static void print_target_ulong(target_ulong a)
{
    fprint_target_ulong(stdout, a);
}

static const char *reg_name[32] = {
"zero", "ra", "sp", "gp", "tp", "t0", "t1", "t2",
"s0", "s1", "a0", "a1", "a2", "a3", "a4", "a5",
"a6", "a7", "s2", "s3", "s4", "s5", "s6", "s7",
"s8", "s9", "s10", "s11", "t3", "t4", "t5", "t6"
};

/* The machine supplies the counter so that the 'time' CSR agrees with the
   timer device; without one, fall back to the host clock. */
static uint64_t riscv_cpu_rtc_time(RISCVCPUState *s)
{
    if (s->rtc_time_source != nullptr) {
        return s->rtc_time_source->RtcTime();
    }
    return host_monotonic_us();
}

static void set_mip(RISCVCPUState *s, uint32_t mask)
{
    s->mip |= mask;
    /* exit from power down if an interrupt is pending */
    if (s->power_down_flag && (s->mip & s->mie) != 0)
        s->power_down_flag = false;
}

/* Sstc drives STIP from the stimecmp comparator instead of leaving it to the
   execution environment. Returns when the next interrupt falls due, or
   UINT64_MAX when the comparator is not in charge or has already fired. */
static uint64_t update_stimer(RISCVCPUState *s)
{
    if (!(s->menvcfg & MENVCFG_STCE))
        return UINT64_MAX;
    if (riscv_cpu_rtc_time(s) >= s->stimecmp) {
        set_mip(s, MIP_STIP);
        return UINT64_MAX;
    }
    s->mip &= ~MIP_STIP;
    return s->stimecmp;
}

static void dump_regs(RISCVCPUState *s)
{
    int i, cols;
    const char priv_str[] = "USHM";
    cols = 256 / MAX_XLEN;
    printf("pc =");
    print_target_ulong(s->pc);
    printf(" ");
    for(i = 1; i < 32; i++) {
        printf("%-3s=", reg_name[i]);
        print_target_ulong(s->reg[i]);
        if ((i & (cols - 1)) == (cols - 1))
            printf("\n");
        else
            printf(" ");
    }
    printf("priv=%c", priv_str[s->priv]);
    printf(" mstatus=");
    print_target_ulong(s->mstatus);
    printf(" cycles=%" PRId64, s->insn_counter);
    printf("\n");
#if 1
    printf(" mideleg=");
    print_target_ulong(s->mideleg);
    printf(" mie=");
    print_target_ulong(s->mie);
    printf(" mip=");
    print_target_ulong(s->mip);
    printf("\n");
#endif
}

static __attribute__((unused)) void cpu_abort(RISCVCPUState *s)
{
    dump_regs(s);
    abort();
}

/* addr must be aligned. Only RAM accesses are supported */
#define PHYS_MEM_READ_WRITE(size, uint_type) \
static __maybe_unused inline void phys_write_u ## size(RISCVCPUState *s, target_ulong addr,\
                                        uint_type val)                   \
{\
    PhysMemoryRange *pr = s->mem_map->FindRange(addr);\
    if (!pr || !pr->is_ram)\
        return;\
    *(uint_type *)(pr->phys_mem + \
                 (uintptr_t)(addr - pr->addr)) = val;\
}\
\
static __maybe_unused inline uint_type phys_read_u ## size(RISCVCPUState *s, target_ulong addr) \
{\
    PhysMemoryRange *pr = s->mem_map->FindRange(addr);\
    if (!pr || !pr->is_ram)\
        return 0;\
    return *(uint_type *)(pr->phys_mem + \
                          (uintptr_t)(addr - pr->addr));     \
}

PHYS_MEM_READ_WRITE(8, uint8_t)
PHYS_MEM_READ_WRITE(32, uint32_t)
PHYS_MEM_READ_WRITE(64, uint64_t)

#define PTE_V_MASK bit_at(0)
#define PTE_U_MASK bit_at(4)
#define PTE_A_MASK bit_at(6)
#define PTE_D_MASK bit_at(7)

/* Bits 63:54 of a 64 bit PTE are claimed by Svnapot (63), Svpbmt (62:61) and
   future standard use (60:54). None of them are implemented here, so a PTE
   that sets any of them is malformed and must fault. */
#if MAX_XLEN >= 64
#define PTE_RESERVED_MASK field_mask<target_ulong>(54, 10)
#else
#define PTE_RESERVED_MASK 0
#endif

#define ACCESS_READ  0
#define ACCESS_WRITE 1
#define ACCESS_CODE  2

/* access = 0: read, 1 = write, 2 = code. Set the exception_pending
   field if necessary. return 0 if OK, -1 if translation error */
static int get_phys_addr(RISCVCPUState *s,
                         target_ulong *ppaddr, target_ulong vaddr,
                         int access)
{
    int mode, levels, pte_bits, pte_idx, pte_size_log2, xwr, priv;
    int vaddr_shift, i, pte_addr_bits;
    target_ulong pte_addr, pte, vaddr_mask, paddr;

    if ((s->mstatus & MSTATUS_MPRV) && access != ACCESS_CODE) {
        /* use previous priviledge */
        priv = get_bits(s->mstatus, MSTATUS_MPP_SHIFT, 2);
    } else {
        priv = s->priv;
    }

    if (priv == PRV_M) {
        if (s->cur_xlen < MAX_XLEN) {
            /* truncate virtual address */
            *ppaddr = get_bits(vaddr, 0, s->cur_xlen);
        } else {
            *ppaddr = vaddr;
        }
        return 0;
    }
#if MAX_XLEN == 32
    /* 32 bits */
    mode = get_bit(s->satp, 31);
    if (mode == 0) {
        /* bare: no translation */
        *ppaddr = vaddr;
        return 0;
    } else {
        /* sv32 */
        levels = 2;
        pte_size_log2 = 2;
        pte_addr_bits = 22;
    }
#else
    mode = get_bits(s->satp, 60, 4);
    if (mode == 0) {
        /* bare: no translation */
        *ppaddr = vaddr;
        return 0;
    } else {
        /* sv39/sv48 */
        levels = mode - 8 + 3;
        pte_size_log2 = 3;
        vaddr_shift = MAX_XLEN - (PG_SHIFT + levels * 9);
        if ((target_ulong)sign_extend(vaddr, MAX_XLEN - vaddr_shift) != vaddr)
            return -1;
        pte_addr_bits = 44;
    }
#endif
    pte_addr = (target_ulong)get_bits(s->satp, 0, pte_addr_bits) << PG_SHIFT;
    pte_bits = 12 - pte_size_log2;
    for(i = 0; i < levels; i++) {
        vaddr_shift = PG_SHIFT + pte_bits * (levels - 1 - i);
        pte_idx = get_bits(vaddr, vaddr_shift, pte_bits);
        pte_addr += pte_idx << pte_size_log2;
        if (pte_size_log2 == 2)
            pte = phys_read_u32(s, pte_addr);
        else
            pte = phys_read_u64(s, pte_addr);
        //printf("pte=0x%08" PRIx64 "\n", pte);
        if ((pte & PTE_RESERVED_MASK) != 0)
            return -1;
        if (!(pte & PTE_V_MASK))
            return -1; /* invalid PTE */
        paddr = (pte >> 10) << PG_SHIFT;
        xwr = get_bits(pte, 1, 3);
        if (xwr == 0) {
            /* pointer to the next level; D, A and U are reserved there */
            if (pte & (PTE_D_MASK | PTE_A_MASK | PTE_U_MASK))
                return -1;
            pte_addr = paddr;
            continue;
        }
        if (xwr == 2 || xwr == 6)
            return -1;
        /* priviledge check */
        if (priv == PRV_S) {
            if ((pte & PTE_U_MASK) && !(s->mstatus & MSTATUS_SUM))
                return -1;
        } else {
            if (!(pte & PTE_U_MASK))
                return -1;
        }
        /* protection check */
        /* MXR allows read access to execute-only pages */
        if (s->mstatus & MSTATUS_MXR)
            xwr |= (xwr >> 2);

        if (!get_bit(xwr, access))
            return -1;
        /* a superpage must be aligned to its own size */
        vaddr_mask = bit_mask<target_ulong>(vaddr_shift);
        if ((paddr & vaddr_mask) != 0)
            return -1;
        /* Svadu updates the A and D bits in place; without it the access
           faults and software sets them */
        if (!(pte & PTE_A_MASK) ||
            (!(pte & PTE_D_MASK) && access == ACCESS_WRITE)) {
            if (!(s->menvcfg & MENVCFG_ADUE))
                return -1;
            pte |= PTE_A_MASK;
            if (access == ACCESS_WRITE)
                pte |= PTE_D_MASK;
            if (pte_size_log2 == 2)
                phys_write_u32(s, pte_addr, pte);
            else
                phys_write_u64(s, pte_addr, pte);
        }
        *ppaddr = (vaddr & vaddr_mask) | (paddr  & ~vaddr_mask);
        return 0;
    }
    return -1;
}

/* return 0 if OK, != 0 if exception */
int target_read_slow(RISCVCPUState *s, mem_uint_t *pval,
                     target_ulong addr, int size_log2)
{
    int size, tlb_idx, err, al;
    target_ulong paddr, offset;
    uint8_t *ptr;
    PhysMemoryRange *pr;
    mem_uint_t ret;

    /* first handle unaligned accesses */
    size = 1 << size_log2;
    al = addr & (size - 1);
    if (al != 0) {
        switch(size_log2) {
        case 1:
            {
                uint8_t v0, v1;
                err = target_read_u8(s, &v0, addr);
                if (err)
                    return err;
                err = target_read_u8(s, &v1, addr + 1);
                if (err)
                    return err;
                ret = v0 | (v1 << 8);
            }
            break;
        case 2:
            {
                uint32_t v0, v1;
                addr -= al;
                err = target_read_u32(s, &v0, addr);
                if (err)
                    return err;
                err = target_read_u32(s, &v1, addr + 4);
                if (err)
                    return err;
                ret = (v0 >> (al * 8)) | (v1 << (32 - al * 8));
            }
            break;
#if MLEN >= 64
        case 3:
            {
                uint64_t v0, v1;
                addr -= al;
                err = target_read_u64(s, &v0, addr);
                if (err)
                    return err;
                err = target_read_u64(s, &v1, addr + 8);
                if (err)
                    return err;
                ret = (v0 >> (al * 8)) | (v1 << (64 - al * 8));
            }
            break;
#endif
#if MLEN >= 128
        case 4:
            {
                uint128_t v0, v1;
                addr -= al;
                err = target_read_u128(s, &v0, addr);
                if (err)
                    return err;
                err = target_read_u128(s, &v1, addr + 16);
                if (err)
                    return err;
                ret = (v0 >> (al * 8)) | (v1 << (128 - al * 8));
            }
            break;
#endif
        default:
            abort();
        }
    } else {
        if (get_phys_addr(s, &paddr, addr, ACCESS_READ)) {
            s->pending_tval = addr;
            s->pending_exception = CAUSE_LOAD_PAGE_FAULT;
            return -1;
        }
        pr = s->mem_map->FindRange(paddr);
        if (!pr) {
#ifdef DUMP_INVALID_MEM_ACCESS
            printf("target_read_slow: invalid physical address 0x");
            print_target_ulong(paddr);
            printf(", PC: ");
            print_target_ulong(s->pc);
            printf("\n");
#endif
            s->pending_tval = addr;
            s->pending_exception = CAUSE_FAULT_LOAD;
            return -1;
        } else if (pr->is_ram) {
            tlb_idx = get_bits(addr, PG_SHIFT, TLB_BITS);
            ptr = pr->phys_mem + (uintptr_t)(paddr - pr->addr);
            s->tlb_read[tlb_idx].vaddr = addr & ~PG_MASK;
            s->tlb_read[tlb_idx].mem_addend = (uintptr_t)ptr - addr;
            switch(size_log2) {
            case 0:
                ret = *(uint8_t *)ptr;
                break;
            case 1:
                ret = *(uint16_t *)ptr;
                break;
            case 2:
                ret = *(uint32_t *)ptr;
                break;
#if MLEN >= 64
            case 3:
                ret = *(uint64_t *)ptr;
                break;
#endif
#if MLEN >= 128
            case 4:
                ret = *(uint128_t *)ptr;
                break;
#endif
            default:
                abort();
            }
        } else {
            DeviceLocker locker(*s->device_lock);
            offset = paddr - pr->addr;
            if (get_bit(pr->devio_flags, size_log2)) {
                ret = pr->io->DeviceRead(offset, size_log2);
            }
#if MLEN >= 64
            else if ((pr->devio_flags & DEVIO_SIZE32) && size_log2 == 3) {
                /* emulate 64 bit access */
                ret = pr->io->DeviceRead(offset, 2);
                ret |= (uint64_t)pr->io->DeviceRead(offset + 4, 2) << 32;
                
            }
#endif
            else {
#ifdef DUMP_INVALID_MEM_ACCESS
                printf("unsupported device read access: addr=0x");
                print_target_ulong(paddr);
                printf(" width=%d bits\n", 1 << (3 + size_log2));
#endif
                ret = 0;
            }
        }
    }
    *pval = ret;
    return 0;
}

/* return 0 if OK, != 0 if exception */
int target_write_slow(RISCVCPUState *s, target_ulong addr,
                      mem_uint_t val, int size_log2)
{
    int size, i, tlb_idx, err;
    target_ulong paddr, offset;
    uint8_t *ptr;
    PhysMemoryRange *pr;
    
    /* first handle unaligned accesses */
    size = 1 << size_log2;
    if ((addr & (size - 1)) != 0) {
        /* XXX: should avoid modifying the memory in case of exception */
        for(i = 0; i < size; i++) {
            err = target_write_u8(s, addr + i, get_bits(val, 8 * i, 8));
            if (err)
                return err;
        }
    } else {
        if (get_phys_addr(s, &paddr, addr, ACCESS_WRITE)) {
            s->pending_tval = addr;
            s->pending_exception = CAUSE_STORE_PAGE_FAULT;
            return -1;
        }
        pr = s->mem_map->FindRange(paddr);
        if (!pr) {
#ifdef DUMP_INVALID_MEM_ACCESS
            printf("target_write_slow: invalid physical address 0x");
            print_target_ulong(paddr);
            printf(", PC: ");
            print_target_ulong(s->pc);
            printf("\n");
#endif
            s->pending_tval = addr;
            s->pending_exception = CAUSE_FAULT_STORE;
            return -1;
        } else if (pr->is_ram) {
            pr->SetDirtyBit(paddr - pr->addr);
            tlb_idx = get_bits(addr, PG_SHIFT, TLB_BITS);
            ptr = pr->phys_mem + (uintptr_t)(paddr - pr->addr);
            s->tlb_write[tlb_idx].vaddr = addr & ~PG_MASK;
            s->tlb_write[tlb_idx].mem_addend = (uintptr_t)ptr - addr;
            switch(size_log2) {
            case 0:
                *(uint8_t *)ptr = val;
                break;
            case 1:
                *(uint16_t *)ptr = val;
                break;
            case 2:
                *(uint32_t *)ptr = val;
                break;
#if MLEN >= 64
            case 3:
                *(uint64_t *)ptr = val;
                break;
#endif
#if MLEN >= 128
            case 4:
                *(uint128_t *)ptr = val;
                break;
#endif
            default:
                abort();
            }
        } else {
            DeviceLocker locker(*s->device_lock);
            offset = paddr - pr->addr;
            if (get_bit(pr->devio_flags, size_log2)) {
                pr->io->DeviceWrite(offset, val, size_log2);
            }
#if MLEN >= 64
            else if ((pr->devio_flags & DEVIO_SIZE32) && size_log2 == 3) {
                /* emulate 64 bit access */
                pr->io->DeviceWrite(offset,
                               get_bits(val, 0, 32), 2);
                pr->io->DeviceWrite(offset + 4,
                               get_bits(val, 32, 32), 2);
            }
#endif
            else {
#ifdef DUMP_INVALID_MEM_ACCESS
                printf("unsupported device write access: addr=0x");
                print_target_ulong(paddr);
                printf(" width=%d bits\n", 1 << (3 + size_log2));
#endif
            }
        }
    }
    return 0;
}

struct __attribute__((packed)) unaligned_u32 {
    uint32_t u32;
};

/* unaligned access at an address known to be a multiple of 2 */
static uint32_t get_insn32(uint8_t *ptr)
{
    return ((struct unaligned_u32 *)ptr)->u32;
}

/* return 0 if OK, != 0 if exception */
static no_inline __exception int target_read_insn_slow(RISCVCPUState *s,
                                                       uint8_t **pptr,
                                                       target_ulong addr)
{
    int tlb_idx;
    target_ulong paddr;
    uint8_t *ptr;
    PhysMemoryRange *pr;
    
    if (get_phys_addr(s, &paddr, addr, ACCESS_CODE)) {
        s->pending_tval = addr;
        s->pending_exception = CAUSE_FETCH_PAGE_FAULT;
        return -1;
    }
    pr = s->mem_map->FindRange(paddr);
    if (!pr || !pr->is_ram) {
        /* XXX: we only access to execute code from RAM */
        s->pending_tval = addr;
        s->pending_exception = CAUSE_FAULT_FETCH;
        return -1;
    }
    tlb_idx = get_bits(addr, PG_SHIFT, TLB_BITS);
    ptr = pr->phys_mem + (uintptr_t)(paddr - pr->addr);
    s->tlb_code[tlb_idx].vaddr = addr & ~PG_MASK;
    s->tlb_code[tlb_idx].mem_addend = (uintptr_t)ptr - addr;
    *pptr = ptr;
    return 0;
}

/* addr must be aligned */
static inline __exception int target_read_insn_u16(RISCVCPUState *s, uint16_t *pinsn,
                                                   target_ulong addr)
{
    uint32_t tlb_idx;
    uint8_t *ptr;
    
    tlb_idx = get_bits(addr, PG_SHIFT, TLB_BITS);
    if (likely(s->tlb_code[tlb_idx].vaddr == (addr & ~PG_MASK))) {
        ptr = (uint8_t *)(s->tlb_code[tlb_idx].mem_addend +
                          (uintptr_t)addr);
    } else {
        if (target_read_insn_slow(s, &ptr, addr))
            return -1;
    }
    *pinsn = *(uint16_t *)ptr;
    return 0;
}

static void tlb_init(RISCVCPUState *s)
{
    int i;
    
    for(i = 0; i < TLB_SIZE; i++) {
        s->tlb_read[i].vaddr = -1;
        s->tlb_write[i].vaddr = -1;
        s->tlb_code[i].vaddr = -1;
    }
}

static void tlb_flush_all(RISCVCPUState *s)
{
    tlb_init(s);
}

/* Entries are cached per 4 KiB page with no record of the page size they came
   from, so a superpage is spread over many slots and there is no way to find
   the rest of them from one address. Everything goes. */
static void tlb_flush_vaddr(RISCVCPUState *s, target_ulong vaddr)
{
    tlb_flush_all(s);
}

/* XXX: inefficient but not critical as long as it is seldom used */
static void glue(riscv_cpu_flush_tlb_write_range_ram,
                 MAX_XLEN)(RISCVCPUState *s,
                           uint8_t *ram_ptr, size_t ram_size)
{
    uint8_t *ptr, *ram_end;
    int i;
    
    ram_end = ram_ptr + ram_size;
    for(i = 0; i < TLB_SIZE; i++) {
        if (s->tlb_write[i].vaddr != -1) {
            ptr = (uint8_t *)(s->tlb_write[i].mem_addend +
                              (uintptr_t)s->tlb_write[i].vaddr);
            if (ptr >= ram_ptr && ptr < ram_end) {
                s->tlb_write[i].vaddr = -1;
            }
        }
    }
}


#define SSTATUS_MASK0 (MSTATUS_SIE | MSTATUS_SPIE |     \
                      MSTATUS_SPP | \
                      MSTATUS_FS | MSTATUS_XS | \
                      MSTATUS_SUM | MSTATUS_MXR)
#if MAX_XLEN >= 64
#define SSTATUS_MASK (SSTATUS_MASK0 | MSTATUS_UXL_MASK)
#else
#define SSTATUS_MASK SSTATUS_MASK0
#endif


#define MSTATUS_MASK (MSTATUS_SIE | MSTATUS_MIE |      \
                      MSTATUS_SPIE | MSTATUS_MPIE |    \
                      MSTATUS_SPP | MSTATUS_MPP | \
                      MSTATUS_FS | \
                      MSTATUS_MPRV | MSTATUS_SUM | MSTATUS_MXR | \
                      MSTATUS_TVM | MSTATUS_TW | MSTATUS_TSR)

/* cycle, time and insn counters */
#define COUNTEREN_MASK bit_mask(3)

/* the time counter cannot be inhibited, and the event counters are hardwired
   to zero, so only mcycle and minstret answer to mcountinhibit */
#define MCOUNTINHIBIT_CY bit_at(0)
#define MCOUNTINHIBIT_IR bit_at(2)
#define MCOUNTINHIBIT_MASK (MCOUNTINHIBIT_CY | MCOUNTINHIBIT_IR)

/* every synchronous cause that can be taken in a mode below M. Machine ECALL
   (11), double trap (16) and the reserved causes 10 and 14 are read-only 0 */
#define MEDELEG_MASK 0x0000b3ff

/* FIOM is settable in both environment configuration registers; ADUE and STCE
   come with Svadu and Sstc. The remaining fields belong to extensions this
   implementation does not provide and stay read-only zero. */
#define MENVCFG_MASK (ENVCFG_FIOM | MENVCFG_ADUE | MENVCFG_STCE)
#define SENVCFG_MASK (ENVCFG_FIOM)

/* return the complete mstatus with the SD bit */
static target_ulong get_mstatus(RISCVCPUState *s, target_ulong mask)
{
    target_ulong val;
    bool sd;
    val = set_bits(s->mstatus, MSTATUS_FS_SHIFT, 2, s->fs);
    val &= mask;
    sd = ((val & MSTATUS_FS) == MSTATUS_FS) |
        ((val & MSTATUS_XS) == MSTATUS_XS);
    if (sd)
        val |= bit_at<target_ulong>(s->cur_xlen - 1);
    return val;
}
                              
static int get_base_from_xlen(int xlen)
{
    if (xlen == 32)
        return 1;
    else if (xlen == 64)
        return 2;
    else
        return 3;
}

static void set_mstatus(RISCVCPUState *s, target_ulong val)
{
    target_ulong mod, mask;

    /* MPP is WARL and 2 is not a supported mode */
    if (get_bits(val, MSTATUS_MPP_SHIFT, 2) == PRV_H) {
        val = set_bits(val, MSTATUS_MPP_SHIFT, 2,
                       get_bits(s->mstatus, MSTATUS_MPP_SHIFT, 2));
    }

    /* flush the TLBs if change of MMU config */
    mod = s->mstatus ^ val;
    if ((mod & (MSTATUS_MPRV | MSTATUS_SUM | MSTATUS_MXR)) != 0 ||
        ((s->mstatus & MSTATUS_MPRV) && (mod & MSTATUS_MPP) != 0)) {
        tlb_flush_all(s);
    }
    s->fs = get_bits(val, MSTATUS_FS_SHIFT, 2);

    mask = MSTATUS_MASK & ~MSTATUS_FS;
#if MAX_XLEN >= 64
    {
        int uxl, sxl;
        uxl = get_bits(val, MSTATUS_UXL_SHIFT, 2);
        if (uxl >= 1 && uxl <= get_base_from_xlen(MAX_XLEN))
            mask |= MSTATUS_UXL_MASK;
        sxl = get_bits(val, MSTATUS_SXL_SHIFT, 2);
        if (sxl >= 1 && sxl <= get_base_from_xlen(MAX_XLEN))
            mask |= MSTATUS_SXL_MASK;
    }
#endif
    s->mstatus = (s->mstatus & ~mask) | (val & mask);
}

/* mcounteren gates S-mode and, together with scounteren, U-mode access to the
   counter CSRs. 'counter' is the index of the counter within either register */
static bool counter_accessible(RISCVCPUState *s, int counter)
{
    uint32_t counteren;

    if (s->priv >= PRV_M)
        return true;
    counteren = s->mcounteren;
    if (s->priv < PRV_S)
        counteren &= s->scounteren;
    return get_bit(counteren, counter);
}

static uint64_t get_mcycle(RISCVCPUState *s)
{
    if (s->mcountinhibit & MCOUNTINHIBIT_CY)
        return s->mcycle_base;
    return s->insn_counter + s->mcycle_base;
}

static uint64_t get_minstret(RISCVCPUState *s)
{
    if (s->mcountinhibit & MCOUNTINHIBIT_IR)
        return s->minstret_base;
    return s->insn_counter + s->minstret_base;
}

static void set_mcycle(RISCVCPUState *s, uint64_t val)
{
    if (s->mcountinhibit & MCOUNTINHIBIT_CY)
        s->mcycle_base = val;
    else
        s->mcycle_base = val - s->insn_counter;
}

static void set_minstret(RISCVCPUState *s, uint64_t val)
{
    if (s->mcountinhibit & MCOUNTINHIBIT_IR)
        s->minstret_base = val;
    else
        s->minstret_base = val - s->insn_counter;
}

/* menvcfg.STCE hands stimecmp to S-mode; without it only M-mode may touch it */
static bool stimecmp_accessible(RISCVCPUState *s)
{
    return s->priv >= PRV_M || (s->menvcfg & MENVCFG_STCE) != 0;
}

/* some CSRs are 64 bit whatever XLEN is, so RV32 writes one half at a time
   through a companion CSR and leaves the other one alone */
static uint64_t csr64_written(RISCVCPUState *s, uint64_t old_val,
                              target_ulong val, bool high)
{
    if (s->cur_xlen != 32)
        return val;
    return set_bits(old_val, high ? 32 : 0, 32, val);
}

/* the order the privileged spec requires simultaneous interrupts to be taken
   in: MEI, MSI, MTI, SEI, SSI, STI */
static const uint8_t irq_priority[] = { 11, 3, 7, 9, 1, 5 };

/* mtopi or stopi: the top pending and enabled interrupt that traps to the
   given level. The iprio arrays are read-only zero, so IPRIO is always 1. */
static target_ulong top_interrupt(RISCVCPUState *s, bool supervisor)
{
    uint32_t pending = s->mip & s->mie;
    int irq;
    size_t i;

    pending &= supervisor ? s->mideleg : ~s->mideleg;
    if (pending == 0)
        return 0;
    irq = ctz32(pending);
    for(i = 0; i < countof(irq_priority); i++) {
        if (get_bit(pending, irq_priority[i])) {
            irq = irq_priority[i];
            break;
        }
    }
    return ((target_ulong)irq << 16) | 1;
}

/* The lowest pending and enabled identity under the threshold, or 0. */
static uint32_t imsic_top(const ImsicFile *f)
{
    for (int w = 0; w < IMSIC_WORDS; w++) {
        uint64_t mask = f->eip[w] & f->eie[w];
        if (mask != 0) {
            uint32_t id = w * 64 + __builtin_ctzll(mask);
            if (f->eithreshold != 0 && id >= f->eithreshold)
                return 0;
            return id;
        }
    }
    return 0;
}

/* An interrupt file drives MEIP or SEIP by itself. */
static void imsic_update(RISCVCPUState *s, int file)
{
    uint32_t bit = file == IMSIC_FILE_M ? MIP_MEIP : MIP_SEIP;

    if (s->imsic[file].eidelivery == 1 && imsic_top(&s->imsic[file]) != 0)
        set_mip(s, bit);
    else
        s->mip &= ~bit;
}

/* mtopei or stopei */
static target_ulong imsic_topei(RISCVCPUState *s, int file)
{
    uint32_t id = imsic_top(&s->imsic[file]);
    return ((target_ulong)id << 16) | id;
}

/* A write to mtopei or stopei claims what it reads. */
static void imsic_claim(RISCVCPUState *s, int file)
{
    uint32_t id = imsic_top(&s->imsic[file]);

    if (id != 0) {
        s->imsic[file].eip[id / 64] &= ~bit_at<uint64_t>(id % 64);
        imsic_update(s, file);
    }
}

/* Take in the interrupt lines and messages other threads posted. */
static void process_inbox(RISCVCPUState *s)
{
    if (likely(!s->inbox_pending.load(std::memory_order_relaxed)))
        return;
    /* cleared first, so that a post racing with this is seen next time */
    s->inbox_pending.store(false);

    s->mip = (s->mip & ~s->irq_line_mask) |
        (s->irq_lines.load() & s->irq_line_mask);
    if (s->intr_arch == RISCV_INTR_AIA_IMSIC) {
        for (int file = 0; file < 2; file++) {
            uint64_t posted = 0;
            for (int w = 0; w < IMSIC_WORDS; w++) {
                uint64_t bits = s->msi_pending[file][w].exchange(0);
                s->imsic[file].eip[w] |= bits;
                posted |= bits;
            }
            if (posted != 0)
                imsic_update(s, file);
        }
    }
    if (s->power_down_flag && (s->mip & s->mie) != 0)
        s->power_down_flag = false;
}

/* The eip or eie register an *iselect value in 0x80-0xff names: a 32 bit half
   of a 64 bit word for RV32, a whole word otherwise. Returns false for the
   odd numbers, which do not exist for RV64. Registers past the implemented
   identities give a null word. */
static bool imsic_array_reg(RISCVCPUState *s, ImsicFile *f, target_ulong sel,
                            uint64_t **pword, int *pshift, int *plen)
{
    uint64_t *array = sel >= 0xc0 ? f->eie : f->eip;
    unsigned int k = get_bits(sel, 0, 6);

    if (s->cur_xlen == 32) {
        *pshift = get_bit(k, 0) ? 32 : 0;
        *plen = 32;
    } else {
        if (get_bit(k, 0))
            return false;
        *pshift = 0;
        *plen = 64;
    }
    *pword = (k / 2) < IMSIC_WORDS ? &array[k / 2] : nullptr;
    return true;
}

/* mireg or sireg. Returns -1 if the selected register does not exist. */
static int ireg_read(RISCVCPUState *s, int file, target_ulong sel,
                     target_ulong *pval)
{
    if (sel >= 0x30 && sel <= 0x3f) {
        /* the iprio array, all read-only zero */
        if (s->cur_xlen != 32 && (sel & 1))
            return -1;
        *pval = 0;
        return 0;
    }
    if (sel >= 0x70 && sel <= 0xff && s->intr_arch == RISCV_INTR_AIA_IMSIC) {
        ImsicFile *f = &s->imsic[file];
        uint64_t *word;
        int shift, len;

        if (sel == 0x70) {
            *pval = f->eidelivery;
        } else if (sel == 0x72) {
            *pval = f->eithreshold;
        } else if (sel >= 0x80) {
            if (!imsic_array_reg(s, f, sel, &word, &shift, &len))
                return -1;
            *pval = word != nullptr ? get_bits(*word, shift, len) : 0;
        } else {
            *pval = 0; /* reserved */
        }
        return 0;
    }
    return -1;
}

static int ireg_write(RISCVCPUState *s, int file, target_ulong sel,
                      target_ulong val)
{
    if (sel >= 0x30 && sel <= 0x3f) {
        if (s->cur_xlen != 32 && (sel & 1))
            return -1;
        return 0;
    }
    if (sel >= 0x70 && sel <= 0xff && s->intr_arch == RISCV_INTR_AIA_IMSIC) {
        ImsicFile *f = &s->imsic[file];
        uint64_t *word;
        int shift, len;

        if (sel == 0x70) {
            f->eidelivery = get_bit(val, 0);
        } else if (sel == 0x72) {
            f->eithreshold = val <= RISCV_IMSIC_NUM_IDS ? val :
                RISCV_IMSIC_NUM_IDS;
        } else if (sel >= 0x80) {
            if (!imsic_array_reg(s, f, sel, &word, &shift, &len))
                return -1;
            if (word != nullptr) {
                *word = set_bits(*word, shift, len, val);
                /* identity 0 does not exist */
                f->eip[0] = set_bit(f->eip[0], 0, false);
                f->eie[0] = set_bit(f->eie[0], 0, false);
            }
        }
        imsic_update(s, file);
        return 0;
    }
    return -1;
}

/* return -1 if invalid CSR. 0 if OK. 'will_write' indicate that the
   csr will be written after (used for CSR access check) */
static int csr_read(RISCVCPUState *s, target_ulong *pval, uint32_t csr,
                     bool will_write)
{
    target_ulong val;

    if (get_bits(csr, 10, 2) == 3 && will_write)
        return -1; /* read-only CSR */
    if (s->priv < get_bits(csr, 8, 2))
        return -1; /* not enough priviledge */
    process_inbox(s);

    switch(csr) {
#if FLEN > 0
    case 0x001: /* fflags */
        if (s->fs == 0)
            return -1;
        val = s->fflags;
        break;
    case 0x002: /* frm */
        if (s->fs == 0)
            return -1;
        val = s->frm;
        break;
    case 0x003:
        if (s->fs == 0)
            return -1;
        val = set_bits(s->fflags, 5, 3, s->frm);
        break;
#endif
    case 0xc00: /* cycle */
        if (!counter_accessible(s, 0))
            goto invalid_csr;
        val = get_mcycle(s);
        break;
    case 0xc01: /* time */
        if (!counter_accessible(s, 1))
            goto invalid_csr;
        val = riscv_cpu_rtc_time(s);
        break;
    case 0xc02: /* instret */
        if (!counter_accessible(s, 2))
            goto invalid_csr;
        val = get_minstret(s);
        break;
    case 0xc03 ... 0xc1f: /* hpmcounter3..31 */
        if (!counter_accessible(s, get_bits(csr, 0, 5)))
            goto invalid_csr;
        val = 0;
        break;
    /* RV32 reads the 64 bit counters as two halves */
    case 0xc80: /* cycleh */
        if (s->cur_xlen != 32 || !counter_accessible(s, 0))
            goto invalid_csr;
        val = get_mcycle(s) >> 32;
        break;
    case 0xc81: /* timeh */
        if (s->cur_xlen != 32 || !counter_accessible(s, 1))
            goto invalid_csr;
        val = riscv_cpu_rtc_time(s) >> 32;
        break;
    case 0xc82: /* instreth */
        if (s->cur_xlen != 32 || !counter_accessible(s, 2))
            goto invalid_csr;
        val = get_minstret(s) >> 32;
        break;
    case 0xc83 ... 0xc9f: /* hpmcounter3..31h */
        if (s->cur_xlen != 32 || !counter_accessible(s, get_bits(csr, 0, 5)))
            goto invalid_csr;
        val = 0;
        break;

    case 0x100:
        val = get_mstatus(s, SSTATUS_MASK);
        break;
    case 0x104: /* sie */
        val = s->mie & s->mideleg;
        break;
    case 0x105:
        val = s->stvec;
        break;
    case 0x106:
        val = s->scounteren;
        break;
    case 0x140:
        val = s->sscratch;
        break;
    case 0x141:
        val = s->sepc;
        break;
    case 0x142:
        val = s->scause;
        break;
    case 0x143:
        val = s->stval;
        break;
    case 0x144: /* sip */
        val = s->mip & s->mideleg;
        break;
    case 0x10a: /* senvcfg */
        val = s->senvcfg;
        break;
    case 0x14d: /* stimecmp */
        if (!stimecmp_accessible(s))
            goto invalid_csr;
        val = s->stimecmp;
        break;
    case 0x15d: /* stimecmph */
        if (s->cur_xlen != 32 || !stimecmp_accessible(s))
            goto invalid_csr;
        val = s->stimecmp >> 32;
        break;
    case 0x180:
        if (s->priv < PRV_M && (s->mstatus & MSTATUS_TVM))
            goto invalid_csr;
        val = s->satp;
        break;

    /* Smaia and Ssaia */
    case 0x150: /* siselect */
        if (s->intr_arch == RISCV_INTR_BASE)
            goto invalid_csr;
        val = s->siselect;
        break;
    case 0x151: /* sireg */
        if (s->intr_arch == RISCV_INTR_BASE ||
            ireg_read(s, IMSIC_FILE_S, s->siselect, &val) < 0)
            goto invalid_csr;
        break;
    case 0x15c: /* stopei */
        if (s->intr_arch != RISCV_INTR_AIA_IMSIC)
            goto invalid_csr;
        val = imsic_topei(s, IMSIC_FILE_S);
        break;
    case 0xdb0: /* stopi */
        if (s->intr_arch == RISCV_INTR_BASE)
            goto invalid_csr;
        val = top_interrupt(s, true);
        break;
    case 0x114: /* sieh */
    case 0x154: /* siph */
        if (s->intr_arch == RISCV_INTR_BASE || s->cur_xlen != 32)
            goto invalid_csr;
        val = 0;
        break;
    case 0x308: /* mvien */
        if (s->intr_arch == RISCV_INTR_BASE)
            goto invalid_csr;
        val = 0;
        break;
    case 0x309: /* mvip */
        if (s->intr_arch == RISCV_INTR_BASE)
            goto invalid_csr;
        /* with mvien read-only zero, only aliases of writable mip bits */
        val = s->mip & MIP_SSIP;
        if (!(s->menvcfg & MENVCFG_STCE))
            val |= s->mip & MIP_STIP;
        break;
    case 0x313: /* midelegh */
    case 0x314: /* mieh */
    case 0x318: /* mvienh */
    case 0x319: /* mviph */
    case 0x354: /* miph */
        if (s->intr_arch == RISCV_INTR_BASE || s->cur_xlen != 32)
            goto invalid_csr;
        val = 0;
        break;
    case 0x350: /* miselect */
        if (s->intr_arch == RISCV_INTR_BASE)
            goto invalid_csr;
        val = s->miselect;
        break;
    case 0x351: /* mireg */
        if (s->intr_arch == RISCV_INTR_BASE ||
            ireg_read(s, IMSIC_FILE_M, s->miselect, &val) < 0)
            goto invalid_csr;
        break;
    case 0x35c: /* mtopei */
        if (s->intr_arch != RISCV_INTR_AIA_IMSIC)
            goto invalid_csr;
        val = imsic_topei(s, IMSIC_FILE_M);
        break;
    case 0xfb0: /* mtopi */
        if (s->intr_arch == RISCV_INTR_BASE)
            goto invalid_csr;
        val = top_interrupt(s, false);
        break;
    case 0x300:
        val = get_mstatus(s, (target_ulong)-1);
        break;
    case 0x301:
        val = set_bits((target_ulong)s->misa, s->cur_xlen - 2, 2, s->mxl);
        break;
    case 0x302:
        val = s->medeleg;
        break;
    case 0x303:
        val = s->mideleg;
        break;
    case 0x304:
        val = s->mie;
        break;
    case 0x305:
        val = s->mtvec;
        break;
    case 0x306:
        val = s->mcounteren;
        break;
    case 0x30a: /* menvcfg */
        val = s->menvcfg;
        break;
    case 0x31a: /* menvcfgh */
        if (s->cur_xlen != 32)
            goto invalid_csr;
        val = s->menvcfg >> 32;
        break;
    case 0x340:
        val = s->mscratch;
        break;
    case 0x341:
        val = s->mepc;
        break;
    case 0x342:
        val = s->mcause;
        break;
    case 0x343:
        val = s->mtval;
        break;
    case 0x344:
        val = s->mip;
        break;
    case 0x310: /* mstatush */
        /* RV32 only. Holds the MBE/SBE endianness controls, both of which
           are zero on this little endian implementation. */
        if (s->cur_xlen != 32)
            goto invalid_csr;
        val = 0;
        break;
    case 0x320: /* mcountinhibit */
        val = s->mcountinhibit;
        break;
    case 0x323 ... 0x33f: /* mhpmevent3..31 */
        val = 0; /* the event counters are hardwired to zero */
        break;
    case 0x3a0 ... 0x3a3: /* pmpcfg0..3 */
        val = 0; /* not implemented */
        break;
    case 0x3b0 ... 0x3bf: /* pmpaddr0..15 */
        val = 0; /* not implemented */
        break;
    case 0xb00: /* mcycle */
        val = get_mcycle(s);
        break;
    case 0xb02: /* minstret */
        val = get_minstret(s);
        break;
    case 0xb03 ... 0xb1f: /* mhpmcounter3..31 */
        val = 0;
        break;
    case 0xb80: /* mcycleh */
        if (s->cur_xlen != 32)
            goto invalid_csr;
        val = get_mcycle(s) >> 32;
        break;
    case 0xb82: /* minstreth */
        if (s->cur_xlen != 32)
            goto invalid_csr;
        val = get_minstret(s) >> 32;
        break;
    case 0xb83 ... 0xb9f: /* mhpmcounter3..31h */
        if (s->cur_xlen != 32)
            goto invalid_csr;
        val = 0;
        break;
    /* Read-only machine identification registers. The privileged spec makes
       them mandatory; zero is the legal "not implemented" value. OpenSBI
       reads all three unconditionally while printing its banner. */
    case 0xf11: /* mvendorid */
    case 0xf12: /* marchid */
    case 0xf13: /* mimpid */
    case 0xf15: /* mconfigptr */
        val = 0;
        break;
    case 0xf14:
        val = s->mhartid;
        break;
    default:
    invalid_csr:
#ifdef DUMP_INVALID_CSR
        /* the 'time' counter is usually emulated */
        if (csr != 0xc01 && csr != 0xc81) {
            printf("csr_read: invalid CSR=0x%x\n", csr);
        }
#endif
        *pval = 0;
        return -1;
    }
    *pval = val;
    return 0;
}

#if FLEN > 0
static void set_frm(RISCVCPUState *s, unsigned int val)
{
    if (val >= 5)
        val = 0;
    s->frm = val;
}

/* return -1 if invalid roundind mode */
static int get_insn_rm(RISCVCPUState *s, unsigned int rm)
{
    if (rm == 7)
        return s->frm;
    if (rm >= 5)
        return -1;
    else
        return rm;
}
#endif

/* return -1 if invalid CSR, 0 if OK, 1 if the interpreter loop must be
   exited (e.g. XLEN was modified), 2 if TLBs have been flushed. */
static int csr_write(RISCVCPUState *s, uint32_t csr, target_ulong val)
{
    target_ulong mask;

#if defined(DUMP_CSR)
    printf("csr_write: csr=0x%03x val=0x", csr);
    print_target_ulong(val);
    printf("\n");
#endif
    process_inbox(s);
    switch(csr) {
#if FLEN > 0
    case 0x001: /* fflags */
        s->fflags = get_bits(val, 0, 5);
        s->fs = 3;
        break;
    case 0x002: /* frm */
        set_frm(s, get_bits(val, 0, 3));
        s->fs = 3;
        break;
    case 0x003: /* fcsr */
        set_frm(s, get_bits(val, 5, 3));
        s->fflags = get_bits(val, 0, 5);
        s->fs = 3;
        break;
#endif
    case 0x100: /* sstatus */
        set_mstatus(s, (s->mstatus & ~SSTATUS_MASK) | (val & SSTATUS_MASK));
        break;
    case 0x104: /* sie */
        mask = s->mideleg;
        s->mie = (s->mie & ~mask) | (val & mask);
        break;
    case 0x105:
        s->stvec = set_bits(val, 0, 2, 0);
        break;
    case 0x106:
        s->scounteren = val & COUNTEREN_MASK;
        break;
    case 0x140:
        s->sscratch = val;
        break;
    case 0x141:
        s->sepc = set_bit(val, 0, false);
        break;
    case 0x142:
        s->scause = val;
        break;
    case 0x143:
        s->stval = val;
        break;
    case 0x144: /* sip */
        /* STIP and SEIP belong to their interrupt controllers; only the
           software interrupt is writable from S-mode */
        mask = s->mideleg & MIP_SSIP;
        s->mip = (s->mip & ~mask) | (val & mask);
        break;
    case 0x10a: /* senvcfg */
        s->senvcfg = val & SENVCFG_MASK;
        break;
    case 0x14d: /* stimecmp */
        if (!stimecmp_accessible(s))
            return -1;
        s->stimecmp = csr64_written(s, s->stimecmp, val, false);
        update_stimer(s);
        break;
    case 0x15d: /* stimecmph */
        if (s->cur_xlen != 32 || !stimecmp_accessible(s))
            return -1;
        s->stimecmp = csr64_written(s, s->stimecmp, val, true);
        update_stimer(s);
        break;
    case 0x180:
        if (s->priv < PRV_M && (s->mstatus & MSTATUS_TVM))
            return -1;
        /* no ASID implemented */
#if MAX_XLEN == 32
        s->satp = set_bit(get_bits(val, 0, 22), 31, get_bit(val, 31));
#else
        {
            int mode, new_mode;
            mode = get_bits(s->satp, 60, 4);
            new_mode = get_bits(val, 60, 4);
            /* bare, or Sv39 through Sv57 */
            if (new_mode == 0 || (new_mode >= 8 && new_mode <= 10))
                mode = new_mode;
            s->satp = set_bits(get_bits(val, 0, 44), 60, 4, mode);
        }
#endif
        tlb_flush_all(s);
        return 2;

    /* Smaia and Ssaia */
    case 0x150: /* siselect */
        if (s->intr_arch == RISCV_INTR_BASE)
            return -1;
        s->siselect = get_bits(val, 0, 9);
        break;
    case 0x151: /* sireg */
        if (s->intr_arch == RISCV_INTR_BASE)
            return -1;
        return ireg_write(s, IMSIC_FILE_S, s->siselect, val);
    case 0x15c: /* stopei */
        if (s->intr_arch != RISCV_INTR_AIA_IMSIC)
            return -1;
        imsic_claim(s, IMSIC_FILE_S);
        break;
    case 0x114: /* sieh */
    case 0x154: /* siph */
        if (s->intr_arch == RISCV_INTR_BASE || s->cur_xlen != 32)
            return -1;
        break;
    case 0x308: /* mvien */
        if (s->intr_arch == RISCV_INTR_BASE)
            return -1;
        break;
    case 0x309: /* mvip */
        if (s->intr_arch == RISCV_INTR_BASE)
            return -1;
        mask = MIP_SSIP;
        if (!(s->menvcfg & MENVCFG_STCE))
            mask |= MIP_STIP;
        s->mip = (s->mip & ~mask) | (val & mask);
        break;
    case 0x313: /* midelegh */
    case 0x314: /* mieh */
    case 0x318: /* mvienh */
    case 0x319: /* mviph */
    case 0x354: /* miph */
        if (s->intr_arch == RISCV_INTR_BASE || s->cur_xlen != 32)
            return -1;
        break;
    case 0x350: /* miselect */
        if (s->intr_arch == RISCV_INTR_BASE)
            return -1;
        /* WARL: the range the implemented registers need */
        s->miselect = get_bits(val, 0,
            s->intr_arch == RISCV_INTR_AIA_IMSIC ? 8 : 6);
        break;
    case 0x351: /* mireg */
        if (s->intr_arch == RISCV_INTR_BASE)
            return -1;
        return ireg_write(s, IMSIC_FILE_M, s->miselect, val);
    case 0x35c: /* mtopei */
        if (s->intr_arch != RISCV_INTR_AIA_IMSIC)
            return -1;
        imsic_claim(s, IMSIC_FILE_M);
        break;

    case 0x300:
        set_mstatus(s, val);
        break;
    case 0x301: /* misa */
#if MAX_XLEN >= 64
        {
            int new_mxl;
            new_mxl = get_bits(val, s->cur_xlen - 2, 2);
            if (new_mxl >= 1 && new_mxl <= get_base_from_xlen(MAX_XLEN)) {
                /* Note: misa is only modified in M level, so cur_xlen
                   = 2^(mxl + 4) */
                if (s->mxl != new_mxl) {
                    s->mxl = new_mxl;
                    s->cur_xlen = 1 << (new_mxl + 4);
                    return 1;
                }
            }
        }
#endif
        break;
    case 0x302:
        mask = MEDELEG_MASK;
        s->medeleg = (s->medeleg & ~mask) | (val & mask);
        break;
    case 0x303:
        mask = MIP_SSIP | MIP_STIP | MIP_SEIP;
        s->mideleg = (s->mideleg & ~mask) | (val & mask);
        break;
    case 0x304:
        mask = MIP_MSIP | MIP_MTIP | MIP_MEIP |
            MIP_SSIP | MIP_STIP | MIP_SEIP;
        s->mie = (s->mie & ~mask) | (val & mask);
        break;
    case 0x305:
        s->mtvec = set_bits(val, 0, 2, 0);
        break;
    case 0x306:
        s->mcounteren = val & COUNTEREN_MASK;
        break;
    case 0x30a: /* menvcfg */
    case 0x31a: /* menvcfgh */
        if (csr == 0x31a && s->cur_xlen != 32)
            return -1;
        s->menvcfg = csr64_written(s, s->menvcfg, val, csr == 0x31a) &
            MENVCFG_MASK;
        update_stimer(s);
        /* ADUE changes how the A and D bits of every PTE are interpreted */
        tlb_flush_all(s);
        return 2;
    case 0x340:
        s->mscratch = val;
        break;
    case 0x341:
        s->mepc = set_bit(val, 0, false);
        break;
    case 0x342:
        s->mcause = val;
        break;
    case 0x343:
        s->mtval = val;
        break;
    case 0x344:
        /* with Sstc enabled STIP belongs to the stimecmp comparator */
        mask = MIP_SSIP;
        if (!(s->menvcfg & MENVCFG_STCE))
            mask |= MIP_STIP;
        s->mip = (s->mip & ~mask) | (val & mask);
        break;
    case 0x310: /* mstatush */
        if (s->cur_xlen != 32)
            return -1;
        /* only MBE/SBE live here and this implementation is little endian */
        break;
    case 0x320: /* mcountinhibit */
        {
            /* read both counters under the old setting and put them back
               under the new one, so that freezing and thawing preserve them */
            uint64_t cycle = get_mcycle(s);
            uint64_t instret = get_minstret(s);
            s->mcountinhibit = val & MCOUNTINHIBIT_MASK;
            set_mcycle(s, cycle);
            set_minstret(s, instret);
        }
        break;
    case 0x323 ... 0x33f: /* mhpmevent3..31 */
        /* the event counters are hardwired to zero */
        break;
    case 0x3a0 ... 0x3a3: /* pmpcfg0..3 */
        /* not implemented */
        break;
    case 0x3b0 ... 0x3bf: /* pmpaddr0..15 */
        /* not implemented */
        break;
    case 0xb00: /* mcycle */
        set_mcycle(s, csr64_written(s, get_mcycle(s), val, false));
        break;
    case 0xb02: /* minstret */
        set_minstret(s, csr64_written(s, get_minstret(s), val, false));
        break;
    case 0xb03 ... 0xb1f: /* mhpmcounter3..31 */
        break;
    case 0xb80: /* mcycleh */
        if (s->cur_xlen != 32)
            return -1;
        set_mcycle(s, csr64_written(s, get_mcycle(s), val, true));
        break;
    case 0xb82: /* minstreth */
        if (s->cur_xlen != 32)
            return -1;
        set_minstret(s, csr64_written(s, get_minstret(s), val, true));
        break;
    case 0xb83 ... 0xb9f: /* mhpmcounter3..31h */
        if (s->cur_xlen != 32)
            return -1;
        break;
    default:
#ifdef DUMP_INVALID_CSR
        printf("csr_write: invalid CSR=0x%x\n", csr);
#endif
        return -1;
    }
    return 0;
}

static void set_priv(RISCVCPUState *s, int priv)
{
    if (s->priv != priv) {
        tlb_flush_all(s);
#if MAX_XLEN >= 64
        /* change the current xlen */
        {
            int mxl;
            if (priv == PRV_S)
                mxl = get_bits(s->mstatus, MSTATUS_SXL_SHIFT, 2);
            else if (priv == PRV_U)
                mxl = get_bits(s->mstatus, MSTATUS_UXL_SHIFT, 2);
            else
                mxl = s->mxl;
            s->cur_xlen = 1 << (4 + mxl);
        }
#endif
        s->priv = priv;
    }
}

static void raise_exception2(RISCVCPUState *s, uint32_t cause,
                             target_ulong tval)
{
    bool deleg;
    target_ulong causel;
    
#if defined(DUMP_EXCEPTIONS) || defined(DUMP_MMU_EXCEPTIONS) || defined(DUMP_INTERRUPTS)
    {
        int flag;
        flag = 0;
#ifdef DUMP_MMU_EXCEPTIONS
        if (cause == CAUSE_FAULT_FETCH ||
            cause == CAUSE_FAULT_LOAD ||
            cause == CAUSE_FAULT_STORE ||
            cause == CAUSE_FETCH_PAGE_FAULT ||
            cause == CAUSE_LOAD_PAGE_FAULT ||
            cause == CAUSE_STORE_PAGE_FAULT)
            flag = 1;
#endif
#ifdef DUMP_INTERRUPTS
        flag |= (cause & CAUSE_INTERRUPT) != 0;
#endif
#ifdef DUMP_EXCEPTIONS
        flag = 1;
        flag = (cause & CAUSE_INTERRUPT) == 0;
        if (cause == CAUSE_SUPERVISOR_ECALL || cause == CAUSE_ILLEGAL_INSTRUCTION)
            flag = 0;
#endif
        if (flag) {
            log_printf("raise_exception: cause=0x%08x tval=0x", cause);
#ifdef CONFIG_LOGFILE
            fprint_target_ulong(log_file, tval);
#else
            print_target_ulong(tval);
#endif
            log_printf("\n");
            dump_regs(s);
        }
    }
#endif

    if (s->priv <= PRV_S) {
        /* delegate the exception to the supervisor priviledge */
        if (cause & CAUSE_INTERRUPT)
            deleg = get_bit(s->mideleg, cause & (MAX_XLEN - 1));
        else
            deleg = get_bit(s->medeleg, cause);
    } else {
        deleg = 0;
    }

    causel = get_bits(cause, 0, 31);
    if (cause & CAUSE_INTERRUPT)
        causel |= bit_at<target_ulong>(s->cur_xlen - 1);
    
    if (deleg) {
        s->scause = causel;
        s->sepc = s->pc;
        s->stval = tval;
        s->mstatus = set_bit(s->mstatus, MSTATUS_SPIE_SHIFT,
                             get_bit(s->mstatus, MSTATUS_SIE_SHIFT));
        s->mstatus = set_bits(s->mstatus, MSTATUS_SPP_SHIFT, 1, s->priv);
        s->mstatus &= ~MSTATUS_SIE;
        set_priv(s, PRV_S);
        s->pc = s->stvec;
    } else {
        s->mcause = causel;
        s->mepc = s->pc;
        s->mtval = tval;
        s->mstatus = set_bit(s->mstatus, MSTATUS_MPIE_SHIFT,
                             get_bit(s->mstatus, MSTATUS_MIE_SHIFT));
        s->mstatus = set_bits(s->mstatus, MSTATUS_MPP_SHIFT, 2, s->priv);
        s->mstatus &= ~MSTATUS_MIE;
        set_priv(s, PRV_M);
        s->pc = s->mtvec;
    }
}

static void raise_exception(RISCVCPUState *s, uint32_t cause)
{
    raise_exception2(s, cause, 0);
}

/* returning to anything below M drops the privilege MPRV borrows */
static void clear_mprv_on_return(RISCVCPUState *s, int new_priv)
{
    if (new_priv != PRV_M && (s->mstatus & MSTATUS_MPRV)) {
        s->mstatus &= ~MSTATUS_MPRV;
        tlb_flush_all(s);
    }
}

static void handle_sret(RISCVCPUState *s)
{
    int spp;
    bool spie;
    spp = get_bit(s->mstatus, MSTATUS_SPP_SHIFT);
    /* set the IE state to previous IE state */
    spie = get_bit(s->mstatus, MSTATUS_SPIE_SHIFT);
    s->mstatus = set_bit(s->mstatus, MSTATUS_SIE_SHIFT, spie);
    /* set SPIE to 1 */
    s->mstatus |= MSTATUS_SPIE;
    /* set SPP to U */
    s->mstatus &= ~MSTATUS_SPP;
    clear_mprv_on_return(s, spp);
    set_priv(s, spp);
    s->pc = s->sepc;
}

static void handle_mret(RISCVCPUState *s)
{
    int mpp;
    bool mpie;
    mpp = get_bits(s->mstatus, MSTATUS_MPP_SHIFT, 2);
    /* set the IE state to previous IE state */
    mpie = get_bit(s->mstatus, MSTATUS_MPIE_SHIFT);
    s->mstatus = set_bit(s->mstatus, MSTATUS_MIE_SHIFT, mpie);
    /* set MPIE to 1 */
    s->mstatus |= MSTATUS_MPIE;
    /* set MPP to U */
    s->mstatus &= ~MSTATUS_MPP;
    clear_mprv_on_return(s, mpp);
    set_priv(s, mpp);
    s->pc = s->mepc;
}

static inline uint32_t get_pending_irq_mask(RISCVCPUState *s)
{
    uint32_t pending_ints, enabled_ints;

    pending_ints = s->mip & s->mie;
    if (pending_ints == 0)
        return 0;

    enabled_ints = 0;
    switch(s->priv) {
    case PRV_M:
        if (s->mstatus & MSTATUS_MIE)
            enabled_ints = ~s->mideleg;
        break;
    case PRV_S:
        enabled_ints = ~s->mideleg;
        if (s->mstatus & MSTATUS_SIE)
            enabled_ints |= s->mideleg;
        break;
    default:
    case PRV_U:
        enabled_ints = -1;
        break;
    }
    return pending_ints & enabled_ints;
}

static __exception int raise_interrupt(RISCVCPUState *s)
{
    uint32_t mask;
    size_t i;
    int irq_num;

    mask = get_pending_irq_mask(s);
    if (mask == 0)
        return 0;
    irq_num = ctz32(mask);
    for(i = 0; i < countof(irq_priority); i++) {
        if (get_bit(mask, irq_priority[i])) {
            irq_num = irq_priority[i];
            break;
        }
    }
    raise_exception(s, irq_num | CAUSE_INTERRUPT);
    return -1;
}

/* The bits of 'val' from 'src_pos' up, moved to 'dst_pos' through
   'dst_pos_max'. */
static inline uint32_t get_field1(uint32_t val, int src_pos,
                                  int dst_pos, int dst_pos_max)
{
    assert(dst_pos_max >= dst_pos);
    return get_bits(val, src_pos, dst_pos_max - dst_pos + 1) << dst_pos;
}

#define XLEN 32
#include "riscv_cpu_template.h"

#if MAX_XLEN >= 64
#define XLEN 64
#include "riscv_cpu_template.h"
#endif

#if MAX_XLEN >= 128
#define XLEN 128
#include "riscv_cpu_template.h"
#endif

static void glue(riscv_cpu_interp, MAX_XLEN)(RISCVCPUState *s, int n_cycles)
{
    uint64_t timeout;

    timeout = s->insn_counter + n_cycles;
    for (;;) {
        /* the interpreter returns early when something is posted */
        process_inbox(s);
        if (s->power_down_flag || (int)(timeout - s->insn_counter) <= 0)
            break;
        n_cycles = timeout - s->insn_counter;
        switch(s->cur_xlen) {
        case 32:
            riscv_cpu_interp_x32(s, n_cycles);
            break;
#if MAX_XLEN >= 64
        case 64:
            riscv_cpu_interp_x64(s, n_cycles);
            break;
#endif
#if MAX_XLEN >= 128
        case 128:
            riscv_cpu_interp_x128(s, n_cycles);
            break;
#endif
        default:
            abort();
        }
    }
    /* Another hart may run before this one resumes and write to the reserved
       address, which nothing here would notice. A spurious SC failure is
       allowed, a spurious success is not. */
    s->load_res = (target_ulong)-1;
}

/* Note: the value is not accurate when called in riscv_cpu_interp() */
static uint64_t glue(riscv_cpu_get_cycles, MAX_XLEN)(RISCVCPUState *s)
{
    return s->insn_counter;
}

static bool glue(riscv_cpu_get_power_down, MAX_XLEN)(RISCVCPUState *s)
{
    process_inbox(s);
    return s->power_down_flag;
}

static RISCVCPUState *glue(riscv_cpu_init, MAX_XLEN)(PhysMemoryMap *mem_map,
                                                     uint32_t hart_id)
{
    RISCVCPUState *s;

    s = new RISCVCPUState();
    s->mem_map = mem_map;
    s->mhartid = hart_id;
    s->load_res = (target_ulong)-1;
    s->pc = 0x1000;
    s->priv = PRV_M;
    s->cur_xlen = MAX_XLEN;
    s->mxl = get_base_from_xlen(MAX_XLEN);
    s->mstatus = ((uint64_t)s->mxl << MSTATUS_UXL_SHIFT) |
        ((uint64_t)s->mxl << MSTATUS_SXL_SHIFT);
    /* the comparator starts beyond any reachable time so that enabling Sstc
       does not immediately post a timer interrupt */
    s->stimecmp = UINT64_MAX;
    s->irq_line_mask = MIP_MSIP | MIP_MTIP | MIP_MEIP | MIP_SEIP;
    s->misa |= MCPUID_SUPER | MCPUID_USER | MCPUID_I | MCPUID_M | MCPUID_A;
#if FLEN >= 32
    s->misa |= MCPUID_F;
#endif
#if FLEN >= 64
    s->misa |= MCPUID_D;
#endif
#if FLEN >= 128
    s->misa |= MCPUID_Q;
#endif
#ifdef CONFIG_EXT_C
    s->misa |= MCPUID_C;
#endif
    tlb_init(s);
    return s;
}

static uint32_t glue(riscv_cpu_get_misa, MAX_XLEN)(RISCVCPUState *s)
{
    return s->misa;
}

/* The interpreter and its helpers stay plain functions over RISCVCPUState;
   these overrides are the only bridge to the abstract interface. */
void RISCVCPUState::Interp(int n_cycles)
{
    glue(riscv_cpu_interp, MAX_XLEN)(this, n_cycles);
}

uint64_t RISCVCPUState::Cycles()
{
    return glue(riscv_cpu_get_cycles, MAX_XLEN)(this);
}

void RISCVCPUState::SetIrqLine(uint32_t mask, bool level)
{
    if (level)
        irq_lines.fetch_or(mask);
    else
        irq_lines.fetch_and(~mask);
    inbox_pending.store(true);
}

uint64_t RISCVCPUState::UpdateSTimer()
{
    return update_stimer(this);
}

bool RISCVCPUState::PowerDown()
{
    return glue(riscv_cpu_get_power_down, MAX_XLEN)(this);
}

uint32_t RISCVCPUState::Misa()
{
    return glue(riscv_cpu_get_misa, MAX_XLEN)(this);
}

void RISCVCPUState::SetRtcTimeSource(RtcTimeSource *source)
{
    rtc_time_source = source;
}

void RISCVCPUState::FlushTlbWriteRangeRam(uint8_t *ram_ptr, size_t ram_size)
{
    glue(riscv_cpu_flush_tlb_write_range_ram, MAX_XLEN)(this, ram_ptr, ram_size);
}

void RISCVCPUState::SetDeviceLock(DeviceLock *lock)
{
    device_lock = lock;
}

void RISCVCPUState::SetInterruptArch(RISCVInterruptArch arch)
{
    intr_arch = arch;
    /* interrupt files drive the external interrupts by themselves */
    irq_line_mask = MIP_MSIP | MIP_MTIP;
    if (arch != RISCV_INTR_AIA_IMSIC)
        irq_line_mask |= MIP_MEIP | MIP_SEIP;
}

void RISCVCPUState::ImsicSetPending(bool supervisor, uint32_t id)
{
    int file = supervisor ? IMSIC_FILE_S : IMSIC_FILE_M;

    if (intr_arch != RISCV_INTR_AIA_IMSIC || id == 0 ||
        id > RISCV_IMSIC_NUM_IDS)
        return;
    msi_pending[file][id / 64].fetch_or((uint64_t)1 << (id % 64));
    inbox_pending.store(true);
}

} // anonymous namespace


RISCVCPU *glue(riscv_cpu_create, MAX_XLEN)(PhysMemoryMap *mem_map,
                                           uint32_t hart_id)
{
    return glue(riscv_cpu_init, MAX_XLEN)(mem_map, hart_id);
}

#if CONFIG_RISCV_MAX_XLEN == MAX_XLEN
RISCVCPU *riscv_cpu_create(PhysMemoryMap *mem_map, int max_xlen,
                           uint32_t hart_id)
{
    switch (max_xlen) {
        case 32:
            return riscv_cpu_create32(mem_map, hart_id);
        case 64:
            return riscv_cpu_create64(mem_map, hart_id);
#if CONFIG_RISCV_MAX_XLEN == 128
        case 128:
            return riscv_cpu_create128(mem_map, hart_id);
#endif
        default:
            return nullptr;
    }
}
#endif /* CONFIG_RISCV_MAX_XLEN == MAX_XLEN */

