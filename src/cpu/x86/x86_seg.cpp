/*
 * x86 CPU emulator: segmentation, interrupts and tasks
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

namespace {

/* The two halves of an 8 byte descriptor or gate. */
struct Descriptor {
    uint32_t e1;
    uint32_t e2;
};

enum {
    TASK_JMP,
    TASK_CALL,
    TASK_IRET,
};

enum {
    TSS_BACK_LINK = 0x00,
    TSS_CR3 = 0x1c,
    TSS_IOMAP = 0x66,
    TSS32_MIN_LIMIT = 0x67,
};

/* Where a TSS of either size keeps the state a task switch exchanges. The
   general registers and the segment selectors are stored in their usual
   order, each field as wide as the TSS. */
struct TssLayout {
    int size;
    uint32_t eip;
    uint32_t eflags;
    uint32_t regs;
    uint32_t segs;
    int seg_count;   /* ES, CS, SS and DS, then FS and GS on a 32 bit TSS */
    uint32_t ldt;
    uint32_t min_limit;
};

const TssLayout kTss16Layout = {SIZE16, 0x0e, 0x10, 0x12, 0x22, 4, 0x2a, 0x2b};
const TssLayout kTss32Layout = {SIZE32, 0x20, 0x24, 0x28, 0x48, 6, 0x60, 0x67};

}


//#pragma mark - descriptors

static uint32_t descriptor_flags(const Descriptor &d)
{
    return set_bits(get_bits(d.e2, 8, 16), 8, 4, 0);
}

static uint32_t descriptor_base(const Descriptor &d)
{
    uint32_t base = get_bits(d.e1, 16, 16);
    base = set_bits(base, 16, 8, get_bits(d.e2, 0, 8));
    return set_bits(base, 24, 8, get_bits(d.e2, 24, 8));
}

static uint32_t descriptor_limit(const Descriptor &d)
{
    uint32_t limit = set_bits(get_bits(d.e1, 0, 16), 16, 4,
                              get_bits(d.e2, 16, 4));
    if (get_bit(descriptor_flags(d), DESC_G)) {
        limit = limit * PAGE_SIZE + (PAGE_SIZE - 1);
    }
    return limit;
}

static uint32_t gate_selector(const Descriptor &d)
{
    return get_bits(d.e1, 16, 16);
}

static uint32_t gate_offset(const Descriptor &d)
{
    return set_bits(get_bits(d.e1, 0, 16), 16, 16, get_bits(d.e2, 16, 16));
}

static int gate_param_count(const Descriptor &d)
{
    return get_bits(d.e2, 0, 5);
}

static uint32_t sel_error(uint32_t sel)
{
    return set_bits(get_bits(sel, 0, 16), 0, 2, 0);
}

static bool is_protected(X86CPUState *s)
{
    return get_bit(s->cr0, CR0_PE) && !get_bit(s->eflags, EFLAGS_VM);
}

static uint32_t flat_flags(bool code, int dpl)
{
    return bit_at(DESC_P) | bit_at(DESC_S) | bit_at(DESC_RW) | bit_at(DESC_A) |
        set_bit(0, DESC_CODE, code) | set_bits(0, DESC_DPL, 2, dpl) |
        bit_at(DESC_DB) | bit_at(DESC_G);
}

static bool descriptor_address(X86CPUState *s, uint32_t sel, uint32_t *addr)
{
    const X86CPUSeg *table = get_bit(sel, 2) ? &s->ldt : &s->gdt;
    uint32_t index = sel_error(set_bit(sel, 2, false));
    if (index + 7 > table->limit) {
        return false;
    }
    *addr = table->base + index;
    return true;
}

static bool load_descriptor(X86CPUState *s, uint32_t sel, Descriptor *d)
{
    uint32_t addr;
    if (!descriptor_address(s, sel, &addr)) {
        return false;
    }
    d->e1 = sys_read(s, addr, SIZE32);
    d->e2 = sys_read(s, addr + 4, SIZE32);
    return true;
}

/* Set or clear one bit of the attribute word of a table entry. */
static void set_descriptor_flag(X86CPUState *s, uint32_t sel, int flag,
                                bool on)
{
    uint32_t addr;
    if (descriptor_address(s, sel, &addr)) {
        uint32_t e2 = sys_read(s, addr + 4, SIZE32);
        sys_write(s, addr + 4, set_bit(e2, 8 + flag, on), SIZE32);
    }
}


//#pragma mark - segment loading

void load_seg_cache(X86CPUState *s, int seg, uint32_t sel, uint32_t base,
                    uint32_t limit, uint32_t flags)
{
    X86CPUSeg *sc = &s->segs[seg];
    sc->sel = sel;
    sc->base = base;
    sc->limit = limit;
    sc->flags = flags;
    s->seg_fast[seg] = seg_fast_access(sc);
    if (seg == SEG_CS || seg == SEG_SS) {
        cpu_update_mode(s);
    }
}

/* In real mode a load changes only the selector and the base, which is what
   lets a limit set in protected mode survive; virtual 8086 mode loads the
   whole of a real mode segment. */
static void load_seg_real(X86CPUState *s, int seg, uint32_t sel)
{
    sel = get_bits(sel, 0, 16);
    if (!get_bit(s->eflags, EFLAGS_VM)) {
        const X86CPUSeg *sc = &s->segs[seg];
        load_seg_cache(s, seg, sel, sel * 16, sc->limit, sc->flags);
        return;
    }
    uint32_t flags = bit_at(DESC_P) | bit_at(DESC_S) | bit_at(DESC_RW) |
        bit_at(DESC_A) | field_mask(DESC_DPL, 2);
    if (seg == SEG_CS) {
        flags = set_bit(flags, DESC_CODE, true);
    }
    load_seg_cache(s, seg, sel, sel * 16, 0xffff, flags);
}

