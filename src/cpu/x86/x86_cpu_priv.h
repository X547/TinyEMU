/*
 * x86 CPU emulator: internal definitions
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
#pragma once

#include <setjmp.h>
#include <stdint.h>
#include <string.h>
#include <atomic>

#include "bits.h"
#include "cutils.h"
#include "iomem.h"
#include "x86_cpu.h"


//#pragma mark - operand sizes

/* Encoded as log2 of the byte count, like DeviceIO sizes. The general
   registers go up to SIZE64; SIZE128 is a vector operand. */
enum {
    SIZE8,
    SIZE16,
    SIZE32,
    SIZE64,
    SIZE128,
};

static inline int size_bytes(int size)
{
    return 1 << size;
}

static inline int size_bits(int size)
{
    return 8 << size;
}

/* A table keeps the many truncations small enough to inline. */
static inline uint64_t size_mask(int size)
{
    static const uint64_t masks[4] = {
        UINT8_MAX, UINT16_MAX, UINT32_MAX, UINT64_MAX
    };
    return masks[size];
}

static inline uint64_t trunc_size(uint64_t val, int size)
{
    return val & size_mask(size);
}

static inline bool msb(uint64_t val, int size)
{
    return get_bit(val, size_bits(size) - 1);
}

static inline uint64_t sign_bit(int size)
{
    return bit_at<uint64_t>(size_bits(size) - 1);
}

static inline int64_t sext_size(uint64_t val, int size)
{
    return sign_extend(val, size_bits(size));
}


//#pragma mark - architectural constants

enum {
    REG_EAX, REG_ECX, REG_EDX, REG_EBX, REG_ESP, REG_EBP, REG_ESI, REG_EDI,
};

/* Same numbering as the X86_CPU_SEG_* constants and the ModRM encoding. */
enum {
    SEG_ES, SEG_CS, SEG_SS, SEG_DS, SEG_FS, SEG_GS, SEG_COUNT
};

enum {
    EFLAGS_CF = 0,
    EFLAGS_PF = 2,
    EFLAGS_AF = 4,
    EFLAGS_ZF = 6,
    EFLAGS_SF = 7,
    EFLAGS_TF = 8,
    EFLAGS_IF = 9,
    EFLAGS_DF = 10,
    EFLAGS_OF = 11,
    EFLAGS_IOPL = 12, /* 2 bits */
    EFLAGS_NT = 14,
    EFLAGS_RF = 16,
    EFLAGS_VM = 17,
    EFLAGS_AC = 18,
    EFLAGS_VIF = 19,
    EFLAGS_VIP = 20,
    EFLAGS_ID = 21,
};

static const uint32_t EFLAGS_CC_MASK = bit_at(EFLAGS_CF) | bit_at(EFLAGS_PF) |
    bit_at(EFLAGS_AF) | bit_at(EFLAGS_ZF) | bit_at(EFLAGS_SF) |
    bit_at(EFLAGS_OF);

/* Bits that exist on an i686; the rest read as zero, bit 1 as one. */
static const uint32_t EFLAGS_VALID_MASK = EFLAGS_CC_MASK | bit_at(EFLAGS_TF) |
    bit_at(EFLAGS_IF) | bit_at(EFLAGS_DF) | field_mask(EFLAGS_IOPL, 2) |
    bit_at(EFLAGS_NT) | bit_at(EFLAGS_RF) | bit_at(EFLAGS_VM) |
    bit_at(EFLAGS_AC) | bit_at(EFLAGS_VIF) | bit_at(EFLAGS_VIP) |
    bit_at(EFLAGS_ID);

static inline int eflags_iopl(uint32_t eflags)
{
    return get_bits(eflags, EFLAGS_IOPL, 2);
}

enum {
    CR0_PE = 0,
    CR0_MP = 1,
    CR0_EM = 2,
    CR0_TS = 3,
    CR0_ET = 4,
    CR0_NE = 5,
    CR0_WP = 16,
    CR0_AM = 18,
    CR0_NW = 29,
    CR0_CD = 30,
    CR0_PG = 31,
};

enum {
    CR4_VME = 0,
    CR4_PVI = 1,
    CR4_TSD = 2,
    CR4_DE = 3,
    CR4_PSE = 4,
    CR4_PAE = 5,
    CR4_MCE = 6,   /* machine checks are never raised */
    CR4_PGE = 7,
    CR4_PCE = 8,
    CR4_OSFXSR = 9,
    CR4_OSXMMEXCPT = 10,
};

enum {
    EFER_SCE = 0,   /* SYSCALL and SYSRET */
    EFER_LME = 8,
    EFER_LMA = 10,  /* IA-32e mode active: long mode enabled and paging on */
    EFER_NXE = 11,
};

