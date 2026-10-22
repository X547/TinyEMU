/*
 * RISCV emulator
 * 
 * Copyright (c) 2016 Fabrice Bellard
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
/* Division and remainder as RISC-V defines them for a zero divisor and for
   the most negative value divided by -1. */
template <typename S>
static inline S rv_div(S a, S b)
{
    if (b == 0) {
        return -1;
    } else if (a == (S)1 << (8 * sizeof(S) - 1) && b == -1) {
        return a;
    } else {
        return a / b;
    }
}

template <typename U>
static inline U rv_divu(U a, U b)
{
    if (b == 0) {
        return -1;
    } else {
        return a / b;
    }
}

template <typename S>
static inline S rv_rem(S a, S b)
{
    if (b == 0) {
        return a;
    } else if (a == (S)1 << (8 * sizeof(S) - 1) && b == -1) {
        return 0;
    } else {
        return a % b;
    }
}

template <typename U>
static inline U rv_remu(U a, U b)
{
    if (b == 0) {
        return a;
    } else {
        return a % b;
    }
}

/* The high half of a product, through a type twice as wide when the host has
   one, else from four half width products. */
template <typename U>
static U rv_mulhu_halves(U a, U b)
{
    constexpr int UHALF_LEN = 4 * sizeof(U);
    typedef uintn_t<UHALF_LEN> UHALF;
    UHALF a0, a1, b0, b1, r2, r3;
    U r00, r01, r10, r11, c;
    a0 = a;
    a1 = a >> UHALF_LEN;
    b0 = b;
    b1 = b >> UHALF_LEN;

    r00 = (U)a0 * (U)b0;
    r01 = (U)a0 * (U)b1;
    r10 = (U)a1 * (U)b0;
    r11 = (U)a1 * (U)b1;

    //    r0 = r00;
    c = (r00 >> UHALF_LEN) + (UHALF)r01 + (UHALF)r10;
    //    r1 = c;
    c = (c >> UHALF_LEN) + (r01 >> UHALF_LEN) + (r10 >> UHALF_LEN) + (UHALF)r11;
    r2 = c;
    r3 = (c >> UHALF_LEN) + (r11 >> UHALF_LEN);

    //    *plow = ((U)r1 << UHALF_LEN) | r0;
    return ((U)r3 << UHALF_LEN) | r2;
}

template <typename U>
static inline U rv_mulhu(U a, U b)
{
    if constexpr (sizeof(U) == 4) {
        return ((uint64_t)a * (uint64_t)b) >> 32;
#ifdef HAVE_INT128
    } else if constexpr (sizeof(U) == 8) {
        return ((uint128_t)a * (uint128_t)b) >> 64;
#endif
    } else {
        return rv_mulhu_halves(a, b);
    }
}

template <typename U>
static inline U rv_mulh(std::make_signed_t<U> a, std::make_signed_t<U> b)
{
    if constexpr (sizeof(U) == 4) {
        return ((int64_t)a * (int64_t)b) >> 32;
#ifdef HAVE_INT128
    } else if constexpr (sizeof(U) == 8) {
        return ((int128_t)a * (int128_t)b) >> 64;
#endif
    } else {
        U r1;
        r1 = rv_mulhu_halves<U>(a, b);
        if (a < 0)
            r1 -= a;
        if (b < 0)
            r1 -= b;
        return r1;
    }
}

template <typename U>
static inline U rv_mulhsu(std::make_signed_t<U> a, U b)
{
    if constexpr (sizeof(U) == 4) {
        return ((int64_t)a * (int64_t)b) >> 32;
#ifdef HAVE_INT128
    } else if constexpr (sizeof(U) == 8) {
        return ((int128_t)a * (int128_t)b) >> 64;
#endif
    } else {
        U r1;
        r1 = rv_mulhu_halves<U>(a, b);
        if (a < 0)
            r1 -= a;
        return r1;
    }
}

/* The bit manipulation primitives of Zbb on U, which is 32 or 64 bits wide. */
template <typename U>
static inline U rv_clz(U a)
{
    if (a == 0)
        return 8 * sizeof(U);
    if constexpr (sizeof(U) == 4)
        return __builtin_clz(a);
    else
        return __builtin_clzll(a);
}

template <typename U>
static inline U rv_ctz(U a)
{
    if (a == 0)
        return 8 * sizeof(U);
    if constexpr (sizeof(U) == 4)
        return __builtin_ctz(a);
    else
        return __builtin_ctzll(a);
}

template <typename U>
static inline U rv_cpop(U a)
{
    if constexpr (sizeof(U) == 4)
        return __builtin_popcount(a);
    else
        return __builtin_popcountll(a);
}

template <typename U>
static inline U rv_rol(U a, unsigned int shamt)
{
    shamt &= 8 * sizeof(U) - 1;
    if (shamt == 0)
        return a;
    return (a << shamt) | (a >> (8 * sizeof(U) - shamt));
}

template <typename U>
static inline U rv_ror(U a, unsigned int shamt)
{
    return rv_rol<U>(a, -shamt);
}

template <typename U>
static inline U rv_rev8(U a)
{
    if constexpr (sizeof(U) == 4)
        return __builtin_bswap32(a);
    else
        return __builtin_bswap64(a);
}

template <typename U>
static inline U rv_orc_b(U a)
{
    U r = 0;
    for (unsigned int i = 0; i < 8 * sizeof(U); i += 8) {
        if ((a >> i) & 0xff)
            r |= (U)0xff << i;
    }
    return r;
}

/* The Zba, Zbb and Zbs instructions in OP, at XLEN 32 or 64 with U that
   wide. False for an encoding that is none of theirs. */
template <typename U>
static bool rv_bitmanip_op(uint32_t insn, U a, U b, U *pres)
{
    typedef std::make_signed_t<U> S;
    constexpr unsigned int len = 8 * sizeof(U);
    U bit = (U)1 << (b & (len - 1));

    switch (get_bits(insn, 25, 7) << 3 | get_bits(insn, 12, 3)) {
    case 0x10 << 3 | 2: /* sh1add */
        *pres = b + (a << 1);
        break;
    case 0x10 << 3 | 4: /* sh2add */
        *pres = b + (a << 2);
        break;
    case 0x10 << 3 | 6: /* sh3add */
        *pres = b + (a << 3);
        break;
    case 0x20 << 3 | 4: /* xnor */
        *pres = ~(a ^ b);
        break;
    case 0x20 << 3 | 6: /* orn */
        *pres = a | ~b;
        break;
    case 0x20 << 3 | 7: /* andn */
        *pres = a & ~b;
        break;
    case 0x05 << 3 | 4: /* min */
        *pres = (S)a < (S)b ? a : b;
        break;
    case 0x05 << 3 | 5: /* minu */
        *pres = a < b ? a : b;
        break;
    case 0x05 << 3 | 6: /* max */
        *pres = (S)a < (S)b ? b : a;
        break;
    case 0x05 << 3 | 7: /* maxu */
        *pres = a < b ? b : a;
        break;
    case 0x30 << 3 | 1: /* rol */
        *pres = rv_rol<U>(a, b);
        break;
    case 0x30 << 3 | 5: /* ror */
        *pres = rv_ror<U>(a, b);
        break;
    case 0x14 << 3 | 1: /* bset */
        *pres = a | bit;
        break;
    case 0x24 << 3 | 1: /* bclr */
        *pres = a & ~bit;
        break;
    case 0x24 << 3 | 5: /* bext */
        *pres = (a & bit) != 0;
        break;
    case 0x34 << 3 | 1: /* binv */
        *pres = a ^ bit;
        break;
    case 0x04 << 3 | 4: /* zext.h, which RV64 has in OP-32 instead */
        if (len != 32 || get_bits(insn, 20, 5) != 0)
            return false;
        *pres = (uint16_t)a;
        break;
    default:
        return false;
    }
    return true;
}

/* The Zbb and Zbs instructions in OP-IMM, at XLEN 32 or 64 with U that
   wide. False for an encoding that is none of theirs. */