uint32_t seg_address_slow(X86CPUState *s, const X86CPUSeg *seg, int excp,
                          uint32_t ea, int size, bool write)
{
    uint32_t flags = seg->flags;
    bool code = get_bit(flags, DESC_CODE);

    if (is_protected(s)) {
        if (!get_bit(flags, DESC_S) || !get_bit(flags, DESC_P)) {
            raise_exception(s, excp, 0);
        }
        if (write ? code || !get_bit(flags, DESC_RW) :
            code && !get_bit(flags, DESC_RW)) {
            raise_exception(s, EXCP_GP, 0);
        }
    }
    uint32_t last = ea + size_bytes(size) - 1;
    if (last < ea) {
        raise_exception(s, excp, 0);
    }
    if (!code && get_bit(flags, DESC_CE)) {
        /* expand down: the valid offsets are above the limit */
        uint32_t top = get_bit(flags, DESC_DB) ? UINT32_MAX : 0xffff;
        if (ea <= seg->limit || last > top) {
            raise_exception(s, excp, 0);
        }
    } else if (last > seg->limit) {
        raise_exception(s, excp, 0);
    }
    return seg->base + ea;
}

static void load_null_seg(X86CPUState *s, int seg, uint32_t sel)
{
    load_seg_cache(s, seg, get_bits(sel, 0, 16), 0, 0, 0);
}

/* MOV, POP and the far pointer loads of a data segment or SS. */
void load_seg(X86CPUState *s, int seg, uint32_t sel)
{
    sel = get_bits(sel, 0, 16);
    if (!is_protected(s)) {
        load_seg_real(s, seg, sel);
        return;
    }

    int cpl = s->cpl;
    int rpl = sel_rpl(sel);
    if (sel_is_null(sel)) {
        if (seg == SEG_SS) {
            raise_exception(s, EXCP_GP, 0);
        }
        load_null_seg(s, seg, sel);
        return;
    }

    Descriptor d;
    if (!load_descriptor(s, sel, &d)) {
        raise_exception(s, EXCP_GP, sel_error(sel));
    }
    uint32_t flags = descriptor_flags(d);
    int dpl = desc_dpl(flags);
    if (seg == SEG_SS) {
        if (!desc_is_data(flags) || !get_bit(flags, DESC_RW) ||
            rpl != cpl || dpl != cpl) {
            raise_exception(s, EXCP_GP, sel_error(sel));
        }
        if (!get_bit(flags, DESC_P)) {
            raise_exception(s, EXCP_SS, sel_error(sel));
        }
    } else {
        if (!get_bit(flags, DESC_S) ||
            (get_bit(flags, DESC_CODE) && !get_bit(flags, DESC_RW))) {
            raise_exception(s, EXCP_GP, sel_error(sel));
        }
        bool conforming = desc_is_code(flags) && get_bit(flags, DESC_CE);
        if (!conforming && (dpl < cpl || dpl < rpl)) {
            raise_exception(s, EXCP_GP, sel_error(sel));
        }
        if (!get_bit(flags, DESC_P)) {
            raise_exception(s, EXCP_NP, sel_error(sel));
        }
    }
    if (!get_bit(flags, DESC_A)) {
        set_descriptor_flag(s, sel, DESC_A, true);
        flags = set_bit(flags, DESC_A, true);
    }
    load_seg_cache(s, seg, sel, descriptor_base(d), descriptor_limit(d),
                   flags);
}

static void load_cs(X86CPUState *s, uint32_t sel, const Descriptor &d, int cpl)
{
    s->cpl = cpl;
    load_seg_cache(s, SEG_CS, set_bits(sel, 0, 2, cpl), descriptor_base(d),
                   descriptor_limit(d), descriptor_flags(d));
}

/* Load the code segment descriptor a far transfer names, checking only that
   it is a present code segment. */
static void load_code_descriptor(X86CPUState *s, uint32_t sel, Descriptor *d)
{
    if (sel_is_null(sel)) {
        raise_exception(s, EXCP_GP, 0);
    }
    if (!load_descriptor(s, sel, d) || !desc_is_code(descriptor_flags(*d))) {
        raise_exception(s, EXCP_GP, sel_error(sel));
    }
}

static void check_present(X86CPUState *s, uint32_t sel, uint32_t flags)
{
    if (!get_bit(flags, DESC_P)) {
        raise_exception(s, EXCP_NP, sel_error(sel));
    }
}

/* Privilege rules of a direct jump or call to a code segment. */
static void check_direct_code(X86CPUState *s, uint32_t sel, uint32_t flags,
                              int rpl)
{
    int dpl = desc_dpl(flags);
    if (get_bit(flags, DESC_CE) ? dpl > s->cpl :
        rpl > s->cpl || dpl != s->cpl) {
        raise_exception(s, EXCP_GP, sel_error(sel));
    }
    check_present(s, sel, flags);
}

static X86CPUSeg seg_cache(uint32_t sel, const Descriptor &d)
{
    return {(uint16_t)sel, (uint16_t)descriptor_flags(d), descriptor_base(d),
            descriptor_limit(d)};
}

/* The stack of privilege level 'dpl' as the current TSS gives it. 'ss'
   receives the segment the stack pointer refers to. */
