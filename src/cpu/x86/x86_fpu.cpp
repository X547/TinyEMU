/*
 * x86 CPU emulator: x87 floating point unit
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
#include <cfenv>
#include <cfloat>
#include <cmath>

#include "x86_cpu_priv.h"

/* The registers are host long doubles. That is the x87 format itself on an
   x86 host; elsewhere precision is whatever the host provides. Precision
   control and the denormal, precision, underflow and overflow exceptions
   are not modelled. */

namespace {

enum {
    FPUS_IE = 0,
    FPUS_DE = 1,
    FPUS_ZE = 2,
    FPUS_OE = 3,
    FPUS_UE = 4,
    FPUS_PE = 5,
    FPUS_SF = 6,
    FPUS_ES = 7,
    FPUS_C0 = 8,
    FPUS_C1 = 9,
    FPUS_C2 = 10,
    FPUS_TOP = 11, /* 3 bits */
    FPUS_C3 = 14,
    FPUS_B = 15,
};

enum {
    FPUC_RC = 10, /* 2 bits */
};

enum {
    RC_NEAREST,
    RC_DOWN,
    RC_UP,
    RC_ZERO,
};

enum {
    TAG_VALID,
    TAG_ZERO,
    TAG_SPECIAL,
    TAG_EMPTY,
};

/* The reg field of the arithmetic opcodes. */
enum {
    FPU_ADD, FPU_MUL, FPU_COM, FPU_COMP, FPU_SUB, FPU_SUBR, FPU_DIV, FPU_DIVR,
};

enum {
    REL_LESS,
    REL_EQUAL,
    REL_GREATER,
    REL_UNORDERED,
};

enum {
    FMT_F32,
    FMT_F64,
    FMT_F80,
    FMT_I16,
    FMT_I32,
    FMT_I64,
};

const uint16_t CONTROL_INIT = 0x37f;
const uint32_t FPUS_EXCEPTIONS = 0x3f;

/* A constant as FLD1 and friends load it. 'rounded' tells which way the
   nearest 64 bit mantissa went: +1 up, -1 down. */
struct FpuConstant {
    uint64_t mant;
    uint16_t sexp;
    int8_t rounded;
};

/* Switches the host to the rounding control of the FPU for the lifetime of
   the object. Nothing in its scope may raise an exception. */
class HostRounding {
private:
    bool fChanged;

public:
    HostRounding(const X87State *f)
    {
        static const int modes[4] = {
            FE_TONEAREST, FE_DOWNWARD, FE_UPWARD, FE_TOWARDZERO
        };
        int rc = get_bits(f->control, FPUC_RC, 2);
        fChanged = rc != RC_NEAREST;
        if (fChanged) {
            fesetround(modes[rc]);
        }
    }

    ~HostRounding()
    {
        if (fChanged) {
            fesetround(FE_TONEAREST);
        }
    }
};

}


//#pragma mark - number format

#if LDBL_MANT_DIG == 64

static long double fx80_to_ld(uint64_t mant, uint16_t sexp)
{
    uint8_t buf[sizeof(long double)] = {};
    long double v;
    memcpy(buf, &mant, sizeof(mant));
    memcpy(buf + sizeof(mant), &sexp, sizeof(sexp));
    memcpy(&v, buf, sizeof(v));
    return v;
}

static void ld_to_fx80(long double v, uint64_t *mant, uint16_t *sexp)
{
    uint8_t buf[sizeof(long double)];
    memcpy(buf, &v, sizeof(v));
    memcpy(mant, buf, sizeof(*mant));
    memcpy(sexp, buf + sizeof(*mant), sizeof(*sexp));
}

#else

static long double fx80_to_ld(uint64_t mant, uint16_t sexp)
{
    int exp = get_bits(sexp, 0, 15);
    long double v;
    if (exp == 0x7fff) {
        v = get_bits(mant, 0, 63) == 0 ? HUGE_VALL : NAN;
    } else if (mant == 0) {
        v = 0;
    } else {
        v = ldexpl((long double)mant, (exp == 0 ? 1 : exp) - 0x3fff - 63);
    }
    return get_bit(sexp, 15) ? -v : v;
}

static void ld_to_fx80(long double v, uint64_t *mant, uint16_t *sexp)
{
    uint16_t sign = set_bit(0, 15, std::signbit(v));
    if (std::isnan(v)) {
        *mant = UINT64_C(0xc000000000000000);
        *sexp = set_bits(sign, 0, 15, 0x7fff);
    } else if (std::isinf(v)) {
        *mant = UINT64_C(0x8000000000000000);
        *sexp = set_bits(sign, 0, 15, 0x7fff);
    } else if (v == 0) {
        *mant = 0;
        *sexp = sign;
    } else {
        int exp;
        long double frac = frexpl(fabsl(v), &exp);
        int biased = exp - 1 + 0x3fff;
        if (biased <= 0) {
            *mant = (uint64_t)ldexpl(frac, 63 + biased);
            *sexp = sign;
        } else {
            *mant = (uint64_t)ldexpl(frac, 64);
            *sexp = set_bits(sign, 0, 15, biased);
        }
    }
}

