/*
 * x86 CPU emulator: MMX, SSE and SSE2
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
#include <limits>
#include <type_traits>

#include "softfp.h"
#include "x86_cpu_priv.h"

/* Integer operations are written once for the 8 byte MMX and the 16 byte
   XMM registers, floating point ones once for single and double precision,
   packed and scalar. The arithmetic is softfp's IEEE 754 with the x86 rules
   for NaN operands, DAZ and FTZ laid over it. An unmasked floating point
   exception leaves the destination unchanged. */

namespace {

/* Traits of the floating point formats, as their bit patterns. */
struct F32 {
    typedef uint32_t Bits;
    static constexpr int SIZE = SIZE32;
    static constexpr int MANT_BITS = 23;
    static constexpr Bits SIGN = UINT32_C(0x80000000);
    static constexpr Bits EXP = UINT32_C(0x7f800000);
    static constexpr Bits MANT = UINT32_C(0x007fffff);
    static constexpr Bits QUIET = UINT32_C(0x00400000);

    static Bits add(Bits a, Bits b, RoundingModeEnum rm, uint32_t *fl)
    {
        return add_sf32(a, b, rm, fl);
    }
    static Bits sub(Bits a, Bits b, RoundingModeEnum rm, uint32_t *fl)
    {
        return sub_sf32(a, b, rm, fl);
    }
    static Bits mul(Bits a, Bits b, RoundingModeEnum rm, uint32_t *fl)
    {
        return mul_sf32(a, b, rm, fl);
    }
    static Bits div(Bits a, Bits b, RoundingModeEnum rm, uint32_t *fl)
    {
        return div_sf32(a, b, rm, fl);
    }
    static Bits sqrt(Bits a, RoundingModeEnum rm, uint32_t *fl)
    {
        return sqrt_sf32(a, rm, fl);
    }
    static bool lt(Bits a, Bits b, uint32_t *fl)
    {
        return lt_sf32(a, b, fl);
    }
    static bool eq(Bits a, Bits b, uint32_t *fl)
    {
        return eq_quiet_sf32(a, b, fl);
    }
    static int32_t to_i32(Bits a, RoundingModeEnum rm, uint32_t *fl)
    {
        return cvt_sf32_i32(a, rm, fl);
    }
    static int64_t to_i64(Bits a, RoundingModeEnum rm, uint32_t *fl)
    {
        return cvt_sf32_i64(a, rm, fl);
    }
    static Bits from_i32(int32_t a, RoundingModeEnum rm, uint32_t *fl)
    {
        return cvt_i32_sf32(a, rm, fl);
    }
    static Bits from_i64(int64_t a, RoundingModeEnum rm, uint32_t *fl)
    {
        return cvt_i64_sf32(a, rm, fl);
    }
};

struct F64 {
    typedef uint64_t Bits;
    static constexpr int SIZE = SIZE64;
    static constexpr int MANT_BITS = 52;
    static constexpr Bits SIGN = UINT64_C(0x8000000000000000);
    static constexpr Bits EXP = UINT64_C(0x7ff0000000000000);
    static constexpr Bits MANT = UINT64_C(0x000fffffffffffff);
    static constexpr Bits QUIET = UINT64_C(0x0008000000000000);

    static Bits add(Bits a, Bits b, RoundingModeEnum rm, uint32_t *fl)
    {
        return add_sf64(a, b, rm, fl);
    }
    static Bits sub(Bits a, Bits b, RoundingModeEnum rm, uint32_t *fl)
    {
        return sub_sf64(a, b, rm, fl);
    }
    static Bits mul(Bits a, Bits b, RoundingModeEnum rm, uint32_t *fl)
    {
        return mul_sf64(a, b, rm, fl);
    }
    static Bits div(Bits a, Bits b, RoundingModeEnum rm, uint32_t *fl)
    {
        return div_sf64(a, b, rm, fl);
    }
    static Bits sqrt(Bits a, RoundingModeEnum rm, uint32_t *fl)
    {
        return sqrt_sf64(a, rm, fl);
    }
    static bool lt(Bits a, Bits b, uint32_t *fl)
    {
        return lt_sf64(a, b, fl);
    }
    static bool eq(Bits a, Bits b, uint32_t *fl)
    {
        return eq_quiet_sf64(a, b, fl);
    }
    static int32_t to_i32(Bits a, RoundingModeEnum rm, uint32_t *fl)
    {
        return cvt_sf64_i32(a, rm, fl);
    }
    static int64_t to_i64(Bits a, RoundingModeEnum rm, uint32_t *fl)
    {
        return cvt_sf64_i64(a, rm, fl);
    }
    static Bits from_i32(int32_t a, RoundingModeEnum rm, uint32_t *fl)
    {
        return cvt_i32_sf64(a, rm, fl);
    }
    static Bits from_i64(int64_t a, RoundingModeEnum rm, uint32_t *fl)
    {
        return cvt_i64_sf64(a, rm, fl);
    }
};

#ifdef HAVE_INT128
struct F128 {
    typedef uint128_t Bits;
    static constexpr int MANT_BITS = 112;
    static constexpr Bits MANT = ((Bits)1 << MANT_BITS) - 1;
    static constexpr Bits EXP = ((Bits)0x7fff) << MANT_BITS;

    static Bits add(Bits a, Bits b, RoundingModeEnum rm, uint32_t *fl)
    {
        return add_sf128(a, b, rm, fl);
    }
    static Bits sub(Bits a, Bits b, RoundingModeEnum rm, uint32_t *fl)
    {
        return sub_sf128(a, b, rm, fl);
    }
    static Bits mul(Bits a, Bits b, RoundingModeEnum rm, uint32_t *fl)
    {
        return mul_sf128(a, b, rm, fl);
    }
    static Bits div(Bits a, Bits b, RoundingModeEnum rm, uint32_t *fl)
    {
        return div_sf128(a, b, rm, fl);
    }
};
#endif

/* A format wide enough to hold any result of F's operations with an
   unbounded exponent, and the exact conversion into it. */
template <typename F>
struct Wider {
    typedef void Type;
};

template <>
struct Wider<F32> {
    typedef F64 Type;
    static uint64_t widen(uint32_t a)
    {
        uint32_t fflags = 0;
        return cvt_sf32_sf64(a, &fflags);
    }
};

#ifdef HAVE_INT128
template <>
struct Wider<F64> {
    typedef F128 Type;
    static uint128_t widen(uint64_t a)
    {
        uint32_t fflags = 0;
        return cvt_sf64_sf128(a, &fflags);
    }
};
#endif

/* The MXCSR controls one instruction runs under, and the exception flags
   its lanes raise. */
struct SimdFp {
    RoundingModeEnum rm;
    bool daz;
    bool ftz;
    bool overflow_masked;
    bool underflow_masked;
    uint32_t flags;

    explicit SimdFp(const X86CPUState *s)
    {
        static const RoundingModeEnum modes[4] = {
            RM_RNE, RM_RDN, RM_RUP, RM_RTZ
        };
        rm = modes[get_bits(s->mxcsr, MXCSR_RC, 2)];
        daz = get_bit(s->mxcsr, MXCSR_DAZ);
        ftz = get_bit(s->mxcsr, MXCSR_FZ);
        overflow_masked = get_bit(s->mxcsr, MXCSR_OM);
        underflow_masked = get_bit(s->mxcsr, MXCSR_UM);
        flags = 0;
    }
};

enum {
    SHIFT_LEFT,
    SHIFT_RIGHT,
    SHIFT_ARITH,
};

const int X87_TOP = 11; /* the TOP field of the x87 status word */

}


//#pragma mark - lanes

template <typename T, int N>
constexpr int lane_count = N / sizeof(T);

template <typename T, int N>
static inline T lane(const VecReg<N> &v, int i)
{
    T val;
    memcpy(&val, v.bytes + i * sizeof(T), sizeof(T));
    return val;
}

template <typename T, int N>
static inline void set_lane(VecReg<N> &v, int i, T val)
{
    memcpy(v.bytes + i * sizeof(T), &val, sizeof(T));
}

template <typename T, int N, typename Fn>
static inline VecReg<N> map_lanes(const VecReg<N> &a, const VecReg<N> &b,
                                  Fn fn)
{
    VecReg<N> r;
    for (int i = 0; i < lane_count<T, N>; i++) {
        set_lane<T>(r, i, (T)fn(lane<T>(a, i), lane<T>(b, i)));
    }
    return r;
}

template <typename T>
static inline T saturate(int64_t val)
{
    if (val < std::numeric_limits<T>::min()) {
        return std::numeric_limits<T>::min();
    }
    if (val > std::numeric_limits<T>::max()) {
        return std::numeric_limits<T>::max();
    }
    return val;
}

/* PUNPCKL and PUNPCKH: the lanes of one half of both operands, taken in
   turn. */