static void load_inner_stack(X86CPUState *s, int dpl, X86CPUSeg *ss,
                             StackPtr *st)
{
    int size = get_bit(desc_type(s->tr.flags), 3) ? SIZE32 : SIZE16;
    uint32_t pos = size_bytes(size) * (2 * dpl + 1);
    if (pos + 2 * size_bytes(size) - 1 > s->tr.limit) {
        raise_exception(s, EXCP_TS, sel_error(s->tr.sel));
    }
    uint32_t esp = sys_read(s, s->tr.base + pos, size);
    uint32_t sel = sys_read(s, s->tr.base + pos + size_bytes(size), SIZE16);

    Descriptor d;
    if (sel_is_null(sel) || !load_descriptor(s, sel, &d)) {
        raise_exception(s, EXCP_TS, sel_error(sel));
    }
    uint32_t flags = descriptor_flags(d);
    if (sel_rpl(sel) != dpl || desc_dpl(flags) != dpl ||
        !desc_is_data(flags) || !get_bit(flags, DESC_RW)) {
        raise_exception(s, EXCP_TS, sel_error(sel));
    }
    if (!get_bit(flags, DESC_P)) {
        raise_exception(s, EXCP_SS, sel_error(sel));
    }
    *ss = seg_cache(sel, d);
    *st = make_stack(ss, esp, dpl);
}

static void load_ss(X86CPUState *s, const X86CPUSeg &ss)
{
    load_seg_cache(s, SEG_SS, ss.sel, ss.base, ss.limit, ss.flags);
}


//#pragma mark - tasks

static void set_tss_busy(X86CPUState *s, uint32_t sel, bool busy)
{
    set_descriptor_flag(s, sel, DESC_RW, busy);
}

static void task_switch(X86CPUState *s, uint32_t sel, int source,
                        uint32_t next_eip)
{
    int error = source == TASK_IRET ? EXCP_TS : EXCP_GP;
    Descriptor d;

    sel = get_bits(sel, 0, 16);
    if (get_bit(sel, 2) || !load_descriptor(s, sel, &d)) {
        raise_exception(s, error, sel_error(sel));
    }
    uint32_t flags = descriptor_flags(d);
    int type = desc_type(flags);
    bool busy = get_bit(type, 1);
    if (get_bit(flags, DESC_S) ||
        (set_bit(type, 1, false) != SYS_TSS16 &&
         set_bit(type, 1, false) != SYS_TSS32) ||
        busy != (source == TASK_IRET)) {
        raise_exception(s, error, sel_error(sel));
    }
    check_present(s, sel, flags);
    const TssLayout &nl = get_bit(type, 3) ? kTss32Layout : kTss16Layout;
    const TssLayout &ol = get_bit(desc_type(s->tr.flags), 3) ? kTss32Layout :
        kTss16Layout;
    int nstep = size_bytes(nl.size);
    int ostep = size_bytes(ol.size);
    uint32_t base = descriptor_base(d);
    uint32_t limit = descriptor_limit(d);
    if (limit < nl.min_limit) {
        raise_exception(s, EXCP_TS, sel_error(sel));
    }

    /* read the new state before changing anything */
    uint32_t new_cr3 = 0;
    if (nl.size == SIZE32) {
        new_cr3 = sys_read(s, base + TSS_CR3, SIZE32);
    }
    uint32_t new_eip = sys_read(s, base + nl.eip, nl.size);
    uint32_t new_eflags = sys_read(s, base + nl.eflags, nl.size);
    uint32_t new_regs[8];
    uint32_t new_segs[SEG_COUNT] = {};
    for (int i = 0; i < 8; i++) {
        new_regs[i] = sys_read(s, base + nl.regs + nstep * i, nl.size);
        if (nl.size == SIZE16) {
            /* the high halves are left all ones, as a 386 does */
            new_regs[i] = set_bits(UINT32_MAX, 0, 16, new_regs[i]);
        }
    }
    for (int i = 0; i < nl.seg_count; i++) {
        new_segs[i] = sys_read(s, base + nl.segs + nstep * i, SIZE16);
    }
    uint32_t new_ldt = sys_read(s, base + nl.ldt, SIZE16);

    /* save the old state; only IRET changes the saved NT */
    uint32_t old_base = s->tr.base;
    uint32_t old_eflags = get_eflags(s);
    if (source == TASK_IRET) {
        old_eflags = set_bit(old_eflags, EFLAGS_NT, false);
    }
    sys_write(s, old_base + ol.eip, next_eip, ol.size);
    sys_write(s, old_base + ol.eflags, old_eflags, ol.size);
    for (int i = 0; i < 8; i++) {
        sys_write(s, old_base + ol.regs + ostep * i, s->regs[i], ol.size);
    }
    for (int i = 0; i < ol.seg_count; i++) {
        sys_write(s, old_base + ol.segs + ostep * i, s->segs[i].sel, SIZE16);
    }
    if (source != TASK_CALL) {
        set_tss_busy(s, s->tr.sel, false);
    } else {
        sys_write(s, base + TSS_BACK_LINK, s->tr.sel, SIZE16);
        new_eflags = set_bit(new_eflags, EFLAGS_NT, true);
    }
    if (source != TASK_IRET) {
        set_tss_busy(s, sel, true);
    }

    /* switch */
    s->tr.sel = sel;
    s->tr.base = base;
    s->tr.limit = limit;
    s->tr.flags = set_bit(flags, DESC_RW, true);
    s->cr0 = set_bit(s->cr0, CR0_TS, true);
    if (nl.size == SIZE32) {
        cpu_set_cr3(s, new_cr3);
    }
    memcpy(s->regs, new_regs, sizeof(s->regs));
    s->eip = new_eip;
    cpu_set_eflags(s, new_eflags, UINT32_MAX);

    if (sel_is_null(new_ldt)) {
        s->ldt = {(uint16_t)new_ldt, 0, 0, 0};
    } else {
        Descriptor ld;
        if (get_bit(new_ldt, 2) || !load_descriptor(s, new_ldt, &ld)) {
            raise_exception(s, EXCP_TS, sel_error(new_ldt));
        }
        uint32_t lflags = descriptor_flags(ld);
        if (get_bit(lflags, DESC_S) || desc_type(lflags) != SYS_LDT ||
            !get_bit(lflags, DESC_P)) {
            raise_exception(s, EXCP_TS, sel_error(new_ldt));
        }
        s->ldt = {(uint16_t)new_ldt, (uint16_t)lflags, descriptor_base(ld),
                  descriptor_limit(ld)};
    }

    if (get_bit(new_eflags, EFLAGS_VM)) {
        s->cpl = 3;
        for (int seg = 0; seg < SEG_COUNT; seg++) {
            load_seg_real(s, seg, new_segs[seg]);
        }
        return;
    }

    uint32_t cs_sel = new_segs[SEG_CS];
    int rpl = sel_rpl(cs_sel);
    Descriptor cd;
    if (sel_is_null(cs_sel) || !load_descriptor(s, cs_sel, &cd)) {
        raise_exception(s, EXCP_TS, sel_error(cs_sel));
    }
    uint32_t cflags = descriptor_flags(cd);
    if (!desc_is_code(cflags) ||
        (get_bit(cflags, DESC_CE) ? desc_dpl(cflags) > rpl :
         desc_dpl(cflags) != rpl)) {
        raise_exception(s, EXCP_TS, sel_error(cs_sel));
    }
    check_present(s, cs_sel, cflags);
    load_cs(s, cs_sel, cd, rpl);
    for (int seg = 0; seg < SEG_COUNT; seg++) {
        if (seg != SEG_CS) {
            load_seg(s, seg, new_segs[seg]);
        }
    }
}