enum {
    PTE_P = 0,
    PTE_RW = 1,
    PTE_US = 2,
    PTE_A = 5,
    PTE_D = 6,
    PTE_PS = 7,
    PTE_XD = 63,  /* PAE entries only */
};

enum {
    EXCP_DE = 0,
    EXCP_DB = 1,
    EXCP_NMI = 2,
    EXCP_BP = 3,
    EXCP_OF = 4,
    EXCP_BR = 5,
    EXCP_UD = 6,
    EXCP_NM = 7,
    EXCP_DF = 8,
    EXCP_TS = 10,
    EXCP_NP = 11,
    EXCP_SS = 12,
    EXCP_GP = 13,
    EXCP_PF = 14,
    EXCP_MF = 16,
    EXCP_AC = 17,
    EXCP_XM = 19,
};

/* Segment attribute word: byte 5 of a descriptor and the flag nibble of
   byte 6, as X86CPUSeg::flags holds it. */
enum {
    DESC_A = 0,     /* accessed; busy (bit 1) for a TSS */
    DESC_RW = 1,    /* readable code / writable data */
    DESC_CE = 2,    /* conforming code / expand-down data */
    DESC_CODE = 3,
    DESC_S = 4,     /* code or data rather than system */
    DESC_DPL = 5,   /* 2 bits */
    DESC_P = 7,
    DESC_L = 13,    /* 64 bit code */
    DESC_DB = 14,
    DESC_G = 15,
};

/* In IA-32e mode types 9, 11, 12, 14 and 15 are the 64 bit TSS and gates,
   16 bytes long like an LDT descriptor, and the other system types are
   invalid. */
enum {
    SYS_TSS16 = 1,
    SYS_LDT = 2,
    SYS_TSS16_BUSY = 3,
    SYS_CALL_GATE16 = 4,
    SYS_TASK_GATE = 5,
    SYS_INT_GATE16 = 6,
    SYS_TRAP_GATE16 = 7,
    SYS_TSS32 = 9,
    SYS_TSS32_BUSY = 11,
    SYS_CALL_GATE32 = 12,
    SYS_INT_GATE32 = 14,
    SYS_TRAP_GATE32 = 15,
};

static inline int desc_type(uint32_t flags)
{
    return get_bits(flags, 0, 4);
}

static inline int desc_dpl(uint32_t flags)
{
    return get_bits(flags, DESC_DPL, 2);
}

static inline bool desc_is_code(uint32_t flags)
{
    return get_bit(flags, DESC_S) && get_bit(flags, DESC_CODE);
}

static inline bool desc_is_data(uint32_t flags)
{
    return get_bit(flags, DESC_S) && !get_bit(flags, DESC_CODE);
}

static inline int sel_rpl(uint32_t sel)
{
    return get_bits(sel, 0, 2);
}

static inline bool sel_is_null(uint32_t sel)
{
    return get_bits(sel, 2, 14) == 0;
}


//#pragma mark - state

enum {
    MMU_SUPERVISOR,
    MMU_USER,
    MMU_COUNT
};

enum {
    ACCESS_READ,
    ACCESS_WRITE,
    ACCESS_CODE,
};

#define PAGE_BITS 12
#define PAGE_SIZE (1 << PAGE_BITS)
#define TLB_BITS 10
#define TLB_SIZE (1 << TLB_BITS)
/* Page tags have their offset bits clear, so this matches no address. */
#define TLB_INVALID UINT64_MAX

static inline uint64_t page_base(uint64_t addr)
{
    return set_bits(addr, 0, PAGE_BITS, 0);
}

static inline uint32_t page_offset(uint64_t addr)
{
    return get_bits(addr, 0, PAGE_BITS);
}

/* Bits 48-63 repeat bit 47. */
static inline bool is_canonical(uint64_t addr)
{
    return (uint64_t)sign_extend(addr, 48) == addr;
}

/* One page of the linear address space. The three tags say for which access
   kinds the host pointer may be used directly. Tags are addresses within
   the linear address width (see lin_mask), and canonical, so an address
   past its end misses and is wrapped or refused on the slow path. */
struct X86TLBEntry {
    uint64_t read;
    uint64_t write;
    uint64_t code;
    uintptr_t addend; /* host pointer minus linear address */
};

/* Lazy condition codes: the flags are computed from the last operation. */
enum {
    CC_OP_EFLAGS, /* the flags are in eflags */
    CC_OP_ADD,
    CC_OP_ADC,
    CC_OP_SUB,
    CC_OP_SBB,
    CC_OP_LOGIC,
    CC_OP_INC,    /* src holds the preserved CF */
    CC_OP_DEC,
    CC_OP_SHL,    /* src holds the value shifted by count - 1 */
    CC_OP_SAR,
    CC_OP_MUL,    /* src is nonzero on overflow */
};

