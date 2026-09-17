/*
 * x86 CPU emulator: instruction interpreter
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
#include <stdio.h>

#include "x86_cpu_priv.h"

/* Instructions modify guest memory before registers and flags: a fault then
   leaves the state as it was when the instruction started. A read-modify-
   write operand is probed for writing before anything is computed. */

namespace {

enum {
    REP_NONE,
    REP_Z,
    REP_NZ,
};

enum {
    ALU_ADD, ALU_OR, ALU_ADC, ALU_SBB, ALU_AND, ALU_SUB, ALU_XOR, ALU_CMP,
};

enum {
    SHIFT_ROL, SHIFT_ROR, SHIFT_RCL, SHIFT_RCR,
    SHIFT_SHL, SHIFT_SHR, SHIFT_SAL, SHIFT_SAR,
};

enum {
    BT_TEST, BT_SET, BT_RESET, BT_COMPLEMENT,
};

enum {
    STR_MOVS, STR_CMPS, STR_STOS, STR_LODS, STR_SCAS, STR_INS, STR_OUTS,
};

/* String iterations run before interrupts are looked at again. */
const int STRING_BATCH = 4096;

struct Decoder {
    uint32_t eip;        /* offset of the next byte */
    uint32_t eip_mask;
    uint32_t cs_base;
    int opsize;
    bool addr32;
    int seg_override;    /* -1 if none */
    int rep;
    bool lock;
    uint32_t esp_addend; /* POP computes its operand with ESP popped */
};

struct Operand {
    bool is_reg;
    uint8_t reg;
    uint8_t seg;
    uint32_t ea;
};

}


//#pragma mark - decoding

static inline int modrm_mod(uint8_t modrm)
{
    return get_bits(modrm, 6, 2);
}

static inline int modrm_reg(uint8_t modrm)
{
    return get_bits(modrm, 3, 3);
}

static inline int modrm_rm(uint8_t modrm)
{
    return get_bits(modrm, 0, 3);
}

static inline uint8_t fetch8(X86CPUState *s, Decoder &d)
{
    uint32_t lin = d.cs_base + (d.eip & d.eip_mask);
    d.eip++;
    if (likely(page_base(lin) == s->code_tag)) {
        return *(uint8_t *)(s->code_addend + lin);
    }
    return fetch_slow(s, lin);
}

static inline uint32_t fetch_imm(X86CPUState *s, Decoder &d, int size)
{
    uint32_t lin = d.cs_base + (d.eip & d.eip_mask);
    if (likely(page_base(lin) == s->code_tag &&
               page_offset(lin) <= (uint32_t)(PAGE_SIZE - size_bytes(size)))) {
        d.eip += size_bytes(size);
        return host_load((uint8_t *)(s->code_addend + lin), size);
    }
    uint32_t val = 0;
    for (int i = 0; i < size_bytes(size); i++) {
        val = set_bits(val, 8 * i, 8, fetch8(s, d));
    }
    return val;
}

static inline int32_t fetch_simm(X86CPUState *s, Decoder &d, int size)
{
    return sext_size(fetch_imm(s, d, size), size);
}

static void decode_prefix(X86CPUState *s, Decoder &d, uint8_t b)
{
    switch (b) {
    case 0x26:
    case 0x2e:
    case 0x36:
    case 0x3e:
        d.seg_override = get_bits(b, 3, 2);
        break;
    case 0x64:
    case 0x65:
        d.seg_override = SEG_FS + get_bit(b, 0);
        break;
    case 0x66:
        d.opsize = s->code32 ? SIZE16 : SIZE32;
        break;
    case 0x67:
        d.addr32 = !s->code32;
        break;
    case 0xf0:
        d.lock = true;
        break;
    case 0xf2:
        d.rep = REP_NZ;
        break;
    case 0xf3:
        d.rep = REP_Z;
        break;
    }
}

/* Whether LOCK may prefix the instruction whose opcode starts with 'b': one
   that modifies a memory operand. Nothing is consumed. */
static bool lock_allowed(X86CPUState *s, const Decoder &d, uint8_t b)
{
    Decoder peek = d;
    int opcode = b == 0x0f ? 0x100 | fetch8(s, peek) : b;
    int reg_min = 0, reg_max = 7;

    switch (opcode) {
    case 0x26: case 0x2e: case 0x36: case 0x3e: case 0x64 ... 0x67:
    case 0xf0: case 0xf2: case 0xf3:
        return true;
    case 0x00: case 0x01: case 0x08: case 0x09: case 0x10: case 0x11:
    case 0x18: case 0x19: case 0x20: case 0x21: case 0x28: case 0x29:
    case 0x30: case 0x31: case 0x86: case 0x87:
    case 0x1ab: case 0x1b3: case 0x1bb: case 0x1b0: case 0x1b1:
    case 0x1c0: case 0x1c1:
        break;
    case 0x80 ... 0x83: /* not CMP */
        reg_max = 6;
        break;
    case 0xf6: case 0xf7: /* NOT, NEG */
        reg_min = 2;
        reg_max = 3;
        break;
    case 0xfe: case 0xff: /* INC, DEC */
        reg_max = 1;
        break;
    case 0x1ba: /* BTS, BTR, BTC */
        reg_min = 5;
        break;
    case 0x1c7: /* CMPXCHG8B */
        reg_min = 1;
        reg_max = 1;
        break;
    default:
        return false;
    }
    uint8_t modrm = fetch8(s, peek);
    int reg = modrm_reg(modrm);
    return modrm_mod(modrm) != 3 && reg >= reg_min && reg <= reg_max;
}

static Operand decode_modrm(X86CPUState *s, Decoder &d, uint8_t modrm)
{
    Operand op;
    int mod = modrm_mod(modrm);
    int rm = modrm_rm(modrm);

    op.reg = rm;
    op.seg = SEG_DS;
    op.ea = 0;
    op.is_reg = mod == 3;
    if (op.is_reg) {
        return op;
    }

    int seg = SEG_DS;
    uint32_t ea = 0;
    if (d.addr32) {
        int base = rm;
        if (rm == REG_ESP) {
            uint8_t sib = fetch8(s, d);
            int index = get_bits(sib, 3, 3);
            base = get_bits(sib, 0, 3);
            if (index != REG_ESP) {
                ea = s->regs[index] << get_bits(sib, 6, 2);
            }
        }
        if (base == REG_EBP && mod == 0) {
            ea += fetch_imm(s, d, SIZE32);
        } else {
            ea += s->regs[base];
            if (base == REG_ESP) {
                ea += d.esp_addend;
            }
            if (base == REG_ESP || base == REG_EBP) {
                seg = SEG_SS;
            }
        }
        if (mod == 1) {
            ea += fetch_simm(s, d, SIZE8);
        } else if (mod == 2) {
            ea += fetch_imm(s, d, SIZE32);
        }
    } else {
        static const uint8_t base_reg[8] = {
            REG_EBX, REG_EBX, REG_EBP, REG_EBP, REG_ESI, REG_EDI, REG_EBP,
            REG_EBX
        };
        static const uint8_t index_reg[8] = {
            REG_ESI, REG_EDI, REG_ESI, REG_EDI, 0xff, 0xff, 0xff, 0xff
        };
        if (mod == 0 && rm == 6) {
            ea = fetch_imm(s, d, SIZE16);
        } else {
            ea = s->regs[base_reg[rm]];
            if (index_reg[rm] != 0xff) {
                ea += s->regs[index_reg[rm]];
            }
            if (base_reg[rm] == REG_EBP) {
                seg = SEG_SS;
            }
            if (mod == 1) {
                ea += fetch_simm(s, d, SIZE8);
            } else if (mod == 2) {
                ea += fetch_imm(s, d, SIZE16);
            }
        }
        ea = get_bits(ea, 0, 16);
    }
    op.seg = d.seg_override >= 0 ? d.seg_override : seg;
    op.ea = ea;
    return op;
}

static inline Operand fetch_modrm(X86CPUState *s, Decoder &d, uint8_t *modrm)
{
    *modrm = fetch8(s, d);
    return decode_modrm(s, d, *modrm);
}

static inline uint32_t addr_mask(const Decoder &d)
{
    return d.addr32 ? UINT32_MAX : 0xffff;
}

static inline int data_seg(const Decoder &d)
{
    return d.seg_override >= 0 ? d.seg_override : SEG_DS;
}

static inline uint32_t next_eip(const Decoder &d)
{
    return d.eip & d.eip_mask;
}

static inline void jump_rel(Decoder &d, uint32_t rel)
{
    d.eip = trunc_size(d.eip + rel, d.opsize);
}


//#pragma mark - operands