#endif

static long double default_nan()
{
    return fx80_to_ld(UINT64_C(0xc000000000000000), 0xffff);
}

static long double ln2()
{
    return fx80_to_ld(UINT64_C(0xb17217f7d1cf79ac), 0x3ffe);
}


//#pragma mark - stack

static int fpu_top(X87State *f)
{
    return get_bits(f->status, FPUS_TOP, 3);
}

static void fpu_set_top(X87State *f, int top)
{
    f->status = set_bits(f->status, FPUS_TOP, 3, top);
}

/* The physical register of ST(i). */
static int st_reg(X87State *f, int i)
{
    return get_bits(fpu_top(f) + i, 0, 3);
}

static void fpu_update_summary(X87State *f)
{
    bool pending = (f->status & ~f->control & FPUS_EXCEPTIONS) != 0;
    f->status = set_bit(f->status, FPUS_ES, pending);
    f->status = set_bit(f->status, FPUS_B, pending);
}

static void fpu_raise(X87State *f, uint32_t flags)
{
    f->status |= flags;
    fpu_update_summary(f);
}

static void set_c1(X87State *f, bool on)
{
    f->status = set_bit(f->status, FPUS_C1, on);
}

static long double st_get(X87State *f, int i)
{
    int reg = st_reg(f, i);
    if (get_bit(f->empty, reg)) {
        set_c1(f, false);
        fpu_raise(f, bit_at(FPUS_IE) | bit_at(FPUS_SF));
        return default_nan();
    }
    return f->st[reg];
}

static void st_set(X87State *f, int i, long double v)
{
    int reg = st_reg(f, i);
    f->st[reg] = v;
    f->empty = set_bit(f->empty, reg, false);
}

static void fpu_push(X87State *f, long double v)
{
    int top = get_bits(fpu_top(f) - 1, 0, 3);
    fpu_set_top(f, top);
    if (!get_bit(f->empty, top)) {
        set_c1(f, true);
        fpu_raise(f, bit_at(FPUS_IE) | bit_at(FPUS_SF));
        v = default_nan();
    }
    f->st[top] = v;
    f->empty = set_bit(f->empty, top, false);
}

static void fpu_pop(X87State *f)
{
    int top = fpu_top(f);
    f->empty = set_bit(f->empty, top, true);
    fpu_set_top(f, top + 1);
}

static uint16_t fpu_tag_word(X87State *f)
{
    uint16_t tags = 0;
    for (int i = 0; i < 8; i++) {
        int tag;
        if (get_bit(f->empty, i)) {
            tag = TAG_EMPTY;
        } else {
            switch (std::fpclassify(f->st[i])) {
            case FP_ZERO:
                tag = TAG_ZERO;
                break;
            case FP_NORMAL:
                tag = TAG_VALID;
                break;
            default:
                tag = TAG_SPECIAL;
                break;
            }
        }
        tags = set_bits(tags, 2 * i, 2, tag);
    }
    return tags;
}

static void fpu_set_tag_word(X87State *f, uint16_t tags)
{
    for (int i = 0; i < 8; i++) {
        f->empty = set_bit(f->empty, i, get_bits(tags, 2 * i, 2) == TAG_EMPTY);
    }
}

static void fpu_init(X87State *f)
{
    f->control = CONTROL_INIT;
    f->status = 0;
    f->empty = 0xff;
    f->opcode = 0;
    f->fip = 0;
    f->fcs = 0;
    f->fdp = 0;
    f->fds = 0;
}


//#pragma mark - arithmetic

static long double fpu_round(X87State *f, long double v)
{
    switch (get_bits(f->control, FPUC_RC, 2)) {
    case RC_NEAREST:
        return nearbyintl(v);
    case RC_DOWN:
        return floorl(v);
    case RC_UP:
        return ceill(v);
    default:
        return truncl(v);
    }
}

static int64_t fpu_to_int(X87State *f, long double v, int bits, bool truncate)
{
    long double r = truncate ? truncl(v) : fpu_round(f, v);
    long double limit = ldexpl(1, bits - 1);
    if (std::isnan(r) || r < -limit || r >= limit) {
        fpu_raise(f, bit_at(FPUS_IE));
        return sign_extend(UINT64_C(1) << (bits - 1), bits);
    }
    return (int64_t)r;
}

static long double fpu_divide(X87State *f, long double a, long double b)
{
    if (b == 0 && std::isfinite(a) && a != 0) {
        fpu_raise(f, bit_at(FPUS_ZE));
    }
    return a / b;
}

static long double fpu_arith(X87State *f, int op, long double a,
                             long double b)
{
    HostRounding rounding(f);
    long double r;
    switch (op) {
    case FPU_ADD:
        r = a + b;
        break;
    case FPU_MUL:
        r = a * b;
        break;
    case FPU_SUB:
        r = a - b;
        break;
    case FPU_SUBR:
        r = b - a;
        break;
    case FPU_DIV:
        r = fpu_divide(f, a, b);
        break;
    default:
        r = fpu_divide(f, b, a);
        break;
    }
    if (std::isnan(r) && !std::isnan(a) && !std::isnan(b)) {
        fpu_raise(f, bit_at(FPUS_IE));
    }
    return r;
}