template <typename T, int N>
static VecReg<N> unpack(const VecReg<N> &a, const VecReg<N> &b, bool high)
{
    const int n = lane_count<T, N>;
    int base = high ? n / 2 : 0;
    VecReg<N> r = {}; /* empty for quadwords in an MMX register */
    for (int i = 0; i < n / 2; i++) {
        set_lane<T>(r, 2 * i, lane<T>(a, base + i));
        set_lane<T>(r, 2 * i + 1, lane<T>(b, base + i));
    }
    return r;
}

/* PACKSS and PACKUS: the lanes of both operands narrowed with
   saturation. */
template <typename From, typename To, int N>
static VecReg<N> pack(const VecReg<N> &a, const VecReg<N> &b)
{
    const int n = lane_count<From, N>;
    VecReg<N> r;
    for (int i = 0; i < n; i++) {
        set_lane<To>(r, i, saturate<To>(lane<From>(a, i)));
        set_lane<To>(r, n + i, saturate<To>(lane<From>(b, i)));
    }
    return r;
}

/* A count beyond the lane width clears it, or fills it with the sign. */
template <typename T, int N>
static VecReg<N> shift_lanes(const VecReg<N> &a, uint64_t count, int kind)
{
    typedef std::make_signed_t<T> S;
    const int bits = 8 * sizeof(T);
    VecReg<N> r;
    for (int i = 0; i < lane_count<T, N>; i++) {
        T val = lane<T>(a, i);
        T res;
        if (kind == SHIFT_ARITH) {
            res = (S)val >> (count >= (uint64_t)bits ? bits - 1 : (int)count);
        } else if (count >= (uint64_t)bits) {
            res = 0;
        } else {
            res = kind == SHIFT_LEFT ? val << count : val >> count;
        }
        set_lane<T>(r, i, res);
    }
    return r;
}

/* PSLLDQ and PSRLDQ. */
static XmmReg shift_bytes(const XmmReg &a, unsigned count, bool left)
{
    XmmReg r = {};
    for (int i = 0; i < 16; i++) {
        int from = left ? i - (int)count : i + (int)count;
        if (count < 16 && from >= 0 && from < 16) {
            r.bytes[i] = a.bytes[from];
        }
    }
    return r;
}

/* PSHUFW, PSHUFD, PSHUFLW and PSHUFHW: each of the four lanes from 'first'
   on picked among them by two bits of the immediate. */
template <typename T, int N>
static VecReg<N> shuffle4(const VecReg<N> &src, uint8_t imm, int first)
{
    VecReg<N> r = src;
    for (int i = 0; i < 4; i++) {
        int sel = get_bits(imm, 2 * i, 2);
        set_lane<T>(r, first + i, lane<T>(src, first + sel));
    }
    return r;
}

/* SHUFPS and SHUFPD: the low half of the lanes picked from 'a', the high
   half from 'b'. */
template <typename T>
static XmmReg shuffle_pair(const XmmReg &a, const XmmReg &b, uint8_t imm)
{
    const int n = lane_count<T, 16>;
    const int sel_bits = n == 4 ? 2 : 1;
    XmmReg r;
    for (int i = 0; i < n; i++) {
        int sel = get_bits(imm, i * sel_bits, sel_bits);
        set_lane<T>(r, i, lane<T>(i < n / 2 ? a : b, sel));
    }
    return r;
}

/* MOVMSKPS, MOVMSKPD and PMOVMSKB. */
template <typename T, int N>
static uint32_t sign_mask(const VecReg<N> &v)
{
    uint32_t mask = 0;
    for (int i = 0; i < lane_count<T, N>; i++) {
        mask = set_bit(mask, i, get_bit(lane<T>(v, i), 8 * sizeof(T) - 1));
    }
    return mask;
}

/* PSADBW: per 8 bytes, the sum of their absolute differences. */
template <int N>
static VecReg<N> sum_abs_diff(const VecReg<N> &a, const VecReg<N> &b)
{
    VecReg<N> r;
    for (int q = 0; q < N / 8; q++) {
        uint64_t sum = 0;
        for (int i = 8 * q; i < 8 * q + 8; i++) {
            int diff = lane<uint8_t>(a, i) - lane<uint8_t>(b, i);
            sum += diff < 0 ? -diff : diff;
        }
        set_lane<uint64_t>(r, q, sum);
    }
    return r;
}

/* PMADDWD: pairs of signed word products added into doublewords. */
template <int N>
static VecReg<N> multiply_add(const VecReg<N> &a, const VecReg<N> &b)
{
    VecReg<N> r;
    for (int i = 0; i < lane_count<int32_t, N>; i++) {
        int64_t sum = 0;
        for (int j = 2 * i; j < 2 * i + 2; j++) {
            sum += (int32_t)lane<int16_t>(a, j) * lane<int16_t>(b, j);
        }
        set_lane<uint32_t>(r, i, sum);
    }
    return r;
}

/* PMULUDQ: the even doublewords multiplied into quadwords. */
template <int N>
static VecReg<N> multiply_even(const VecReg<N> &a, const VecReg<N> &b)
{
    VecReg<N> r;
    for (int i = 0; i < lane_count<uint64_t, N>; i++) {
        set_lane<uint64_t>(r, i, (uint64_t)lane<uint32_t>(a, 2 * i) *
                           lane<uint32_t>(b, 2 * i));
    }
    return r;
}

/* The integer operations of the form 'mm, mm/m64' and 'xmm, xmm/m128', by
   the byte after 0F. */
template <int N>
static VecReg<N> int_op(uint8_t opcode, const VecReg<N> &a,
                        const VecReg<N> &b)
{
    auto add = [](auto x, auto y) { return x + y; };
    auto sub = [](auto x, auto y) { return x - y; };
    auto add_sat = [](auto x, auto y) {
        return saturate<decltype(x)>((int64_t)x + y);
    };
    auto sub_sat = [](auto x, auto y) {
        return saturate<decltype(x)>((int64_t)x - y);
    };
    auto min = [](auto x, auto y) { return x < y ? x : y; };
    auto max = [](auto x, auto y) { return x > y ? x : y; };
    auto avg = [](auto x, auto y) { return (x + y + 1) >> 1; };
    auto eq = [](auto x, auto y) { return x == y ? -1 : 0; };
    auto gt = [](auto x, auto y) { return x > y ? -1 : 0; };
    auto mul_high = [](auto x, auto y) { return ((int64_t)x * y) >> 16; };
    uint64_t count = lane<uint64_t>(b, 0);

    switch (opcode) {
    case 0x60: return unpack<uint8_t>(a, b, false);
    case 0x61: return unpack<uint16_t>(a, b, false);
    case 0x62: return unpack<uint32_t>(a, b, false);
    case 0x63: return pack<int16_t, int8_t>(a, b);
    case 0x64: return map_lanes<int8_t>(a, b, gt);
    case 0x65: return map_lanes<int16_t>(a, b, gt);
    case 0x66: return map_lanes<int32_t>(a, b, gt);
    case 0x67: return pack<int16_t, uint8_t>(a, b);
    case 0x68: return unpack<uint8_t>(a, b, true);
    case 0x69: return unpack<uint16_t>(a, b, true);
    case 0x6a: return unpack<uint32_t>(a, b, true);
    case 0x6b: return pack<int32_t, int16_t>(a, b);
    case 0x6c: return unpack<uint64_t>(a, b, false);
    case 0x6d: return unpack<uint64_t>(a, b, true);
    case 0x74: return map_lanes<uint8_t>(a, b, eq);
    case 0x75: return map_lanes<uint16_t>(a, b, eq);
    case 0x76: return map_lanes<uint32_t>(a, b, eq);
    case 0xd1: return shift_lanes<uint16_t>(a, count, SHIFT_RIGHT);
    case 0xd2: return shift_lanes<uint32_t>(a, count, SHIFT_RIGHT);
    case 0xd3: return shift_lanes<uint64_t>(a, count, SHIFT_RIGHT);
    case 0xd4: return map_lanes<uint64_t>(a, b, add);
    case 0xd5: return map_lanes<int16_t>(a, b, [](auto x, auto y) {
        return x * y;
    });
    case 0xd8: return map_lanes<uint8_t>(a, b, sub_sat);
    case 0xd9: return map_lanes<uint16_t>(a, b, sub_sat);
    case 0xda: return map_lanes<uint8_t>(a, b, min);
    case 0xdb: return map_lanes<uint64_t>(a, b, [](auto x, auto y) {
        return x & y;
    });
    case 0xdc: return map_lanes<uint8_t>(a, b, add_sat);
    case 0xdd: return map_lanes<uint16_t>(a, b, add_sat);
    case 0xde: return map_lanes<uint8_t>(a, b, max);
    case 0xdf: return map_lanes<uint64_t>(a, b, [](auto x, auto y) {
        return ~x & y;
    });
    case 0xe0: return map_lanes<uint8_t>(a, b, avg);
    case 0xe1: return shift_lanes<uint16_t>(a, count, SHIFT_ARITH);
    case 0xe2: return shift_lanes<uint32_t>(a, count, SHIFT_ARITH);
    case 0xe3: return map_lanes<uint16_t>(a, b, avg);
    case 0xe4: return map_lanes<uint16_t>(a, b, mul_high);
    case 0xe5: return map_lanes<int16_t>(a, b, mul_high);
    case 0xe8: return map_lanes<int8_t>(a, b, sub_sat);
    case 0xe9: return map_lanes<int16_t>(a, b, sub_sat);
    case 0xea: return map_lanes<int16_t>(a, b, min);
    case 0xeb: return map_lanes<uint64_t>(a, b, [](auto x, auto y) {
        return x | y;
    });
    case 0xec: return map_lanes<int8_t>(a, b, add_sat);
    case 0xed: return map_lanes<int16_t>(a, b, add_sat);
    case 0xee: return map_lanes<int16_t>(a, b, max);
    case 0xef: return map_lanes<uint64_t>(a, b, [](auto x, auto y) {
        return x ^ y;
    });
    case 0xf1: return shift_lanes<uint16_t>(a, count, SHIFT_LEFT);
    case 0xf2: return shift_lanes<uint32_t>(a, count, SHIFT_LEFT);
    case 0xf3: return shift_lanes<uint64_t>(a, count, SHIFT_LEFT);
    case 0xf4: return multiply_even(a, b);
    case 0xf5: return multiply_add(a, b);
    case 0xf6: return sum_abs_diff(a, b);
    case 0xf8: return map_lanes<uint8_t>(a, b, sub);
    case 0xf9: return map_lanes<uint16_t>(a, b, sub);
    case 0xfa: return map_lanes<uint32_t>(a, b, sub);
    case 0xfb: return map_lanes<uint64_t>(a, b, sub);
    case 0xfc: return map_lanes<uint8_t>(a, b, add);
    case 0xfd: return map_lanes<uint16_t>(a, b, add);
    default: return map_lanes<uint32_t>(a, b, add); /* 0xfe */
    }
}