template <typename U>
static bool rv_bitmanip_op_imm(uint32_t insn, U a, U *pres)
{
    typedef std::make_signed_t<U> S;
    constexpr unsigned int len = 8 * sizeof(U);
    uint32_t imm = get_bits(insn, 20, 12);
    uint32_t shamt = imm & 0x3f;
    U bit = (U)1 << (shamt & (len - 1));

    if (get_bits(insn, 12, 3) == 1) {
        switch (imm) {
        case 0x600: /* clz */
            *pres = rv_clz<U>(a);
            return true;
        case 0x601: /* ctz */
            *pres = rv_ctz<U>(a);
            return true;
        case 0x602: /* cpop */
            *pres = rv_cpop<U>(a);
            return true;
        case 0x604: /* sext.b */
            *pres = (S)(int8_t)a;
            return true;
        case 0x605: /* sext.h */
            *pres = (S)(int16_t)a;
            return true;
        }
        if (shamt >= len)
            return false;
        switch (imm >> 6) {
        case 0x0a: /* bseti */
            *pres = a | bit;
            return true;
        case 0x12: /* bclri */
            *pres = a & ~bit;
            return true;
        case 0x1a: /* binvi */
            *pres = a ^ bit;
            return true;
        }
    } else {
        if (imm == 0x287) { /* orc.b */
            *pres = rv_orc_b<U>(a);
            return true;
        }
        if (imm == (len == 32 ? 0x698 : 0x6b8)) { /* rev8 */
            *pres = rv_rev8<U>(a);
            return true;
        }
        if (shamt >= len)
            return false;
        switch (imm >> 6) {
        case 0x12: /* bexti */
            *pres = (a & bit) != 0;
            return true;
        case 0x18: /* rori */
            *pres = rv_ror<U>(a, shamt);
            return true;
        }
    }
    return false;
}

/* The Zba and Zbb instructions in OP-32, at XLEN 64. The result is the full
   register value. False for an encoding that is none of theirs. */
static inline bool rv_bitmanip_op_32(uint32_t insn, uint64_t a, uint64_t b,
                                     uint64_t *pres)
{
    switch (get_bits(insn, 25, 7) << 3 | get_bits(insn, 12, 3)) {
    case 0x04 << 3 | 0: /* add.uw */
        *pres = b + (uint32_t)a;
        break;
    case 0x04 << 3 | 4: /* zext.h */
        if (get_bits(insn, 20, 5) != 0)
            return false;
        *pres = (uint16_t)a;
        break;
    case 0x10 << 3 | 2: /* sh1add.uw */
        *pres = b + ((uint64_t)(uint32_t)a << 1);
        break;
    case 0x10 << 3 | 4: /* sh2add.uw */
        *pres = b + ((uint64_t)(uint32_t)a << 2);
        break;
    case 0x10 << 3 | 6: /* sh3add.uw */
        *pres = b + ((uint64_t)(uint32_t)a << 3);
        break;
    case 0x30 << 3 | 1: /* rolw */
        *pres = (int32_t)rv_rol<uint32_t>(a, b);
        break;
    case 0x30 << 3 | 5: /* rorw */
        *pres = (int32_t)rv_ror<uint32_t>(a, b);
        break;
    default:
        return false;
    }
    return true;
}

/* The Zba and Zbb instructions in OP-IMM-32, at XLEN 64. The result is the
   full register value. False for an encoding that is none of theirs. */
static inline bool rv_bitmanip_op_imm_32(uint32_t insn, uint64_t a,
                                         uint64_t *pres)
{
    uint32_t imm = get_bits(insn, 20, 12);

    if (get_bits(insn, 12, 3) == 1) {
        switch (imm) {
        case 0x600: /* clzw */
            *pres = rv_clz<uint32_t>(a);
            return true;
        case 0x601: /* ctzw */
            *pres = rv_ctz<uint32_t>(a);
            return true;
        case 0x602: /* cpopw */
            *pres = rv_cpop<uint32_t>(a);
            return true;
        }
        if ((imm >> 6) == 0x02) { /* slli.uw */
            *pres = (uint64_t)(uint32_t)a << (imm & 0x3f);
            return true;
        }
    } else if ((imm >> 5) == 0x30) { /* roriw */
        *pres = (int32_t)rv_ror<uint32_t>(a, imm & 0x1f);
        return true;
    }
    return false;
}

/* how an instruction that a helper executed ended, if not normally */
#define INSN_ILLEGAL   (-1)
#define INSN_EXCEPTION 1

/* LR, SC and the AMOs of U's width, from funct5 of 'insn'. With other harts
   about, an operand in RAM is updated through 'ptr': an SC stores only if
   memory still holds what the LR read, and an AMO computes its result again
   until its compare and exchange finds the value it read. */
template <int MaxXlen>
template <typename U>
int RISCVCPUState<MaxXlen>::exec_amo(RISCVCPUState *s, uint32_t insn,
                                     uint32_t rs1, uint32_t rs2,
                                     target_ulong *pval)
{
    typedef std::make_signed_t<U> S;
    U rval;
    uint8_t *ptr;
    target_ulong addr, val, val2;
    uint32_t funct3;

    addr = s->reg[rs1];
    funct3 = get_bits(insn, 27, 5);
    switch(funct3) {
    case 2: /* lr.w */
        if (rs2 != 0)
            return INSN_ILLEGAL;
        if (target_read(s, &rval, addr))
            return INSN_EXCEPTION;
        lr_acquire(s, insn);
        val = (S)rval;
        s->load_res = addr;
        s->load_val = rval;
        break;
    case 3: /* sc.w */
        if (s->load_res == addr) {
            if (rmw_ptr(s, addr, access_size_log2<U>, &ptr))
                return INSN_EXCEPTION;
            if (ptr != nullptr) {
                rval = s->load_val;
                val = !host_cmpxchg<U>(ptr, &rval, s->reg[rs2]);
            } else {
                if (target_write<U>(s, addr, s->reg[rs2]))
                    return INSN_EXCEPTION;
                val = 0;
            }
        } else {
            val = 1;
        }
        /* an SC always ends the reservation */
        s->load_res = (target_ulong)-1;
        break;
    case 1: /* amiswap.w */
    case 0: /* amoadd.w */
    case 4: /* amoxor.w */
    case 0xc: /* amoand.w */
    case 0x8: /* amoor.w */
    case 0x10: /* amomin.w */
    case 0x14: /* amomax.w */
    case 0x18: /* amominu.w */
    case 0x1c: /* amomaxu.w */
        if (rmw_ptr(s, addr, access_size_log2<U>, &ptr))
            return INSN_EXCEPTION;
        if (ptr != nullptr)
            rval = host_atomic_load<U>(ptr);
        else if (target_read(s, &rval, addr))
            return INSN_EXCEPTION;
    amo_retry:
        val = (S)rval;
        val2 = s->reg[rs2];
        switch(funct3) {
        case 1: /* amiswap.w */
            break;
        case 0: /* amoadd.w */
            val2 = (S)(val + val2);
            break;
        case 4: /* amoxor.w */
            val2 = (S)(val ^ val2);
            break;
        case 0xc: /* amoand.w */
            val2 = (S)(val & val2);
            break;
        case 0x8: /* amoor.w */
            val2 = (S)(val | val2);
            break;
        case 0x10: /* amomin.w */
            if ((S)val < (S)val2)
                val2 = (S)val;
            break;
        case 0x14: /* amomax.w */
            if ((S)val > (S)val2)
                val2 = (S)val;
            break;
        case 0x18: /* amominu.w */
            if ((U)val < (U)val2)
                val2 = (S)val;
            break;
        case 0x1c: /* amomaxu.w */
            if ((U)val > (U)val2)
                val2 = (S)val;
            break;
        default:
            return INSN_ILLEGAL;
        }
        if (ptr != nullptr) {
            if (!host_cmpxchg<U>(ptr, &rval, val2))
                goto amo_retry;
        } else if (target_write<U>(s, addr, val2)) {
            return INSN_EXCEPTION;
        }
        break;
    default:
        return INSN_ILLEGAL;
    }
    *pval = val;
    return 0;
}

#include "riscv_cpu_fp.h"

#define DUP2(F, n) F(n) F(n+1)
#define DUP4(F, n) DUP2(F, n) DUP2(F, n + 2)
#define DUP8(F, n) DUP4(F, n) DUP4(F, n + 4)
#define DUP16(F, n) DUP8(F, n) DUP8(F, n + 8)
#define DUP32(F, n) DUP16(F, n) DUP16(F, n + 16)

#define C_QUADRANT(n) \
    case n+(0 << 2): case n+(1 << 2): case n+(2 << 2): case n+(3 << 2): \
    case n+(4 << 2): case n+(5 << 2): case n+(6 << 2): case n+(7 << 2): \
    case n+(8 << 2): case n+(9 << 2): case n+(10 << 2): case n+(11 << 2): \
    case n+(12 << 2): case n+(13 << 2): case n+(14 << 2): case n+(15 << 2): \
    case n+(16 << 2): case n+(17 << 2): case n+(18 << 2): case n+(19 << 2): \
    case n+(20 << 2): case n+(21 << 2): case n+(22 << 2): case n+(23 << 2): \
    case n+(24 << 2): case n+(25 << 2): case n+(26 << 2): case n+(27 << 2): \
    case n+(28 << 2): case n+(29 << 2): case n+(30 << 2): case n+(31 << 2):

#define GET_PC() (target_ulong)((uintptr_t)code_ptr + code_to_pc_addend)
#define GET_INSN_COUNTER() (insn_counter_addend - s->n_cycles)