static inline uint32_t reg_read(X86CPUState *s, int reg, int size)
{
    switch (size) {
    case SIZE8:
        return get_bits(s->regs[get_bits(reg, 0, 2)],
                        get_bit(reg, 2) ? 8 : 0, 8);
    case SIZE16:
        return get_bits(s->regs[reg], 0, 16);
    default:
        return s->regs[reg];
    }
}

static inline void reg_write(X86CPUState *s, int reg, uint32_t val, int size)
{
    switch (size) {
    case SIZE8: {
        uint32_t *r = &s->regs[get_bits(reg, 0, 2)];
        *r = set_bits(*r, get_bit(reg, 2) ? 8 : 0, 8, val);
        break;
    }
    case SIZE16:
        s->regs[reg] = set_bits(s->regs[reg], 0, 16, val);
        break;
    default:
        s->regs[reg] = val;
        break;
    }
}

static inline void reg_add_masked(X86CPUState *s, int reg, int32_t delta,
                                  uint32_t mask)
{
    s->regs[reg] = (s->regs[reg] & ~mask) | ((s->regs[reg] + delta) & mask);
}

/* The linear address of 'size' bytes 'disp' bytes into a memory operand. */
static inline uint32_t op_address(X86CPUState *s, const Operand &op,
                                  uint32_t disp, int size, bool write)
{
    return seg_address(s, op.seg, op.ea + disp, size, write);
}

static inline uint32_t rm_read(X86CPUState *s, const Operand &op, int size)
{
    if (op.is_reg) {
        return reg_read(s, op.reg, size);
    }
    return mem_read(s, op_address(s, op, 0, size, false), size);
}

static inline void rm_write(X86CPUState *s, const Operand &op, uint32_t val,
                            int size)
{
    if (op.is_reg) {
        reg_write(s, op.reg, val, size);
    } else {
        mem_write(s, op_address(s, op, 0, size, true), val, size);
    }
}

static inline uint32_t mem_read_modify(X86CPUState *s, uint32_t lin, int size)
{
    X86TLBEntry *e = tlb_entry(s, s->mmu_idx, lin);
    if (likely(tlb_hit(e->write, lin, size))) {
        return host_load((uint8_t *)(e->addend + lin), size);
    }
    mem_probe_write(s, lin, size);
    return mem_read(s, lin, size);
}

/* Read an operand that is written back afterwards. */
static inline uint32_t rm_read_modify(X86CPUState *s, const Operand &op,
                                      int size)
{
    if (op.is_reg) {
        return reg_read(s, op.reg, size);
    }
    return mem_read_modify(s, op_address(s, op, 0, size, true), size);
}

static inline void push(X86CPUState *s, uint32_t val, int size)
{
    StackPtr st = current_stack(s);
    stack_push(s, &st, val, size);
    stack_commit(s, st);
}

static inline uint32_t pop(X86CPUState *s, int size)
{
    StackPtr st = current_stack(s);
    uint32_t val = stack_pop(s, &st, size);
    stack_commit(s, st);
    return val;
}

static inline bool in_protected_mode(X86CPUState *s)
{
    return get_bit(s->cr0, CR0_PE) && !get_bit(s->eflags, EFLAGS_VM);
}

static void require_cpl0(X86CPUState *s)
{
    if (s->cpl != 0) {
        raise_exception(s, EXCP_GP, 0);
    }
}

static void require_protected_mode(X86CPUState *s)
{
    if (!in_protected_mode(s)) {
        raise_exception(s, EXCP_UD);
    }
}

static void set_zero_flag(X86CPUState *s, bool zero)
{
    set_cc_eflags(s, set_bit(cc_eflags(s), EFLAGS_ZF, zero));
}

static void set_carry_flag(X86CPUState *s, bool carry)
{
    set_cc_eflags(s, set_bit(cc_eflags(s), EFLAGS_CF, carry));
}

static uint32_t port_in(X86CPUState *s, uint32_t port, int size)
{
    if (s->port_io == nullptr) {
        return size_mask(size);
    }
    DeviceLocker locker(*s->device_lock);
    return trunc_size(s->port_io->DeviceRead(port, size), size);
}

static void port_out(X86CPUState *s, uint32_t port, uint32_t val, int size)
{
    if (s->port_io != nullptr) {
        DeviceLocker locker(*s->device_lock);
        s->port_io->DeviceWrite(port, trunc_size(val, size), size);
    }
}


//#pragma mark - arithmetic

static uint32_t alu(X86CPUState *s, int op, uint32_t a, uint32_t b, int size)
{
    uint32_t r;
    bool carry;

    switch (op) {
    case ALU_ADD:
        r = a + b;
        set_cc(s, CC_OP_ADD, size, b, r);
        break;
    case ALU_OR:
        r = a | b;
        set_cc(s, CC_OP_LOGIC, size, 0, r);
        break;
    case ALU_ADC:
        carry = cc_carry(s);
        r = a + b + carry;
        set_cc_carry(s, CC_OP_ADC, size, b, r, carry);
        break;
    case ALU_SBB:
        carry = cc_carry(s);
        r = a - b - carry;
        set_cc_carry(s, CC_OP_SBB, size, b, r, carry);
        break;
    case ALU_AND:
        r = a & b;
        set_cc(s, CC_OP_LOGIC, size, 0, r);
        break;
    case ALU_XOR:
        r = a ^ b;
        set_cc(s, CC_OP_LOGIC, size, 0, r);
        break;
    default: /* SUB, CMP */
        r = a - b;
        set_cc(s, CC_OP_SUB, size, b, r);
        break;
    }
    return trunc_size(r, size);
}

static uint32_t inc_dec(X86CPUState *s, uint32_t val, bool dec, int size)
{
    bool carry = cc_carry(s);
    uint32_t r = dec ? val - 1 : val + 1;
    set_cc(s, dec ? CC_OP_DEC : CC_OP_INC, size, carry, r);
    return trunc_size(r, size);
}

static uint32_t rotate(uint32_t val, unsigned count, int bits, bool left)
{
    count %= bits;
    if (count == 0) {
        return val;
    }
    if (!left) {
        count = bits - count;
    }
    return ((val << count) | (val >> (bits - count))) & bit_mask(bits);
}

static uint32_t shift(X86CPUState *s, int op, uint32_t val, unsigned count,
                      int size)
{
    int bits = size_bits(size);
    uint32_t r;
    bool cf;

    count = get_bits(count, 0, 5);
    if (count == 0) {
        return val;
    }
    switch (op) {
    case SHIFT_ROL:
        r = rotate(val, count, bits, true);
        cf = get_bit(r, 0);
        break;
    case SHIFT_ROR:
        r = rotate(val, count, bits, false);
        cf = msb(r, size);
        break;
    case SHIFT_RCL:
    case SHIFT_RCR: {
        count %= bits + 1;
        if (count == 0) {
            return val;
        }
        /* a rotation of the carry and the operand together */
        uint64_t wide = concat_bits(cc_carry(s), val, bits);
        if (op == SHIFT_RCR) {
            count = bits + 1 - count;
        }
        wide = get_bits((wide << count) | (wide >> (bits + 1 - count)), 0,
                          bits + 1);
        r = trunc_size(wide, size);
        cf = get_bits(wide, bits, 1);
        break;
    }
    case SHIFT_SHR:
        set_cc(s, CC_OP_SAR, size, val >> (count - 1), val >> count);
        return trunc_size(val >> count, size);
    case SHIFT_SAR: {
        int32_t sval = sext_size(val, size);
        set_cc(s, CC_OP_SAR, size, sval >> (count - 1), sval >> count);
        return trunc_size(sval >> count, size);
    }
    default: /* SHL, SAL */
        set_cc(s, CC_OP_SHL, size, val << (count - 1), val << count);
        return trunc_size(val << count, size);
    }

    /* rotations change CF and OF only */
    bool of = op == SHIFT_ROL || op == SHIFT_RCL ? msb(r, size) != cf :
        msb(r, size) != get_bit(r, bits - 2);
    uint32_t flags = set_bit(cc_eflags(s), EFLAGS_CF, cf);
    set_cc_eflags(s, set_bit(flags, EFLAGS_OF, of));
    return r;
}

static uint32_t shift_double(X86CPUState *s, bool left, uint32_t dst,
                             uint32_t src, unsigned count, int size)
{
    int bits = size_bits(size);

    count = get_bits(count, 0, 5);
    if (count == 0) {
        return dst;
    }
    if (left) {
        uint64_t wide = concat_bits(dst, src, bits);
        uint32_t r = get_bits(wide << count, bits, bits);
        set_cc(s, CC_OP_SHL, size, get_bits(wide << (count - 1), bits, bits),
               r);
        return r;
    }
    uint64_t wide = concat_bits(src, dst, bits);
    uint32_t r = trunc_size(wide >> count, size);
    set_cc(s, CC_OP_SAR, size, wide >> (count - 1), r);
    return r;
}