//#pragma mark - floating point

template <typename F>
static inline bool is_nan(typename F::Bits a)
{
    return (a & F::EXP) == F::EXP && (a & F::MANT) != 0;
}

template <typename F>
static inline bool is_snan(typename F::Bits a)
{
    return is_nan<F>(a) && !(a & F::QUIET);
}

template <typename F>
static inline bool is_denormal(typename F::Bits a)
{
    return (a & F::EXP) == 0 && (a & F::MANT) != 0;
}

/* The QNaN an invalid operation returns. */
template <typename F>
static inline typename F::Bits indefinite()
{
    return F::SIGN | F::EXP | F::QUIET;
}

static uint32_t mxcsr_flags(uint32_t fflags)
{
    return set_bit(0, MXCSR_IE, fflags & FFLAG_INVALID_OP) |
        set_bit(0, MXCSR_ZE, fflags & FFLAG_DIVIDE_ZERO) |
        set_bit(0, MXCSR_OE, fflags & FFLAG_OVERFLOW) |
        set_bit(0, MXCSR_UE, fflags & FFLAG_UNDERFLOW) |
        set_bit(0, MXCSR_PE, fflags & FFLAG_INEXACT);
}

/* A denormal operand is zero under DAZ, otherwise noted in 'denormal'. */
template <typename F>
static typename F::Bits fp_operand(SimdFp &fp, typename F::Bits a,
                                   bool *denormal)
{
    if (is_denormal<F>(a)) {
        if (fp.daz) {
            return a & F::SIGN;
        }
        *denormal = true;
    }
    return a;
}

/* The same for an operation that reports denormal operands whatever it
   computes. */
template <typename F>
static typename F::Bits fp_operand(SimdFp &fp, typename F::Bits a)
{
    bool denormal = false;
    a = fp_operand<F>(fp, a, &denormal);
    if (denormal) {
        fp.flags |= bit_at(MXCSR_DE);
    }
    return a;
}

/* An invalid operation or a division by zero takes precedence over a
   denormal operand. */
static void report_denormal(SimdFp &fp, bool denormal, uint32_t fflags)
{
    if (denormal && !(fflags & (FFLAG_INVALID_OP | FFLAG_DIVIDE_ZERO))) {
        fp.flags |= bit_at(MXCSR_DE);
    }
}

/* Whether a value of format W has more than 'bits' significant bits. */
template <typename W>
static bool wider_than(typename W::Bits a, int bits)
{
    typename W::Bits mant = a & W::MANT;
    if ((a & W::EXP) != 0) {
        mant |= W::MANT + 1;
    }
    if (mant == 0) {
        return false;
    }
    while (!(mant & 1)) {
        mant >>= 1;
    }
    int width = 0;
    for (; mant != 0; mant >>= 1) {
        width++;
    }
    return width > bits;
}

/* A computed result: a NaN from operands that were not NaNs is the
   indefinite, and a tiny result flushes to zero under FTZ. An unmasked
   overflow or underflow reports underflow whenever the result is tiny, and
   precision as for the result with an unbounded exponent, which
   'unbounded_inexact' tells. */
template <typename F, typename Inexact>
static typename F::Bits fp_result(SimdFp &fp, typename F::Bits r,
                                  uint32_t fflags, Inexact unbounded_inexact)
{
    uint32_t flags = mxcsr_flags(fflags);
    bool tiny = is_denormal<F>(r) || (fflags & FFLAG_UNDERFLOW);

    if (is_nan<F>(r)) {
        r = indefinite<F>();
    } else if (tiny && !fp.underflow_masked) {
        flags = set_bit(flags, MXCSR_UE, true);
        flags = set_bit(flags, MXCSR_PE, unbounded_inexact());
    } else if (tiny && fp.ftz) {
        flags |= bit_at(MXCSR_UE) | bit_at(MXCSR_PE);
        r &= F::SIGN;
    } else if (get_bit(flags, MXCSR_OE) && !fp.overflow_masked) {
        flags = set_bit(flags, MXCSR_PE, unbounded_inexact());
    }
    fp.flags |= flags;
    return r;
}

/* ADD, SUB, MUL and DIV, by opcode. */
template <typename F>
static typename F::Bits fp_compute(uint8_t opcode, typename F::Bits a,
                                   typename F::Bits b, RoundingModeEnum rm,
                                   uint32_t *fflags)
{
    switch (opcode) {
    case 0x58:
        return F::add(a, b, rm, fflags);
    case 0x59:
        return F::mul(a, b, rm, fflags);
    case 0x5c:
        return F::sub(a, b, rm, fflags);
    default:
        return F::div(a, b, rm, fflags);
    }
}

/* Whether the result of an operation, its exponent unbounded, is inexact:
   it is exact in the wider format if it fits the narrower one. */
template <typename F>
static bool unbounded_inexact(uint8_t opcode, typename F::Bits a,
                              typename F::Bits b, uint32_t fflags)
{
    typedef Wider<F> Wide;
    if constexpr (std::is_void_v<typename Wide::Type>) {
        return fflags & FFLAG_INEXACT; /* no wider format on this host */
    } else {
        typedef typename Wide::Type W;
        uint32_t wide_flags = 0;
        typename W::Bits r = fp_compute<W>(opcode, Wide::widen(a),
                                           Wide::widen(b), RM_RNE,
                                           &wide_flags);
        return (wide_flags & FFLAG_INEXACT) ||
            wider_than<W>(r, F::MANT_BITS + 1);
    }
}

/* With a NaN operand the result is the first NaN, quietened. */
template <typename F>
static bool nan_operands(SimdFp &fp, typename F::Bits a, typename F::Bits b,
                         typename F::Bits *r)
{
    if (!is_nan<F>(a) && !is_nan<F>(b)) {
        return false;
    }
    if (is_snan<F>(a) || is_snan<F>(b)) {
        fp.flags |= bit_at(MXCSR_IE);
    }
    *r = (is_nan<F>(a) ? a : b) | F::QUIET;
    return true;
}

template <typename F>
static typename F::Bits fp_arith(SimdFp &fp, uint8_t opcode,
                                 typename F::Bits a, typename F::Bits b)
{
    typename F::Bits r;
    if (nan_operands<F>(fp, a, b, &r)) {
        return r;
    }
    bool denormal = false;
    a = fp_operand<F>(fp, a, &denormal);
    b = fp_operand<F>(fp, b, &denormal);
    uint32_t fflags = 0;
    r = fp_compute<F>(opcode, a, b, fp.rm, &fflags);
    report_denormal(fp, denormal, fflags);
    return fp_result<F>(fp, r, fflags, [&] {
        return unbounded_inexact<F>(opcode, a, b, fflags);
    });
}