/* An x87 register as the part holds it. An MMX register is the
   significand. */
struct Fx80 {
    uint64_t mant;
    uint16_t sexp;       /* sign and 15 bit exponent */
};

struct X87State {
    Fx80 st[8];          /* indexed by physical register */
    uint16_t control;
    uint16_t status;     /* TOP is bits 11-13 */
    uint8_t empty;       /* one bit per physical register */
    uint16_t opcode;
    uint16_t fcs;
    uint16_t fds;
    uint64_t fip;
    uint64_t fdp;
};

/* An MMX (8 bytes) or XMM (16 bytes) register. Lanes are host integers in
   the guest's little endian order, lane 0 lowest; see lane(). */
template <int N>
struct VecReg {
    alignas(N) uint8_t bytes[N];
};

typedef VecReg<8> MmxReg;
typedef VecReg<16> XmmReg;

/* 16 in 64 bit mode; the others see the first 8. */
#define XMM_COUNT 16

enum {
    MXCSR_IE = 0,  /* exception flags, bits 0-5 */
    MXCSR_DE = 1,
    MXCSR_ZE = 2,
    MXCSR_OE = 3,
    MXCSR_UE = 4,
    MXCSR_PE = 5,
    MXCSR_DAZ = 6,
    MXCSR_IM = 7,  /* exception masks, bits 7-12, in the order of the flags */
    MXCSR_OM = 10,
    MXCSR_UM = 11,
    MXCSR_RC = 13, /* 2 bits */
    MXCSR_FZ = 15,
};

static const uint32_t MXCSR_RESET = 0x1f80;
/* The bits MXCSR implements, as FXSAVE reports them. */
static const uint32_t MXCSR_MASK = 0xffff;

/* The general registers of 64 bit mode. Outside it only the first 8 exist,
   and their upper halves stay zero: 32 bit writes zero extend. */
#define GPR_COUNT 16

struct X86CPUState {
    uint64_t regs[GPR_COUNT];
    uint64_t rip;
    uint32_t eflags;

    uint8_t cc_op;
    uint8_t cc_size;
    bool cc_carry;       /* carry in of ADC and SBB */
    uint64_t cc_src;
    uint64_t cc_dst;     /* the result */

    X86CPUSeg segs[SEG_COUNT];
    uint8_t seg_fast[SEG_COUNT]; /* see seg_fast_access() */
    X86CPUSeg ldt;
    X86CPUSeg tr;
    X86CPUSeg gdt;
    X86CPUSeg idt;

    uint32_t cr0;
    uint64_t cr2;
    uint64_t cr3;
    uint32_t cr4;
    uint8_t cr8;         /* the task priority, which nothing reads */
    uint64_t efer;
    /* the page directory pointers PAE paging loads along with CR3 */
    uint64_t pdpte[4];
    uint64_t dr[8];
    uint32_t sysenter_cs;
    uint64_t sysenter_esp;
    uint64_t sysenter_eip;
    uint64_t star;       /* SYSCALL and SYSRET selectors */
    uint64_t lstar;      /* SYSCALL entry from 64 bit mode */
    uint64_t cstar;      /* from compatibility mode, never used */
    uint32_t sfmask;     /* RFLAGS bits SYSCALL clears */
    uint64_t kernel_gs_base; /* SWAPGS exchanges it with the GS base */
    uint64_t tsc_offset;
    /* P6 performance counters; they hold what was written and never count */
    uint32_t pmc_evtsel[2];
    uint64_t pmc_ctr[2];
    uint32_t misc_enable;
    uint64_t pat;

    /* derived state, see cpu_update_mode() */
    uint8_t cpl;
    uint8_t mmu_idx;
    bool code32;
    bool code64;         /* 64 bit mode: IA-32e and a 64 bit CS */
    uint8_t code_opsize; /* the default operand and address sizes */
    uint8_t code_addr_size;
    uint64_t eip_mask;
    uint64_t sp_mask;    /* the part of RSP the stack uses */
    /* The linear address width: 32 bits, or 64 in IA-32e mode, where
       addresses must be canonical instead. */
    uint64_t lin_mask;
    uint8_t rex;         /* the REX prefix of the instruction, 0 if none */

    X87State fpu;
    XmmReg xmm[XMM_COUNT];
    uint32_t mxcsr;