/* A NaN result from operands that are not NaNs is an invalid operation. */
static long double fpu_checked(X87State *f, long double r, long double a,
                               long double b = 0)
{
    if (std::isnan(r) && !std::isnan(a) && !std::isnan(b)) {
        fpu_raise(f, bit_at(FPUS_IE));
    }
    return r;
}

static int fpu_compare(X87State *f, long double a, long double b, bool quiet)
{
    if (std::isnan(a) || std::isnan(b)) {
        if (!quiet) {
            fpu_raise(f, bit_at(FPUS_IE));
        }
        return REL_UNORDERED;
    }
    return a < b ? REL_LESS : a == b ? REL_EQUAL : REL_GREATER;
}

static void set_fpu_condition(X87State *f, int rel)
{
    f->status = set_bit(f->status, FPUS_C0,
                        rel == REL_LESS || rel == REL_UNORDERED);
    f->status = set_bit(f->status, FPUS_C2, rel == REL_UNORDERED);
    f->status = set_bit(f->status, FPUS_C3,
                        rel == REL_EQUAL || rel == REL_UNORDERED);
    set_c1(f, false);
}

static void set_eflags_condition(X86CPUState *s, int rel)
{
    uint32_t flags = set_bit(0, EFLAGS_CF,
                             rel == REL_LESS || rel == REL_UNORDERED) |
        set_bit(0, EFLAGS_PF, rel == REL_UNORDERED) |
        set_bit(0, EFLAGS_ZF, rel == REL_EQUAL || rel == REL_UNORDERED);
    set_cc_eflags(s, flags);
    set_c1(&s->fpu, false);
}

/* D8 and friends: ST(0) against an operand. */
static void fpu_arith_st0(X87State *f, int op, long double v)
{
    switch (op) {
    case FPU_COM:
    case FPU_COMP:
        set_fpu_condition(f, fpu_compare(f, st_get(f, 0), v, false));
        if (op == FPU_COMP) {
            fpu_pop(f);
        }
        break;
    default:
        st_set(f, 0, fpu_arith(f, op, st_get(f, 0), v));
        break;
    }
}

/* FPREM and FPREM1. A partial remainder reduces the exponent difference
   below 64 over several calls, as the part does. */
static void fpu_remainder(X87State *f, bool ieee)
{
    long double a = st_get(f, 0);
    long double b = st_get(f, 1);

    f->status &= ~(bit_at(FPUS_C0) | bit_at(FPUS_C1) | bit_at(FPUS_C2) |
                   bit_at(FPUS_C3));
    if (std::isnan(a) || std::isnan(b) || std::isinf(a) || b == 0) {
        st_set(f, 0, fpu_checked(f, fmodl(a, b), a, b));
        return;
    }
    if (a == 0 || std::isinf(b)) {
        return;
    }
    int expdif = ilogbl(a) - ilogbl(b);
    if (expdif < 64) {
        /* The remainders are exact and ignore the rounding control. The
           low quotient bits come from the remainder of 8 * b, as the
           quotient itself may need 64 bits. */
        long double r = fmodl(a, b);
        long double r8 = fmodl(a, ldexpl(b, 3));
        uint64_t qbits = (uint64_t)roundl(fabsl((r8 - r) / b));
        if (ieee) {
            long double nearest = remainderl(a, b);
            qbits += nearest != r;
            r = nearest;
        }
        st_set(f, 0, r);
        f->status = set_bit(f->status, FPUS_C0, get_bit(qbits, 2));
        f->status = set_bit(f->status, FPUS_C3, get_bit(qbits, 1));
        set_c1(f, get_bit(qbits, 0));
    } else {
        int n = 32 + get_bits(expdif, 0, 5);
        st_set(f, 0, fmodl(a, ldexpl(b, expdif - n)));
        f->status = set_bit(f->status, FPUS_C2, true);
    }
}

static void fpu_examine(X87State *f)
{
    int reg = st_reg(f, 0);
    long double v = f->st[reg];
    bool c0 = false, c2 = false, c3 = false;

    if (get_bit(f->empty, reg)) {
        c0 = true;
        c3 = true;
    } else {
        switch (std::fpclassify(v)) {
        case FP_NAN:
            c0 = true;
            break;
        case FP_INFINITE:
            c0 = true;
            c2 = true;
            break;
        case FP_ZERO:
            c3 = true;
            break;
        case FP_SUBNORMAL:
            c2 = true;
            c3 = true;
            break;
        default:
            c2 = true;
            break;
        }
    }
    set_c1(f, std::signbit(v));
    f->status = set_bit(f->status, FPUS_C0, c0);
    f->status = set_bit(f->status, FPUS_C2, c2);
    f->status = set_bit(f->status, FPUS_C3, c3);
}