template <typename F>
static typename F::Bits fp_sqrt(SimdFp &fp, typename F::Bits a)
{
    typename F::Bits r;
    if (nan_operands<F>(fp, a, a, &r)) {
        return r;
    }
    bool denormal = false;
    a = fp_operand<F>(fp, a, &denormal);
    uint32_t fflags = 0;
    r = F::sqrt(a, fp.rm, &fflags);
    report_denormal(fp, denormal, fflags);
    /* a square root neither overflows nor underflows */
    return fp_result<F>(fp, r, fflags, [] { return false; });
}

/* MIN and MAX return the second operand unless the first is strictly
   smaller or larger: for a NaN, and for zeros of either sign. */
template <typename F>
static typename F::Bits fp_min_max(SimdFp &fp, bool max, typename F::Bits a,
                                   typename F::Bits b)
{
    bool denormal = false;
    a = fp_operand<F>(fp, a, &denormal);
    b = fp_operand<F>(fp, b, &denormal);
    if (is_nan<F>(a) || is_nan<F>(b)) {
        fp.flags |= bit_at(MXCSR_IE);
        return b;
    }
    if (denormal) {
        fp.flags |= bit_at(MXCSR_DE);
    }
    uint32_t fflags = 0;
    return (max ? F::lt(b, a, &fflags) : F::lt(a, b, &fflags)) ? a : b;
}

/* The CMPPS predicates: EQ, LT, LE, UNORD, then their negations. LT and
   LE and their negations signal on a QNaN too. */
template <typename F>
static bool fp_compare(SimdFp &fp, int pred, typename F::Bits a,
                       typename F::Bits b)
{
    int rel = get_bits(pred, 0, 2);
    bool negate = get_bit(pred, 2);
    if (is_nan<F>(a) || is_nan<F>(b)) {
        if (rel == 1 || rel == 2 || is_snan<F>(a) || is_snan<F>(b)) {
            fp.flags |= bit_at(MXCSR_IE);
        }
        return (rel == 3) != negate;
    }
    a = fp_operand<F>(fp, a);
    b = fp_operand<F>(fp, b);
    uint32_t fflags = 0;
    bool r;
    switch (rel) {
    case 0:
        r = F::eq(a, b, &fflags);
        break;
    case 1:
        r = F::lt(a, b, &fflags);
        break;
    case 2:
        r = F::lt(a, b, &fflags) || F::eq(a, b, &fflags);
        break;
    default:
        r = false;
        break;
    }
    return r != negate;
}

/* RCPPS and RSQRTPS, rounded to nearest where the part approximates. A
   denormal operand is zero and a denormal result is zero; no flags. */
static uint32_t fp_reciprocal(uint32_t a, bool sqrt)
{
    typedef F32 F;
    const uint64_t one = UINT64_C(0x3ff0000000000000);
    uint32_t sign = a & F::SIGN;
    uint32_t fflags = 0;

    if (is_nan<F>(a)) {
        return a | F::QUIET;
    }
    if ((a & F::EXP) == 0) {
        return sign | F::EXP;
    }
    if (sqrt && sign) {
        return indefinite<F>();
    }
    if ((a & ~F::SIGN) == F::EXP) {
        return sign;
    }
    uint64_t d = cvt_sf32_sf64(a, &fflags);
    if (sqrt) {
        d = sqrt_sf64(d, RM_RNE, &fflags);
    }
    uint32_t r = cvt_sf64_sf32(div_sf64(one, d, RM_RNE, &fflags), RM_RNE,
                               &fflags);
    return is_denormal<F>(r) ? sign : r;
}

/* A NaN converted to another format keeps its sign and the top of its
   payload, quietened. */
template <typename From, typename To>
static typename To::Bits convert_nan(typename From::Bits a)
{
    typedef typename To::Bits T;
    T mant;
    if constexpr (From::MANT_BITS > To::MANT_BITS) {
        mant = (a & From::MANT) >> (From::MANT_BITS - To::MANT_BITS);
    } else {
        mant = (T)(a & From::MANT) << (To::MANT_BITS - From::MANT_BITS);
    }
    return ((a & From::SIGN) ? To::SIGN : 0) | To::EXP | To::QUIET | mant;
}

static uint64_t fp_single_to_double(SimdFp &fp, uint32_t a)
{
    if (is_nan<F32>(a)) {
        if (is_snan<F32>(a)) {
            fp.flags |= bit_at(MXCSR_IE);
        }
        return convert_nan<F32, F64>(a);
    }
    uint32_t fflags = 0;
    return cvt_sf32_sf64(fp_operand<F32>(fp, a), &fflags);
}

static uint32_t fp_double_to_single(SimdFp &fp, uint64_t a)
{
    if (is_nan<F64>(a)) {
        if (is_snan<F64>(a)) {
            fp.flags |= bit_at(MXCSR_IE);
        }
        return convert_nan<F64, F32>(a);
    }
    a = fp_operand<F64>(fp, a);
    uint32_t fflags = 0;
    uint32_t r = cvt_sf64_sf32(a, fp.rm, &fflags);
    return fp_result<F32>(fp, r, fflags, [a] {
        return wider_than<F64>(a, F32::MANT_BITS + 1);
    });
}

/* A NaN or a value out of range gives the integer indefinite, the most
   negative value. */
template <typename F, typename I>
static I fp_to_int(SimdFp &fp, typename F::Bits a, bool truncate)
{
    const I indefinite = std::numeric_limits<I>::min();
    if (is_nan<F>(a)) {
        fp.flags |= bit_at(MXCSR_IE);
        return indefinite;
    }
    bool denormal = false; /* not reported by conversions to integers */
    a = fp_operand<F>(fp, a, &denormal);
    RoundingModeEnum rm = truncate ? RM_RTZ : fp.rm;
    uint32_t fflags = 0;
    I r;
    if constexpr (sizeof(I) == 4) {
        r = F::to_i32(a, rm, &fflags);
    } else {
        r = F::to_i64(a, rm, &fflags);
    }
    if (fflags & FFLAG_INVALID_OP) {
        fp.flags |= bit_at(MXCSR_IE);
        return indefinite;
    }
    fp.flags |= mxcsr_flags(fflags);
    return r;
}

template <typename F, typename I>
static typename F::Bits int_to_fp(SimdFp &fp, I val)
{
    uint32_t fflags = 0;
    typename F::Bits r;
    if constexpr (sizeof(I) == 4) {
        r = F::from_i32(val, fp.rm, &fflags);
    } else {
        r = F::from_i64(val, fp.rm, &fflags);
    }
    fp.flags |= mxcsr_flags(fflags);
    return r;
}


//#pragma mark - state and operands

static void sse_check(X86CPUState *s)
{
    if (get_bit(s->cr0, CR0_EM) || !get_bit(s->cr4, CR4_OSFXSR)) {
        raise_exception(s, EXCP_UD);
    }
    if (get_bit(s->cr0, CR0_TS)) {
        raise_exception(s, EXCP_NM);
    }
}

static void mmx_check(X86CPUState *s)
{
    if (get_bit(s->cr0, CR0_EM)) {
        raise_exception(s, EXCP_UD);
    }
    if (get_bit(s->cr0, CR0_TS)) {
        raise_exception(s, EXCP_NM);
    }
    fpu_check_pending(s);
}

/* An instruction on MMX registers puts the x87 unit in MMX state, TOP 0
   and every register in use, once it can no longer fault. */
static void mmx_enter(X86CPUState *s)
{
    s->fpu.status = set_bits(s->fpu.status, X87_TOP, 3, 0);
    s->fpu.empty = 0;
}

static MmxReg mmx_get(X86CPUState *s, int reg)
{
    MmxReg v;
    memcpy(v.bytes, &s->fpu.st[reg].mant, sizeof(v.bytes));
    return v;
}

/* A written MMX register reads as a NaN or an infinity on the x87 side. */
static void mmx_set(X86CPUState *s, int reg, const MmxReg &v)
{
    memcpy(&s->fpu.st[reg].mant, v.bytes, sizeof(v.bytes));
    s->fpu.st[reg].sexp = 0xffff;
}

/* Record the exception flags; an unmasked one faults before anything is
   written. An unmasked invalid, denormal or divide by zero in any lane
   stops the operation before the others (overflow, underflow and
   precision) are detected. */
static void fp_finish(X86CPUState *s, const SimdFp &fp)
{
    const uint32_t before = bit_at(MXCSR_IE) | bit_at(MXCSR_DE) |
        bit_at(MXCSR_ZE);
    uint32_t unmasked = ~get_bits(s->mxcsr, MXCSR_IM, 6);
    uint32_t flags = fp.flags;
    if (flags & before & unmasked) {
        flags &= before;
    }
    s->mxcsr |= flags;
    if (flags & unmasked) {
        raise_exception(s, get_bit(s->cr4, CR4_OSXMMEXCPT) ? EXCP_XM :
                        EXCP_UD);
    }
}