    std::atomic<bool> irq_level; /* set from any thread */
    bool irq_inhibit;    /* for one instruction after STI or a load of SS */
    bool power_down;
    /* Other processors share the memory: LOCK and XCHG are atomic, and
       MFENCE and the accessed and dirty bits order and update memory as
       they do on a multiprocessor. */
    bool smp;
    /* The RAM operand of the locked read-modify-write in progress, null if
       the operand is not in RAM, and the value its write compares with. */
    uint8_t *lock_ptr;
    uint64_t lock_old;
    /* the flags as the locked instruction found them, for a retry */
    uint8_t lock_cc_op;
    uint8_t lock_cc_size;
    bool lock_cc_carry;
    uint64_t lock_cc_src;
    uint64_t lock_cc_dst;
    uint32_t lock_eflags;
    int old_exception;   /* the exception being delivered, or -1 */
    int64_t cycles;
    int64_t cycles_end;
    jmp_buf jmp_env;

    X86TLBEntry tlb[MMU_COUNT][TLB_SIZE];
    bool tlb_large_pages;
    uint64_t code_tag;   /* the page the fetch shortcut points into */
    uintptr_t code_addend;

    PhysMemoryMap *mem_map;
    DeviceLock *device_lock; /* taken around every device access */
    DeviceIO *port_io;
    X86HardIntnoSource *hard_intno_source;
    X86LocalApicTarget *local_apic; /* null without one */
    X86TscSource *tsc_source;
};


/* The 64 bit value RDTSC, RDMSR, WRMSR and RDPMC pass in EDX:EAX. */
static inline uint64_t get_edx_eax(X86CPUState *s)
{
    return concat_bits(get_bits(s->regs[REG_EDX], 0, 32), s->regs[REG_EAX],
                       32);
}

static inline void set_edx_eax(X86CPUState *s, uint64_t val)
{
    s->regs[REG_EAX] = get_bits(val, 0, 32);
    s->regs[REG_EDX] = get_bits(val, 32, 32);
}


//#pragma mark - decoded instructions

/* The r/m operand of a ModRM byte. */
struct Operand {
    bool is_reg;
    bool locked;         /* memory under LOCK, or of XCHG */
    uint8_t reg;
    uint8_t seg;
    uint64_t ea;
};

/* The mandatory prefix of a vector instruction. */
enum {
    SIMD_NONE,
    SIMD_66,
    SIMD_F3,
    SIMD_F2,
};

/* A 0F xx vector instruction, decoded up to its immediate byte. */
struct SimdInsn {
    uint8_t opcode;      /* the byte after 0F */
    uint8_t prefix;
    uint8_t reg;         /* ModRM.reg, with REX.R; rm.reg has REX.B */
    uint8_t imm;
    bool rex_w;          /* a general register operand is 64 bits */
    Operand rm;
    /* where MASKMOVQ and MASKMOVDQU store */
    uint8_t data_seg;
    uint64_t addr_mask;
};


//#pragma mark - cross-file functions

/* x86_cpu.cpp */
[[noreturn]] void raise_exception(X86CPUState *s, int intno,
                                  int error_code = 0);
uint64_t mem_read_slow(X86CPUState *s, uint64_t lin, int size, int mmu_idx);
void mem_write_slow(X86CPUState *s, uint64_t lin, uint64_t val, int size,
                    int mmu_idx);
void mem_probe_write(X86CPUState *s, uint64_t lin, int size);
uint64_t mem_read_locked(X86CPUState *s, uint64_t lin, int size);
void mem_write_locked(X86CPUState *s, uint64_t lin, uint64_t val, int size);
bool mem_cmpxchg_wide(X86CPUState *s, uint64_t lin, int half, uint64_t *low,
                      uint64_t *high, uint64_t new_low, uint64_t new_high);
uint8_t fetch_slow(X86CPUState *s, uint64_t lin);
void tlb_flush_all(X86CPUState *s);
void tlb_flush_page(X86CPUState *s, uint64_t lin);
void cpu_update_mode(X86CPUState *s);
void cpu_set_eflags(X86CPUState *s, uint32_t val, uint32_t mask);
void cpu_set_cr0(X86CPUState *s, uint32_t val);
void cpu_set_cr3(X86CPUState *s, uint64_t val);
void cpu_set_cr4(X86CPUState *s, uint32_t val);
void cpu_cpuid(X86CPUState *s);
uint64_t cpu_get_cr8(X86CPUState *s);
void cpu_set_cr8(X86CPUState *s, uint64_t val);
void cpu_rdmsr(X86CPUState *s);
void cpu_wrmsr(X86CPUState *s);
void cpu_rdpmc(X86CPUState *s);
uint64_t cpu_get_tsc(X86CPUState *s);

/* x86_seg.cpp */
uint64_t seg_address_slow(X86CPUState *s, const X86CPUSeg *seg, int excp,
                          uint64_t ea, int size, bool write);
void load_seg_cache(X86CPUState *s, int seg, uint32_t sel, uint64_t base,
                    uint32_t limit, uint32_t flags);