//#pragma mark - interrupts

static bool exception_has_error_code(int intno)
{
    switch (intno) {
    case EXCP_DF:
    case EXCP_TS:
    case EXCP_NP:
    case EXCP_SS:
    case EXCP_GP:
    case EXCP_PF:
    case EXCP_AC:
        return true;
    default:
        return false;
    }
}

static void do_interrupt_real(X86CPUState *s, int intno, uint32_t ret_eip)
{
    uint32_t entry = s->idt.base + intno * 4;
    uint32_t offset = sys_read(s, entry, SIZE16);
    uint32_t sel = sys_read(s, entry + 2, SIZE16);

    StackPtr st = current_stack(s);
    stack_push(s, &st, get_eflags(s), SIZE16);
    stack_push(s, &st, s->segs[SEG_CS].sel, SIZE16);
    stack_push(s, &st, ret_eip, SIZE16);
    stack_commit(s, st);

    s->eflags &= ~(bit_at(EFLAGS_IF) | bit_at(EFLAGS_TF) | bit_at(EFLAGS_AC) |
                   bit_at(EFLAGS_RF));
    load_seg_real(s, SEG_CS, sel);
    s->eip = offset;
}

static void do_interrupt_protected(X86CPUState *s, int intno, bool is_soft,
                                   bool has_error, int error_code,
                                   uint32_t ret_eip)
{
    uint32_t vector_error = intno * 8 + 2;
    if ((uint32_t)intno * 8 + 7 > s->idt.limit) {
        raise_exception(s, EXCP_GP, vector_error);
    }
    Descriptor gate;
    gate.e1 = sys_read(s, s->idt.base + intno * 8, SIZE32);
    gate.e2 = sys_read(s, s->idt.base + intno * 8 + 4, SIZE32);
    uint32_t gflags = descriptor_flags(gate);
    int type = desc_type(gflags);

    switch (type) {
    case SYS_TASK_GATE:
    case SYS_INT_GATE16:
    case SYS_TRAP_GATE16:
    case SYS_INT_GATE32:
    case SYS_TRAP_GATE32:
        if (!get_bit(gflags, DESC_S)) {
            break;
        }
        [[fallthrough]];
    default:
        raise_exception(s, EXCP_GP, vector_error);
    }
    if (is_soft && desc_dpl(gflags) < s->cpl) {
        raise_exception(s, EXCP_GP, vector_error);
    }
    if (!get_bit(gflags, DESC_P)) {
        raise_exception(s, EXCP_NP, vector_error);
    }

    if (type == SYS_TASK_GATE) {
        task_switch(s, gate_selector(gate), TASK_CALL, ret_eip);
        if (has_error) {
            StackPtr st = current_stack(s);
            stack_push(s, &st, error_code, SIZE32);
            stack_commit(s, st);
        }
        return;
    }

    int size = get_bit(type, 3) ? SIZE32 : SIZE16;
    uint32_t sel = gate_selector(gate);
    uint32_t offset = trunc_size(gate_offset(gate), size);
    Descriptor code;
    load_code_descriptor(s, sel, &code);
    uint32_t cflags = descriptor_flags(code);
    int dpl = desc_dpl(cflags);
    if (dpl > s->cpl) {
        raise_exception(s, EXCP_GP, sel_error(sel));
    }
    check_present(s, sel, cflags);

    bool vm86 = get_bit(s->eflags, EFLAGS_VM);
    bool inner = !get_bit(cflags, DESC_CE) && dpl < s->cpl;
    StackPtr st;
    X86CPUSeg ss;
    if (inner) {
        load_inner_stack(s, dpl, &ss, &st);
    } else {
        if (vm86) {
            raise_exception(s, EXCP_GP, sel_error(sel));
        }
        st = current_stack(s);
        dpl = s->cpl;
    }

    if (inner) {
        if (vm86) {
            stack_push(s, &st, s->segs[SEG_GS].sel, size);
            stack_push(s, &st, s->segs[SEG_FS].sel, size);
            stack_push(s, &st, s->segs[SEG_DS].sel, size);
            stack_push(s, &st, s->segs[SEG_ES].sel, size);
        }
        stack_push(s, &st, s->segs[SEG_SS].sel, size);
        stack_push(s, &st, s->regs[REG_ESP], size);
    }
    stack_push(s, &st, get_eflags(s), size);
    stack_push(s, &st, s->segs[SEG_CS].sel, size);
    stack_push(s, &st, ret_eip, size);
    if (has_error) {
        stack_push(s, &st, error_code, size);
    }

    s->eflags &= ~(bit_at(EFLAGS_TF) | bit_at(EFLAGS_VM) | bit_at(EFLAGS_RF) |
                   bit_at(EFLAGS_NT));
    if (!get_bit(type, 0)) {
        s->eflags = set_bit(s->eflags, EFLAGS_IF, false);
    }
    s->cpl = dpl;
    if (inner) {
        if (vm86) {
            load_null_seg(s, SEG_ES, 0);
            load_null_seg(s, SEG_DS, 0);
            load_null_seg(s, SEG_FS, 0);
            load_null_seg(s, SEG_GS, 0);
        }
        load_ss(s, ss);
    }
    stack_commit(s, st);
    load_cs(s, sel, code, dpl);
    s->eip = offset;
}