static uint32_t imul(X86CPUState *s, uint32_t a, uint32_t b, int size)
{
    int64_t r = (int64_t)sext_size(a, size) * sext_size(b, size);
    uint32_t low = trunc_size(r, size);
    set_cc(s, CC_OP_MUL, size, r != sext_size(low, size), low);
    return low;
}

/* MUL, IMUL, DIV and IDIV of the accumulator, by group 3 index. */
static void mul_div(X86CPUState *s, int op, uint32_t val, int size)
{
    int bits = size_bits(size);
    int high_reg = size == SIZE8 ? 4 /* AH */ : REG_EDX;
    uint64_t low = reg_read(s, REG_EAX, size);
    uint64_t high = reg_read(s, high_reg, size);
    uint32_t mask = size_mask(size);
    uint32_t res_low, res_high;

    switch (op) {
    case 4: {
        uint64_t r = low * val;
        res_low = get_bits(r, 0, bits);
        res_high = get_bits(r, bits, bits);
        set_cc(s, CC_OP_MUL, size, res_high != 0, res_low);
        break;
    }
    case 5: {
        int64_t r = (int64_t)sext_size(low, size) * sext_size(val, size);
        res_low = get_bits(r, 0, bits);
        res_high = get_bits(r, bits, bits);
        set_cc(s, CC_OP_MUL, size, r != sext_size(res_low, size), res_low);
        break;
    }
    case 6: {
        uint64_t n = concat_bits(high, low, bits);
        if (val == 0 || n / val > mask) {
            raise_exception(s, EXCP_DE);
        }
        res_low = n / val;
        res_high = n % val;
        break;
    }
    default: {
        int64_t n = sign_extend(concat_bits(high, low, bits), 2 * bits);
        int64_t v = sext_size(val, size);
        if (v == 0 || (n == INT64_MIN && v == -1)) {
            raise_exception(s, EXCP_DE);
        }
        int64_t q = n / v;
        if (q != sext_size(q & mask, size)) {
            raise_exception(s, EXCP_DE);
        }
        res_low = q & mask;
        res_high = (n % v) & mask;
        break;
    }
    }
    reg_write(s, REG_EAX, res_low, size);
    reg_write(s, high_reg, res_high, size);
}

static uint32_t bit_test(X86CPUState *s, int op, uint32_t val,
                         uint32_t offset, int size)
{
    int pos = get_bits(offset, 0, 3 + size);
    bool bit = get_bit(val, pos);
    set_carry_flag(s, bit);
    switch (op) {
    case BT_SET:
        return set_bit(val, pos, true);
    case BT_RESET:
        return set_bit(val, pos, false);
    case BT_COMPLEMENT:
        return set_bit(val, pos, !bit);
    default:
        return val;
    }
}

/* Flags of a result, with CF and AF given. */
static void set_result_flags(X86CPUState *s, uint32_t val, int size, bool cf,
                             bool af)
{
    set_cc(s, CC_OP_LOGIC, size, 0, val);
    uint32_t flags = set_bit(cc_eflags(s), EFLAGS_CF, cf);
    set_cc_eflags(s, set_bit(flags, EFLAGS_AF, af));
}

static void decimal_adjust(X86CPUState *s, bool subtract)
{
    uint32_t flags = cc_eflags(s);
    uint32_t al = reg_read(s, REG_EAX, SIZE8);
    bool cf = al > 0x99 || get_bit(flags, EFLAGS_CF);
    bool af = get_bits(al, 0, 4) > 9 || get_bit(flags, EFLAGS_AF);
    int32_t adjust = (af ? 0x06 : 0) + (cf ? 0x60 : 0);
    /* a borrow out of the low adjustment also sets CF */
    cf = cf || (subtract && af && al < 0x06);
    al = trunc_size(subtract ? al - adjust : al + adjust, SIZE8);
    reg_write(s, REG_EAX, al, SIZE8);
    set_result_flags(s, al, SIZE8, cf, af);
}

static void ascii_adjust(X86CPUState *s, bool subtract)
{
    uint32_t flags = cc_eflags(s);
    uint32_t ax = reg_read(s, REG_EAX, SIZE16);
    bool adjust = get_bits(ax, 0, 4) > 9 || get_bit(flags, EFLAGS_AF);
    if (adjust) {
        if (subtract) {
            ax -= 6;
            ax = set_bits(ax, 8, 8, get_bits(ax, 8, 8) - 1);
        } else {
            ax += 0x106;
        }
    }
    reg_write(s, REG_EAX, set_bits(ax, 4, 4, 0), SIZE16);
    flags = set_bit(flags, EFLAGS_CF, adjust);
    set_cc_eflags(s, set_bit(flags, EFLAGS_AF, adjust));
}


//#pragma mark - instruction groups

static void exec_alu(X86CPUState *s, Decoder &d, uint8_t b)
{
    int op = get_bits(b, 3, 3);
    int size = get_bit(b, 0) ? d.opsize : SIZE8;
    uint8_t modrm;

    switch (get_bits(b, 0, 3)) {
    case 0:
    case 1: {
        Operand dst = fetch_modrm(s, d, &modrm);
        uint32_t a = op == ALU_CMP ? rm_read(s, dst, size) :
            rm_read_modify(s, dst, size);
        uint32_t r = alu(s, op, a, reg_read(s, modrm_reg(modrm), size), size);
        if (op != ALU_CMP) {
            rm_write(s, dst, r, size);
        }
        break;
    }
    case 2:
    case 3: {
        Operand src = fetch_modrm(s, d, &modrm);
        int reg = modrm_reg(modrm);
        uint32_t r = alu(s, op, reg_read(s, reg, size), rm_read(s, src, size),
                         size);
        if (op != ALU_CMP) {
            reg_write(s, reg, r, size);
        }
        break;
    }
    default: {
        uint32_t imm = fetch_imm(s, d, size);
        uint32_t r = alu(s, op, reg_read(s, REG_EAX, size), imm, size);
        if (op != ALU_CMP) {
            reg_write(s, REG_EAX, r, size);
        }
        break;
    }
    }
}

static void exec_group1(X86CPUState *s, Decoder &d, uint8_t b)
{
    int size = get_bit(b, 0) ? d.opsize : SIZE8;
    uint8_t modrm;
    Operand dst = fetch_modrm(s, d, &modrm);
    int op = modrm_reg(modrm);
    uint32_t imm = b == 0x83 ? fetch_simm(s, d, SIZE8) : fetch_imm(s, d, size);
    uint32_t a = op == ALU_CMP ? rm_read(s, dst, size) :
        rm_read_modify(s, dst, size);
    uint32_t r = alu(s, op, a, trunc_size(imm, size), size);
    if (op != ALU_CMP) {
        rm_write(s, dst, r, size);
    }
}

static void exec_shift_group(X86CPUState *s, Decoder &d, uint8_t b)
{
    int size = get_bit(b, 0) ? d.opsize : SIZE8;
    uint8_t modrm;
    Operand dst = fetch_modrm(s, d, &modrm);
    unsigned count;
    if (b <= 0xc1) {
        count = fetch8(s, d);
    } else if (b <= 0xd1) {
        count = 1;
    } else {
        count = reg_read(s, REG_ECX, SIZE8);
    }
    uint32_t val = rm_read_modify(s, dst, size);
    rm_write(s, dst, shift(s, modrm_reg(modrm), val, count, size), size);
}

static void exec_group3(X86CPUState *s, Decoder &d, uint8_t b)
{
    int size = get_bit(b, 0) ? d.opsize : SIZE8;
    uint8_t modrm;
    Operand dst = fetch_modrm(s, d, &modrm);
    int op = modrm_reg(modrm);
    uint32_t val;

    switch (op) {
    case 0:
    case 1:
        val = rm_read(s, dst, size) & fetch_imm(s, d, size);
        set_cc(s, CC_OP_LOGIC, size, 0, val);
        break;
    case 2:
        val = rm_read_modify(s, dst, size);
        rm_write(s, dst, ~val, size);
        break;
    case 3:
        val = rm_read_modify(s, dst, size);
        rm_write(s, dst, -val, size);
        set_cc(s, CC_OP_SUB, size, val, -val);
        break;
    default:
        mul_div(s, op, rm_read(s, dst, size), size);
        break;
    }
}