void load_seg(X86CPUState *s, int seg, uint32_t sel);
void do_interrupt(X86CPUState *s, int intno, bool is_soft, int error_code,
                  uint64_t ret_eip, bool is_hw);
void far_jump(X86CPUState *s, uint32_t sel, uint64_t offset,
              uint64_t next_eip);
void far_call(X86CPUState *s, uint32_t sel, uint64_t offset, int opsize,
              uint64_t next_eip);
void far_return(X86CPUState *s, int opsize, uint32_t addend);
void interrupt_return(X86CPUState *s, int opsize, uint64_t next_eip);
void check_io_permission(X86CPUState *s, uint32_t port, int size);
void load_ldt(X86CPUState *s, uint32_t sel);
void load_tr(X86CPUState *s, uint32_t sel);
bool seg_access_rights(X86CPUState *s, uint32_t sel, uint32_t *val);
bool seg_limit(X86CPUState *s, uint32_t sel, uint32_t *val);
bool seg_verify(X86CPUState *s, uint32_t sel, bool write);
void cpu_sysenter(X86CPUState *s);
void cpu_sysexit(X86CPUState *s, int opsize);
void cpu_syscall(X86CPUState *s, uint64_t next_rip);
void cpu_sysret(X86CPUState *s, int opsize);

/* x86_fpu.cpp */
void fpu_reset(X86CPUState *s);
void fpu_exec(X86CPUState *s, uint8_t opcode, uint8_t modrm, uint64_t lin,
              uint64_t ea, int ea_seg, int opsize);
void fpu_check_pending(X86CPUState *s);
void fpu_fxsave(X86CPUState *s, uint64_t lin, bool wide);
void fpu_fxrstor(X86CPUState *s, uint64_t lin, bool wide);

/* x86_interp.cpp */
void x86_exec(X86CPUState *s);

/* x86_simd.cpp */
void simd_reset(X86CPUState *s);
bool simd_has_imm8(uint8_t opcode);
void simd_exec(X86CPUState *s, const SimdInsn &insn);


//#pragma mark - memory access

static inline uint64_t host_load(const uint8_t *ptr, int size)
{
    switch (size) {
    case SIZE8:
        return *ptr;
    case SIZE16: {
        uint16_t val;
        memcpy(&val, ptr, sizeof(val));
        return val;
    }
    case SIZE32: {
        uint32_t val;
        memcpy(&val, ptr, sizeof(val));
        return val;
    }
    default: {
        uint64_t val;
        memcpy(&val, ptr, sizeof(val));
        return val;
    }
    }
}

static inline void host_store(uint8_t *ptr, uint64_t val, int size)
{
    switch (size) {
    case SIZE8:
        *ptr = val;
        break;
    case SIZE16: {
        uint16_t v = val;
        memcpy(ptr, &v, sizeof(v));
        break;
    }
    case SIZE32: {
        uint32_t v = val;
        memcpy(ptr, &v, sizeof(v));
        break;
    }
    default:
        memcpy(ptr, &val, sizeof(val));
        break;
    }
}

/* An x86 host keeps the order of loads and stores the guest expects. Other
   hosts are told to, once another processor could see the difference:
   loads are acquires and stores releases, which is x86 ordering. */
#if defined(__x86_64__) || defined(__i386__)
#define HOST_ORDERS_LIKE_X86 1
#else
#define HOST_ORDERS_LIKE_X86 0
#endif

static inline uint64_t guest_load(X86CPUState *s, const uint8_t *ptr, int size)
{
    uint64_t val = host_load(ptr, size);
    if (!HOST_ORDERS_LIKE_X86 && s->smp) {
        std::atomic_thread_fence(std::memory_order_acquire);
    }
    return val;
}

static inline void guest_store(X86CPUState *s, uint8_t *ptr, uint64_t val,
                               int size)
{
    if (!HOST_ORDERS_LIKE_X86 && s->smp) {
        std::atomic_thread_fence(std::memory_order_release);
    }
    host_store(ptr, val, size);
}

static inline X86TLBEntry *tlb_entry(X86CPUState *s, int mmu_idx, uint64_t lin)
{
    return &s->tlb[mmu_idx][get_bits(lin, PAGE_BITS, TLB_BITS)];
}

/* True if an access of 'size' at 'lin' stays in the page the tag names. */
static inline bool tlb_hit(uint64_t tag, uint64_t lin, int size)
{
    return tag == page_base(lin) &&
        page_offset(lin) <= (uint32_t)(PAGE_SIZE - size_bytes(size));
}

/* General register sized accesses, up to SIZE64. */
static inline uint64_t mem_read_mmu(X86CPUState *s, uint64_t lin, int size,
                                    int mmu_idx)
{
    X86TLBEntry *e = tlb_entry(s, mmu_idx, lin);
    if (likely(tlb_hit(e->read, lin, size))) {
        return guest_load(s, (uint8_t *)(e->addend + lin), size);
    }
    return mem_read_slow(s, lin, size, mmu_idx);
}