void do_interrupt(X86CPUState *s, int intno, bool is_soft, int error_code,
                  uint32_t ret_eip, bool is_hw)
{
    if (!get_bit(s->cr0, CR0_PE)) {
        do_interrupt_real(s, intno, ret_eip);
        return;
    }
    if (is_soft && get_bit(s->eflags, EFLAGS_VM) &&
        eflags_iopl(s->eflags) < 3) {
        raise_exception(s, EXCP_GP, 0);
    }
    bool has_error = !is_soft && !is_hw && exception_has_error_code(intno);
    do_interrupt_protected(s, intno, is_soft, has_error, error_code, ret_eip);
}


//#pragma mark - far transfers

void far_jump(X86CPUState *s, uint32_t sel, uint32_t offset,
              uint32_t next_eip)
{
    sel = get_bits(sel, 0, 16);
    if (!is_protected(s)) {
        load_seg_real(s, SEG_CS, sel);
        s->eip = offset;
        return;
    }

    Descriptor d;
    if (sel_is_null(sel)) {
        raise_exception(s, EXCP_GP, 0);
    }
    if (!load_descriptor(s, sel, &d)) {
        raise_exception(s, EXCP_GP, sel_error(sel));
    }
    uint32_t flags = descriptor_flags(d);
    int dpl = desc_dpl(flags);
    int rpl = sel_rpl(sel);

    if (get_bit(flags, DESC_S)) {
        if (!desc_is_code(flags)) {
            raise_exception(s, EXCP_GP, sel_error(sel));
        }
        check_direct_code(s, sel, flags, rpl);
        load_cs(s, sel, d, s->cpl);
        s->eip = offset;
        return;
    }

    int type = desc_type(flags);
    switch (type) {
    case SYS_TSS16:
    case SYS_TSS32:
    case SYS_TASK_GATE:
    case SYS_CALL_GATE16:
    case SYS_CALL_GATE32:
        break;
    default:
        raise_exception(s, EXCP_GP, sel_error(sel));
    }
    if (dpl < s->cpl || dpl < rpl) {
        raise_exception(s, EXCP_GP, sel_error(sel));
    }
    check_present(s, sel, flags);

    if (type == SYS_TSS16 || type == SYS_TSS32) {
        task_switch(s, sel, TASK_JMP, next_eip);
        return;
    }
    if (type == SYS_TASK_GATE) {
        task_switch(s, gate_selector(d), TASK_JMP, next_eip);
        return;
    }

    uint32_t csel = gate_selector(d);
    Descriptor cd;
    load_code_descriptor(s, csel, &cd);
    check_direct_code(s, csel, descriptor_flags(cd), s->cpl);
    load_cs(s, csel, cd, s->cpl);
    s->eip = trunc_size(gate_offset(d),
                        type == SYS_CALL_GATE32 ? SIZE32 : SIZE16);
}