/* Whether FSIN, FCOS, FPTAN or FSINCOS is to compute a result. A finite
   argument out of range is left alone; a NaN or an infinity gives NaNs,
   two of them for the operations that push. */
static bool fpu_trig_argument(X87State *f, long double v, bool pushes)
{
    f->status = set_bit(f->status, FPUS_C2, false);
    if (std::isnan(v) || std::isinf(v)) {
        long double nan = v;
        if (std::isinf(v)) {
            fpu_raise(f, bit_at(FPUS_IE));
            nan = default_nan();
        }
        st_set(f, 0, nan);
        if (pushes) {
            fpu_push(f, nan);
        }
        return false;
    }
    if (fabsl(v) >= ldexpl(1, 63)) {
        f->status = set_bit(f->status, FPUS_C2, true);
        return false;
    }
    return true;
}


//#pragma mark - memory operands

static uint64_t read64(X86CPUState *s, uint32_t lin)
{
    uint32_t low = mem_read(s, lin, SIZE32);
    return concat_bits(mem_read(s, lin + 4, SIZE32), low, 32);
}

static void write64(X86CPUState *s, uint32_t lin, uint64_t val)
{
    mem_probe_write(s, lin + 4, SIZE32);
    mem_write(s, lin, get_bits(val, 0, 32), SIZE32);
    mem_write(s, lin + 4, get_bits(val, 32, 32), SIZE32);
}

static long double fpu_load(X86CPUState *s, uint32_t lin, int fmt)
{
    switch (fmt) {
    case FMT_F32: {
        uint32_t bits = mem_read(s, lin, SIZE32);
        float v;
        memcpy(&v, &bits, sizeof(v));
        return v;
    }
    case FMT_F64: {
        uint64_t bits = read64(s, lin);
        double v;
        memcpy(&v, &bits, sizeof(v));
        return v;
    }
    case FMT_F80: {
        uint64_t mant = read64(s, lin);
        return fx80_to_ld(mant, mem_read(s, lin + 8, SIZE16));
    }
    case FMT_I16:
        return (int16_t)mem_read(s, lin, SIZE16);
    case FMT_I32:
        return (int32_t)mem_read(s, lin, SIZE32);
    default:
        return (int64_t)read64(s, lin);
    }
}

static void fpu_store(X86CPUState *s, uint32_t lin, long double v, int fmt,
                      bool truncate)
{
    X87State *f = &s->fpu;

    switch (fmt) {
    case FMT_F32: {
        uint32_t bits;
        {
            HostRounding rounding(f);
            float fv = v;
            memcpy(&bits, &fv, sizeof(bits));
        }
        mem_write(s, lin, bits, SIZE32);
        break;
    }
    case FMT_F64: {
        uint64_t bits;
        {
            HostRounding rounding(f);
            double dv = v;
            memcpy(&bits, &dv, sizeof(bits));
        }
        write64(s, lin, bits);
        break;
    }
    case FMT_F80: {
        uint64_t mant;
        uint16_t sexp;
        ld_to_fx80(v, &mant, &sexp);
        mem_probe_write(s, lin + 8, SIZE16);
        write64(s, lin, mant);
        mem_write(s, lin + 8, sexp, SIZE16);
        break;
    }
    case FMT_I16:
        mem_probe_write(s, lin, SIZE16);
        mem_write(s, lin, fpu_to_int(f, v, 16, truncate), SIZE16);
        break;
    case FMT_I32:
        mem_probe_write(s, lin, SIZE32);
        mem_write(s, lin, fpu_to_int(f, v, 32, truncate), SIZE32);
        break;
    default:
        mem_probe_write(s, lin, SIZE32);
        mem_probe_write(s, lin + 4, SIZE32);
        write64(s, lin, fpu_to_int(f, v, 64, truncate));
        break;
    }
}

static void fpu_load_bcd(X86CPUState *s, uint32_t lin)
{
    long double v = 0;
    for (int i = 8; i >= 0; i--) {
        uint32_t byte = mem_read(s, lin + i, SIZE8);
        v = v * 100 + get_bits(byte, 4, 4) * 10 + get_bits(byte, 0, 4);
    }
    if (get_bit(mem_read(s, lin + 9, SIZE8), 7)) {
        v = -v;
    }
    fpu_push(&s->fpu, v);
}

static void fpu_store_bcd(X86CPUState *s, uint32_t lin)
{
    X87State *f = &s->fpu;
    long double v = fpu_round(f, st_get(f, 0));
    uint8_t bytes[10] = {};

    if (std::isnan(v) || fabsl(v) >= 1e18L) {
        fpu_raise(f, bit_at(FPUS_IE));
        memset(bytes + 7, 0xff, 3);
        bytes[7] = 0xc0;
    } else {
        uint64_t n = (uint64_t)fabsl(v);
        for (int i = 0; i < 9; i++) {
            bytes[i] = set_bits(n % 10, 4, 4, (n / 10) % 10);
            n /= 100;
        }
        bytes[9] = set_bit(0, 7, std::signbit(v));
    }
    mem_probe_write(s, lin, SIZE32);
    mem_probe_write(s, lin + 4, SIZE32);
    mem_probe_write(s, lin + 8, SIZE16);
    for (int i = 0; i < 10; i++) {
        mem_write(s, lin + i, bytes[i], SIZE8);
    }
    fpu_pop(f);
}