static inline void mem_write_mmu(X86CPUState *s, uint64_t lin, uint64_t val,
                                 int size, int mmu_idx)
{
    X86TLBEntry *e = tlb_entry(s, mmu_idx, lin);
    if (likely(tlb_hit(e->write, lin, size))) {
        guest_store(s, (uint8_t *)(e->addend + lin), val, size);
        return;
    }
    mem_write_slow(s, lin, val, size, mmu_idx);
}

static inline uint64_t mem_read(X86CPUState *s, uint64_t lin, int size)
{
    return mem_read_mmu(s, lin, size, s->mmu_idx);
}

static inline void mem_write(X86CPUState *s, uint64_t lin, uint64_t val,
                             int size)
{
    mem_write_mmu(s, lin, val, size, s->mmu_idx);
}

/* Descriptor tables and the TSS are accessed with supervisor rights. */
static inline uint64_t sys_read(X86CPUState *s, uint64_t lin, int size)
{
    return mem_read_mmu(s, lin, size, MMU_SUPERVISOR);
}

static inline void sys_write(X86CPUState *s, uint64_t lin, uint64_t val,
                             int size)
{
    mem_write_mmu(s, lin, val, size, MMU_SUPERVISOR);
}

/* Operands of any size, SIZE128 included, as byte images. Wider ones are
   split into 32 bit accesses off the fast path. */
static inline void mem_read_bytes(X86CPUState *s, uint64_t lin, void *buf,
                                  int size)
{
    uint8_t *p = (uint8_t *)buf;
    X86TLBEntry *e = tlb_entry(s, s->mmu_idx, lin);
    if (likely(tlb_hit(e->read, lin, size))) {
        memcpy(p, (uint8_t *)(e->addend + lin), size_bytes(size));
        if (!HOST_ORDERS_LIKE_X86 && s->smp) {
            std::atomic_thread_fence(std::memory_order_acquire);
        }
        return;
    }
    int chunk = size < SIZE32 ? size : SIZE32;
    for (int i = 0; i < size_bytes(size); i += size_bytes(chunk)) {
        uint32_t val = mem_read(s, lin + i, chunk);
        memcpy(p + i, &val, size_bytes(chunk));
    }
}

/* Both pages are checked before either is written. */
static inline void mem_write_bytes(X86CPUState *s, uint64_t lin,
                                   const void *buf, int size)
{
    const uint8_t *p = (const uint8_t *)buf;
    X86TLBEntry *e = tlb_entry(s, s->mmu_idx, lin);
    if (likely(tlb_hit(e->write, lin, size))) {
        if (!HOST_ORDERS_LIKE_X86 && s->smp) {
            std::atomic_thread_fence(std::memory_order_release);
        }
        memcpy((uint8_t *)(e->addend + lin), p, size_bytes(size));
        return;
    }
    int chunk = size < SIZE32 ? size : SIZE32;
    if (size > SIZE32) {
        mem_probe_write(s, lin, size);
    }
    for (int i = 0; i < size_bytes(size); i += size_bytes(chunk)) {
        uint32_t val = 0;
        memcpy(&val, p + i, size_bytes(chunk));
        mem_write(s, lin + i, val, chunk);
    }
}


//#pragma mark - segments

enum {
    SEG_FAST_READ = 1,
    SEG_FAST_WRITE = 2,
};

/* The accesses a segment allows without checking the offset or the type:
   those of a 4 GB segment of the right kind. */
static inline uint8_t seg_fast_access(const X86CPUSeg *seg)
{
    uint32_t flags = seg->flags;
    if (seg->limit != UINT32_MAX || !get_bit(flags, DESC_S) ||
        !get_bit(flags, DESC_P)) {
        return 0;
    }
    if (get_bit(flags, DESC_CODE)) {
        return get_bit(flags, DESC_RW) ? SEG_FAST_READ : 0;
    }
    if (get_bit(flags, DESC_CE)) {
        return 0;
    }
    return SEG_FAST_READ | (get_bit(flags, DESC_RW) ? SEG_FAST_WRITE : 0);
}

/* 64 bit mode checks neither limits nor types. */
static inline void seg_update_fast(X86CPUState *s, int seg)
{
    s->seg_fast[seg] = s->code64 ? SEG_FAST_READ | SEG_FAST_WRITE :
        seg_fast_access(&s->segs[seg]);
}

/* The linear address of 'size' bytes at 'ea' in a segment register, after
   the limit and type checks. It may run past the linear address width,
   which the memory accesses wrap. */