static void require_reg(X86CPUState *s, const SimdInsn &insn)
{
    if (!insn.rm.is_reg) {
        raise_exception(s, EXCP_UD);
    }
}

static void require_mem(X86CPUState *s, const SimdInsn &insn)
{
    if (insn.rm.is_reg) {
        raise_exception(s, EXCP_UD);
    }
}

/* A 16 byte operand must be aligned unless the instruction says
   otherwise. */
static uint64_t simd_address(X86CPUState *s, const Operand &op, int size,
                             bool write, bool aligned)
{
    uint64_t lin = seg_address(s, op.seg, op.ea, size, write);
    if (aligned && get_bits(lin, 0, size) != 0) {
        raise_exception(s, EXCP_GP, 0);
    }
    return lin;
}

/* The r/m operand of an XMM instruction: the register, or 'size' bytes of
   memory in the low lanes and zeros above. */
static XmmReg xmm_rm(X86CPUState *s, const SimdInsn &insn, int size,
                     bool aligned = true)
{
    if (insn.rm.is_reg) {
        return s->xmm[insn.rm.reg];
    }
    XmmReg v = {};
    uint64_t lin = simd_address(s, insn.rm, size, false,
                                aligned && size == SIZE128);
    mem_read_bytes(s, lin, v.bytes, size);
    return v;
}

static MmxReg mmx_rm(X86CPUState *s, const SimdInsn &insn)
{
    if (insn.rm.is_reg) {
        return mmx_get(s, insn.rm.reg);
    }
    MmxReg v;
    mem_read_bytes(s, simd_address(s, insn.rm, SIZE64, false, false), v.bytes,
                   SIZE64);
    return v;
}

static void mem_store(X86CPUState *s, const SimdInsn &insn, const void *src,
                      int size, bool aligned = false)
{
    mem_write_bytes(s, simd_address(s, insn.rm, size, true, aligned), src,
                    size);
}

/* A general register or memory operand; 32 bits outside 64 bit mode. */
static uint32_t gpr_rm_read(X86CPUState *s, const SimdInsn &insn)
{
    if (insn.rm.is_reg) {
        return s->regs[insn.rm.reg];
    }
    return mem_read(s, simd_address(s, insn.rm, SIZE32, false, false),
                    SIZE32);
}

static void gpr_rm_write(X86CPUState *s, const SimdInsn &insn, uint32_t val)
{
    if (insn.rm.is_reg) {
        s->regs[insn.rm.reg] = val;
    } else {
        mem_write(s, simd_address(s, insn.rm, SIZE32, true, false), val,
                  SIZE32);
    }
}


//#pragma mark - instruction forms

/* An operation on all the lanes of 'xmm, xmm/m128' (PS, PD) or on lane 0 of
   'xmm, xmm/m32' (SS) and 'xmm, xmm/m64' (SD). 'op' takes the format as a
   tag: op(F(), fp, a, b). */
template <typename F, typename Op>
static void fp_lanes(X86CPUState *s, const SimdInsn &insn, bool scalar, Op op)
{
    typedef typename F::Bits Bits;
    XmmReg b = xmm_rm(s, insn, scalar ? F::SIZE : SIZE128);
    XmmReg r = s->xmm[insn.reg];
    SimdFp fp(s);
    int n = scalar ? 1 : lane_count<Bits, 16>;
    for (int i = 0; i < n; i++) {
        set_lane<Bits>(r, i, op(F(), fp, lane<Bits>(r, i), lane<Bits>(b, i)));
    }
    fp_finish(s, fp);
    s->xmm[insn.reg] = r;
}

/* PS, PD, SS or SD, as the mandatory prefix selects. */
template <typename Op>
static void fp_by_prefix(X86CPUState *s, const SimdInsn &insn, Op op)
{
    sse_check(s);
    switch (insn.prefix) {
    case SIMD_NONE:
        fp_lanes<F32>(s, insn, false, op);
        break;
    case SIMD_66:
        fp_lanes<F64>(s, insn, false, op);
        break;
    case SIMD_F3:
        fp_lanes<F32>(s, insn, true, op);
        break;
    default:
        fp_lanes<F64>(s, insn, true, op);
        break;
    }
}

/* The integer operations with an MMX and an XMM form. */
static void exec_int_op(X86CPUState *s, const SimdInsn &insn, uint8_t opcode)
{
    if (insn.prefix == SIMD_NONE) {
        mmx_check(s);
        MmxReg b = mmx_rm(s, insn);
        mmx_enter(s);
        mmx_set(s, insn.reg, int_op(opcode, mmx_get(s, insn.reg), b));
    } else if (insn.prefix == SIMD_66) {
        sse_check(s);
        XmmReg b = xmm_rm(s, insn, SIZE128);
        s->xmm[insn.reg] = int_op(opcode, s->xmm[insn.reg], b);
    } else {
        raise_exception(s, EXCP_UD);
    }
}

/* 0F 71, 72 and 73: shifts of a register by an immediate. */
static void exec_shift_imm(X86CPUState *s, const SimdInsn &insn)
{
    /* the equivalent shift by a register operand */
    static const uint8_t by_reg[3][8] = {
        {0, 0, 0xd1, 0, 0xe1, 0, 0xf1, 0},
        {0, 0, 0xd2, 0, 0xe2, 0, 0xf2, 0},
        {0, 0, 0xd3, 0, 0, 0, 0xf3, 0},
    };
    uint8_t opcode = by_reg[insn.opcode - 0x71][insn.reg];
    bool shift_bytes_op = insn.opcode == 0x73 &&
        (insn.reg == 3 || insn.reg == 7);
    int reg = insn.rm.reg;

    require_reg(s, insn);
    if (insn.prefix == SIMD_NONE && opcode != 0) {
        mmx_check(s);
        mmx_enter(s);
        MmxReg count = {};
        set_lane<uint64_t>(count, 0, insn.imm);
        mmx_set(s, reg, int_op(opcode, mmx_get(s, reg), count));
    } else if (insn.prefix == SIMD_66 && shift_bytes_op) {
        sse_check(s);
        s->xmm[reg] = shift_bytes(s->xmm[reg], insn.imm, insn.reg == 7);
    } else if (insn.prefix == SIMD_66 && opcode != 0) {
        sse_check(s);
        XmmReg count = {};
        set_lane<uint64_t>(count, 0, insn.imm);
        s->xmm[reg] = int_op(opcode, s->xmm[reg], count);
    } else {
        raise_exception(s, EXCP_UD);
    }
}

/* MOVUPS, MOVUPD, MOVSS, MOVSD, MOVAPS, MOVAPD, MOVNTPS and MOVNTPD. A
   scalar move between registers keeps the upper lanes; from memory it
   clears them. */
static void exec_move(X86CPUState *s, const SimdInsn &insn)
{
    uint8_t op = insn.opcode;
    bool store = op != 0x10 && op != 0x28;
    bool aligned = op != 0x10 && op != 0x11;
    int size = SIZE128;

    if (insn.prefix == SIMD_F3 || insn.prefix == SIMD_F2) {
        if (op >= 0x28) {
            raise_exception(s, EXCP_UD);
        }
        size = insn.prefix == SIMD_F3 ? SIZE32 : SIZE64;
    }
    if (op == 0x2b) {
        require_mem(s, insn);
    }
    sse_check(s);
    if (!insn.rm.is_reg) {
        if (store) {
            mem_store(s, insn, s->xmm[insn.reg].bytes, size, aligned);
        } else {
            s->xmm[insn.reg] = xmm_rm(s, insn, size, aligned);
        }
    } else if (store) {
        memcpy(s->xmm[insn.rm.reg].bytes, s->xmm[insn.reg].bytes,
               size_bytes(size));
    } else {
        memcpy(s->xmm[insn.reg].bytes, s->xmm[insn.rm.reg].bytes,
               size_bytes(size));
    }
}

/* MOVLPS, MOVHPS, MOVLPD, MOVHPD, MOVHLPS and MOVLHPS: one quadword. */
static void exec_move_half(X86CPUState *s, const SimdInsn &insn)
{
    bool high = insn.opcode >= 0x16;
    bool store = get_bit(insn.opcode, 0);

    if (insn.prefix > SIMD_66 ||
        ((insn.prefix == SIMD_66 || store) && insn.rm.is_reg)) {
        raise_exception(s, EXCP_UD);
    }
    sse_check(s);
    XmmReg &x = s->xmm[insn.reg];
    if (store) {
        uint64_t val = lane<uint64_t>(x, high);
        mem_store(s, insn, &val, SIZE64);
        return;
    }
    /* between registers the other half of the source */
    uint64_t val = lane<uint64_t>(xmm_rm(s, insn, SIZE64),
                                  insn.rm.is_reg ? !high : 0);
    set_lane<uint64_t>(x, high, val);
}