#define C_NEXT_INSN code_ptr += 2; break
#define NEXT_INSN code_ptr += 4; break
#define JUMP_INSN do {   \
        code_ptr = NULL;           \
        code_end = NULL;           \
        code_to_pc_addend = s->pc; \
        goto jump_insn;            \
    } while (0)

/* the interpreter for code running at XLEN Xlen */
template <int MaxXlen>
template <int Xlen>
void RISCVCPUState<MaxXlen>::riscv_cpu_interp_x(RISCVCPUState *s,
                                                int n_cycles1)
{
    typedef uintn_t<Xlen> uintx_t;
    typedef intn_t<Xlen> intx_t;
    uint32_t opcode, insn, rd, rs1, rs2, funct3;
    int32_t imm, cond, err;
    target_ulong addr, val, val2;
    uint8_t *code_ptr, *code_end;
    target_ulong code_to_pc_addend;
    uint64_t insn_counter_addend;
    uint32_t rs3;
    int32_t rm;

    if (n_cycles1 == 0)
        return;
    insn_counter_addend = s->insn_counter + n_cycles1;
    s->n_cycles = n_cycles1;

    /* check pending interrupts */
    if (unlikely((s->mip & s->mie) != 0)) {
        if (raise_interrupt(s)) {
            s->n_cycles--; 
            goto done_interp;
        }
    }

    s->pending_exception = -1;
    /* Note: we assume NULL is represented as a zero number */
    code_ptr = NULL;
    code_end = NULL;
    code_to_pc_addend = s->pc;
    
    /* we use a single execution loop to keep a simple control flow */
    for(;;) {
        if (unlikely(code_ptr >= code_end)) {
            uint32_t tlb_idx;
            uint16_t insn_high;
            target_ulong addr;
            uint8_t *ptr;
            
            s->pc = GET_PC();
            /* we test n_cycles only between blocks so that timer
               interrupts only happen between the blocks. It is
               important to reduce the translated code size. */
            if (unlikely(s->n_cycles <= 0))
                goto the_end;
            /* interrupt lines posted by another thread are taken in by the
               caller before mip is looked at */
            if (unlikely(s->inbox_pending.load(std::memory_order_relaxed)))
                goto the_end;

            /* check pending interrupts */
            if (unlikely((s->mip & s->mie) != 0)) {
                if (raise_interrupt(s)) {
                    s->n_cycles--; 
                    goto the_end;
                }
            }
    
            addr = s->pc;
            tlb_idx = get_bits(addr, PG_SHIFT, TLB_BITS);
            if (likely(s->tlb_code[tlb_idx].vaddr == (addr & ~PG_MASK))) {
                /* TLB match */ 
                ptr = (uint8_t *)(s->tlb_code[tlb_idx].mem_addend +
                                  (uintptr_t)addr);
            } else {
                if (unlikely(target_read_insn_slow(s, &ptr, addr)))
                    goto mmu_exception;
            }
            code_ptr = ptr;
            code_end = ptr + (PG_MASK - 1 - (addr & PG_MASK));
            code_to_pc_addend = addr - (uintptr_t)code_ptr;
            if (unlikely(code_ptr >= code_end)) {
                /* instruction is potentially half way between two
                   pages ? */
                insn = *(uint16_t *)code_ptr;
                if ((insn & 3) == 3) {
                    /* instruction is half way between two pages */
                    if (unlikely(target_read_insn_u16(s, &insn_high, addr + 2)))
                        goto mmu_exception;
                    insn |= insn_high << 16;
                }
            } else {
                insn = get_insn32(code_ptr);
            }
        } else {
            /* fast path */
            insn = get_insn32(code_ptr);
        }
        s->n_cycles--;
#if 0
        if (1) {
#ifdef CONFIG_LOGFILE
            log_printf("pc=0x"); fprint_target_ulong(log_file, GET_PC()); log_printf(" insn=%08x\n", insn);
            fflush(log_file);
#else
            printf("pc=0x"); print_target_ulong(GET_PC()); printf(" insn=%08x\n", insn);
            //            dump_regs(s);
#endif
        }
#endif
        opcode = get_bits(insn, 0, 7);
        rd = get_bits(insn, 7, 5);
        rs1 = get_bits(insn, 15, 5);
        rs2 = get_bits(insn, 20, 5);
        switch(opcode) {
#ifdef CONFIG_EXT_C
        C_QUADRANT(0)
            funct3 = get_bits(insn, 13, 3);
            rd = get_bits(insn, 2, 3) | 8;
            switch(funct3) {
            case 0: /* c.addi4spn */
                imm = get_field1(insn, 11, 4, 5) |
                    get_field1(insn, 7, 6, 9) |
                    get_field1(insn, 6, 2, 2) |
                    get_field1(insn, 5, 3, 3);
                if (imm == 0)
                    goto illegal_insn;
                s->reg[rd] = (intx_t)(s->reg[2] + imm);
                break;
            case 1:
                if constexpr (Xlen >= 128) {
                    /* c.lq */
                    imm = get_field1(insn, 11, 4, 5) |
                        get_field1(insn, 10, 8, 8) |
                        get_field1(insn, 5, 6, 7);
                    rs1 = get_bits(insn, 7, 3) | 8;
                    addr = (intx_t)(s->reg[rs1] + imm);
                    if (target_read<uintx_t>(s, &val, addr))
                        goto mmu_exception;
                    s->reg[rd] = val;
                } else {
                    /* c.fld */
                    uint64_t rval;
                    if (s->fs == 0)
                        goto illegal_insn;
                    imm = get_field1(insn, 10, 3, 5) |
                        get_field1(insn, 5, 6, 7);
                    rs1 = get_bits(insn, 7, 3) | 8;
                    addr = (intx_t)(s->reg[rs1] + imm);
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    s->fp_reg[rd] = rval | fp_high<64>();
                    s->fs = 3;
                }
                break;
            case 2: /* c.lw */
                {
                    uint32_t rval;
                    imm = get_field1(insn, 10, 3, 5) |
                        get_field1(insn, 6, 2, 2) |
                        get_field1(insn, 5, 6, 6);
                    rs1 = get_bits(insn, 7, 3) | 8;
                    addr = (intx_t)(s->reg[rs1] + imm);
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    s->reg[rd] = (int32_t)rval;
                }
                break;
            case 3:
                if constexpr (Xlen >= 64) {
                    /* c.ld */
                    uint64_t rval;
                    imm = get_field1(insn, 10, 3, 5) |
                        get_field1(insn, 5, 6, 7);
                    rs1 = get_bits(insn, 7, 3) | 8;
                    addr = (intx_t)(s->reg[rs1] + imm);
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    s->reg[rd] = (int64_t)rval;
                } else {
                    /* c.flw */
                    uint32_t rval;
                    if (s->fs == 0)
                        goto illegal_insn;
                    imm = get_field1(insn, 10, 3, 5) |
                        get_field1(insn, 6, 2, 2) |
                        get_field1(insn, 5, 6, 6);
                    rs1 = get_bits(insn, 7, 3) | 8;
                    addr = (intx_t)(s->reg[rs1] + imm);
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    s->fp_reg[rd] = rval | fp_high<32>();
                    s->fs = 3;
                }
                break;
            case 5:
                if constexpr (Xlen >= 128) {
                    /* c.sq */
                    imm = get_field1(insn, 11, 4, 5) |
                        get_field1(insn, 10, 8, 8) |
                        get_field1(insn, 5, 6, 7);
                    rs1 = get_bits(insn, 7, 3) | 8;
                    addr = (intx_t)(s->reg[rs1] + imm);
                    val = s->reg[rd];
                    if (target_write<uintx_t>(s, addr, val))
                        goto mmu_exception;
                } else {
                    /* c.fsd */
                    if (s->fs == 0)
                        goto illegal_insn;
                    imm = get_field1(insn, 10, 3, 5) |
                        get_field1(insn, 5, 6, 7);
                    rs1 = get_bits(insn, 7, 3) | 8;
                    addr = (intx_t)(s->reg[rs1] + imm);
                    if (target_write<uint64_t>(s, addr, s->fp_reg[rd]))
                        goto mmu_exception;
                }
                break;
            case 6: /* c.sw */
                imm = get_field1(insn, 10, 3, 5) |
                    get_field1(insn, 6, 2, 2) |
                    get_field1(insn, 5, 6, 6);
                rs1 = get_bits(insn, 7, 3) | 8;
                addr = (intx_t)(s->reg[rs1] + imm);
                val = s->reg[rd];
                if (target_write<uint32_t>(s, addr, val))
                    goto mmu_exception;
                break;
            case 7:
                if constexpr (Xlen >= 64) {
                    /* c.sd */
                    imm = get_field1(insn, 10, 3, 5) |
                        get_field1(insn, 5, 6, 7);
                    rs1 = get_bits(insn, 7, 3) | 8;
                    addr = (intx_t)(s->reg[rs1] + imm);
                    val = s->reg[rd];
                    if (target_write<uint64_t>(s, addr, val))
                        goto mmu_exception;
                } else {
                    /* c.fsw */
                    if (s->fs == 0)
                        goto illegal_insn;
                    imm = get_field1(insn, 10, 3, 5) |
                        get_field1(insn, 6, 2, 2) |
                        get_field1(insn, 5, 6, 6);
                    rs1 = get_bits(insn, 7, 3) | 8;
                    addr = (intx_t)(s->reg[rs1] + imm);
                    if (target_write<uint32_t>(s, addr, s->fp_reg[rd]))
                        goto mmu_exception;
                }
                break;
            default:
                goto illegal_insn;
            }
            C_NEXT_INSN;
        C_QUADRANT(1)
            funct3 = get_bits(insn, 13, 3);
            switch(funct3) {
            case 0: /* c.addi/c.nop */
                if (rd != 0) {
                    imm = sign_extend(get_field1(insn, 12, 5, 5) |
                                      get_field1(insn, 2, 0, 4), 6);
                    s->reg[rd] = (intx_t)(s->reg[rd] + imm);
                }
                break;
            case 1:
                if constexpr (Xlen == 32) {
                    /* c.jal */
                    imm = sign_extend(get_field1(insn, 12, 11, 11) |
                                      get_field1(insn, 11, 4, 4) |
                                      get_field1(insn, 9, 8, 9) |
                                      get_field1(insn, 8, 10, 10) |
                                      get_field1(insn, 7, 6, 6) |
                                      get_field1(insn, 6, 7, 7) |
                                      get_field1(insn, 3, 1, 3) |
                                      get_field1(insn, 2, 5, 5), 12);
                    s->reg[1] = GET_PC() + 2;
                    s->pc = (intx_t)(GET_PC() + imm);
                    JUMP_INSN;
                } else {
                    /* c.addiw */
                    if (rd != 0) {
                        imm = sign_extend(get_field1(insn, 12, 5, 5) |
                                          get_field1(insn, 2, 0, 4), 6);
                        s->reg[rd] = (int32_t)(s->reg[rd] + imm);
                    }
                }
                break;
            case 2: /* c.li */
                if (rd != 0) {
                    imm = sign_extend(get_field1(insn, 12, 5, 5) |
                                      get_field1(insn, 2, 0, 4), 6);
                    s->reg[rd] = imm;
                }
                break;
            case 3:
                if (rd == 2) {
                    /* c.addi16sp */
                    imm = sign_extend(get_field1(insn, 12, 9, 9) |
                                      get_field1(insn, 6, 4, 4) |
                                      get_field1(insn, 5, 6, 6) |
                                      get_field1(insn, 3, 7, 8) |
                                      get_field1(insn, 2, 5, 5), 10);
                    if (imm == 0)
                        goto illegal_insn;
                    s->reg[2] = (intx_t)(s->reg[2] + imm);
                } else if (rd != 0) {
                    /* c.lui */
                    imm = sign_extend(get_field1(insn, 12, 17, 17) |
                                      get_field1(insn, 2, 12, 16), 18);
                    s->reg[rd] = imm;
                }
                break;
            case 4: 
                funct3 = get_bits(insn, 10, 2);
                rd = get_bits(insn, 7, 3) | 8;
                switch(funct3) {
                case 0: /* c.srli */ 
                case 1: /* c.srai */ 
                    imm = get_field1(insn, 12, 5, 5) |
                        get_field1(insn, 2, 0, 4);
                    if constexpr (Xlen == 32) {
                        if (get_bit(imm, 5))
                            goto illegal_insn;
                    } else if constexpr (Xlen == 128) {
                        if (imm == 0)
                            imm = 64;
                        else if (imm >= 32)
                            imm = 128 - imm;
                    }
                    if (funct3 == 0)
                        s->reg[rd] = (intx_t)((uintx_t)s->reg[rd] >> imm);
                    else
                        s->reg[rd] = (intx_t)s->reg[rd] >> imm;
                    
                    break;
                case 2: /* c.andi */
                    imm = sign_extend(get_field1(insn, 12, 5, 5) |
                                      get_field1(insn, 2, 0, 4), 6);
                    s->reg[rd] &= imm;
                    break;
                case 3: 
                    rs2 = get_bits(insn, 2, 3) | 8;
                    funct3 = get_bits(insn, 5, 2) | (get_bit(insn, 12) << 2);
                    switch(funct3) {
                    case 0: /* c.sub */
                        s->reg[rd] = (intx_t)(s->reg[rd] - s->reg[rs2]);
                        break;
                    case 1: /* c.xor */
                        s->reg[rd] = s->reg[rd] ^ s->reg[rs2];
                        break;
                    case 2: /* c.or */
                        s->reg[rd] = s->reg[rd] | s->reg[rs2];
                        break;
                    case 3: /* c.and */
                        s->reg[rd] = s->reg[rd] & s->reg[rs2];
                        break;
                    case 4: /* c.subw */
                        if constexpr (Xlen < 64)
                            goto illegal_insn;
                        s->reg[rd] = (int32_t)(s->reg[rd] - s->reg[rs2]);
                        break;
                    case 5: /* c.addw */
                        if constexpr (Xlen < 64)
                            goto illegal_insn;
                        s->reg[rd] = (int32_t)(s->reg[rd] + s->reg[rs2]);
                        break;
                    default:
                        goto illegal_insn;
                    }
                    break;
                }
                break;
            case 5: /* c.j */
                imm = sign_extend(get_field1(insn, 12, 11, 11) | 
                                  get_field1(insn, 11, 4, 4) |
                                  get_field1(insn, 9, 8, 9) |
                                  get_field1(insn, 8, 10, 10) |
                                  get_field1(insn, 7, 6, 6) |
                                  get_field1(insn, 6, 7, 7) |
                                  get_field1(insn, 3, 1, 3) |
                                  get_field1(insn, 2, 5, 5), 12);
                s->pc = (intx_t)(GET_PC() + imm);
                JUMP_INSN;
            case 6: /* c.beqz */
                rs1 = get_bits(insn, 7, 3) | 8;
                imm = sign_extend(get_field1(insn, 12, 8, 8) | 
                                  get_field1(insn, 10, 3, 4) |
                                  get_field1(insn, 5, 6, 7) |
                                  get_field1(insn, 3, 1, 2) |
                                  get_field1(insn, 2, 5, 5), 9);
                if (s->reg[rs1] == 0) {
                    s->pc = (intx_t)(GET_PC() + imm);
                    JUMP_INSN;
                }
                break;
            case 7: /* c.bnez */
                rs1 = get_bits(insn, 7, 3) | 8;
                imm = sign_extend(get_field1(insn, 12, 8, 8) | 
                                  get_field1(insn, 10, 3, 4) |
                                  get_field1(insn, 5, 6, 7) |
                                  get_field1(insn, 3, 1, 2) |
                                  get_field1(insn, 2, 5, 5), 9);
                if (s->reg[rs1] != 0) {
                    s->pc = (intx_t)(GET_PC() + imm);
                    JUMP_INSN;
                }
                break;
            default:
                goto illegal_insn;
            }
            C_NEXT_INSN;
        C_QUADRANT(2)
            funct3 = get_bits(insn, 13, 3);
            rs2 = get_bits(insn, 2, 5);
            switch(funct3) {
            case 0: /* c.slli */
                imm = get_field1(insn, 12, 5, 5) | rs2;
                if constexpr (Xlen == 32) {
                    if (get_bit(imm, 5))
                        goto illegal_insn;
                } else if constexpr (Xlen == 128) {
                    if (imm == 0)
                        imm = 64;
                }
                if (rd != 0)
                    s->reg[rd] = (intx_t)(s->reg[rd] << imm);
                break;
            case 1:
                if constexpr (Xlen == 128) {
                    /* c.lqsp */
                    imm = get_field1(insn, 12, 5, 5) |
                        (rs2 & bit_at(4)) |
                        get_field1(insn, 2, 6, 9);
                    addr = (intx_t)(s->reg[2] + imm);
                    if (target_read<uintx_t>(s, &val, addr))
                        goto mmu_exception;
                    if (rd != 0)
                        s->reg[rd] = val;
                } else {
                    /* c.fldsp */
                    uint64_t rval;
                    if (s->fs == 0)
                        goto illegal_insn;
                    imm = get_field1(insn, 12, 5, 5) |
                        (rs2 & field_mask(3, 2)) |
                        get_field1(insn, 2, 6, 8);
                    addr = (intx_t)(s->reg[2] + imm);
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    s->fp_reg[rd] = rval | fp_high<64>();
                    s->fs = 3;
                }
                break;
            case 2: /* c.lwsp */
                {
                    uint32_t rval;
                    imm = get_field1(insn, 12, 5, 5) |
                        (rs2 & field_mask(2, 3)) |
                        get_field1(insn, 2, 6, 7);
                    addr = (intx_t)(s->reg[2] + imm);
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    if (rd != 0)
                        s->reg[rd] = (int32_t)rval;
                }
                break;
            case 3:
                if constexpr (Xlen >= 64) {
                    /* c.ldsp */
                    uint64_t rval;
                    imm = get_field1(insn, 12, 5, 5) |
                        (rs2 & field_mask(3, 2)) |
                        get_field1(insn, 2, 6, 8);
                    addr = (intx_t)(s->reg[2] + imm);
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    if (rd != 0)
                        s->reg[rd] = (int64_t)rval;
                } else {
                    /* c.flwsp */
                    uint32_t rval;
                    if (s->fs == 0)
                        goto illegal_insn;
                    imm = get_field1(insn, 12, 5, 5) |
                        (rs2 & field_mask(2, 3)) |
                        get_field1(insn, 2, 6, 7);
                    addr = (intx_t)(s->reg[2] + imm);
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    s->fp_reg[rd] = rval | fp_high<32>();
                    s->fs = 3;
                }
                break;
            case 4:
                if (!get_bit(insn, 12)) {
                    if (rs2 == 0) {
                        /* c.jr */
                        if (rd == 0)
                            goto illegal_insn;
                        s->pc = s->reg[rd] & ~1;
                        JUMP_INSN;
                    } else {
                        /* c.mv */
                        if (rd != 0)
                            s->reg[rd] = s->reg[rs2];
                    }
                } else {
                    if (rs2 == 0) {
                        if (rd == 0) {
                            /* c.ebreak */
                            s->pending_exception = CAUSE_BREAKPOINT;
                            goto exception;
                        } else {
                            /* c.jalr */
                            val = GET_PC() + 2;
                            s->pc = s->reg[rd] & ~1;
                            s->reg[1] = val;
                            JUMP_INSN;
                        }
                    } else {
                        if (rd != 0) {
                            s->reg[rd] = (intx_t)(s->reg[rd] + s->reg[rs2]);
                        }
                    }
                }
                break;
            case 5:
                if constexpr (Xlen == 128) {
                    /* c.sqsp */
                    imm = get_field1(insn, 10, 3, 5) |
                        get_field1(insn, 7, 6, 8);
                    addr = (intx_t)(s->reg[2] + imm);
                    if (target_write<uintx_t>(s, addr, s->reg[rs2]))
                        goto mmu_exception;
                } else {
                    /* c.fsdsp */
                    if (s->fs == 0)
                        goto illegal_insn;
                    imm = get_field1(insn, 10, 3, 5) |
                        get_field1(insn, 7, 6, 8);
                    addr = (intx_t)(s->reg[2] + imm);
                    if (target_write<uint64_t>(s, addr, s->fp_reg[rs2]))
                        goto mmu_exception;
                }
                break;
            case 6: /* c.swsp */
                imm = get_field1(insn, 9, 2, 5) |
                    get_field1(insn, 7, 6, 7);
                addr = (intx_t)(s->reg[2] + imm);
                if (target_write<uint32_t>(s, addr, s->reg[rs2]))
                    goto mmu_exception;
                break;
            case 7:
                if constexpr (Xlen >= 64) {
                    /* c.sdsp */
                    imm = get_field1(insn, 10, 3, 5) |
                        get_field1(insn, 7, 6, 8);
                    addr = (intx_t)(s->reg[2] + imm);
                    if (target_write<uint64_t>(s, addr, s->reg[rs2]))
                        goto mmu_exception;
                } else {
                    /* c.fswsp */
                    if (s->fs == 0)
                        goto illegal_insn;
                    imm = get_field1(insn, 9, 2, 5) |
                        get_field1(insn, 7, 6, 7);
                    addr = (intx_t)(s->reg[2] + imm);
                    if (target_write<uint32_t>(s, addr, s->fp_reg[rs2]))
                        goto mmu_exception;
                }
                break;
            default:
                goto illegal_insn;
            }
            C_NEXT_INSN;
#endif /* CONFIG_EXT_C */

        case 0x37: /* lui */
            if (rd != 0)
                s->reg[rd] = (int32_t)set_bits(insn, 0, 12, 0);
            NEXT_INSN;
        case 0x17: /* auipc */
            if (rd != 0) {
                s->reg[rd] = (intx_t)(GET_PC() +
                                      (int32_t)set_bits(insn, 0, 12, 0));
            }
            NEXT_INSN;
        case 0x6f: /* jal */
            imm = sign_extend(get_field1(insn, 31, 20, 20) |
                              get_field1(insn, 21, 1, 10) |
                              get_field1(insn, 20, 11, 11) |
                              get_field1(insn, 12, 12, 19), 21);
            if (rd != 0)
                s->reg[rd] = GET_PC() + 4;
            s->pc = (intx_t)(GET_PC() + imm);
            JUMP_INSN;
        case 0x67: /* jalr */
            imm = sign_extend(insn >> 20, 12);
            val = GET_PC() + 4;
            s->pc = (intx_t)(s->reg[rs1] + imm) & ~1;
            if (rd != 0)
                s->reg[rd] = val;
            JUMP_INSN;
        case 0x63:
            funct3 = get_bits(insn, 12, 3);
            switch(funct3 >> 1) {
            case 0: /* beq/bne */
                cond = (s->reg[rs1] == s->reg[rs2]);
                break;
            case 2: /* blt/bge */
                cond = ((target_long)s->reg[rs1] < (target_long)s->reg[rs2]);
                break;
            case 3: /* bltu/bgeu */
                cond = (s->reg[rs1] < s->reg[rs2]);
                break;
            default:
                goto illegal_insn;
            }
            cond ^= (funct3 & 1);
            if (cond) {
                imm = sign_extend(get_field1(insn, 31, 12, 12) |
                                  get_field1(insn, 25, 5, 10) |
                                  get_field1(insn, 8, 1, 4) |
                                  get_field1(insn, 7, 11, 11), 13);
                s->pc = (intx_t)(GET_PC() + imm);
                JUMP_INSN;
            }
            NEXT_INSN;
        case 0x03: /* load */
            funct3 = get_bits(insn, 12, 3);
            imm = sign_extend(insn >> 20, 12);
            addr = s->reg[rs1] + imm;
            switch(funct3) {
            case 0: /* lb */
                {
                    uint8_t rval;
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    val = (int8_t)rval;
                }
                break;
            case 1: /* lh */
                {
                    uint16_t rval;
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    val = (int16_t)rval;
                }
                break;
            case 2: /* lw */
                {
                    uint32_t rval;
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    val = (int32_t)rval;
                }
                break;
            case 4: /* lbu */
                {
                    uint8_t rval;
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    val = rval;
                }
                break;
            case 5: /* lhu */
                {
                    uint16_t rval;
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    val = rval;
                }
                break;
            case 3: /* ld */
                if constexpr (Xlen < 64)
                    goto illegal_insn;
                {
                    uint64_t rval;
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    val = (int64_t)rval;
                }
                break;
            case 6: /* lwu */
                if constexpr (Xlen < 64)
                    goto illegal_insn;
                {
                    uint32_t rval;
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    val = rval;
                }
                break;
            case 7: /* ldu */
                if constexpr (Xlen < 128)
                    goto illegal_insn;
                {
                    uint64_t rval;
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    val = rval;
                }
                break;
            default:
                goto illegal_insn;
            }
            if (rd != 0)
                s->reg[rd] = val;
            NEXT_INSN;
        case 0x23: /* store */
            funct3 = get_bits(insn, 12, 3);
            imm = sign_extend(rd | get_field1(insn, 25, 5, 11), 12);
            addr = s->reg[rs1] + imm;
            val = s->reg[rs2];
            switch(funct3) {
            case 0: /* sb */
                if (target_write<uint8_t>(s, addr, val))
                    goto mmu_exception;
                break;
            case 1: /* sh */
                if (target_write<uint16_t>(s, addr, val))
                    goto mmu_exception;
                break;
            case 2: /* sw */
                if (target_write<uint32_t>(s, addr, val))
                    goto mmu_exception;
                break;
            case 3: /* sd */
                if constexpr (Xlen < 64)
                    goto illegal_insn;
                if (target_write<uint64_t>(s, addr, val))
                    goto mmu_exception;
                break;
            case 4: /* sq */
                if constexpr (Xlen < 128)
                    goto illegal_insn;
                if (target_write<uintx_t>(s, addr, val))
                    goto mmu_exception;
                break;
            default:
                goto illegal_insn;
            }
            NEXT_INSN;
        case 0x13:
            funct3 = get_bits(insn, 12, 3);
            imm = sign_extend(insn >> 20, 12);
            switch(funct3) {
            case 0: /* addi */
                val = (intx_t)(s->reg[rs1] + imm);
                break;
            case 1: /* slli */
                if constexpr (Xlen <= 64) {
                    if (get_bits(insn, 26, 6) != 0) {
                        uintx_t r;
                        if (!rv_bitmanip_op_imm<uintx_t>(insn, s->reg[rs1], &r))
                            goto illegal_insn;
                        val = (intx_t)r;
                        break;
                    }
                }
                if ((imm & ~(Xlen - 1)) != 0)
                    goto illegal_insn;
                val = (intx_t)(s->reg[rs1] << (imm & (Xlen - 1)));
                break;
            case 2: /* slti */
                val = (target_long)s->reg[rs1] < (target_long)imm;
                break;
            case 3: /* sltiu */
                val = s->reg[rs1] < (target_ulong)imm;
                break;
            case 4: /* xori */
                val = s->reg[rs1] ^ imm;
                break;
            case 5: /* srli/srai */
                if constexpr (Xlen <= 64) {
                    if ((get_bits(insn, 26, 6) & ~0x10) != 0) {
                        uintx_t r;
                        if (!rv_bitmanip_op_imm<uintx_t>(insn, s->reg[rs1], &r))
                            goto illegal_insn;
                        val = (intx_t)r;
                        break;
                    }
                }
                if ((imm & ~((Xlen - 1) | 0x400)) != 0)
                    goto illegal_insn;
                if (get_bit(imm, 10))
                    val = (intx_t)s->reg[rs1] >> (imm & (Xlen - 1));
                else
                    val = (intx_t)((uintx_t)s->reg[rs1] >> (imm & (Xlen - 1)));
                break;
            case 6: /* ori */
                val = s->reg[rs1] | imm;
                break;
            default:
            case 7: /* andi */
                val = s->reg[rs1] & imm;
                break;
            }
            if (rd != 0)
                s->reg[rd] = val;
            NEXT_INSN;
        case 0x1b:/* OP-IMM-32 */
            if constexpr (Xlen < 64)
                goto illegal_insn;
            funct3 = get_bits(insn, 12, 3);
            imm = sign_extend(insn >> 20, 12);
            val = s->reg[rs1];
            if constexpr (Xlen == 64) {
                if ((funct3 == 1 && get_bits(insn, 25, 7) != 0) ||
                    (funct3 == 5 && (get_bits(insn, 25, 7) & ~0x20) != 0)) {
                    uint64_t r;
                    if (!rv_bitmanip_op_imm_32(insn, val, &r))
                        goto illegal_insn;
                    if (rd != 0)
                        s->reg[rd] = (int64_t)r;
                    NEXT_INSN;
                }
            }
            switch(funct3) {
            case 0: /* addiw */
                val = (int32_t)(val + imm);
                break;
            case 1: /* slliw */
                if ((imm & ~31) != 0)
                    goto illegal_insn;
                val = (int32_t)(val << (imm & 31));
                break;
            case 5: /* srliw/sraiw */
                if ((imm & ~(31 | 0x400)) != 0)
                    goto illegal_insn;
                if (get_bit(imm, 10))
                    val = (int32_t)val >> (imm & 31);
                else
                    val = (int32_t)((uint32_t)val >> (imm & 31));
                break;
            default:
                goto illegal_insn;
            }
            if (rd != 0)
                s->reg[rd] = val;
            NEXT_INSN;
        case 0x5b: /* OP-IMM-64 */
            if constexpr (Xlen < 128)
                goto illegal_insn;
            funct3 = get_bits(insn, 12, 3);
            imm = sign_extend(insn >> 20, 12);
            val = s->reg[rs1];
            switch(funct3) {
            case 0: /* addid */
                val = (int64_t)(val + imm);
                break;
            case 1: /* sllid */
                if ((imm & ~63) != 0)
                    goto illegal_insn;
                val = (int64_t)(val << (imm & 63));
                break;
            case 5: /* srlid/sraid */
                if ((imm & ~(63 | 0x400)) != 0)
                    goto illegal_insn;
                if (get_bit(imm, 10))
                    val = (int64_t)val >> (imm & 63);
                else
                    val = (int64_t)((uint64_t)val >> (imm & 63));
                break;
            default:
                goto illegal_insn;
            }
            if (rd != 0)
                s->reg[rd] = val;
            NEXT_INSN;
        case 0x33:
            imm = get_bits(insn, 25, 7);
            val = s->reg[rs1];
            val2 = s->reg[rs2];
            if (imm == 1) {
                funct3 = get_bits(insn, 12, 3);
                switch(funct3) {
                case 0: /* mul */
                    val = (intx_t)((intx_t)val * (intx_t)val2);
                    break;
                case 1: /* mulh */
                    val = (intx_t)rv_mulh<uintx_t>(val, val2);
                    break;
                case 2:/* mulhsu */
                    val = (intx_t)rv_mulhsu<uintx_t>(val, val2);
                    break;
                case 3:/* mulhu */
                    val = (intx_t)rv_mulhu<uintx_t>(val, val2);
                    break;
                case 4:/* div */
                    val = rv_div<intx_t>(val, val2);
                    break;
                case 5:/* divu */
                    val = (intx_t)rv_divu<uintx_t>(val, val2);
                    break;
                case 6:/* rem */
                    val = rv_rem<intx_t>(val, val2);
                    break;
                case 7:/* remu */
                    val = (intx_t)rv_remu<uintx_t>(val, val2);
                    break;
                default:
                    goto illegal_insn;
                }
            } else {
                if constexpr (Xlen <= 64) {
                    uintx_t r;
                    if (imm != 0 &&
                        rv_bitmanip_op<uintx_t>(insn, val, val2, &r)) {
                        if (rd != 0)
                            s->reg[rd] = (intx_t)r;
                        NEXT_INSN;
                    }
                }
                if (imm & ~0x20)
                    goto illegal_insn;
                funct3 = get_bits(insn, 12, 3) | (get_bit(insn, 30) << 3);
                switch(funct3) {
                case 0: /* add */
                    val = (intx_t)(val + val2);
                    break;
                case 0 | 8: /* sub */
                    val = (intx_t)(val - val2);
                    break;
                case 1: /* sll */
                    val = (intx_t)(val << (val2 & (Xlen - 1)));
                    break;
                case 2: /* slt */
                    val = (target_long)val < (target_long)val2;
                    break;
                case 3: /* sltu */
                    val = val < val2;
                    break;
                case 4: /* xor */
                    val = val ^ val2;
                    break;
                case 5: /* srl */
                    val = (intx_t)((uintx_t)val >> (val2 & (Xlen - 1)));
                    break;
                case 5 | 8: /* sra */
                    val = (intx_t)val >> (val2 & (Xlen - 1));
                    break;
                case 6: /* or */
                    val = val | val2;
                    break;
                case 7: /* and */
                    val = val & val2;
                    break;
                default:
                    goto illegal_insn;
                }
            }
            if (rd != 0)
                s->reg[rd] = val;
            NEXT_INSN;
        case 0x3b: /* OP-32 */
            if constexpr (Xlen < 64)
                goto illegal_insn;
            imm = get_bits(insn, 25, 7);
            val = s->reg[rs1];
            val2 = s->reg[rs2];
            if (imm == 1) {
                funct3 = get_bits(insn, 12, 3);
                switch(funct3) {
                case 0: /* mulw */
                    val = (int32_t)((int32_t)val * (int32_t)val2);
                    break;
                case 4:/* divw */
                    val = rv_div<int32_t>(val, val2);
                    break;
                case 5:/* divuw */
                    val = (int32_t)rv_divu<uint32_t>(val, val2);
                    break;
                case 6:/* remw */
                    val = rv_rem<int32_t>(val, val2);
                    break;
                case 7:/* remuw */
                    val = (int32_t)rv_remu<uint32_t>(val, val2);
                    break;
                default:
                    goto illegal_insn;
                }
            } else {
                if constexpr (Xlen == 64) {
                    uint64_t r;
                    if (imm != 0 && imm != 0x20 &&
                        rv_bitmanip_op_32(insn, val, val2, &r)) {
                        if (rd != 0)
                            s->reg[rd] = (int64_t)r;
                        NEXT_INSN;
                    }
                }
                if (imm & ~0x20)
                    goto illegal_insn;
                funct3 = get_bits(insn, 12, 3) | (get_bit(insn, 30) << 3);
                switch(funct3) {
                case 0: /* addw */
                    val = (int32_t)(val + val2);
                    break;
                case 0 | 8: /* subw */
                    val = (int32_t)(val - val2);
                    break;
                case 1: /* sllw */
                    val = (int32_t)((uint32_t)val << (val2 & 31));
                    break;
                case 5: /* srlw */
                    val = (int32_t)((uint32_t)val >> (val2 & 31));
                    break;
                case 5 | 8: /* sraw */
                    val = (int32_t)val >> (val2 & 31);
                    break;
                default:
                    goto illegal_insn;
                }
            }
            if (rd != 0)
                s->reg[rd] = val;
            NEXT_INSN;
        case 0x7b: /* OP-64 */
            if constexpr (Xlen < 128)
                goto illegal_insn;
            imm = get_bits(insn, 25, 7);
            val = s->reg[rs1];
            val2 = s->reg[rs2];
            if (imm == 1) {
                funct3 = get_bits(insn, 12, 3);
                switch(funct3) {
                case 0: /* muld */
                    val = (int64_t)((int64_t)val * (int64_t)val2);
                    break;
                case 4:/* divd */
                    val = rv_div<int64_t>(val, val2);
                    break;
                case 5:/* divud */
                    val = (int64_t)rv_divu<uint64_t>(val, val2);
                    break;
                case 6:/* remd */
                    val = rv_rem<int64_t>(val, val2);
                    break;
                case 7:/* remud */
                    val = (int64_t)rv_remu<uint64_t>(val, val2);
                    break;
                default:
                    goto illegal_insn;
                }
            } else {
                if (imm & ~0x20)
                    goto illegal_insn;
                funct3 = get_bits(insn, 12, 3) | (get_bit(insn, 30) << 3);
                switch(funct3) {
                case 0: /* addd */
                    val = (int64_t)(val + val2);
                    break;
                case 0 | 8: /* subd */
                    val = (int64_t)(val - val2);
                    break;
                case 1: /* slld */
                    val = (int64_t)((uint64_t)val << (val2 & 63));
                    break;
                case 5: /* srld */
                    val = (int64_t)((uint64_t)val >> (val2 & 63));
                    break;
                case 5 | 8: /* srad */
                    val = (int64_t)val >> (val2 & 63);
                    break;
                default:
                    goto illegal_insn;
                }
            }
            if (rd != 0)
                s->reg[rd] = val;
            NEXT_INSN;
        case 0x73:
            funct3 = get_bits(insn, 12, 3);
            imm = get_bits(insn, 20, 12);
            if (funct3 & 4)
                val = rs1;
            else
                val = s->reg[rs1];
            funct3 &= 3;
            switch(funct3) {
            case 1: /* csrrw */
                s->insn_counter = GET_INSN_COUNTER();
                if (csr_read(s, &val2, imm, true))
                    goto illegal_insn;
                val2 = (intx_t)val2;
                err = csr_write(s, imm, val);
                if (err < 0)
                    goto illegal_insn;
                /* an interrupt the write unmasked is taken before the next
                   instruction, so leave the block for the loop to check */
                if (err == 0 && get_pending_irq_mask(s) != 0)
                    err = 2;
                if (rd != 0)
                    s->reg[rd] = val2;
                if (err > 0) {
                    s->pc = GET_PC() + 4;
                    if (err == 2)
                        JUMP_INSN;
                    else
                        goto done_interp;
                }
                break;
            case 2: /* csrrs */
            case 3: /* csrrc */
                s->insn_counter = GET_INSN_COUNTER();
                if (csr_read(s, &val2, imm, (rs1 != 0)))
                    goto illegal_insn;
                val2 = (intx_t)val2;
                if (rs1 != 0) {
                    if (funct3 == 2)
                        val = val2 | val;
                    else
                        val = val2 & ~val;
                    err = csr_write(s, imm, val);
                    if (err < 0)
                        goto illegal_insn;
                    if (err == 0 && get_pending_irq_mask(s) != 0)
                        err = 2;
                } else {
                    err = 0;
                }
                if (rd != 0)
                    s->reg[rd] = val2;
                if (err > 0) {
                    s->pc = GET_PC() + 4;
                    if (err == 2)
                        JUMP_INSN;
                    else
                        goto done_interp;
                }
                break;
            case 0:
                switch(imm) {
                case 0x000: /* ecall */
                    if (get_bits(insn, 7, 13) != 0)
                        goto illegal_insn;
                    s->pending_exception = CAUSE_USER_ECALL + s->priv;
                    goto exception;
                case 0x001: /* ebreak */
                    if (get_bits(insn, 7, 13) != 0)
                        goto illegal_insn;
                    s->pending_exception = CAUSE_BREAKPOINT;
                    goto exception;
                case 0x102: /* sret */
                    {
                        if (get_bits(insn, 7, 13) != 0)
                            goto illegal_insn;
                        if (s->priv < PRV_S)
                            goto illegal_insn;
                        if (s->priv < PRV_M && (s->mstatus & MSTATUS_TSR))
                            goto illegal_insn;
                        s->pc = GET_PC();
                        handle_sret(s);
                        goto done_interp;
                    }
                    break;
                case 0x302: /* mret */
                    {
                        if (get_bits(insn, 7, 13) != 0)
                            goto illegal_insn;
                        if (s->priv < PRV_M)
                            goto illegal_insn;
                        s->pc = GET_PC();
                        handle_mret(s);
                        goto done_interp;
                    }
                    break;
                case 0x105: /* wfi */
                    if (get_bits(insn, 7, 8) != 0)
                        goto illegal_insn;
                    if (s->priv == PRV_U)
                        goto illegal_insn;
                    if (s->priv < PRV_M && (s->mstatus & MSTATUS_TW))
                        goto illegal_insn;
                    /* go to power down if no enabled interrupts are
                       pending */
                    process_inbox(s);
                    if ((s->mip & s->mie) == 0) {
                        s->power_down_flag = true;
                        s->pc = GET_PC() + 4;
                        goto done_interp;
                    }
                    break;
                case 0x180: /* sfence.w.inval */
                case 0x181: /* sfence.inval.ir */
                    /* every invalidation here takes effect at once, so the
                       ordering these place around sinval.vma already holds */
                    if (get_bits(insn, 7, 13) != 0)
                        goto illegal_insn;
                    if (s->priv == PRV_U)
                        goto illegal_insn;
                    break;
                default:
                    /* sfence.vma and its sinval.vma counterpart */
                    if ((imm >> 5) == 0x09 || (imm >> 5) == 0x0b) {
                        if (get_bits(insn, 7, 8) != 0)
                            goto illegal_insn;
                        if (s->priv == PRV_U)
                            goto illegal_insn;
                        if (s->priv < PRV_M && (s->mstatus & MSTATUS_TVM))
                            goto illegal_insn;
                        if (rs1 == 0) {
                            tlb_flush_all(s);
                        } else {
                            tlb_flush_vaddr(s, s->reg[rs1]);
                        }
                        /* the current code TLB may have been flushed */
                        s->pc = GET_PC() + 4;
                        JUMP_INSN;
                    } else {
                        goto illegal_insn;
                    }
                    break;
                }
                break;
            default:
                goto illegal_insn;
            }
            NEXT_INSN;
        case 0x0f: /* misc-mem */
            funct3 = get_bits(insn, 12, 3);
            switch(funct3) {
            case 0: /* fence, fence.tso */
                fence(s, insn);
                break;
            case 1: /* fence.i */
                if (insn != 0x0000100f)
                    goto illegal_insn;
                break;
            case 2: /* lq */
                if constexpr (Xlen < 128) {
                    goto illegal_insn;
                } else {
                    imm = sign_extend(insn >> 20, 12);
                    addr = s->reg[rs1] + imm;
                    if (target_read<uintx_t>(s, &val, addr))
                        goto mmu_exception;
                    if (rd != 0)
                        s->reg[rd] = val;
                }
                break;
            default:
                goto illegal_insn;
            }
            NEXT_INSN;
        case 0x2f:
            funct3 = get_bits(insn, 12, 3);
            switch(funct3) {
            case 2:
                err = exec_amo<uint32_t>(s, insn, rs1, rs2, &val);
                break;
            case 3:
                if constexpr (Xlen < 64)
                    goto illegal_insn;
                err = exec_amo<uint64_t>(s, insn, rs1, rs2, &val);
                break;
            case 4:
                if constexpr (Xlen < 128)
                    goto illegal_insn;
                err = exec_amo<uintx_t>(s, insn, rs1, rs2, &val);
                break;
            default:
                goto illegal_insn;
            }
            if (unlikely(err != 0)) {
                if (err == INSN_ILLEGAL)
                    goto illegal_insn;
                goto mmu_exception;
            }
            if (rd != 0)
                s->reg[rd] = val;
            NEXT_INSN;
            /* FPU */
        case 0x07: /* fp load */
            if (s->fs == 0)
                goto illegal_insn;
            funct3 = get_bits(insn, 12, 3);
            imm = sign_extend(insn >> 20, 12);
            addr = s->reg[rs1] + imm;
            switch(funct3) {
            case 2: /* flw */
                {
                    uint32_t rval;
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    s->fp_reg[rd] = rval | fp_high<32>();
                }
                break;
            case 3: /* fld */
                {
                    uint64_t rval;
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    s->fp_reg[rd] = rval | fp_high<64>();
                }
                break;
            case 4: /* flq */
                if constexpr (FLEN < 128) {
                    goto illegal_insn;
                } else {
                    fp_uint rval;
                    if (target_read(s, &rval, addr))
                        goto mmu_exception;
                    s->fp_reg[rd] = rval;
                }
                break;
            default:
                goto illegal_insn;
            }
            s->fs = 3;
            NEXT_INSN;
        case 0x27: /* fp store */
            if (s->fs == 0)
                goto illegal_insn;
            funct3 = get_bits(insn, 12, 3);
            imm = sign_extend(rd | get_field1(insn, 25, 5, 11), 12);
            addr = s->reg[rs1] + imm;
            switch(funct3) {
            case 2: /* fsw */
                if (target_write<uint32_t>(s, addr, s->fp_reg[rs2]))
                    goto mmu_exception;
                break;
            case 3: /* fsd */
                if (target_write<uint64_t>(s, addr, s->fp_reg[rs2]))
                    goto mmu_exception;
                break;
            case 4: /* fsq */
                if constexpr (FLEN < 128)
                    goto illegal_insn;
                if (target_write<fp_uint>(s, addr, s->fp_reg[rs2]))
                    goto mmu_exception;
                break;
            default:
                goto illegal_insn;
            }
            NEXT_INSN;
        case 0x43: /* fmadd */
            if (s->fs == 0)
                goto illegal_insn;
            funct3 = get_bits(insn, 25, 2);
            rs3 = get_bits(insn, 27, 5);
            rm = get_insn_rm(s, get_bits(insn, 12, 3));
            if (rm < 0)
                goto illegal_insn;
            switch(funct3) {
            case 0:
                s->fp_reg[rd] = fma_sf32(s->fp_reg[rs1], s->fp_reg[rs2],
                                         s->fp_reg[rs3], RM(rm), &s->fflags) | fp_high<32>();
                break;
            case 1:
                s->fp_reg[rd] = fma_sf64(s->fp_reg[rs1], s->fp_reg[rs2],
                                         s->fp_reg[rs3], RM(rm), &s->fflags) | fp_high<64>();
                break;
            case 3:
                if constexpr (FLEN < 128) {
                    goto illegal_insn;
                } else {
                    typedef SoftFloat<FLEN> SF;
                    s->fp_reg[rd] = SF::fma(s->fp_reg[rs1], s->fp_reg[rs2],
                                            s->fp_reg[rs3], RM(rm),
                                            &s->fflags);
                }
                break;
            default:
                goto illegal_insn;
            }
            s->fs = 3;
            NEXT_INSN;
        case 0x47: /* fmsub */
            if (s->fs == 0)
                goto illegal_insn;
            funct3 = get_bits(insn, 25, 2);
            rs3 = get_bits(insn, 27, 5);
            rm = get_insn_rm(s, get_bits(insn, 12, 3));
            if (rm < 0)
                goto illegal_insn;
            switch(funct3) {
            case 0:
                s->fp_reg[rd] = fma_sf32(s->fp_reg[rs1],
                                         s->fp_reg[rs2],
                                         s->fp_reg[rs3] ^ FSIGN_MASK32,
                                         RM(rm), &s->fflags) | fp_high<32>();
                break;
            case 1:
                s->fp_reg[rd] = fma_sf64(s->fp_reg[rs1],
                                         s->fp_reg[rs2],
                                         s->fp_reg[rs3] ^ FSIGN_MASK64,
                                         RM(rm), &s->fflags) | fp_high<64>();
                break;
            case 3:
                if constexpr (FLEN < 128) {
                    goto illegal_insn;
                } else {
                    typedef SoftFloat<FLEN> SF;
                    s->fp_reg[rd] = SF::fma(s->fp_reg[rs1],
                                            s->fp_reg[rs2],
                                            s->fp_reg[rs3] ^ SF::sign_mask,
                                            RM(rm), &s->fflags);
                }
                break;
            default:
                goto illegal_insn;
            }
            s->fs = 3;
            NEXT_INSN;
        case 0x4b: /* fnmsub */
            if (s->fs == 0)
                goto illegal_insn;
            funct3 = get_bits(insn, 25, 2);
            rs3 = get_bits(insn, 27, 5);
            rm = get_insn_rm(s, get_bits(insn, 12, 3));
            if (rm < 0)
                goto illegal_insn;
            switch(funct3) {
            case 0:
                s->fp_reg[rd] = fma_sf32(s->fp_reg[rs1] ^ FSIGN_MASK32,
                                         s->fp_reg[rs2],
                                         s->fp_reg[rs3],
                                         RM(rm), &s->fflags) | fp_high<32>();
                break;
            case 1:
                s->fp_reg[rd] = fma_sf64(s->fp_reg[rs1] ^ FSIGN_MASK64,
                                         s->fp_reg[rs2],
                                         s->fp_reg[rs3],
                                         RM(rm), &s->fflags) | fp_high<64>();
                break;
            case 3:
                if constexpr (FLEN < 128) {
                    goto illegal_insn;
                } else {
                    typedef SoftFloat<FLEN> SF;
                    s->fp_reg[rd] = SF::fma(s->fp_reg[rs1] ^ SF::sign_mask,
                                            s->fp_reg[rs2],
                                            s->fp_reg[rs3],
                                            RM(rm), &s->fflags);
                }
                break;
            default:
                goto illegal_insn;
            }
            s->fs = 3;
            NEXT_INSN;
        case 0x4f: /* fnmadd */
            if (s->fs == 0)
                goto illegal_insn;
            funct3 = get_bits(insn, 25, 2);
            rs3 = get_bits(insn, 27, 5);
            rm = get_insn_rm(s, get_bits(insn, 12, 3));
            if (rm < 0)
                goto illegal_insn;
            switch(funct3) {
            case 0:
                s->fp_reg[rd] = fma_sf32(s->fp_reg[rs1] ^ FSIGN_MASK32,
                                         s->fp_reg[rs2],
                                         s->fp_reg[rs3] ^ FSIGN_MASK32,
                                         RM(rm), &s->fflags) | fp_high<32>();
                break;
            case 1:
                s->fp_reg[rd] = fma_sf64(s->fp_reg[rs1] ^ FSIGN_MASK64,
                                         s->fp_reg[rs2],
                                         s->fp_reg[rs3] ^ FSIGN_MASK64,
                                         RM(rm), &s->fflags) | fp_high<64>();
                break;
            case 3:
                if constexpr (FLEN < 128) {
                    goto illegal_insn;
                } else {
                    typedef SoftFloat<FLEN> SF;
                    s->fp_reg[rd] = SF::fma(s->fp_reg[rs1] ^ SF::sign_mask,
                                            s->fp_reg[rs2],
                                            s->fp_reg[rs3] ^ SF::sign_mask,
                                            RM(rm), &s->fflags);
                }
                break;
            default:
                goto illegal_insn;
            }
            s->fs = 3;
            NEXT_INSN;
        case 0x53:
            if (s->fs == 0)
                goto illegal_insn;
            imm = get_bits(insn, 25, 7);
            rm = get_bits(insn, 12, 3);
            /* the low bits of funct7 are the format */
            switch(imm & 3) {
            case 0:
                err = exec_fp_op<32, Xlen>(s, imm >> 2, rd, rs1, rs2, rm);
                break;
            case 1:
                err = exec_fp_op<64, Xlen>(s, imm >> 2, rd, rs1, rs2, rm);
                break;
            case 3:
                if constexpr (FLEN < 128)
                    goto illegal_insn;
                else
                    err = exec_fp_op<FLEN, Xlen>(s, imm >> 2, rd, rs1, rs2, rm);
                break;
            default:
                goto illegal_insn;
            }
            if (err != 0)
                goto illegal_insn;
            NEXT_INSN;
        default:
            goto illegal_insn;
        }
        /* update PC for next instruction */
    jump_insn: ;
    } /* end of main loop */
 illegal_insn:
    s->pending_exception = CAUSE_ILLEGAL_INSTRUCTION;
    s->pending_tval = insn;
 mmu_exception:
 exception:
    s->pc = GET_PC();
    if (s->pending_exception >= 0) {
        /* Note: the idea is that one exception counts for one cycle. */
        s->n_cycles--; 
        raise_exception2(s, s->pending_exception, s->pending_tval);
    }
    /* we exit because XLEN may have changed */
 done_interp:
the_end:
    s->insn_counter = GET_INSN_COUNTER();
#if 0
    printf("done interp %lx int=%x mstatus=%lx prv=%d\n",
           (uint64_t)s->insn_counter, s->mip & s->mie, (uint64_t)s->mstatus,
           s->priv);
#endif
}