static int env_size(int opsize)
{
    return opsize == SIZE32 ? 28 : 14;
}

/* The protected mode layouts are used in every mode. */
static void fpu_store_env(X86CPUState *s, uint32_t lin, int opsize)
{
    X87State *f = &s->fpu;
    uint32_t fields[7] = {
        f->control, f->status, fpu_tag_word(f), f->fip,
        set_bits(f->fcs, 16, 11, f->opcode), f->fdp, f->fds
    };
    int size = opsize == SIZE32 ? SIZE32 : SIZE16;

    mem_probe_write(s, lin + env_size(opsize) - size_bytes(size), size);
    for (int i = 0; i < 7; i++) {
        uint32_t val = fields[i];
        if (size == SIZE32 && (i == 0 || i == 1 || i == 2 || i == 6)) {
            val = set_bits(val, 16, 16, 0xffff);
        }
        if (size == SIZE16 && i == 4) {
            val = f->fcs;
        }
        mem_write(s, lin + i * size_bytes(size), val, size);
    }
}

static void fpu_load_env(X86CPUState *s, uint32_t lin, int opsize)
{
    X87State *f = &s->fpu;
    int size = opsize == SIZE32 ? SIZE32 : SIZE16;
    uint32_t fields[7];

    for (int i = 0; i < 7; i++) {
        fields[i] = mem_read(s, lin + i * size_bytes(size), size);
    }
    f->control = fields[0];
    f->status = fields[1];
    fpu_set_tag_word(f, fields[2]);
    f->fip = fields[3];
    f->fcs = get_bits(fields[4], 0, 16);
    f->opcode = size == SIZE32 ? get_bits(fields[4], 16, 11) : 0;
    f->fdp = fields[5];
    f->fds = fields[6];
    fpu_update_summary(f);
}

static void fpu_save(X86CPUState *s, uint32_t lin, int opsize)
{
    X87State *f = &s->fpu;
    uint32_t reg_base = lin + env_size(opsize);

    mem_probe_write(s, reg_base + 8 * 10 - 4, SIZE32);
    fpu_store_env(s, lin, opsize);
    for (int i = 0; i < 8; i++) {
        uint64_t mant;
        uint16_t sexp;
        ld_to_fx80(f->st[st_reg(f, i)], &mant, &sexp);
        write64(s, reg_base + i * 10, mant);
        mem_write(s, reg_base + i * 10 + 8, sexp, SIZE16);
    }
    fpu_init(f);
}

static void fpu_restore(X86CPUState *s, uint32_t lin, int opsize)
{
    X87State *f = &s->fpu;
    uint32_t reg_base = lin + env_size(opsize);
    long double st[8];

    for (int i = 0; i < 8; i++) {
        st[i] = fpu_load(s, reg_base + i * 10, FMT_F80);
    }
    fpu_load_env(s, lin, opsize);
    for (int i = 0; i < 8; i++) {
        f->st[st_reg(f, i)] = st[i];
    }
}


//#pragma mark - instructions