void far_call(X86CPUState *s, uint32_t sel, uint32_t offset, int opsize,
              uint32_t next_eip)
{
    sel = get_bits(sel, 0, 16);
    if (!is_protected(s)) {
        StackPtr st = current_stack(s);
        stack_push(s, &st, s->segs[SEG_CS].sel, opsize);
        stack_push(s, &st, next_eip, opsize);
        stack_commit(s, st);
        load_seg_real(s, SEG_CS, sel);
        s->eip = offset;
        return;
    }

    Descriptor d;
    if (sel_is_null(sel)) {
        raise_exception(s, EXCP_GP, 0);
    }
    if (!load_descriptor(s, sel, &d)) {
        raise_exception(s, EXCP_GP, sel_error(sel));
    }
    uint32_t flags = descriptor_flags(d);
    int dpl = desc_dpl(flags);
    int rpl = sel_rpl(sel);

    if (get_bit(flags, DESC_S)) {
        if (!desc_is_code(flags)) {
            raise_exception(s, EXCP_GP, sel_error(sel));
        }
        check_direct_code(s, sel, flags, rpl);
        StackPtr st = current_stack(s);
        stack_push(s, &st, s->segs[SEG_CS].sel, opsize);
        stack_push(s, &st, next_eip, opsize);
        stack_commit(s, st);
        load_cs(s, sel, d, s->cpl);
        s->eip = offset;
        return;
    }

    int type = desc_type(flags);
    switch (type) {
    case SYS_TSS16:
    case SYS_TSS32:
    case SYS_TASK_GATE:
    case SYS_CALL_GATE16:
    case SYS_CALL_GATE32:
        break;
    default:
        raise_exception(s, EXCP_GP, sel_error(sel));
    }
    if (dpl < s->cpl || dpl < rpl) {
        raise_exception(s, EXCP_GP, sel_error(sel));
    }
    check_present(s, sel, flags);

    if (type == SYS_TSS16 || type == SYS_TSS32) {
        task_switch(s, sel, TASK_CALL, next_eip);
        return;
    }
    if (type == SYS_TASK_GATE) {
        task_switch(s, gate_selector(d), TASK_CALL, next_eip);
        return;
    }

    /* call gate */
    int size = type == SYS_CALL_GATE32 ? SIZE32 : SIZE16;
    uint32_t csel = gate_selector(d);
    Descriptor cd;
    load_code_descriptor(s, csel, &cd);
    uint32_t cflags = descriptor_flags(cd);
    int cdpl = desc_dpl(cflags);
    if (cdpl > s->cpl) {
        raise_exception(s, EXCP_GP, sel_error(csel));
    }
    check_present(s, csel, cflags);

    if (!get_bit(cflags, DESC_CE) && cdpl < s->cpl) {
        X86CPUSeg ss;
        StackPtr st;
        load_inner_stack(s, cdpl, &ss, &st);

        stack_push(s, &st, s->segs[SEG_SS].sel, size);
        stack_push(s, &st, s->regs[REG_ESP], size);
        for (int i = gate_param_count(d) - 1; i >= 0; i--) {
            StackPtr param = current_stack(s);
            param.sp += i * size_bytes(size);
            stack_push(s, &st, stack_pop(s, &param, size), size);
        }
        stack_push(s, &st, s->segs[SEG_CS].sel, size);
        stack_push(s, &st, next_eip, size);

        s->cpl = cdpl;
        load_ss(s, ss);
        stack_commit(s, st);
        load_cs(s, csel, cd, cdpl);
    } else {
        StackPtr st = current_stack(s);
        stack_push(s, &st, s->segs[SEG_CS].sel, size);
        stack_push(s, &st, next_eip, size);
        stack_commit(s, st);
        load_cs(s, csel, cd, s->cpl);
    }
    s->eip = trunc_size(gate_offset(d), size);
}

static void return_to_vm86(X86CPUState *s, StackPtr *st, uint32_t new_eip,
                           uint32_t new_cs, uint32_t new_eflags)
{
    static const int seg_order[4] = {SEG_ES, SEG_DS, SEG_FS, SEG_GS};
    uint32_t new_esp = stack_pop(s, st, SIZE32);
    uint32_t new_ss = stack_pop(s, st, SIZE32);
    uint32_t sels[4];
    for (int i = 0; i < 4; i++) {
        sels[i] = stack_pop(s, st, SIZE32);
    }

    cpu_set_eflags(s, new_eflags, UINT32_MAX);
    s->cpl = 3;
    load_seg_real(s, SEG_CS, new_cs);
    load_seg_real(s, SEG_SS, new_ss);
    for (int i = 0; i < 4; i++) {
        load_seg_real(s, seg_order[i], sels[i]);
    }
    s->regs[REG_ESP] = new_esp;
    s->eip = get_bits(new_eip, 0, 16);
}