/* 0F 2A: from integers. CVTPI2PS and CVTPI2PD enter MMX state only for a
   register source, once it is read. */
static void exec_cvt_from_int(X86CPUState *s, const SimdInsn &insn)
{
    bool from_mmx = insn.prefix <= SIMD_66 && insn.rm.is_reg;
    sse_check(s);
    if (from_mmx) {
        mmx_check(s);
    }
    XmmReg r = s->xmm[insn.reg];
    SimdFp fp(s);

    switch (insn.prefix) {
    case SIMD_NONE: { /* CVTPI2PS */
        MmxReg src = mmx_rm(s, insn);
        for (int i = 0; i < 2; i++) {
            set_lane<uint32_t>(r, i, int_to_fp<F32>(fp, lane<int32_t>(src, i)));
        }
        break;
    }
    case SIMD_66: { /* CVTPI2PD */
        MmxReg src = mmx_rm(s, insn);
        for (int i = 0; i < 2; i++) {
            set_lane<uint64_t>(r, i, int_to_fp<F64>(fp, lane<int32_t>(src, i)));
        }
        break;
    }
    case SIMD_F3: { /* CVTSI2SS */
        int32_t val = gpr_rm_read(s, insn);
        set_lane<uint32_t>(r, 0, int_to_fp<F32>(fp, val));
        break;
    }
    default: { /* CVTSI2SD */
        int32_t val = gpr_rm_read(s, insn);
        set_lane<uint64_t>(r, 0, int_to_fp<F64>(fp, val));
        break;
    }
    }
    if (from_mmx) {
        mmx_enter(s);
    }
    fp_finish(s, fp);
    s->xmm[insn.reg] = r;
}

/* CVTPS2PI and CVTPD2PI with their truncating forms. */
template <typename F>
static void cvt_to_mmx(X86CPUState *s, const SimdInsn &insn, bool truncate)
{
    typedef typename F::Bits Bits;
    mmx_check(s);
    XmmReg src = xmm_rm(s, insn, 2 * sizeof(Bits) == 8 ? SIZE64 : SIZE128);
    SimdFp fp(s);
    MmxReg r;
    for (int i = 0; i < 2; i++) {
        set_lane<int32_t>(r, i, fp_to_int<F, int32_t>(fp, lane<Bits>(src, i),
                                                      truncate));
    }
    mmx_enter(s);
    fp_finish(s, fp);
    mmx_set(s, insn.reg, r);
}

/* CVTSS2SI and CVTSD2SI with their truncating forms. */
template <typename F, typename I>
static void cvt_to_gpr(X86CPUState *s, const SimdInsn &insn, bool truncate)
{
    XmmReg src = xmm_rm(s, insn, F::SIZE);
    SimdFp fp(s);
    I r = fp_to_int<F, I>(fp, lane<typename F::Bits>(src, 0), truncate);
    fp_finish(s, fp);
    s->regs[insn.reg] = (std::make_unsigned_t<I>)r;
}

/* 0F 2C and 2D: to integers. */
static void exec_cvt_to_int(X86CPUState *s, const SimdInsn &insn)
{
    bool truncate = insn.opcode == 0x2c;
    sse_check(s);
    switch (insn.prefix) {
    case SIMD_NONE:
        cvt_to_mmx<F32>(s, insn, truncate);
        break;
    case SIMD_66:
        cvt_to_mmx<F64>(s, insn, truncate);
        break;
    case SIMD_F3:
        cvt_to_gpr<F32, int32_t>(s, insn, truncate);
        break;
    default:
        cvt_to_gpr<F64, int32_t>(s, insn, truncate);
        break;
    }
}

/* COMISS, COMISD, UCOMISS and UCOMISD. */
template <typename F>
static void compare_eflags(X86CPUState *s, const SimdInsn &insn,
                           bool signaling)
{
    typedef typename F::Bits Bits;
    XmmReg src = xmm_rm(s, insn, F::SIZE);
    Bits a = lane<Bits>(s->xmm[insn.reg], 0);
    Bits b = lane<Bits>(src, 0);
    SimdFp fp(s);
    uint32_t flags;

    if (is_nan<F>(a) || is_nan<F>(b)) {
        if (signaling || is_snan<F>(a) || is_snan<F>(b)) {
            fp.flags |= bit_at(MXCSR_IE);
        }
        flags = bit_at(EFLAGS_ZF) | bit_at(EFLAGS_PF) | bit_at(EFLAGS_CF);
    } else {
        a = fp_operand<F>(fp, a);
        b = fp_operand<F>(fp, b);
        uint32_t fflags = 0;
        if (F::eq(a, b, &fflags)) {
            flags = bit_at(EFLAGS_ZF);
        } else if (F::lt(a, b, &fflags)) {
            flags = bit_at(EFLAGS_CF);
        } else {
            flags = 0;
        }
    }
    fp_finish(s, fp);
    set_cc_eflags(s, flags);
}

/* 0F 5A: between single and double precision. */
static void exec_cvt_float(X86CPUState *s, const SimdInsn &insn)
{
    sse_check(s);
    SimdFp fp(s);
    XmmReg r = s->xmm[insn.reg];

    switch (insn.prefix) {
    case SIMD_NONE: { /* CVTPS2PD */
        XmmReg src = xmm_rm(s, insn, SIZE64);
        for (int i = 0; i < 2; i++) {
            set_lane<uint64_t>(r, i, fp_single_to_double(
                fp, lane<uint32_t>(src, i)));
        }
        break;
    }
    case SIMD_66: { /* CVTPD2PS */
        XmmReg src = xmm_rm(s, insn, SIZE128);
        for (int i = 0; i < 2; i++) {
            set_lane<uint32_t>(r, i, fp_double_to_single(
                fp, lane<uint64_t>(src, i)));
        }
        set_lane<uint64_t>(r, 1, 0);
        break;
    }
    case SIMD_F3: { /* CVTSS2SD */
        XmmReg src = xmm_rm(s, insn, SIZE32);
        set_lane<uint64_t>(r, 0, fp_single_to_double(
            fp, lane<uint32_t>(src, 0)));
        break;
    }
    default: { /* CVTSD2SS */
        XmmReg src = xmm_rm(s, insn, SIZE64);
        set_lane<uint32_t>(r, 0, fp_double_to_single(
            fp, lane<uint64_t>(src, 0)));
        break;
    }
    }
    fp_finish(s, fp);
    s->xmm[insn.reg] = r;
}

/* 0F 5B and 0F E6: between doubleword integers and floats. */
static void exec_cvt_packed_int(X86CPUState *s, const SimdInsn &insn)
{
    sse_check(s);
    SimdFp fp(s);
    XmmReg r = {};
    XmmReg src;

    switch (insn.opcode * 4 + insn.prefix) {
    case 0x5b * 4 + SIMD_NONE: /* CVTDQ2PS */
        src = xmm_rm(s, insn, SIZE128);
        for (int i = 0; i < 4; i++) {
            set_lane<uint32_t>(r, i, int_to_fp<F32>(fp, lane<int32_t>(src, i)));
        }
        break;
    case 0x5b * 4 + SIMD_66: /* CVTPS2DQ */
    case 0x5b * 4 + SIMD_F3: /* CVTTPS2DQ */
        src = xmm_rm(s, insn, SIZE128);
        for (int i = 0; i < 4; i++) {
            set_lane<int32_t>(r, i, fp_to_int<F32, int32_t>(
                fp, lane<uint32_t>(src, i), insn.prefix == SIMD_F3));
        }
        break;
    case 0xe6 * 4 + SIMD_66: /* CVTTPD2DQ */
    case 0xe6 * 4 + SIMD_F2: /* CVTPD2DQ */
        src = xmm_rm(s, insn, SIZE128);
        for (int i = 0; i < 2; i++) {
            set_lane<int32_t>(r, i, fp_to_int<F64, int32_t>(
                fp, lane<uint64_t>(src, i), insn.prefix == SIMD_66));
        }
        break;
    case 0xe6 * 4 + SIMD_F3: /* CVTDQ2PD */
        src = xmm_rm(s, insn, SIZE64);
        for (int i = 0; i < 2; i++) {
            set_lane<uint64_t>(r, i, int_to_fp<F64>(fp, lane<int32_t>(src, i)));
        }
        break;
    default:
        raise_exception(s, EXCP_UD);
    }
    fp_finish(s, fp);
    s->xmm[insn.reg] = r;
}