static void fpu_exec_mem(X86CPUState *s, int op, int reg, uint32_t lin,
                         int opsize)
{
    static const int arith_formats[4] = {FMT_F32, FMT_I32, FMT_F64, FMT_I16};
    X87State *f = &s->fpu;

    if (!get_bit(op, 0)) {
        fpu_arith_st0(f, reg, fpu_load(s, lin, arith_formats[op / 2]));
        return;
    }

    if (op == 1) {
        switch (reg) {
        case 0: /* FLD m32 */
            fpu_push(f, fpu_load(s, lin, FMT_F32));
            return;
        case 2: /* FST m32 */
        case 3: /* FSTP m32 */
            fpu_store(s, lin, st_get(f, 0), FMT_F32, false);
            if (reg == 3) {
                fpu_pop(f);
            }
            return;
        case 4: /* FLDENV */
            fpu_load_env(s, lin, opsize);
            return;
        case 5: /* FLDCW */
            f->control = mem_read(s, lin, SIZE16);
            fpu_update_summary(f);
            return;
        case 6: /* FNSTENV */
            fpu_store_env(s, lin, opsize);
            f->control |= FPUS_EXCEPTIONS;
            fpu_update_summary(f);
            return;
        case 7: /* FNSTCW */
            mem_write(s, lin, f->control, SIZE16);
            return;
        default:
            raise_exception(s, EXCP_UD);
        }
    }

    if (op != 5 && reg <= 3) {
        int int_fmt = op == 3 ? FMT_I32 : FMT_I16;
        if (reg == 0) { /* FILD */
            fpu_push(f, fpu_load(s, lin, int_fmt));
            return;
        }
        /* FISTTP, FIST, FISTP */
        fpu_store(s, lin, st_get(f, 0), int_fmt, reg == 1);
        if (reg != 2) {
            fpu_pop(f);
        }
        return;
    }

    switch (op * 8 + reg) {
    case 3 * 8 + 5: /* FLD m80 */
        fpu_push(f, fpu_load(s, lin, FMT_F80));
        break;
    case 3 * 8 + 7: /* FSTP m80 */
        fpu_store(s, lin, st_get(f, 0), FMT_F80, false);
        fpu_pop(f);
        break;
    case 5 * 8 + 0: /* FLD m64 */
        fpu_push(f, fpu_load(s, lin, FMT_F64));
        break;
    case 5 * 8 + 1: /* FISTTP m64 */
        fpu_store(s, lin, st_get(f, 0), FMT_I64, true);
        fpu_pop(f);
        break;
    case 5 * 8 + 2: /* FST m64 */
    case 5 * 8 + 3: /* FSTP m64 */
        fpu_store(s, lin, st_get(f, 0), FMT_F64, false);
        if (reg == 3) {
            fpu_pop(f);
        }
        break;
    case 5 * 8 + 4: /* FRSTOR */
        fpu_restore(s, lin, opsize);
        break;
    case 5 * 8 + 6: /* FNSAVE */
        fpu_save(s, lin, opsize);
        break;
    case 5 * 8 + 7: /* FNSTSW m16 */
        mem_write(s, lin, f->status, SIZE16);
        break;
    case 7 * 8 + 4: /* FBLD */
        fpu_load_bcd(s, lin);
        break;
    case 7 * 8 + 5: /* FILD m64 */
        fpu_push(f, fpu_load(s, lin, FMT_I64));
        break;
    case 7 * 8 + 6: /* FBSTP */
        fpu_store_bcd(s, lin);
        break;
    case 7 * 8 + 7: /* FISTP m64 */
        fpu_store(s, lin, st_get(f, 0), FMT_I64, false);
        fpu_pop(f);
        break;
    default:
        raise_exception(s, EXCP_UD);
    }
}

static void fpu_load_constant(X87State *f, int index)
{
    static const FpuConstant constants[7] = {
        {UINT64_C(0x8000000000000000), 0x3fff, 0},  /* 1 */
        {UINT64_C(0xd49a784bcd1b8afe), 0x4000, -1}, /* log2(10) */
        {UINT64_C(0xb8aa3b295c17f0bc), 0x3fff, 1},  /* log2(e) */
        {UINT64_C(0xc90fdaa22168c235), 0x4000, 1},  /* pi */
        {UINT64_C(0x9a209a84fbcff799), 0x3ffd, 1},  /* log10(2) */
        {UINT64_C(0xb17217f7d1cf79ac), 0x3ffe, 1},  /* ln(2) */
        {0, 0, 0},                                  /* 0 */
    };
    const FpuConstant &c = constants[index];
    uint64_t mant = c.mant;
    switch (get_bits(f->control, FPUC_RC, 2)) {
    case RC_DOWN:
    case RC_ZERO:
        mant -= c.rounded > 0;
        break;
    case RC_UP:
        mant += c.rounded < 0;
        break;
    }
    fpu_push(f, fx80_to_ld(mant, c.sexp));
}