static void exec_string(X86CPUState *s, Decoder &d, int kind, int size)
{
    uint32_t amask = addr_mask(d);
    int32_t step = get_bit(s->eflags, EFLAGS_DF) ? -size_bytes(size) :
        size_bytes(size);
    int src_seg = data_seg(d);
    uint32_t port = reg_read(s, REG_EDX, SIZE16);
    bool rep = d.rep != REP_NONE;
    bool uses_src = kind == STR_MOVS || kind == STR_CMPS ||
        kind == STR_LODS || kind == STR_OUTS;
    bool uses_dst = kind != STR_LODS && kind != STR_OUTS;
    bool writes_dst = kind == STR_MOVS || kind == STR_STOS || kind == STR_INS;

    if (kind == STR_INS || kind == STR_OUTS) {
        check_io_permission(s, port, size);
    }
    for (int count = 0;; count++) {
        if (rep) {
            if ((s->regs[REG_ECX] & amask) == 0) {
                break;
            }
            if (count == STRING_BATCH) {
                /* come back to it after looking at interrupts */
                d.eip = s->eip;
                break;
            }
        }
        uint32_t src = 0, dst = 0;
        if (uses_src) {
            src = seg_address(s, src_seg, s->regs[REG_ESI] & amask, size,
                              false);
        }
        if (uses_dst) {
            dst = seg_address(s, SEG_ES, s->regs[REG_EDI] & amask, size,
                              writes_dst);
        }
        switch (kind) {
        case STR_MOVS:
            mem_write(s, dst, mem_read(s, src, size), size);
            break;
        case STR_CMPS: {
            uint32_t a = mem_read(s, src, size);
            alu(s, ALU_CMP, a, mem_read(s, dst, size), size);
            break;
        }
        case STR_STOS:
            mem_write(s, dst, reg_read(s, REG_EAX, size), size);
            break;
        case STR_LODS:
            reg_write(s, REG_EAX, mem_read(s, src, size), size);
            break;
        case STR_SCAS:
            alu(s, ALU_CMP, reg_read(s, REG_EAX, size),
                mem_read(s, dst, size), size);
            break;
        case STR_INS:
            /* the port is read once the buffer is known to be writable */
            mem_probe_write(s, dst, size);
            mem_write(s, dst, port_in(s, port, size), size);
            break;
        default:
            port_out(s, port, mem_read(s, src, size), size);
            break;
        }
        if (uses_src) {
            reg_add_masked(s, REG_ESI, step, amask);
        }
        if (uses_dst) {
            reg_add_masked(s, REG_EDI, step, amask);
        }
        if (!rep) {
            break;
        }
        reg_add_masked(s, REG_ECX, -1, amask);
        if ((kind == STR_CMPS || kind == STR_SCAS) &&
            cc_zero(s) != (d.rep == REP_Z)) {
            break;
        }
    }
}

static void pop_seg(X86CPUState *s, int seg, int opsize)
{
    StackPtr st = current_stack(s);
    uint32_t sel = stack_pop(s, &st, opsize);
    load_seg(s, seg, sel);
    stack_commit(s, st);
    if (seg == SEG_SS) {
        s->irq_inhibit = true;
    }
}

/* LDS, LES, LFS, LGS and LSS. */
static void load_far_pointer(X86CPUState *s, Decoder &d, int seg)
{
    uint8_t modrm;
    Operand src = fetch_modrm(s, d, &modrm);
    if (src.is_reg) {
        raise_exception(s, EXCP_UD);
    }
    uint32_t offset = mem_read(s, op_address(s, src, 0, d.opsize, false),
                               d.opsize);
    uint32_t sel = mem_read(s, op_address(s, src, size_bytes(d.opsize), SIZE16,
                                          false), SIZE16);
    load_seg(s, seg, sel);
    reg_write(s, modrm_reg(modrm), offset, d.opsize);
}

/* Store a selector or machine status word: registers take the operand
   size, memory 16 bits. */
static void store_word(X86CPUState *s, Decoder &d, const Operand &dst,
                       uint32_t val)
{
    if (dst.is_reg) {
        reg_write(s, dst.reg, val, d.opsize);
    } else {
        mem_write(s, op_address(s, dst, 0, SIZE16, true), val, SIZE16);
    }
}

static void exec_group6(X86CPUState *s, Decoder &d)
{
    require_protected_mode(s);
    uint8_t modrm;
    Operand op = fetch_modrm(s, d, &modrm);

    switch (modrm_reg(modrm)) {
    case 0: /* SLDT */
        store_word(s, d, op, s->ldt.sel);
        break;
    case 1: /* STR */
        store_word(s, d, op, s->tr.sel);
        break;
    case 2: /* LLDT */
        require_cpl0(s);
        load_ldt(s, rm_read(s, op, SIZE16));
        break;
    case 3: /* LTR */
        require_cpl0(s);
        load_tr(s, rm_read(s, op, SIZE16));
        break;
    case 4: /* VERR */
    case 5: /* VERW */
        set_zero_flag(s, seg_verify(s, rm_read(s, op, SIZE16),
                                    modrm_reg(modrm) == 5));
        break;
    default:
        raise_exception(s, EXCP_UD);
    }
}

static void exec_group7(X86CPUState *s, Decoder &d)
{
    uint8_t modrm;
    Operand op = fetch_modrm(s, d, &modrm);
    int reg = modrm_reg(modrm);
    X86CPUSeg *table = get_bit(reg, 0) ? &s->idt : &s->gdt;

    if (op.is_reg && reg != 4 && reg != 6) {
        raise_exception(s, EXCP_UD);
    }
    switch (reg) {
    case 0: /* SGDT */
    case 1: /* SIDT */ {
        uint32_t lin = op_address(s, op, 0, SIZE16, true);
        uint32_t lin_base = op_address(s, op, 2, SIZE32, true);
        uint32_t base = d.opsize == SIZE16 ? get_bits(table->base, 0, 24) :
            table->base;
        mem_probe_write(s, lin_base, SIZE32);
        mem_write(s, lin, table->limit, SIZE16);
        mem_write(s, lin_base, base, SIZE32);
        break;
    }
    case 2: /* LGDT */
    case 3: /* LIDT */ {
        require_cpl0(s);
        uint32_t limit = mem_read(s, op_address(s, op, 0, SIZE16, false),
                                  SIZE16);
        uint32_t base = mem_read(s, op_address(s, op, 2, SIZE32, false),
                                 SIZE32);
        table->base = d.opsize == SIZE16 ? get_bits(base, 0, 24) : base;
        table->limit = limit;
        break;
    }
    case 4: /* SMSW */
        store_word(s, d, op, s->cr0);
        break;
    case 6: /* LMSW, which cannot clear PE */
        require_cpl0(s);
        cpu_set_cr0(s, set_bits(s->cr0, 0, 4, rm_read(s, op, SIZE16) |
                                get_bit(s->cr0, CR0_PE)));
        break;
    case 7: /* INVLPG */
        require_cpl0(s);
        tlb_flush_page(s, s->segs[op.seg].base + op.ea);
        break;
    default:
        raise_exception(s, EXCP_UD);
    }
}

static void exec_fpu(X86CPUState *s, Decoder &d, uint8_t b)
{
    if (get_bit(s->cr0, CR0_EM) || get_bit(s->cr0, CR0_TS)) {
        raise_exception(s, EXCP_NM);
    }
    uint8_t modrm = fetch8(s, d);
    uint32_t lin = 0, ea = 0;
    int seg = SEG_DS;
    if (modrm_mod(modrm) != 3) {
        Operand op = decode_modrm(s, d, modrm);
        /* only the first byte is checked against the segment */
        lin = op_address(s, op, 0, SIZE8, false);
        ea = op.ea;
        seg = op.seg;
    }
    fpu_exec(s, b, modrm, lin, ea, seg, d.opsize);
}