/* 0F 6E and 7E: MOVD and MOVQ between vector and general registers. */
static void exec_movd(X86CPUState *s, const SimdInsn &insn)
{
    bool store = insn.opcode == 0x7e;

    switch (insn.prefix) {
    case SIMD_NONE:
        mmx_check(s);
        if (store) {
            gpr_rm_write(s, insn, lane<uint32_t>(mmx_get(s, insn.reg), 0));
        } else {
            MmxReg r = {};
            set_lane<uint32_t>(r, 0, gpr_rm_read(s, insn));
            mmx_set(s, insn.reg, r);
        }
        mmx_enter(s);
        break;
    case SIMD_66:
        sse_check(s);
        if (store) {
            gpr_rm_write(s, insn, lane<uint32_t>(s->xmm[insn.reg], 0));
        } else {
            XmmReg r = {};
            set_lane<uint32_t>(r, 0, gpr_rm_read(s, insn));
            s->xmm[insn.reg] = r;
        }
        break;
    case SIMD_F3:
        if (store) { /* MOVQ xmm, xmm/m64 */
            sse_check(s);
            XmmReg src = xmm_rm(s, insn, SIZE64);
            XmmReg r = {};
            set_lane<uint64_t>(r, 0, lane<uint64_t>(src, 0));
            s->xmm[insn.reg] = r;
            break;
        }
        /* fall through */
    default:
        raise_exception(s, EXCP_UD);
    }
}

/* 0F 6F and 7F: MOVQ, MOVDQA and MOVDQU. */
static void exec_movq_dq(X86CPUState *s, const SimdInsn &insn)
{
    bool store = insn.opcode == 0x7f;

    switch (insn.prefix) {
    case SIMD_NONE:
        mmx_check(s);
        if (!store) {
            mmx_set(s, insn.reg, mmx_rm(s, insn));
        } else if (insn.rm.is_reg) {
            mmx_set(s, insn.rm.reg, mmx_get(s, insn.reg));
        } else {
            mem_store(s, insn, mmx_get(s, insn.reg).bytes, SIZE64);
        }
        mmx_enter(s);
        break;
    case SIMD_66:
    case SIMD_F3: {
        bool aligned = insn.prefix == SIMD_66;
        sse_check(s);
        if (!store) {
            s->xmm[insn.reg] = xmm_rm(s, insn, SIZE128, aligned);
        } else if (insn.rm.is_reg) {
            s->xmm[insn.rm.reg] = s->xmm[insn.reg];
        } else {
            mem_store(s, insn, s->xmm[insn.reg].bytes, SIZE128, aligned);
        }
        break;
    }
    default:
        raise_exception(s, EXCP_UD);
    }
}

/* 0F 70: PSHUFW, PSHUFD, PSHUFHW and PSHUFLW. */
static void exec_shuffle(X86CPUState *s, const SimdInsn &insn)
{
    if (insn.prefix == SIMD_NONE) {
        mmx_check(s);
        mmx_set(s, insn.reg, shuffle4<uint16_t>(mmx_rm(s, insn), insn.imm, 0));
        mmx_enter(s);
        return;
    }
    sse_check(s);
    XmmReg src = xmm_rm(s, insn, SIZE128);
    switch (insn.prefix) {
    case SIMD_66:
        s->xmm[insn.reg] = shuffle4<uint32_t>(src, insn.imm, 0);
        break;
    case SIMD_F3:
        s->xmm[insn.reg] = shuffle4<uint16_t>(src, insn.imm, 4);
        break;
    default:
        s->xmm[insn.reg] = shuffle4<uint16_t>(src, insn.imm, 0);
        break;
    }
}

/* 0F AE: FXSAVE, FXRSTOR, LDMXCSR, STMXCSR, CLFLUSH and the fences. */
static void exec_group15(X86CPUState *s, const SimdInsn &insn)
{
    if (insn.prefix != SIMD_NONE) {
        raise_exception(s, EXCP_UD);
    }
    if (insn.rm.is_reg) {
        /* LFENCE, MFENCE and SFENCE order nothing for one processor */
        if (insn.reg < 5) {
            raise_exception(s, EXCP_UD);
        }
        return;
    }
    switch (insn.reg) {
    case 0: /* FXSAVE */
    case 1: /* FXRSTOR */ {
        if (get_bit(s->cr0, CR0_EM) || get_bit(s->cr0, CR0_TS)) {
            raise_exception(s, EXCP_NM);
        }
        /* 512 bytes */
        uint64_t lin = seg_address(s, insn.rm.seg, insn.rm.ea, 9,
                                   insn.reg == 0);
        if (get_bits(lin, 0, 4) != 0) {
            raise_exception(s, EXCP_GP, 0);
        }
        if (insn.reg == 0) {
            fpu_fxsave(s, lin);
        } else {
            fpu_fxrstor(s, lin);
        }
        break;
    }
    case 2: { /* LDMXCSR */
        sse_check(s);
        uint32_t val = mem_read(
            s, simd_address(s, insn.rm, SIZE32, false, false), SIZE32);
        if (val & ~MXCSR_MASK) {
            raise_exception(s, EXCP_GP, 0);
        }
        s->mxcsr = val;
        break;
    }
    case 3: /* STMXCSR */
        sse_check(s);
        mem_write(s, simd_address(s, insn.rm, SIZE32, true, false), s->mxcsr,
                  SIZE32);
        break;
    case 7: /* CLFLUSH: there is no cache, only the segment check */
        simd_address(s, insn.rm, SIZE8, false, false);
        break;
    default:
        raise_exception(s, EXCP_UD);
    }
}

/* MASKMOVQ and MASKMOVDQU: the bytes of 'data' whose 'mask' byte has its
   top bit set, stored at DS:EDI. */
template <int N>
static void mask_move(X86CPUState *s, const SimdInsn &insn,
                      const VecReg<N> &data, const VecReg<N> &mask)
{
    int size = N == 8 ? SIZE64 : SIZE128;
    if (sign_mask<uint8_t>(mask) == 0) {
        return;
    }
    uint64_t lin = seg_address(s, insn.data_seg,
                               s->regs[REG_EDI] & insn.addr_mask, size, true);
    for (int i = 0; i < N; i++) {
        if (get_bit(mask.bytes[i], 7)) {
            mem_probe_write(s, lin + i, SIZE8);
        }
    }
    for (int i = 0; i < N; i++) {
        if (get_bit(mask.bytes[i], 7)) {
            mem_write(s, lin + i, data.bytes[i], SIZE8);
        }
    }
}


//#pragma mark - dispatch

bool simd_has_imm8(uint8_t opcode)
{
    switch (opcode) {
    case 0x70 ... 0x73:
    case 0xc2:
    case 0xc4 ... 0xc6:
        return true;
    default:
        return false;
    }
}

void simd_reset(X86CPUState *s)
{
    memset(s->xmm, 0, sizeof(s->xmm));
    s->mxcsr = MXCSR_RESET;
}

