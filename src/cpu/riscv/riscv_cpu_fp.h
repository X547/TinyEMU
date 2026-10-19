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

/* softfp's functions for the format of F bits */
template <int F> struct SoftFloat;

template <> struct SoftFloat<32> {
    static constexpr auto sign_mask = FSIGN_MASK32;
    static constexpr auto add = add_sf32;
    static constexpr auto sub = sub_sf32;
    static constexpr auto mul = mul_sf32;
    static constexpr auto div = div_sf32;
    static constexpr auto sqrt = sqrt_sf32;
    static constexpr auto fma = fma_sf32;
    static constexpr auto min = min_sf32;
    static constexpr auto max = max_sf32;
    static constexpr auto eq_quiet = eq_quiet_sf32;
    static constexpr auto le = le_sf32;
    static constexpr auto lt = lt_sf32;
    static constexpr auto fclass = fclass_sf32;
    static constexpr auto to_i32 = cvt_sf32_i32;
    static constexpr auto to_u32 = cvt_sf32_u32;
    static constexpr auto to_i64 = cvt_sf32_i64;
    static constexpr auto to_u64 = cvt_sf32_u64;
    static constexpr auto from_i32 = cvt_i32_sf32;
    static constexpr auto from_u32 = cvt_u32_sf32;
    static constexpr auto from_i64 = cvt_i64_sf32;
    static constexpr auto from_u64 = cvt_u64_sf32;
    static constexpr auto from_sf64 = cvt_sf64_sf32;
#ifdef HAVE_INT128
    static constexpr auto to_i128 = cvt_sf32_i128;
    static constexpr auto to_u128 = cvt_sf32_u128;
    static constexpr auto from_i128 = cvt_i128_sf32;
    static constexpr auto from_u128 = cvt_u128_sf32;
    static constexpr auto from_sf128 = cvt_sf128_sf32;
#endif
};

template <> struct SoftFloat<64> {
    static constexpr auto sign_mask = FSIGN_MASK64;
    static constexpr auto add = add_sf64;
    static constexpr auto sub = sub_sf64;
    static constexpr auto mul = mul_sf64;
    static constexpr auto div = div_sf64;
    static constexpr auto sqrt = sqrt_sf64;
    static constexpr auto fma = fma_sf64;
    static constexpr auto min = min_sf64;
    static constexpr auto max = max_sf64;
    static constexpr auto eq_quiet = eq_quiet_sf64;
    static constexpr auto le = le_sf64;
    static constexpr auto lt = lt_sf64;
    static constexpr auto fclass = fclass_sf64;
    static constexpr auto to_i32 = cvt_sf64_i32;
    static constexpr auto to_u32 = cvt_sf64_u32;
    static constexpr auto to_i64 = cvt_sf64_i64;
    static constexpr auto to_u64 = cvt_sf64_u64;
    static constexpr auto from_i32 = cvt_i32_sf64;
    static constexpr auto from_u32 = cvt_u32_sf64;
    static constexpr auto from_i64 = cvt_i64_sf64;
    static constexpr auto from_u64 = cvt_u64_sf64;
    static constexpr auto from_sf32 = cvt_sf32_sf64;
#ifdef HAVE_INT128
    static constexpr auto to_i128 = cvt_sf64_i128;
    static constexpr auto to_u128 = cvt_sf64_u128;
    static constexpr auto from_i128 = cvt_i128_sf64;
    static constexpr auto from_u128 = cvt_u128_sf64;
    static constexpr auto from_sf128 = cvt_sf128_sf64;
#endif
};

#ifdef HAVE_INT128
template <> struct SoftFloat<128> {
    static constexpr auto sign_mask = FSIGN_MASK128;
    static constexpr auto add = add_sf128;
    static constexpr auto sub = sub_sf128;
    static constexpr auto mul = mul_sf128;
    static constexpr auto div = div_sf128;
    static constexpr auto sqrt = sqrt_sf128;
    static constexpr auto fma = fma_sf128;
    static constexpr auto min = min_sf128;
    static constexpr auto max = max_sf128;
    static constexpr auto eq_quiet = eq_quiet_sf128;
    static constexpr auto le = le_sf128;
    static constexpr auto lt = lt_sf128;
    static constexpr auto fclass = fclass_sf128;
    static constexpr auto to_i32 = cvt_sf128_i32;
    static constexpr auto to_u32 = cvt_sf128_u32;
    static constexpr auto to_i64 = cvt_sf128_i64;
    static constexpr auto to_u64 = cvt_sf128_u64;
    static constexpr auto from_i32 = cvt_i32_sf128;
    static constexpr auto from_u32 = cvt_u32_sf128;
    static constexpr auto from_i64 = cvt_i64_sf128;
    static constexpr auto from_u64 = cvt_u64_sf128;
    static constexpr auto to_i128 = cvt_sf128_i128;
    static constexpr auto to_u128 = cvt_sf128_u128;
    static constexpr auto from_i128 = cvt_i128_sf128;
    static constexpr auto from_u128 = cvt_u128_sf128;
    static constexpr auto from_sf32 = cvt_sf32_sf128;
    static constexpr auto from_sf64 = cvt_sf64_sf128;
};
#endif