/* Two byte opcodes. Returns false if the instruction set EIP itself. */
static bool exec_0f(X86CPUState *s, Decoder &d)
{
    uint8_t b = fetch8(s, d);
    uint8_t modrm;
    Operand op;
    uint32_t val;

    switch (b) {
    case 0x00:
        exec_group6(s, d);
        break;
    case 0x01:
        exec_group7(s, d);
        break;
    case 0x02: /* LAR */
    case 0x03: /* LSL */ {
        require_protected_mode(s);
        op = fetch_modrm(s, d, &modrm);
        uint32_t sel = rm_read(s, op, SIZE16);
        bool ok = b == 0x02 ? seg_access_rights(s, sel, &val) :
            seg_limit(s, sel, &val);
        if (ok) {
            reg_write(s, modrm_reg(modrm), val, d.opsize);
        }
        set_zero_flag(s, ok);
        break;
    }
    case 0x06: /* CLTS */
        require_cpl0(s);
        s->cr0 = set_bit(s->cr0, CR0_TS, false);
        break;
    case 0x08: /* INVD */
    case 0x09: /* WBINVD */
        require_cpl0(s);
        break;
    case 0x18 ... 0x1f: /* hint NOPs */
        fetch_modrm(s, d, &modrm);
        break;
    case 0x20: /* MOV r32, CRn */
    case 0x22: /* MOV CRn, r32 */ {
        require_cpl0(s);
        modrm = fetch8(s, d);
        int rm = modrm_rm(modrm);
        if (b == 0x20) {
            switch (modrm_reg(modrm)) {
            case 0: val = s->cr0; break;
            case 2: val = s->cr2; break;
            case 3: val = s->cr3; break;
            case 4: val = s->cr4; break;
            default: raise_exception(s, EXCP_UD);
            }
            s->regs[rm] = val;
        } else {
            val = s->regs[rm];
            switch (modrm_reg(modrm)) {
            case 0: cpu_set_cr0(s, val); break;
            case 2: s->cr2 = val; break;
            case 3: cpu_set_cr3(s, val); break;
            case 4: cpu_set_cr4(s, val); break;
            default: raise_exception(s, EXCP_UD);
            }
        }
        break;
    }
    case 0x21: /* MOV r32, DRn */
    case 0x23: /* MOV DRn, r32 */ {
        require_cpl0(s);
        modrm = fetch8(s, d);
        int dr = modrm_reg(modrm);
        if (dr == 4 || dr == 5) {
            if (get_bit(s->cr4, CR4_DE)) {
                raise_exception(s, EXCP_UD);
            }
            dr += 2;
        }
        if (b == 0x21) {
            s->regs[modrm_rm(modrm)] = s->dr[dr];
        } else {
            s->dr[dr] = s->regs[modrm_rm(modrm)];
        }
        break;
    }
    case 0x30:
        cpu_wrmsr(s);
        break;
    case 0x31: { /* RDTSC */
        if (get_bit(s->cr4, CR4_TSD) && s->cpl != 0) {
            raise_exception(s, EXCP_GP, 0);
        }
        set_edx_eax(s, cpu_get_tsc(s));
        break;
    }
    case 0x32:
        cpu_rdmsr(s);
        break;
    case 0x34:
        cpu_sysenter(s);
        return false;
    case 0x35:
        cpu_sysexit(s);
        return false;
    case 0x40 ... 0x4f: /* CMOVcc */
        op = fetch_modrm(s, d, &modrm);
        val = rm_read(s, op, d.opsize);
        if (test_condition(s, get_bits(b, 0, 4))) {
            reg_write(s, modrm_reg(modrm), val, d.opsize);
        }
        break;
    case 0x80 ... 0x8f: /* Jcc */
        val = fetch_imm(s, d, d.opsize);
        if (test_condition(s, get_bits(b, 0, 4))) {
            jump_rel(d, val);
        }
        break;
    case 0x90 ... 0x9f: /* SETcc */
        op = fetch_modrm(s, d, &modrm);
        rm_write(s, op, test_condition(s, get_bits(b, 0, 4)), SIZE8);
        break;
    case 0xa0:
    case 0xa8:
        push(s, s->segs[b == 0xa0 ? SEG_FS : SEG_GS].sel, d.opsize);
        break;
    case 0xa1:
    case 0xa9:
        pop_seg(s, b == 0xa1 ? SEG_FS : SEG_GS, d.opsize);
        break;
    case 0xa2:
        cpu_cpuid(s);
        break;
    case 0xa3: /* BT */
    case 0xab: /* BTS */
    case 0xb3: /* BTR */
    case 0xbb: /* BTC */ {
        int bt = get_bits(b, 3, 2);
        op = fetch_modrm(s, d, &modrm);
        uint32_t offset = reg_read(s, modrm_reg(modrm), d.opsize);
        if (!op.is_reg) {
            /* the offset reaches outside the operand */
            int32_t disp = (sext_size(offset, d.opsize) >> (3 + d.opsize)) *
                size_bytes(d.opsize);
            op.ea = (op.ea + disp) & addr_mask(d);
        }
        val = bt == BT_TEST ? rm_read(s, op, d.opsize) :
            rm_read_modify(s, op, d.opsize);
        val = bit_test(s, bt, val, offset, d.opsize);
        if (bt != BT_TEST) {
            rm_write(s, op, val, d.opsize);
        }
        break;
    }
    case 0xba: { /* BT, BTS, BTR, BTC with an immediate */
        op = fetch_modrm(s, d, &modrm);
        if (modrm_reg(modrm) < 4) {
            raise_exception(s, EXCP_UD);
        }
        int bt = modrm_reg(modrm) - 4;
        uint32_t offset = fetch8(s, d);
        val = bt == BT_TEST ? rm_read(s, op, d.opsize) :
            rm_read_modify(s, op, d.opsize);
        val = bit_test(s, bt, val, offset, d.opsize);
        if (bt != BT_TEST) {
            rm_write(s, op, val, d.opsize);
        }
        break;
    }
    case 0xa4: /* SHLD Ib */
    case 0xa5: /* SHLD CL */
    case 0xac: /* SHRD Ib */
    case 0xad: /* SHRD CL */ {
        op = fetch_modrm(s, d, &modrm);
        unsigned count = get_bit(b, 0) ? reg_read(s, REG_ECX, SIZE8) :
            fetch8(s, d);
        val = rm_read_modify(s, op, d.opsize);
        val = shift_double(s, b <= 0xa5, val,
                           reg_read(s, modrm_reg(modrm), d.opsize), count,
                           d.opsize);
        rm_write(s, op, val, d.opsize);
        break;
    }
    case 0xaf: /* IMUL Gv, Ev */ {
        op = fetch_modrm(s, d, &modrm);
        int reg = modrm_reg(modrm);
        val = imul(s, reg_read(s, reg, d.opsize), rm_read(s, op, d.opsize),
                   d.opsize);
        reg_write(s, reg, val, d.opsize);
        break;
    }
    case 0xb0: /* CMPXCHG */
    case 0xb1: {
        int size = get_bit(b, 0) ? d.opsize : SIZE8;
        op = fetch_modrm(s, d, &modrm);
        uint32_t dst = rm_read_modify(s, op, size);
        uint32_t acc = reg_read(s, REG_EAX, size);
        alu(s, ALU_CMP, acc, dst, size);
        if (acc == dst) {
            rm_write(s, op, reg_read(s, modrm_reg(modrm), size), size);
        } else {
            if (!op.is_reg) {
                rm_write(s, op, dst, size);
            }
            reg_write(s, REG_EAX, dst, size);
        }
        break;
    }
    case 0xb2:
        load_far_pointer(s, d, SEG_SS);
        s->irq_inhibit = true;
        break;
    case 0xb4:
        load_far_pointer(s, d, SEG_FS);
        break;
    case 0xb5:
        load_far_pointer(s, d, SEG_GS);
        break;
    case 0xb6: /* MOVZX */
    case 0xb7:
    case 0xbe: /* MOVSX */
    case 0xbf: {
        int src_size = get_bit(b, 0) ? SIZE16 : SIZE8;
        op = fetch_modrm(s, d, &modrm);
        val = rm_read(s, op, src_size);
        if (b >= 0xbe) {
            val = sext_size(val, src_size);
        }
        reg_write(s, modrm_reg(modrm), val, d.opsize);
        break;
    }
    case 0xbc: /* BSF */
    case 0xbd: /* BSR */
        op = fetch_modrm(s, d, &modrm);
        val = rm_read(s, op, d.opsize);
        if (val != 0) {
            reg_write(s, modrm_reg(modrm),
                      b == 0xbc ? __builtin_ctz(val) : 31 - __builtin_clz(val),
                      d.opsize);
        }
        set_zero_flag(s, val == 0);
        break;
    case 0xc0: /* XADD */
    case 0xc1: {
        int size = get_bit(b, 0) ? d.opsize : SIZE8;
        op = fetch_modrm(s, d, &modrm);
        int reg = modrm_reg(modrm);
        uint32_t dst = rm_read_modify(s, op, size);
        uint32_t sum = alu(s, ALU_ADD, dst, reg_read(s, reg, size), size);
        if (op.is_reg) {
            reg_write(s, reg, dst, size);
            reg_write(s, op.reg, sum, size);
        } else {
            rm_write(s, op, sum, size);
            reg_write(s, reg, dst, size);
        }
        break;
    }
    case 0xc7: { /* CMPXCHG8B */
        op = fetch_modrm(s, d, &modrm);
        if (modrm_reg(modrm) != 1 || op.is_reg) {
            raise_exception(s, EXCP_UD);
        }
        uint32_t lin = op_address(s, op, 0, SIZE32, true);
        op_address(s, op, 4, SIZE32, true);
        uint32_t low = mem_read_modify(s, lin, SIZE32);
        uint32_t high = mem_read_modify(s, lin + 4, SIZE32);
        bool equal = low == s->regs[REG_EAX] && high == s->regs[REG_EDX];
        if (equal) {
            mem_write(s, lin, s->regs[REG_EBX], SIZE32);
            mem_write(s, lin + 4, s->regs[REG_ECX], SIZE32);
        } else {
            mem_write(s, lin, low, SIZE32);
            mem_write(s, lin + 4, high, SIZE32);
            s->regs[REG_EAX] = low;
            s->regs[REG_EDX] = high;
        }
        set_zero_flag(s, equal);
        break;
    }
    case 0xc8 ... 0xcf: /* BSWAP */ {
        int reg = get_bits(b, 0, 3);
        if (d.opsize == SIZE32) {
            s->regs[reg] = __builtin_bswap32(s->regs[reg]);
        } else {
            reg_write(s, reg, 0, SIZE16);
        }
        break;
    }
    default:
        raise_exception(s, EXCP_UD);
    }
    return true;
}