static inline uint64_t seg_address(X86CPUState *s, int seg, uint64_t ea,
                                   int size, bool write)
{
    if (likely(get_bit(s->seg_fast[seg], write))) {
        return s->segs[seg].base + ea;
    }
    return seg_address_slow(s, &s->segs[seg],
                            seg == SEG_SS ? EXCP_SS : EXCP_GP, ea, size,
                            write);
}


//#pragma mark - stack

/* A stack pointer being moved before it is committed, so that a fault
   part way through leaves ESP untouched. */
struct StackPtr {
    const X86CPUSeg *ss;
    uint64_t mask;
    uint64_t sp;
    int mmu_idx;
    uint8_t fast;
};

static inline StackPtr make_stack(const X86CPUSeg *ss, uint64_t sp, int cpl)
{
    StackPtr st;
    st.ss = ss;
    st.mask = get_bit(ss->flags, DESC_DB) ? UINT32_MAX : 0xffff;
    st.sp = sp;
    st.mmu_idx = cpl == 3 ? MMU_USER : MMU_SUPERVISOR;
    st.fast = seg_fast_access(ss);
    return st;
}

static inline StackPtr current_stack(X86CPUState *s)
{
    StackPtr st;
    st.ss = &s->segs[SEG_SS];
    st.mask = s->sp_mask;
    st.sp = s->regs[REG_ESP];
    st.mmu_idx = s->mmu_idx;
    st.fast = s->seg_fast[SEG_SS];
    return st;
}

static inline uint64_t stack_address(X86CPUState *s, const StackPtr *st,
                                     int size, bool write)
{
    uint64_t offset = st->sp & st->mask;
    if (likely(get_bit(st->fast, write))) {
        return st->ss->base + offset;
    }
    return seg_address_slow(s, st->ss, EXCP_SS, offset, size, write);
}

static inline void stack_push(X86CPUState *s, StackPtr *st, uint64_t val,
                              int size)
{
    st->sp -= size_bytes(size);
    mem_write_mmu(s, stack_address(s, st, size, true), val, size,
                  st->mmu_idx);
}

static inline uint64_t stack_pop(X86CPUState *s, StackPtr *st, int size)
{
    uint64_t val = mem_read_mmu(s, stack_address(s, st, size, false), size,
                                st->mmu_idx);
    st->sp += size_bytes(size);
    return val;
}

static inline void stack_commit(X86CPUState *s, const StackPtr &st)
{
    s->regs[REG_ESP] = (s->regs[REG_ESP] & ~st.mask) | (st.sp & st.mask);
}


//#pragma mark - condition codes

static inline void set_cc(X86CPUState *s, int op, int size, uint64_t src,
                          uint64_t dst)
{
    s->cc_op = op;
    s->cc_size = size;
    s->cc_src = trunc_size(src, size);
    s->cc_dst = trunc_size(dst, size);
}

static inline void set_cc_carry(X86CPUState *s, int op, int size, uint64_t src,
                                uint64_t dst, bool carry)
{
    set_cc(s, op, size, src, dst);
    s->cc_carry = carry;
}

/* Replace the arithmetic flags with explicit values. */
static inline void set_cc_eflags(X86CPUState *s, uint32_t flags)
{
    s->eflags = (s->eflags & ~EFLAGS_CC_MASK) | (flags & EFLAGS_CC_MASK);
    s->cc_op = CC_OP_EFLAGS;
}

/* The left operand of the last add or subtract. */
static force_inline uint64_t cc_first_operand(X86CPUState *s)
{
    uint64_t src = s->cc_src, dst = s->cc_dst;
    switch (s->cc_op) {
    case CC_OP_ADD:
        return trunc_size(dst - src, s->cc_size);
    case CC_OP_ADC:
        return trunc_size(dst - src - s->cc_carry, s->cc_size);
    case CC_OP_SUB:
        return trunc_size(dst + src, s->cc_size);
    case CC_OP_SBB:
        return trunc_size(dst + src + s->cc_carry, s->cc_size);
    default:
        return 0;
    }
}

static force_inline bool cc_carry(X86CPUState *s)
{
    uint64_t src = s->cc_src, dst = s->cc_dst;
    switch (s->cc_op) {
    case CC_OP_EFLAGS:
        return get_bit(s->eflags, EFLAGS_CF);
    case CC_OP_ADD:
        return dst < src;
    case CC_OP_ADC:
        return s->cc_carry ? dst <= src : dst < src;
    case CC_OP_SUB:
        return cc_first_operand(s) < src;
    case CC_OP_SBB:
        return s->cc_carry ? cc_first_operand(s) <= src :
            cc_first_operand(s) < src;
    case CC_OP_INC:
    case CC_OP_DEC:
        return src;
    case CC_OP_SHL:
        return msb(src, s->cc_size);
    case CC_OP_SAR:
        return get_bit(src, 0);
    case CC_OP_MUL:
        return src != 0;
    default:
        return false;
    }
}