/* RETF and IRET in protected mode. */
static void return_protected(X86CPUState *s, int opsize, bool is_iret,
                             uint32_t addend)
{
    StackPtr st = current_stack(s);
    uint32_t new_eip = stack_pop(s, &st, opsize);
    uint32_t new_cs = get_bits(stack_pop(s, &st, opsize), 0, 16);
    uint32_t new_eflags = 0;
    if (is_iret) {
        new_eflags = stack_pop(s, &st, opsize);
        if (opsize == SIZE32 && get_bit(new_eflags, EFLAGS_VM) &&
            s->cpl == 0) {
            return_to_vm86(s, &st, new_eip, new_cs, new_eflags);
            return;
        }
    }

    int cpl = s->cpl;
    int rpl = sel_rpl(new_cs);
    Descriptor cd;
    load_code_descriptor(s, new_cs, &cd);
    uint32_t cflags = descriptor_flags(cd);
    if (rpl < cpl || (get_bit(cflags, DESC_CE) ? desc_dpl(cflags) > rpl :
                      desc_dpl(cflags) != rpl)) {
        raise_exception(s, EXCP_GP, sel_error(new_cs));
    }
    check_present(s, new_cs, cflags);

    st.sp += addend;
    if (rpl == cpl) {
        stack_commit(s, st);
        load_cs(s, new_cs, cd, cpl);
    } else {
        uint32_t new_esp = stack_pop(s, &st, opsize);
        uint32_t new_ss = get_bits(stack_pop(s, &st, opsize), 0, 16);
        Descriptor ss;
        if (sel_is_null(new_ss)) {
            raise_exception(s, EXCP_GP, 0);
        }
        if (!load_descriptor(s, new_ss, &ss)) {
            raise_exception(s, EXCP_GP, sel_error(new_ss));
        }
        uint32_t ssflags = descriptor_flags(ss);
        if (sel_rpl(new_ss) != rpl || !desc_is_data(ssflags) ||
            !get_bit(ssflags, DESC_RW) || desc_dpl(ssflags) != rpl) {
            raise_exception(s, EXCP_GP, sel_error(new_ss));
        }
        if (!get_bit(ssflags, DESC_P)) {
            raise_exception(s, EXCP_SS, sel_error(new_ss));
        }

        load_cs(s, new_cs, cd, rpl);
        load_ss(s, seg_cache(new_ss, ss));
        StackPtr nst = current_stack(s);
        nst.sp = new_esp + addend;
        stack_commit(s, nst);

        /* data segments the outer level may not use become null */
        static const int data_segs[4] = {SEG_ES, SEG_DS, SEG_FS, SEG_GS};
        for (int seg : data_segs) {
            uint32_t flags = s->segs[seg].flags;
            bool conforming = desc_is_code(flags) && get_bit(flags, DESC_CE);
            if (!conforming && desc_dpl(flags) < rpl) {
                load_null_seg(s, seg, 0);
            }
        }
    }
    s->eip = new_eip;

    if (is_iret) {
        uint32_t mask = EFLAGS_CC_MASK | bit_at(EFLAGS_TF) |
            bit_at(EFLAGS_DF) | bit_at(EFLAGS_NT) | bit_at(EFLAGS_RF) |
            bit_at(EFLAGS_AC) | bit_at(EFLAGS_ID);
        if (cpl == 0) {
            mask |= field_mask(EFLAGS_IOPL, 2);
        }
        if (cpl <= eflags_iopl(s->eflags)) {
            mask = set_bit(mask, EFLAGS_IF, true);
        }
        cpu_set_eflags(s, new_eflags, trunc_size(mask, opsize));
    }
}

void far_return(X86CPUState *s, int opsize, uint32_t addend)
{
    if (is_protected(s)) {
        return_protected(s, opsize, false, addend);
        return;
    }
    StackPtr st = current_stack(s);
    uint32_t new_eip = stack_pop(s, &st, opsize);
    uint32_t new_cs = stack_pop(s, &st, opsize);
    st.sp += addend;
    stack_commit(s, st);
    load_seg_real(s, SEG_CS, new_cs);
    s->eip = new_eip;
}

void interrupt_return(X86CPUState *s, int opsize, uint32_t next_eip)
{
    if (is_protected(s)) {
        if (get_bit(s->eflags, EFLAGS_NT)) {
            task_switch(s, sys_read(s, s->tr.base + TSS_BACK_LINK, SIZE16),
                        TASK_IRET, next_eip);
        } else {
            return_protected(s, opsize, true, 0);
        }
        return;
    }

    bool vm86 = get_bit(s->eflags, EFLAGS_VM);
    if (vm86 && eflags_iopl(s->eflags) != 3) {
        raise_exception(s, EXCP_GP, 0);
    }
    StackPtr st = current_stack(s);
    uint32_t new_eip = stack_pop(s, &st, opsize);
    uint32_t new_cs = stack_pop(s, &st, opsize);
    uint32_t new_eflags = stack_pop(s, &st, opsize);
    uint32_t mask = EFLAGS_CC_MASK | bit_at(EFLAGS_TF) | bit_at(EFLAGS_IF) |
        bit_at(EFLAGS_DF) | bit_at(EFLAGS_NT) | bit_at(EFLAGS_RF) |
        bit_at(EFLAGS_AC) | bit_at(EFLAGS_ID);
    if (!vm86) {
        mask |= field_mask(EFLAGS_IOPL, 2);
    }
    stack_commit(s, st);
    load_seg_real(s, SEG_CS, new_cs);
    s->eip = new_eip;
    cpu_set_eflags(s, new_eflags, trunc_size(mask, opsize));
}


//#pragma mark - system instructions

void check_io_permission(X86CPUState *s, uint32_t port, int size)
{
    if (!get_bit(s->cr0, CR0_PE) ||
        (!get_bit(s->eflags, EFLAGS_VM) && s->cpl <= eflags_iopl(s->eflags))) {
        return;
    }
    int type = desc_type(s->tr.flags);
    if ((type != SYS_TSS32 && type != SYS_TSS32_BUSY) ||
        s->tr.limit < TSS32_MIN_LIMIT) {
        raise_exception(s, EXCP_GP, 0);
    }
    uint32_t pos = sys_read(s, s->tr.base + TSS_IOMAP, SIZE16) + port / 8;
    if (pos + 1 > s->tr.limit) {
        raise_exception(s, EXCP_GP, 0);
    }
    uint32_t bits = sys_read(s, s->tr.base + pos, SIZE16);
    if (get_bits(bits, get_bits(port, 0, 3), size_bytes(size)) != 0) {
        raise_exception(s, EXCP_GP, 0);
    }
}