//#pragma mark - one byte opcodes

static force_inline void exec_insn(X86CPUState *s)
{
    Decoder d;
    d.cs_base = s->segs[SEG_CS].base;
    d.eip = s->eip;
    d.eip_mask = s->code32 ? UINT32_MAX : 0xffff;
    d.opsize = s->code32 ? SIZE32 : SIZE16;
    d.addr32 = s->code32;
    d.seg_override = -1;
    d.rep = REP_NONE;
    d.lock = false;
    d.esp_addend = 0;

    if (unlikely(!s->seg_fast[SEG_CS]) && s->eip > s->segs[SEG_CS].limit) {
        raise_exception(s, EXCP_GP, 0);
    }

    int prefixes = 0;
    uint8_t b;
    int size;
    uint8_t modrm;
    Operand op;
    uint32_t val;

next_byte:
    b = fetch8(s, d);
    /* the low bit selects between byte and full size for most opcodes */
    size = get_bit(b, 0) ? d.opsize : SIZE8;
    if (unlikely(d.lock) && !lock_allowed(s, d, b)) {
        raise_exception(s, EXCP_UD);
    }

    switch (b) {
    case 0x26:
    case 0x2e:
    case 0x36:
    case 0x3e:
    case 0x64 ... 0x67:
    case 0xf0:
    case 0xf2:
    case 0xf3:
        decode_prefix(s, d, b);
        if (++prefixes == 15) {
            raise_exception(s, EXCP_GP, 0);
        }
        goto next_byte;
    case 0x00 ... 0x05:
    case 0x08 ... 0x0d:
    case 0x10 ... 0x15:
    case 0x18 ... 0x1d:
    case 0x20 ... 0x25:
    case 0x28 ... 0x2d:
    case 0x30 ... 0x35:
    case 0x38 ... 0x3d:
        exec_alu(s, d, b);
        break;
    case 0x06: /* PUSH ES, CS, SS, DS */
    case 0x0e:
    case 0x16:
    case 0x1e:
        push(s, s->segs[get_bits(b, 3, 2)].sel, d.opsize);
        break;
    case 0x07: /* POP ES, SS, DS */
    case 0x17:
    case 0x1f:
        pop_seg(s, get_bits(b, 3, 2), d.opsize);
        break;
    case 0x0f:
        if (!exec_0f(s, d)) {
            return;
        }
        break;
    case 0x27: /* DAA */
    case 0x2f: /* DAS */
        decimal_adjust(s, b == 0x2f);
        break;
    case 0x37: /* AAA */
    case 0x3f: /* AAS */
        ascii_adjust(s, b == 0x3f);
        break;
    case 0x40 ... 0x47: /* INC */
    case 0x48 ... 0x4f: /* DEC */ {
        int reg = get_bits(b, 0, 3);
        val = inc_dec(s, reg_read(s, reg, d.opsize), get_bit(b, 3), d.opsize);
        reg_write(s, reg, val, d.opsize);
        break;
    }
    case 0x50 ... 0x57:
        push(s, reg_read(s, get_bits(b, 0, 3), d.opsize), d.opsize);
        break;
    case 0x58 ... 0x5f:
        val = pop(s, d.opsize);
        reg_write(s, get_bits(b, 0, 3), val, d.opsize);
        break;
    case 0x60: { /* PUSHA */
        StackPtr st = current_stack(s);
        uint32_t esp = s->regs[REG_ESP];
        for (int reg = REG_EAX; reg <= REG_EDI; reg++) {
            stack_push(s, &st, reg == REG_ESP ? esp : s->regs[reg], d.opsize);
        }
        stack_commit(s, st);
        break;
    }
    case 0x61: { /* POPA */
        StackPtr st = current_stack(s);
        uint32_t vals[8];
        for (int reg = REG_EDI; reg >= REG_EAX; reg--) {
            vals[reg] = stack_pop(s, &st, d.opsize);
        }
        for (int reg = REG_EAX; reg <= REG_EDI; reg++) {
            if (reg != REG_ESP) {
                reg_write(s, reg, vals[reg], d.opsize);
            }
        }
        stack_commit(s, st);
        break;
    }
    case 0x62: { /* BOUND */
        op = fetch_modrm(s, d, &modrm);
        if (op.is_reg) {
            raise_exception(s, EXCP_UD);
        }
        int n = size_bytes(d.opsize);
        int32_t index = sext_size(reg_read(s, modrm_reg(modrm), d.opsize),
                                  d.opsize);
        int32_t lower = sext_size(
            mem_read(s, op_address(s, op, 0, d.opsize, false), d.opsize),
            d.opsize);
        int32_t upper = sext_size(
            mem_read(s, op_address(s, op, n, d.opsize, false), d.opsize),
            d.opsize);
        if (index < lower || index > upper) {
            raise_exception(s, EXCP_BR);
        }
        break;
    }
    case 0x63: { /* ARPL */
        require_protected_mode(s);
        op = fetch_modrm(s, d, &modrm);
        /* written only when adjusted, so a read-only operand may not fault */
        uint32_t dst = rm_read(s, op, SIZE16);
        int rpl = sel_rpl(reg_read(s, modrm_reg(modrm), SIZE16));
        bool adjust = sel_rpl(dst) < rpl;
        if (adjust) {
            rm_write(s, op, set_bits(dst, 0, 2, rpl), SIZE16);
        }
        set_zero_flag(s, adjust);
        break;
    }
    case 0x68:
        push(s, fetch_imm(s, d, d.opsize), d.opsize);
        break;
    case 0x6a:
        push(s, fetch_simm(s, d, SIZE8), d.opsize);
        break;
    case 0x69: /* IMUL Gv, Ev, Iz */
    case 0x6b: /* IMUL Gv, Ev, Ib */ {
        op = fetch_modrm(s, d, &modrm);
        uint32_t imm = b == 0x69 ? fetch_imm(s, d, d.opsize) :
            fetch_simm(s, d, SIZE8);
        val = imul(s, rm_read(s, op, d.opsize), imm, d.opsize);
        reg_write(s, modrm_reg(modrm), val, d.opsize);
        break;
    }
    case 0x6c:
    case 0x6d:
        exec_string(s, d, STR_INS, size);
        break;
    case 0x6e:
    case 0x6f:
        exec_string(s, d, STR_OUTS, size);
        break;
    case 0x70 ... 0x7f: /* Jcc */
        val = fetch_simm(s, d, SIZE8);
        if (test_condition(s, get_bits(b, 0, 4))) {
            jump_rel(d, val);
        }
        break;
    case 0x80 ... 0x83:
        exec_group1(s, d, b);
        break;
    case 0x84: /* TEST */
    case 0x85:
        op = fetch_modrm(s, d, &modrm);
        val = rm_read(s, op, size) & reg_read(s, modrm_reg(modrm), size);
        set_cc(s, CC_OP_LOGIC, size, 0, val);
        break;
    case 0x86: /* XCHG */
    case 0x87: {
        op = fetch_modrm(s, d, &modrm);
        int reg = modrm_reg(modrm);
        val = rm_read_modify(s, op, size);
        rm_write(s, op, reg_read(s, reg, size), size);
        reg_write(s, reg, val, size);
        break;
    }
    case 0x88: /* MOV Ev, Gv */
    case 0x89:
        op = fetch_modrm(s, d, &modrm);
        rm_write(s, op, reg_read(s, modrm_reg(modrm), size), size);
        break;
    case 0x8a: /* MOV Gv, Ev */
    case 0x8b:
        op = fetch_modrm(s, d, &modrm);
        reg_write(s, modrm_reg(modrm), rm_read(s, op, size), size);
        break;
    case 0x8c: /* MOV Ew, Sw */
        op = fetch_modrm(s, d, &modrm);
        if (modrm_reg(modrm) >= SEG_COUNT) {
            raise_exception(s, EXCP_UD);
        }
        store_word(s, d, op, s->segs[modrm_reg(modrm)].sel);
        break;
    case 0x8d: /* LEA */
        op = fetch_modrm(s, d, &modrm);
        if (op.is_reg) {
            raise_exception(s, EXCP_UD);
        }
        reg_write(s, modrm_reg(modrm), op.ea, d.opsize);
        break;
    case 0x8e: { /* MOV Sw, Ew */
        op = fetch_modrm(s, d, &modrm);
        int seg = modrm_reg(modrm);
        if (seg == SEG_CS || seg >= SEG_COUNT) {
            raise_exception(s, EXCP_UD);
        }
        load_seg(s, seg, rm_read(s, op, SIZE16));
        if (seg == SEG_SS) {
            s->irq_inhibit = true;
        }
        break;
    }
    case 0x8f: { /* POP Ev */
        modrm = fetch8(s, d);
        if (modrm_reg(modrm) != 0) {
            raise_exception(s, EXCP_UD);
        }
        StackPtr st = current_stack(s);
        val = stack_pop(s, &st, d.opsize);
        d.esp_addend = size_bytes(d.opsize);
        op = decode_modrm(s, d, modrm);
        if (op.is_reg) {
            stack_commit(s, st);
            reg_write(s, op.reg, val, d.opsize);
        } else {
            rm_write(s, op, val, d.opsize);
            stack_commit(s, st);
        }
        break;
    }
    case 0x90: /* NOP, PAUSE */
        break;
    case 0x91 ... 0x97: { /* XCHG eAX */
        int reg = get_bits(b, 0, 3);
        val = reg_read(s, REG_EAX, d.opsize);
        reg_write(s, REG_EAX, reg_read(s, reg, d.opsize), d.opsize);
        reg_write(s, reg, val, d.opsize);
        break;
    }
    case 0x98: /* CBW, CWDE */
        val = sext_size(reg_read(s, REG_EAX, d.opsize - 1), d.opsize - 1);
        reg_write(s, REG_EAX, val, d.opsize);
        break;
    case 0x99: /* CWD, CDQ */
        val = msb(reg_read(s, REG_EAX, d.opsize), d.opsize) ? UINT32_MAX : 0;
        reg_write(s, REG_EDX, val, d.opsize);
        break;
    case 0x9a: { /* CALL Ap */
        uint32_t offset = fetch_imm(s, d, d.opsize);
        uint32_t sel = fetch_imm(s, d, SIZE16);
        far_call(s, sel, offset, d.opsize, next_eip(d));
        return;
    }
    case 0x9b: /* FWAIT */
        if (get_bit(s->cr0, CR0_MP) && get_bit(s->cr0, CR0_TS)) {
            raise_exception(s, EXCP_NM);
        }
        fpu_check_pending(s);
        break;
    case 0x9c: /* PUSHF */
        if (get_bit(s->eflags, EFLAGS_VM) && eflags_iopl(s->eflags) < 3) {
            raise_exception(s, EXCP_GP, 0);
        }
        val = get_eflags(s) & ~(bit_at(EFLAGS_VM) | bit_at(EFLAGS_RF));
        push(s, val, d.opsize);
        break;
    case 0x9d: { /* POPF */
        if (get_bit(s->eflags, EFLAGS_VM) && eflags_iopl(s->eflags) < 3) {
            raise_exception(s, EXCP_GP, 0);
        }
        uint32_t mask = EFLAGS_CC_MASK | bit_at(EFLAGS_TF) |
            bit_at(EFLAGS_DF) | bit_at(EFLAGS_NT) | bit_at(EFLAGS_AC) |
            bit_at(EFLAGS_ID);
        if (s->cpl == 0) {
            mask |= field_mask(EFLAGS_IOPL, 2);
        }
        if (s->cpl <= eflags_iopl(s->eflags)) {
            mask = set_bit(mask, EFLAGS_IF, true);
        }
        val = pop(s, d.opsize);
        cpu_set_eflags(s, val, trunc_size(mask, d.opsize));
        break;
    }
    case 0x9e: /* SAHF */ {
        uint32_t mask = EFLAGS_CC_MASK & bit_mask(8);
        val = reg_read(s, 4 /* AH */, SIZE8);
        set_cc_eflags(s, (cc_eflags(s) & ~mask) | (val & mask));
        break;
    }
    case 0x9f: /* LAHF */
        val = (cc_eflags(s) & bit_mask(8)) | bit_at(1);
        reg_write(s, 4 /* AH */, val, SIZE8);
        break;
    case 0xa0 ... 0xa3: { /* MOV with a direct address */
        uint32_t addr = fetch_imm(s, d, d.addr32 ? SIZE32 : SIZE16);
        bool store = get_bit(b, 1);
        uint32_t lin = seg_address(s, data_seg(d), addr, size, store);
        if (store) {
            mem_write(s, lin, reg_read(s, REG_EAX, size), size);
        } else {
            reg_write(s, REG_EAX, mem_read(s, lin, size), size);
        }
        break;
    }
    case 0xa4:
    case 0xa5:
        exec_string(s, d, STR_MOVS, size);
        break;
    case 0xa6:
    case 0xa7:
        exec_string(s, d, STR_CMPS, size);
        break;
    case 0xa8: /* TEST eAX, Iz */
    case 0xa9:
        val = reg_read(s, REG_EAX, size) & fetch_imm(s, d, size);
        set_cc(s, CC_OP_LOGIC, size, 0, val);
        break;
    case 0xaa:
    case 0xab:
        exec_string(s, d, STR_STOS, size);
        break;
    case 0xac:
    case 0xad:
        exec_string(s, d, STR_LODS, size);
        break;
    case 0xae:
    case 0xaf:
        exec_string(s, d, STR_SCAS, size);
        break;
    case 0xb0 ... 0xb7:
        reg_write(s, get_bits(b, 0, 3), fetch_imm(s, d, SIZE8), SIZE8);
        break;
    case 0xb8 ... 0xbf:
        reg_write(s, get_bits(b, 0, 3), fetch_imm(s, d, d.opsize), d.opsize);
        break;
    case 0xc0:
    case 0xc1:
    case 0xd0 ... 0xd3:
        exec_shift_group(s, d, b);
        break;
    case 0xc2: /* RET Iw */
    case 0xc3: { /* RET */
        uint32_t addend = b == 0xc2 ? fetch_imm(s, d, SIZE16) : 0;
        StackPtr st = current_stack(s);
        val = stack_pop(s, &st, d.opsize);
        st.sp += addend;
        stack_commit(s, st);
        d.eip = val;
        break;
    }
    case 0xc4:
        load_far_pointer(s, d, SEG_ES);
        break;
    case 0xc5:
        load_far_pointer(s, d, SEG_DS);
        break;
    case 0xc6: /* MOV Ev, Iz */
    case 0xc7:
        op = fetch_modrm(s, d, &modrm);
        if (modrm_reg(modrm) != 0) {
            raise_exception(s, EXCP_UD);
        }
        rm_write(s, op, fetch_imm(s, d, size), size);
        break;
    case 0xc8: { /* ENTER */
        uint32_t alloc = fetch_imm(s, d, SIZE16);
        int level = get_bits(fetch8(s, d), 0, 5);
        StackPtr st = current_stack(s);
        stack_push(s, &st, reg_read(s, REG_EBP, d.opsize), d.opsize);
        /* ESP as it stands after the push, high word kept on a 16 bit stack */
        uint32_t frame = (s->regs[REG_ESP] & ~st.mask) | (st.sp & st.mask);
        if (level > 0) {
            StackPtr frames = current_stack(s);
            frames.sp = s->regs[REG_EBP];
            for (int i = 1; i < level; i++) {
                frames.sp -= size_bytes(d.opsize);
                val = mem_read(s, stack_address(s, &frames, d.opsize, false),
                               d.opsize);
                stack_push(s, &st, val, d.opsize);
            }
            stack_push(s, &st, frame, d.opsize);
        }
        st.sp -= alloc;
        /* the final stack pointer must be writable before anything changes */
        mem_probe_write(s, stack_address(s, &st, d.opsize, true), d.opsize);
        /* the stack size, not the operand size, says how much of EBP */
        reg_write(s, REG_EBP, frame, st.mask == 0xffff ? SIZE16 : SIZE32);
        stack_commit(s, st);
        break;
    }
    case 0xc9: { /* LEAVE */
        StackPtr st = current_stack(s);
        st.sp = s->regs[REG_EBP];
        val = stack_pop(s, &st, d.opsize);
        stack_commit(s, st);
        reg_write(s, REG_EBP, val, d.opsize);
        break;
    }
    case 0xca: /* RETF Iw */
    case 0xcb: /* RETF */
        far_return(s, d.opsize, b == 0xca ? fetch_imm(s, d, SIZE16) : 0);
        return;
    case 0xcc: /* INT3 */
        do_interrupt(s, EXCP_BP, true, 0, next_eip(d), false);
        return;
    case 0xcd: { /* INT */
        int intno = fetch8(s, d);
        do_interrupt(s, intno, true, 0, next_eip(d), false);
        return;
    }
    case 0xce: /* INTO */
        if (cc_overflow(s)) {
            do_interrupt(s, EXCP_OF, true, 0, next_eip(d), false);
            return;
        }
        break;
    case 0xcf: /* IRET */
        interrupt_return(s, d.opsize, next_eip(d));
        return;
    case 0xd4: { /* AAM */
        uint32_t base = fetch8(s, d);
        if (base == 0) {
            raise_exception(s, EXCP_DE);
        }
        uint32_t al = reg_read(s, REG_EAX, SIZE8);
        reg_write(s, REG_EAX, set_bits(al % base, 8, 8, al / base), SIZE16);
        set_cc(s, CC_OP_LOGIC, SIZE8, 0, al % base);
        break;
    }
    case 0xd5: { /* AAD */
        uint32_t base = fetch8(s, d);
        uint32_t ax = reg_read(s, REG_EAX, SIZE16);
        val = trunc_size(get_bits(ax, 0, 8) + get_bits(ax, 8, 8) * base,
                         SIZE8);
        reg_write(s, REG_EAX, val, SIZE16);
        set_cc(s, CC_OP_LOGIC, SIZE8, 0, val);
        break;
    }
    case 0xd6: /* SALC */
        reg_write(s, REG_EAX, cc_carry(s) ? 0xff : 0, SIZE8);
        break;
    case 0xd7: { /* XLAT */
        uint32_t addr = (s->regs[REG_EBX] + reg_read(s, REG_EAX, SIZE8)) &
            addr_mask(d);
        val = mem_read(s, seg_address(s, data_seg(d), addr, SIZE8, false),
                       SIZE8);
        reg_write(s, REG_EAX, val, SIZE8);
        break;
    }
    case 0xd8 ... 0xdf:
        exec_fpu(s, d, b);
        break;
    case 0xe0: /* LOOPNZ */
    case 0xe1: /* LOOPZ */
    case 0xe2: { /* LOOP */
        val = fetch_simm(s, d, SIZE8);
        uint32_t amask = addr_mask(d);
        reg_add_masked(s, REG_ECX, -1, amask);
        bool taken = (s->regs[REG_ECX] & amask) != 0;
        if (b != 0xe2) {
            taken = taken && cc_zero(s) == (b == 0xe1);
        }
        if (taken) {
            jump_rel(d, val);
        }
        break;
    }
    case 0xe3: /* JCXZ */
        val = fetch_simm(s, d, SIZE8);
        if ((s->regs[REG_ECX] & addr_mask(d)) == 0) {
            jump_rel(d, val);
        }
        break;
    case 0xe4 ... 0xe7: /* IN, OUT with an immediate port */
    case 0xec ... 0xef: { /* IN, OUT with DX */
        uint32_t port = b <= 0xe7 ? fetch8(s, d) :
            reg_read(s, REG_EDX, SIZE16);
        check_io_permission(s, port, size);
        if (get_bit(b, 1)) {
            port_out(s, port, reg_read(s, REG_EAX, size), size);
        } else {
            reg_write(s, REG_EAX, port_in(s, port, size), size);
        }
        break;
    }
    case 0xe8: { /* CALL Jz */
        uint32_t rel = fetch_imm(s, d, d.opsize);
        push(s, d.eip, d.opsize);
        jump_rel(d, rel);
        break;
    }
    case 0xe9: /* JMP Jz */
        val = fetch_imm(s, d, d.opsize);
        jump_rel(d, val);
        break;
    case 0xea: { /* JMP Ap */
        uint32_t offset = fetch_imm(s, d, d.opsize);
        uint32_t sel = fetch_imm(s, d, SIZE16);
        far_jump(s, sel, offset, next_eip(d));
        return;
    }
    case 0xeb: /* JMP Jb */
        val = fetch_simm(s, d, SIZE8);
        jump_rel(d, val);
        break;
    case 0xf1: /* INT1 */
        do_interrupt(s, EXCP_DB, false, 0, next_eip(d), false);
        return;
    case 0xf4: /* HLT */
        require_cpl0(s);
        s->power_down = true;
        break;
    case 0xf5: /* CMC */
        set_carry_flag(s, !cc_carry(s));
        break;
    case 0xf6:
    case 0xf7:
        exec_group3(s, d, b);
        break;
    case 0xf8: /* CLC */
    case 0xf9: /* STC */
        set_carry_flag(s, b == 0xf9);
        break;
    case 0xfa: /* CLI */
    case 0xfb: /* STI */
        if (get_bit(s->cr0, CR0_PE) && s->cpl > eflags_iopl(s->eflags)) {
            raise_exception(s, EXCP_GP, 0);
        }
        if (b == 0xfb && !get_bit(s->eflags, EFLAGS_IF)) {
            s->irq_inhibit = true;
        }
        s->eflags = set_bit(s->eflags, EFLAGS_IF, b == 0xfb);
        break;
    case 0xfc: /* CLD */
    case 0xfd: /* STD */
        s->eflags = set_bit(s->eflags, EFLAGS_DF, b == 0xfd);
        break;
    case 0xfe:
    case 0xff: {
        op = fetch_modrm(s, d, &modrm);
        int reg = modrm_reg(modrm);
        if (b == 0xfe && reg > 1) {
            raise_exception(s, EXCP_UD);
        }
        switch (reg) {
        case 0: /* INC */
        case 1: /* DEC */
            val = rm_read_modify(s, op, size);
            rm_write(s, op, inc_dec(s, val, reg == 1, size), size);
            break;
        case 2: /* CALL Ev */
            val = rm_read(s, op, d.opsize);
            push(s, d.eip, d.opsize);
            d.eip = val;
            break;
        case 3: /* CALL Mp */
        case 5: /* JMP Mp */ {
            if (op.is_reg) {
                raise_exception(s, EXCP_UD);
            }
            uint32_t offset = mem_read(
                s, op_address(s, op, 0, d.opsize, false), d.opsize);
            uint32_t sel = mem_read(
                s, op_address(s, op, size_bytes(d.opsize), SIZE16, false),
                SIZE16);
            if (reg == 3) {
                far_call(s, sel, offset, d.opsize, next_eip(d));
            } else {
                far_jump(s, sel, offset, next_eip(d));
            }
            return;
        }
        case 4: /* JMP Ev */
            d.eip = rm_read(s, op, d.opsize);
            break;
        case 6: /* PUSH Ev */
            push(s, rm_read(s, op, d.opsize), d.opsize);
            break;
        default:
            raise_exception(s, EXCP_UD);
        }
        break;
    }
    default:
        raise_exception(s, EXCP_UD);
    }
    s->eip = d.eip & d.eip_mask;
}

void x86_exec(X86CPUState *s)
{
    while (s->cycles < s->cycles_end && !s->power_down) {
        if (unlikely(s->irq_inhibit)) {
            s->irq_inhibit = false;
        } else if (unlikely(s->irq_level.load(std::memory_order_relaxed)) &&
                   get_bit(s->eflags, EFLAGS_IF)) {
            int intno;
            {
                DeviceLocker locker(*s->device_lock);
                intno = s->hard_intno_source->HardIntno();
            }
            do_interrupt(s, intno, false, 0, s->eip, true);
        }
        bool single_step = get_bit(s->eflags, EFLAGS_TF);
        exec_insn(s);
        s->cycles++;
        if (unlikely(single_step) && get_bit(s->eflags, EFLAGS_TF)) {
            s->dr[6] = set_bit(s->dr[6], 14, true);
            raise_exception(s, EXCP_DB);
        }
    }
}