/* D9 E0-FF: the operations without operands. */
static void fpu_exec_d9_special(X86CPUState *s, uint8_t modrm)
{
    X87State *f = &s->fpu;
    long double v;

    switch (modrm) {
    case 0xe2:
    case 0xe3:
    case 0xe6:
    case 0xe7:
    case 0xef:
        raise_exception(s, EXCP_UD);
    }

    HostRounding rounding(f);
    switch (modrm) {
    case 0xe0: /* FCHS */
        st_set(f, 0, -st_get(f, 0));
        break;
    case 0xe1: /* FABS */
        st_set(f, 0, fabsl(st_get(f, 0)));
        break;
    case 0xe4: /* FTST */
        set_fpu_condition(f, fpu_compare(f, st_get(f, 0), 0, false));
        break;
    case 0xe5: /* FXAM */
        fpu_examine(f);
        break;
    case 0xe8 ... 0xee: /* FLD1, FLDL2T, FLDL2E, FLDPI, FLDLG2, FLDLN2, FLDZ */
        fpu_load_constant(f, modrm - 0xe8);
        break;
    case 0xf0: /* F2XM1 */
        v = st_get(f, 0);
        st_set(f, 0, expm1l(v * ln2()));
        break;
    case 0xf1: /* FYL2X */
    case 0xf9: /* FYL2XP1 */ {
        v = st_get(f, 0);
        long double y = st_get(f, 1);
        if (modrm == 0xf1 && v == 0 && std::isfinite(y) && y != 0) {
            fpu_raise(f, bit_at(FPUS_ZE));
        }
        long double log = modrm == 0xf1 ? log2l(v) : log1pl(v) / ln2();
        st_set(f, 1, fpu_checked(f, y * log, v, y));
        fpu_pop(f);
        break;
    }
    case 0xf2: /* FPTAN */
        v = st_get(f, 0);
        if (fpu_trig_argument(f, v, true)) {
            st_set(f, 0, tanl(v));
            fpu_push(f, 1);
        }
        break;
    case 0xf3: /* FPATAN */
        st_set(f, 1, atan2l(st_get(f, 1), st_get(f, 0)));
        fpu_pop(f);
        break;
    case 0xf4: /* FXTRACT */
        v = st_get(f, 0);
        if (v == 0) {
            fpu_raise(f, bit_at(FPUS_ZE));
            st_set(f, 0, -HUGE_VALL);
            fpu_push(f, v);
        } else if (std::isinf(v)) {
            st_set(f, 0, HUGE_VALL);
            fpu_push(f, v);
        } else if (std::isnan(v)) {
            fpu_push(f, v);
        } else {
            int exp = ilogbl(v);
            st_set(f, 0, exp);
            fpu_push(f, scalbnl(v, -exp));
        }
        break;
    case 0xf5: /* FPREM1 */
        fpu_remainder(f, true);
        break;
    case 0xf6: /* FDECSTP */
        fpu_set_top(f, fpu_top(f) - 1);
        set_c1(f, false);
        break;
    case 0xf7: /* FINCSTP */
        fpu_set_top(f, fpu_top(f) + 1);
        set_c1(f, false);
        break;
    case 0xf8: /* FPREM */
        fpu_remainder(f, false);
        break;
    case 0xfa: /* FSQRT */
        v = st_get(f, 0);
        st_set(f, 0, fpu_checked(f, sqrtl(v), v));
        break;
    case 0xfb: /* FSINCOS */
        v = st_get(f, 0);
        if (fpu_trig_argument(f, v, true)) {
            st_set(f, 0, sinl(v));
            fpu_push(f, cosl(v));
        }
        break;
    case 0xfc: /* FRNDINT */
        st_set(f, 0, fpu_round(f, st_get(f, 0)));
        break;
    case 0xfd: { /* FSCALE */
        long double scale = st_get(f, 1);
        long double r;
        v = st_get(f, 0);
        if (std::isnan(v) || std::isnan(scale)) {
            r = v + scale;
        } else if (std::isinf(scale)) {
            /* 0 * 2^inf and inf * 2^-inf are invalid */
            if (scale > 0 ? v == 0 : std::isinf(v)) {
                fpu_raise(f, bit_at(FPUS_IE));
                r = default_nan();
            } else {
                r = copysignl(scale > 0 ? HUGE_VALL : 0, v);
            }
        } else {
            long double exp = fminl(fmaxl(truncl(scale), -65536), 65536);
            r = scalbnl(v, (int)exp);
        }
        st_set(f, 0, r);
        break;
    }
    case 0xfe: /* FSIN */
    case 0xff: /* FCOS */
        v = st_get(f, 0);
        if (fpu_trig_argument(f, v, false)) {
            st_set(f, 0, modrm == 0xfe ? sinl(v) : cosl(v));
        }
        break;
    }
}