void load_ldt(X86CPUState *s, uint32_t sel)
{
    sel = get_bits(sel, 0, 16);
    if (sel_is_null(sel)) {
        s->ldt = {(uint16_t)sel, 0, 0, 0};
        return;
    }
    Descriptor d;
    if (get_bit(sel, 2) || !load_descriptor(s, sel, &d)) {
        raise_exception(s, EXCP_GP, sel_error(sel));
    }
    uint32_t flags = descriptor_flags(d);
    if (get_bit(flags, DESC_S) || desc_type(flags) != SYS_LDT) {
        raise_exception(s, EXCP_GP, sel_error(sel));
    }
    check_present(s, sel, flags);
    s->ldt = {(uint16_t)sel, (uint16_t)flags, descriptor_base(d),
              descriptor_limit(d)};
}

void load_tr(X86CPUState *s, uint32_t sel)
{
    sel = get_bits(sel, 0, 16);
    if (sel_is_null(sel)) {
        raise_exception(s, EXCP_GP, 0);
    }
    Descriptor d;
    if (get_bit(sel, 2) || !load_descriptor(s, sel, &d)) {
        raise_exception(s, EXCP_GP, sel_error(sel));
    }
    uint32_t flags = descriptor_flags(d);
    int type = desc_type(flags);
    if (get_bit(flags, DESC_S) || (type != SYS_TSS16 && type != SYS_TSS32)) {
        raise_exception(s, EXCP_GP, sel_error(sel));
    }
    check_present(s, sel, flags);
    set_tss_busy(s, sel, true);
    s->tr = {(uint16_t)sel, (uint16_t)set_bit(flags, DESC_RW, true),
             descriptor_base(d), descriptor_limit(d)};
}

/* Whether the current privilege may see the descriptor, for LAR, LSL,
   VERR and VERW. */
static bool load_visible_descriptor(X86CPUState *s, uint32_t sel,
                                    Descriptor *d)
{
    sel = get_bits(sel, 0, 16);
    if (sel_is_null(sel) || !load_descriptor(s, sel, d)) {
        return false;
    }
    uint32_t flags = descriptor_flags(*d);
    int dpl = desc_dpl(flags);
    bool conforming = desc_is_code(flags) && get_bit(flags, DESC_CE);
    return conforming || (dpl >= s->cpl && dpl >= sel_rpl(sel));
}

bool seg_access_rights(X86CPUState *s, uint32_t sel, uint32_t *val)
{
    Descriptor d;
    if (!load_visible_descriptor(s, sel, &d)) {
        return false;
    }
    uint32_t flags = descriptor_flags(d);
    if (!get_bit(flags, DESC_S)) {
        switch (desc_type(flags)) {
        case SYS_TSS16:
        case SYS_LDT:
        case SYS_TSS16_BUSY:
        case SYS_CALL_GATE16:
        case SYS_TASK_GATE:
        case SYS_TSS32:
        case SYS_TSS32_BUSY:
        case SYS_CALL_GATE32:
            break;
        default:
            return false;
        }
    }
    *val = set_bits(0, 8, 16, flags);
    return true;
}

bool seg_limit(X86CPUState *s, uint32_t sel, uint32_t *val)
{
    Descriptor d;
    if (!load_visible_descriptor(s, sel, &d)) {
        return false;
    }
    uint32_t flags = descriptor_flags(d);
    if (!get_bit(flags, DESC_S)) {
        switch (desc_type(flags)) {
        case SYS_TSS16:
        case SYS_LDT:
        case SYS_TSS16_BUSY:
        case SYS_TSS32:
        case SYS_TSS32_BUSY:
            break;
        default:
            return false;
        }
    }
    *val = descriptor_limit(d);
    return true;
}

bool seg_verify(X86CPUState *s, uint32_t sel, bool write)
{
    Descriptor d;
    if (!load_visible_descriptor(s, sel, &d)) {
        return false;
    }
    uint32_t flags = descriptor_flags(d);
    if (!get_bit(flags, DESC_S)) {
        return false;
    }
    if (get_bit(flags, DESC_CODE)) {
        return !write && get_bit(flags, DESC_RW);
    }
    return !write || get_bit(flags, DESC_RW);
}

void cpu_sysenter(X86CPUState *s)
{
    if (!get_bit(s->cr0, CR0_PE) || sel_is_null(s->sysenter_cs)) {
        raise_exception(s, EXCP_GP, 0);
    }
    uint32_t sel = sel_error(s->sysenter_cs);
    s->eflags &= ~(bit_at(EFLAGS_VM) | bit_at(EFLAGS_IF) | bit_at(EFLAGS_RF));
    s->cpl = 0;
    load_seg_cache(s, SEG_CS, sel, 0, UINT32_MAX, flat_flags(true, 0));
    load_seg_cache(s, SEG_SS, sel + 8, 0, UINT32_MAX, flat_flags(false, 0));
    s->regs[REG_ESP] = s->sysenter_esp;
    s->eip = s->sysenter_eip;
}

void cpu_sysexit(X86CPUState *s)
{
    if (!get_bit(s->cr0, CR0_PE) || s->cpl != 0 ||
        sel_is_null(s->sysenter_cs)) {
        raise_exception(s, EXCP_GP, 0);
    }
    uint32_t sel = sel_error(s->sysenter_cs);
    s->cpl = 3;
    load_seg_cache(s, SEG_CS, set_bits(sel + 16, 0, 2, 3), 0, UINT32_MAX,
                   flat_flags(true, 3));
    load_seg_cache(s, SEG_SS, set_bits(sel + 24, 0, 2, 3), 0, UINT32_MAX,
                   flat_flags(false, 3));
    s->regs[REG_ESP] = s->regs[REG_ECX];
    s->eip = s->regs[REG_EDX];
}