void simd_exec(X86CPUState *s, const SimdInsn &insn)
{
    uint8_t op = insn.opcode;
    int prefix = insn.prefix;
    bool packed_only = prefix <= SIMD_66;

    switch (op) {
    case 0x10:
    case 0x11:
    case 0x28:
    case 0x29:
    case 0x2b:
        exec_move(s, insn);
        break;
    case 0x12:
    case 0x13:
    case 0x16:
    case 0x17:
        exec_move_half(s, insn);
        break;
    case 0x14: /* UNPCKLPS, UNPCKLPD */
    case 0x15: /* UNPCKHPS, UNPCKHPD */ {
        if (!packed_only) {
            raise_exception(s, EXCP_UD);
        }
        sse_check(s);
        XmmReg src = xmm_rm(s, insn, SIZE128);
        XmmReg &x = s->xmm[insn.reg];
        x = prefix == SIMD_NONE ? unpack<uint32_t>(x, src, op == 0x15) :
            unpack<uint64_t>(x, src, op == 0x15);
        break;
    }
    case 0x2a:
        exec_cvt_from_int(s, insn);
        break;
    case 0x2c:
    case 0x2d:
        exec_cvt_to_int(s, insn);
        break;
    case 0x2e: /* UCOMISS, UCOMISD */
    case 0x2f: /* COMISS, COMISD */
        if (!packed_only) {
            raise_exception(s, EXCP_UD);
        }
        sse_check(s);
        if (prefix == SIMD_NONE) {
            compare_eflags<F32>(s, insn, op == 0x2f);
        } else {
            compare_eflags<F64>(s, insn, op == 0x2f);
        }
        break;
    case 0x50: /* MOVMSKPS, MOVMSKPD */
        require_reg(s, insn);
        if (!packed_only) {
            raise_exception(s, EXCP_UD);
        }
        sse_check(s);
        s->regs[insn.reg] = prefix == SIMD_NONE ?
            sign_mask<uint32_t>(s->xmm[insn.rm.reg]) :
            sign_mask<uint64_t>(s->xmm[insn.rm.reg]);
        break;
    case 0x51:
        fp_by_prefix(s, insn, [](auto f, SimdFp &fp, auto, auto b) {
            return fp_sqrt<decltype(f)>(fp, b);
        });
        break;
    case 0x52: /* RSQRTPS, RSQRTSS */
    case 0x53: /* RCPPS, RCPSS */
        if (prefix != SIMD_NONE && prefix != SIMD_F3) {
            raise_exception(s, EXCP_UD);
        }
        sse_check(s);
        fp_lanes<F32>(s, insn, prefix == SIMD_F3,
                      [op](auto, SimdFp &, auto, uint32_t b) {
            return fp_reciprocal(b, op == 0x52);
        });
        break;
    case 0x54 ... 0x57: { /* ANDPS, ANDNPS, ORPS, XORPS and the PD forms */
        static const uint8_t logic_ops[4] = {0xdb, 0xdf, 0xeb, 0xef};
        if (!packed_only) {
            raise_exception(s, EXCP_UD);
        }
        sse_check(s);
        XmmReg src = xmm_rm(s, insn, SIZE128);
        s->xmm[insn.reg] = int_op(logic_ops[op - 0x54], s->xmm[insn.reg],
                                  src);
        break;
    }
    case 0x58: /* ADD */
    case 0x59: /* MUL */
    case 0x5c: /* SUB */
    case 0x5e: /* DIV */
        fp_by_prefix(s, insn, [op](auto f, SimdFp &fp, auto a, auto b) {
            return fp_arith<decltype(f)>(fp, op, a, b);
        });
        break;
    case 0x5d: /* MIN */
    case 0x5f: /* MAX */
        fp_by_prefix(s, insn, [op](auto f, SimdFp &fp, auto a, auto b) {
            return fp_min_max<decltype(f)>(fp, op == 0x5f, a, b);
        });
        break;
    case 0x5a:
        exec_cvt_float(s, insn);
        break;
    case 0x5b:
    case 0xe6:
        exec_cvt_packed_int(s, insn);
        break;
    case 0x6c: /* PUNPCKLQDQ */
    case 0x6d: /* PUNPCKHQDQ */
        if (prefix != SIMD_66) {
            raise_exception(s, EXCP_UD);
        }
        exec_int_op(s, insn, op);
        break;
    case 0x60 ... 0x6b:
    case 0x74 ... 0x76:
    case 0xd1 ... 0xd5:
    case 0xd8 ... 0xdf:
    case 0xe0 ... 0xe5:
    case 0xe8 ... 0xef:
    case 0xf1 ... 0xf6:
    case 0xf8 ... 0xfe:
        exec_int_op(s, insn, op);
        break;
    case 0x6e:
    case 0x7e:
        exec_movd(s, insn);
        break;
    case 0x6f:
    case 0x7f:
        exec_movq_dq(s, insn);
        break;
    case 0x70:
        exec_shuffle(s, insn);
        break;
    case 0x71 ... 0x73:
        exec_shift_imm(s, insn);
        break;
    case 0x77: /* EMMS: every register empty, TOP 0 */
        if (prefix != SIMD_NONE) {
            raise_exception(s, EXCP_UD);
        }
        mmx_check(s);
        s->fpu.status = set_bits(s->fpu.status, X87_TOP, 3, 0);
        s->fpu.empty = 0xff;
        break;
    case 0xae:
        exec_group15(s, insn);
        break;
    case 0xc2: { /* CMPPS, CMPPD, CMPSS, CMPSD */
        int pred = get_bits(insn.imm, 0, 3);
        fp_by_prefix(s, insn, [pred](auto f, SimdFp &fp, auto a, auto b) {
            typedef decltype(a) Bits;
            return fp_compare<decltype(f)>(fp, pred, a, b) ? (Bits)~(Bits)0 :
                (Bits)0;
        });
        break;
    }
    case 0xc4: { /* PINSRW */
        uint16_t val = insn.rm.is_reg ? s->regs[insn.rm.reg] : 0;
        if (prefix == SIMD_NONE) {
            mmx_check(s);
        } else if (prefix == SIMD_66) {
            sse_check(s);
        } else {
            raise_exception(s, EXCP_UD);
        }
        if (!insn.rm.is_reg) {
            val = mem_read(s, simd_address(s, insn.rm, SIZE16, false, false),
                           SIZE16);
        }
        if (prefix == SIMD_NONE) {
            mmx_enter(s);
            MmxReg r = mmx_get(s, insn.reg);
            set_lane<uint16_t>(r, get_bits(insn.imm, 0, 2), val);
            mmx_set(s, insn.reg, r);
        } else {
            set_lane<uint16_t>(s->xmm[insn.reg], get_bits(insn.imm, 0, 3), val);
        }
        break;
    }
    case 0xc5: /* PEXTRW */
        require_reg(s, insn);
        if (prefix == SIMD_NONE) {
            mmx_check(s);
            mmx_enter(s);
            s->regs[insn.reg] = lane<uint16_t>(mmx_get(s, insn.rm.reg),
                                               get_bits(insn.imm, 0, 2));
        } else if (prefix == SIMD_66) {
            sse_check(s);
            s->regs[insn.reg] = lane<uint16_t>(s->xmm[insn.rm.reg],
                                               get_bits(insn.imm, 0, 3));
        } else {
            raise_exception(s, EXCP_UD);
        }
        break;
    case 0xc6: { /* SHUFPS, SHUFPD */
        if (!packed_only) {
            raise_exception(s, EXCP_UD);
        }
        sse_check(s);
        XmmReg src = xmm_rm(s, insn, SIZE128);
        XmmReg &x = s->xmm[insn.reg];
        x = prefix == SIMD_NONE ? shuffle_pair<uint32_t>(x, src, insn.imm) :
            shuffle_pair<uint64_t>(x, src, insn.imm);
        break;
    }
    case 0xd6:
        switch (prefix) {
        case SIMD_66: { /* MOVQ xmm/m64, xmm */
            sse_check(s);
            uint64_t val = lane<uint64_t>(s->xmm[insn.reg], 0);
            if (insn.rm.is_reg) {
                XmmReg r = {};
                set_lane<uint64_t>(r, 0, val);
                s->xmm[insn.rm.reg] = r;
            } else {
                mem_store(s, insn, &val, SIZE64);
            }
            break;
        }
        case SIMD_F3: { /* MOVQ2DQ xmm, mm */
            require_reg(s, insn);
            sse_check(s);
            mmx_check(s);
            mmx_enter(s);
            XmmReg r = {};
            memcpy(r.bytes, mmx_get(s, insn.rm.reg).bytes, sizeof(MmxReg));
            s->xmm[insn.reg] = r;
            break;
        }
        case SIMD_F2: { /* MOVDQ2Q mm, xmm */
            require_reg(s, insn);
            sse_check(s);
            mmx_check(s);
            mmx_enter(s);
            MmxReg r;
            memcpy(r.bytes, s->xmm[insn.rm.reg].bytes, sizeof(MmxReg));
            mmx_set(s, insn.reg, r);
            break;
        }
        default:
            raise_exception(s, EXCP_UD);
        }
        break;
    case 0xd7: /* PMOVMSKB */
        require_reg(s, insn);
        if (prefix == SIMD_NONE) {
            mmx_check(s);
            mmx_enter(s);
            s->regs[insn.reg] = sign_mask<uint8_t>(mmx_get(s, insn.rm.reg));
        } else if (prefix == SIMD_66) {
            sse_check(s);
            s->regs[insn.reg] = sign_mask<uint8_t>(s->xmm[insn.rm.reg]);
        } else {
            raise_exception(s, EXCP_UD);
        }
        break;
    case 0xe7: /* MOVNTQ, MOVNTDQ */
        require_mem(s, insn);
        if (prefix == SIMD_NONE) {
            mmx_check(s);
            mem_store(s, insn, mmx_get(s, insn.reg).bytes, SIZE64);
            mmx_enter(s);
        } else if (prefix == SIMD_66) {
            sse_check(s);
            mem_store(s, insn, s->xmm[insn.reg].bytes, SIZE128, true);
        } else {
            raise_exception(s, EXCP_UD);
        }
        break;
    case 0xf7: /* MASKMOVQ, MASKMOVDQU */
        require_reg(s, insn);
        if (prefix == SIMD_NONE) {
            mmx_check(s);
            mask_move(s, insn, mmx_get(s, insn.reg), mmx_get(s, insn.rm.reg));
            mmx_enter(s);
        } else if (prefix == SIMD_66) {
            sse_check(s);
            mask_move(s, insn, s->xmm[insn.reg], s->xmm[insn.rm.reg]);
        } else {
            raise_exception(s, EXCP_UD);
        }
        break;
    default:
        raise_exception(s, EXCP_UD);
    }
}