static void fpu_exec_reg(X86CPUState *s, int op, uint8_t modrm)
{
    /* DC and DE store into ST(i), which swaps the reversed operations */
    static const int to_st_i[8] = {
        FPU_ADD, FPU_MUL, FPU_COM, FPU_COMP, FPU_SUBR, FPU_SUB, FPU_DIVR,
        FPU_DIV
    };
    /* FCMOVB, FCMOVE, FCMOVBE, FCMOVU as Jcc condition codes */
    static const int fcmov_conditions[4] = {0x2, 0x4, 0x6, 0xa};
    X87State *f = &s->fpu;
    int reg = get_bits(modrm, 3, 3);
    int rm = get_bits(modrm, 0, 3);
    long double v;

    switch (op) {
    case 0: /* D8 */
        fpu_arith_st0(f, reg, st_get(f, rm));
        return;
    case 4: /* DC */
    case 6: /* DE */
        if (reg == FPU_COM || reg == FPU_COMP) {
            bool compp = op == 6 && reg == FPU_COMP;
            if (compp && rm != 1) {
                raise_exception(s, EXCP_UD);
            }
            set_fpu_condition(f, fpu_compare(f, st_get(f, 0), st_get(f, rm),
                                             false));
            if (reg == FPU_COMP || op == 6) {
                fpu_pop(f);
            }
            if (compp) {
                fpu_pop(f);
            }
        } else {
            st_set(f, rm, fpu_arith(f, to_st_i[reg], st_get(f, rm),
                                    st_get(f, 0)));
            if (op == 6) {
                fpu_pop(f);
            }
        }
        return;
    }

    switch (op * 8 + reg) {
    case 1 * 8 + 0: /* FLD ST(i) */
        fpu_push(f, st_get(f, rm));
        break;
    case 1 * 8 + 1: /* FXCH */
    case 5 * 8 + 1:
    case 7 * 8 + 1:
        v = st_get(f, 0);
        st_set(f, 0, st_get(f, rm));
        st_set(f, rm, v);
        break;
    case 1 * 8 + 2: /* FNOP */
        if (rm != 0) {
            raise_exception(s, EXCP_UD);
        }
        break;
    case 1 * 8 + 3: /* FSTP aliases */
    case 7 * 8 + 2:
    case 7 * 8 + 3:
    case 5 * 8 + 3: /* FSTP ST(i) */
        st_set(f, rm, st_get(f, 0));
        fpu_pop(f);
        break;
    case 5 * 8 + 2: /* FST ST(i) */
        st_set(f, rm, st_get(f, 0));
        break;
    case 1 * 8 + 4 ... 1 * 8 + 7:
        fpu_exec_d9_special(s, modrm);
        break;
    case 2 * 8 + 0 ... 2 * 8 + 3: /* FCMOVcc */
    case 3 * 8 + 0 ... 3 * 8 + 3: { /* FCMOVNcc */
        /* a stack underflow stores the indefinite whatever the condition */
        bool underflow = get_bit(f->empty, st_reg(f, 0)) ||
            get_bit(f->empty, st_reg(f, rm));
        v = st_get(f, rm);
        if (underflow || test_condition(s, fcmov_conditions[reg] + (op == 3))) {
            st_set(f, 0, underflow ? default_nan() : v);
        }
        break;
    }
    case 2 * 8 + 5: /* FUCOMPP */
        if (rm != 1) {
            raise_exception(s, EXCP_UD);
        }
        set_fpu_condition(f, fpu_compare(f, st_get(f, 0), st_get(f, 1),
                                         true));
        fpu_pop(f);
        fpu_pop(f);
        break;
    case 3 * 8 + 4:
        switch (rm) {
        case 0: /* FENI, FDISI, FSETPM */
        case 1:
        case 4:
            break;
        case 2: /* FNCLEX */
            f->status &= ~(FPUS_EXCEPTIONS | bit_at(FPUS_SF) |
                           bit_at(FPUS_ES) | bit_at(FPUS_B));
            break;
        case 3: /* FNINIT */
            fpu_init(f);
            break;
        default:
            raise_exception(s, EXCP_UD);
        }
        break;
    case 3 * 8 + 5: /* FUCOMI */
    case 3 * 8 + 6: /* FCOMI */
    case 7 * 8 + 5: /* FUCOMIP */
    case 7 * 8 + 6: /* FCOMIP */
        set_eflags_condition(s, fpu_compare(f, st_get(f, 0), st_get(f, rm),
                                            reg == 5));
        if (op == 7) {
            fpu_pop(f);
        }
        break;
    case 5 * 8 + 0: /* FFREE */
    case 7 * 8 + 0: /* FFREEP */
        f->empty = set_bit(f->empty, st_reg(f, rm), true);
        if (op == 7) {
            fpu_pop(f);
        }
        break;
    case 5 * 8 + 4: /* FUCOM */
    case 5 * 8 + 5: /* FUCOMP */
        set_fpu_condition(f, fpu_compare(f, st_get(f, 0), st_get(f, rm),
                                         true));
        if (reg == 5) {
            fpu_pop(f);
        }
        break;
    case 7 * 8 + 4: /* FNSTSW AX */
        if (rm != 0) {
            raise_exception(s, EXCP_UD);
        }
        s->regs[REG_EAX] = set_bits(s->regs[REG_EAX], 0, 16, f->status);
        break;
    default:
        raise_exception(s, EXCP_UD);
    }
}

/* Instructions that neither report pending exceptions nor record the
   instruction pointers. */
static bool fpu_is_control(int op, uint8_t modrm, bool is_mem)
{
    int reg = get_bits(modrm, 3, 3);
    if (is_mem) {
        return (op == 1 && reg >= 4) || (op == 5 && reg >= 4);
    }
    return (op == 3 && reg == 4) || (op == 7 && modrm == 0xe0);
}

void fpu_check_pending(X86CPUState *s)
{
    if (get_bit(s->fpu.status, FPUS_ES) && get_bit(s->cr0, CR0_NE)) {
        raise_exception(s, EXCP_MF);
    }
}

void fpu_exec(X86CPUState *s, uint8_t opcode, uint8_t modrm, uint32_t lin,
              uint32_t ea, int ea_seg, int opsize)
{
    X87State *f = &s->fpu;
    int op = get_bits(opcode, 0, 3);
    bool is_mem = get_bits(modrm, 6, 2) != 3;

    if (!fpu_is_control(op, modrm, is_mem)) {
        fpu_check_pending(s);
        f->fip = s->eip;
        f->fcs = s->segs[SEG_CS].sel;
        f->opcode = set_bits(modrm, 8, 3, op);
        if (is_mem) {
            f->fdp = ea;
            f->fds = s->segs[ea_seg].sel;
        }
    }
    if (is_mem) {
        fpu_exec_mem(s, op, get_bits(modrm, 3, 3), lin, opsize);
    } else {
        fpu_exec_reg(s, op, modrm);
    }
}

void fpu_reset(X86CPUState *s)
{
    fpu_init(&s->fpu);
}