static inline bool cc_overflow(X86CPUState *s)
{
    uint64_t src = s->cc_src, dst = s->cc_dst, a;
    int size = s->cc_size;
    switch (s->cc_op) {
    case CC_OP_EFLAGS:
        return get_bit(s->eflags, EFLAGS_OF);
    case CC_OP_ADD:
    case CC_OP_ADC:
        a = cc_first_operand(s);
        return msb((a ^ dst) & (src ^ dst), size);
    case CC_OP_SUB:
    case CC_OP_SBB:
        a = cc_first_operand(s);
        return msb((a ^ src) & (a ^ dst), size);
    case CC_OP_INC:
        return dst == sign_bit(size);
    case CC_OP_DEC:
        return dst == sign_bit(size) - 1;
    case CC_OP_SHL:
        return msb(dst ^ src, size);
    case CC_OP_SAR:
        return msb(src ^ dst, size);
    case CC_OP_MUL:
        return src != 0;
    default:
        return false;
    }
}

static inline bool cc_zero(X86CPUState *s)
{
    if (s->cc_op == CC_OP_EFLAGS) {
        return get_bit(s->eflags, EFLAGS_ZF);
    }
    return s->cc_dst == 0;
}

static inline bool cc_sign(X86CPUState *s)
{
    if (s->cc_op == CC_OP_EFLAGS) {
        return get_bit(s->eflags, EFLAGS_SF);
    }
    return msb(s->cc_dst, s->cc_size);
}

static inline bool cc_parity(X86CPUState *s)
{
    if (s->cc_op == CC_OP_EFLAGS) {
        return get_bit(s->eflags, EFLAGS_PF);
    }
    return !__builtin_parity(get_bits(s->cc_dst, 0, 8));
}

static inline bool cc_adjust(X86CPUState *s)
{
    switch (s->cc_op) {
    case CC_OP_EFLAGS:
        return get_bit(s->eflags, EFLAGS_AF);
    case CC_OP_ADD:
    case CC_OP_ADC:
    case CC_OP_SUB:
    case CC_OP_SBB:
        return get_bit(cc_first_operand(s) ^ s->cc_src ^ s->cc_dst, 4);
    case CC_OP_INC:
        return get_bits(s->cc_dst, 0, 4) == 0;
    case CC_OP_DEC:
        return get_bits(s->cc_dst, 0, 4) == 0xf;
    default:
        return false;
    }
}

/* The arithmetic flags as EFLAGS bits. */
static inline uint32_t cc_eflags(X86CPUState *s)
{
    if (s->cc_op == CC_OP_EFLAGS) {
        return s->eflags & EFLAGS_CC_MASK;
    }
    return set_bit(0, EFLAGS_CF, cc_carry(s)) |
        set_bit(0, EFLAGS_PF, cc_parity(s)) |
        set_bit(0, EFLAGS_AF, cc_adjust(s)) |
        set_bit(0, EFLAGS_ZF, cc_zero(s)) |
        set_bit(0, EFLAGS_SF, cc_sign(s)) |
        set_bit(0, EFLAGS_OF, cc_overflow(s));
}

static inline uint32_t get_eflags(X86CPUState *s)
{
    return (s->eflags & ~EFLAGS_CC_MASK) | cc_eflags(s);
}

/* Condition code 'cc' as encoded in the low nibble of Jcc, SETcc and
   CMOVcc. */
static inline bool test_condition(X86CPUState *s, int cc)
{
    bool result;
    bool sub = s->cc_op == CC_OP_SUB;
    uint64_t a = sub ? cc_first_operand(s) : 0;
    int size = s->cc_size;

    switch (get_bits(cc, 1, 3)) {
    case 0: /* O */
        result = cc_overflow(s);
        break;
    case 1: /* B */
        result = sub ? a < s->cc_src : cc_carry(s);
        break;
    case 2: /* Z */
        result = cc_zero(s);
        break;
    case 3: /* BE */
        result = sub ? a <= s->cc_src : cc_carry(s) || cc_zero(s);
        break;
    case 4: /* S */
        result = cc_sign(s);
        break;
    case 5: /* P */
        result = cc_parity(s);
        break;
    case 6: /* L */
        result = sub ? sext_size(a, size) < sext_size(s->cc_src, size) :
            cc_sign(s) != cc_overflow(s);
        break;
    default: /* LE */
        result = sub ? sext_size(a, size) <= sext_size(s->cc_src, size) :
            cc_zero(s) || cc_sign(s) != cc_overflow(s);
        break;
    }
    return result != get_bit(cc, 0);
}