/* OP-FP of the format of F bits: 'funct5' is the operation, 'rm' the rounding
   mode field. Returns 0, or INSN_ILLEGAL. */
template <int MaxXlen>
template <int F, int Xlen>
int RISCVCPUState<MaxXlen>::exec_fp_op(RISCVCPUState *s, uint32_t funct5,
                                       uint32_t rd, uint32_t rs1,
                                       uint32_t rs2, int32_t rm)
{
    typedef SoftFloat<F> SF;
    constexpr fp_uint high = fp_high<F>();
    target_ulong val;

    switch(funct5) {
    case 0x00:
        rm = get_insn_rm(s, rm);
        if (rm < 0)
            return INSN_ILLEGAL;
        s->fp_reg[rd] = SF::add(s->fp_reg[rs1], s->fp_reg[rs2],
                                RM(rm), &s->fflags) | high;
        s->fs = 3;
        break;
    case 0x01:
        rm = get_insn_rm(s, rm);
        if (rm < 0)
            return INSN_ILLEGAL;
        s->fp_reg[rd] = SF::sub(s->fp_reg[rs1], s->fp_reg[rs2],
                                RM(rm), &s->fflags) | high;
        s->fs = 3;
        break;
    case 0x02:
        rm = get_insn_rm(s, rm);
        if (rm < 0)
            return INSN_ILLEGAL;
        s->fp_reg[rd] = SF::mul(s->fp_reg[rs1], s->fp_reg[rs2],
                                RM(rm), &s->fflags) | high;
        s->fs = 3;
        break;
    case 0x03:
        rm = get_insn_rm(s, rm);
        if (rm < 0)
            return INSN_ILLEGAL;
        s->fp_reg[rd] = SF::div(s->fp_reg[rs1], s->fp_reg[rs2],
                                RM(rm), &s->fflags) | high;
        s->fs = 3;
        break;
    case 0x0b:
        rm = get_insn_rm(s, rm);
        if (rm < 0 || rs2 != 0)
            return INSN_ILLEGAL;
        s->fp_reg[rd] = SF::sqrt(s->fp_reg[rs1], RM(rm), &s->fflags) | high;
        s->fs = 3;
        break;
    case 0x04:
        switch(rm) {
        case 0: /* fsgnj */
            s->fp_reg[rd] = (s->fp_reg[rs1] & ~SF::sign_mask) |
                (s->fp_reg[rs2] & SF::sign_mask);
            break;
        case 1: /* fsgnjn */
            s->fp_reg[rd] = (s->fp_reg[rs1] & ~SF::sign_mask) |
                ((s->fp_reg[rs2] & SF::sign_mask) ^ SF::sign_mask);
            break;
        case 2: /* fsgnjx */
            s->fp_reg[rd] = s->fp_reg[rs1] ^
                (s->fp_reg[rs2] & SF::sign_mask);
            break;
        default:
            return INSN_ILLEGAL;
        }
        s->fs = 3;
        break;
    case 0x05:
        switch(rm) {
        case 0: /* fmin */
            s->fp_reg[rd] = SF::min(s->fp_reg[rs1], s->fp_reg[rs2],
                                    &s->fflags, FMINMAX_IEEE754_201X) | high;
            break;
        case 1: /* fmax */
            s->fp_reg[rd] = SF::max(s->fp_reg[rs1], s->fp_reg[rs2],
                                    &s->fflags, FMINMAX_IEEE754_201X) | high;
            break;
        default:
            return INSN_ILLEGAL;
        }
        s->fs = 3;
        break;
    case 0x18:
        rm = get_insn_rm(s, rm);
        if (rm < 0)
            return INSN_ILLEGAL;
        switch(rs2) {
        case 0: /* fcvt.w.[sdq] */
            val = (int32_t)SF::to_i32(s->fp_reg[rs1], RM(rm), &s->fflags);
            break;
        case 1: /* fcvt.wu.[sdq] */
            val = (int32_t)SF::to_u32(s->fp_reg[rs1], RM(rm), &s->fflags);
            break;
        case 2: /* fcvt.l.[sdq] */
            if constexpr (Xlen < 64)
                return INSN_ILLEGAL;
            val = (int64_t)SF::to_i64(s->fp_reg[rs1], RM(rm), &s->fflags);
            break;
        case 3: /* fcvt.lu.[sdq] */
            if constexpr (Xlen < 64)
                return INSN_ILLEGAL;
            val = (int64_t)SF::to_u64(s->fp_reg[rs1], RM(rm), &s->fflags);
            break;
        /* XXX: the index is not defined in the spec */
        case 4: /* fcvt.t.[sdq] */
            if constexpr (Xlen < 128)
                return INSN_ILLEGAL;
            else
                val = SF::to_i128(s->fp_reg[rs1], RM(rm), &s->fflags);
            break;
        case 5: /* fcvt.tu.[sdq] */
            if constexpr (Xlen < 128)
                return INSN_ILLEGAL;
            else
                val = SF::to_u128(s->fp_reg[rs1], RM(rm), &s->fflags);
            break;
        default:
            return INSN_ILLEGAL;
        }
        if (rd != 0)
            s->reg[rd] = val;
        break;
    case 0x14:
        switch(rm) {
        case 0: /* fle */
            val = SF::le(s->fp_reg[rs1], s->fp_reg[rs2], &s->fflags);
            break;
        case 1: /* flt */
            val = SF::lt(s->fp_reg[rs1], s->fp_reg[rs2], &s->fflags);
            break;
        case 2: /* feq */
            val = SF::eq_quiet(s->fp_reg[rs1], s->fp_reg[rs2], &s->fflags);
            break;
        default:
            return INSN_ILLEGAL;
        }
        if (rd != 0)
            s->reg[rd] = val;
        break;
    case 0x1a:
        rm = get_insn_rm(s, rm);
        if (rm < 0)
            return INSN_ILLEGAL;
        switch(rs2) {
        case 0: /* fcvt.[sdq].w */
            s->fp_reg[rd] = SF::from_i32(s->reg[rs1], RM(rm),
                                         &s->fflags) | high;
            break;
        case 1: /* fcvt.[sdq].wu */
            s->fp_reg[rd] = SF::from_u32(s->reg[rs1], RM(rm),
                                         &s->fflags) | high;
            break;
        case 2: /* fcvt.[sdq].l */
            if constexpr (Xlen < 64)
                return INSN_ILLEGAL;
            s->fp_reg[rd] = SF::from_i64(s->reg[rs1], RM(rm),
                                         &s->fflags) | high;
            break;
        case 3: /* fcvt.[sdq].lu */
            if constexpr (Xlen < 64)
                return INSN_ILLEGAL;
            s->fp_reg[rd] = SF::from_u64(s->reg[rs1], RM(rm),
                                         &s->fflags) | high;
            break;
        /* XXX: the index is not defined in the spec */
        case 4: /* fcvt.[sdq].t */
            if constexpr (Xlen < 128)
                return INSN_ILLEGAL;
            else
                s->fp_reg[rd] = SF::from_i128(s->reg[rs1], RM(rm),
                                              &s->fflags) | high;
            break;
        case 5: /* fcvt.[sdq].tu */
            if constexpr (Xlen < 128)
                return INSN_ILLEGAL;
            else
                s->fp_reg[rd] = SF::from_u128(s->reg[rs1], RM(rm),
                                              &s->fflags) | high;
            break;
        default:
            return INSN_ILLEGAL;
        }
        s->fs = 3;
        break;
    case 0x08:
        rm = get_insn_rm(s, rm);
        if (rm < 0)
            return INSN_ILLEGAL;
        if constexpr (F == 32) {
            switch(rs2) {
            case 1: /* cvt.s.d */
                s->fp_reg[rd] = SF::from_sf64(s->fp_reg[rs1], RM(rm),
                                              &s->fflags) | high;
                break;
            case 3: /* cvt.s.q */
                if constexpr (FLEN < 128)
                    return INSN_ILLEGAL;
                else
                    s->fp_reg[rd] = SF::from_sf128(s->fp_reg[rs1], RM(rm),
                                                   &s->fflags) | high;
                break;
            default:
                return INSN_ILLEGAL;
            }
        } else if constexpr (F == 64) {
            switch(rs2) {
            case 0: /* cvt.d.s */
                s->fp_reg[rd] = SF::from_sf32(s->fp_reg[rs1],
                                              &s->fflags) | high;
                break;
            case 1: /* cvt.d.q */
                if constexpr (FLEN < 128)
                    return INSN_ILLEGAL;
                else
                    s->fp_reg[rd] = SF::from_sf128(s->fp_reg[rs1], RM(rm),
                                                   &s->fflags) | high;
                break;
            default:
                return INSN_ILLEGAL;
            }
        } else {
            switch(rs2) {
            case 0: /* cvt.q.s */
                s->fp_reg[rd] = SF::from_sf32(s->fp_reg[rs1], &s->fflags);
                break;
            case 1: /* cvt.q.d */
                s->fp_reg[rd] = SF::from_sf64(s->fp_reg[rs1], &s->fflags);
                break;
            default:
                return INSN_ILLEGAL;
            }
        }
        s->fs = 3;
        break;
    case 0x1c:
        if (rs2 != 0)
            return INSN_ILLEGAL;
        switch(rm) {
        case 0: /* fmv.x.s */
            if constexpr (F > Xlen)
                return INSN_ILLEGAL;
            val = (intn_t<F>)s->fp_reg[rs1];
            break;
        case 1: /* fclass */
            val = SF::fclass(s->fp_reg[rs1]);
            break;
        default:
            return INSN_ILLEGAL;
        }
        if (rd != 0)
            s->reg[rd] = val;
        break;
    case 0x1e: /* fmv.s.x */
        if constexpr (F > Xlen)
            return INSN_ILLEGAL;
        if (rs2 != 0 || rm != 0)
            return INSN_ILLEGAL;
        s->fp_reg[rd] = (intn_t<F>)s->reg[rs1];
        s->fs = 3;
        break;
    default:
        return INSN_ILLEGAL;
    }
    return 0;
}
