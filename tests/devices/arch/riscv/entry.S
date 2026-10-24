/*
 * RISC-V entry, relocation and trap vectors
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
#include "asm.h"

/* Entered with a0 = hart ID and a1 = the device tree, either as the
   machine's firmware in M-mode, where every hart arrives, or as a kernel in
   S-mode under an SBI, where only the boot hart does. Nothing here may use
   an absolute address before the relocations are applied. */

    .section .text.entry, "ax"
    .globl _start
_start:
    /* Which mode: reading mstatus traps in S-mode, to stvec, which either
       mode may write. */
    lla t0, .Lprobe_trap
    csrw stvec, t0
    li s2, 1
    csrr t1, mstatus
    j .Lprobed
    .balign 4
.Lprobe_trap:
    li s2, 0
.Lprobed:
    /* In M-mode every hart is here: the first to arrive boots, the others
       wait. */
    beqz s2, .Lboot
    lla t0, boot_lottery
    li t1, 1
    amoswap.w t1, t1, (t0)
    bnez t1, .Lpark

.Lboot:
    mv s0, a0
    mv s1, a1

    /* Linked at 0, so the runtime address of the image is the amount to
       add to every relocated word. */
    lla t0, __image_start
    lla t1, __rela_start
    lla t2, __rela_end
1:  bgeu t1, t2, 3f
    REG_L t3, 0(t1)
    REG_L t4, REGBYTES(t1)
    REG_L t5, (2 * REGBYTES)(t1)
    andi t4, t4, 0xff
    li t6, R_RISCV_RELATIVE
    bne t4, t6, 2f
    add t3, t3, t0
    add t5, t5, t0
    REG_S t5, 0(t3)
2:  addi t1, t1, 3 * REGBYTES
    j 1b
3:
    lla t1, __bss_start
    lla t2, __bss_end
4:  bgeu t1, t2, 5f
    REG_S zero, 0(t1)
    addi t1, t1, REGBYTES
    j 4b
5:
    fence.i
    lla sp, __stack_top
    mv a0, s0
    mv a1, s1
    mv a2, s2
    mv tp, s0
    call riscv_boot

/* The harts that lost the lottery wait for the boot hart to release them
   with an entry point and a stack in their hart ID's slot, woken by their
   machine software interrupt. a0 is the hart ID. */
.Lpark:
    li t0, 1 << 3 /* MSIE, so that wfi wakes; MIE stays off */
    csrw mie, t0
    wfi
    li t0, MAX_HARTS
    bgeu a0, t0, .Lpark
    slli t1, a0, REGBYTES_LOG2
    lla t0, riscv_hart_entry
    add t0, t0, t1
    REG_L t2, 0(t0)
    beqz t2, .Lpark
    lla t0, riscv_hart_stack
    add t0, t0, t1
    REG_L sp, 0(t0)
    csrw mie, zero
    mv tp, a0
    jr t2

/* A hart the SBI started: a0 its hart ID, a1 the stack. */
    .globl riscv_hart_start_s
    .balign 4
riscv_hart_start_s:
    mv sp, a1
    mv tp, a0
    lla t0, riscv_hart_entry_s
    REG_L t0, 0(t0)
    jr t0


/* The trap vectors: everything is saved on the stack the trap arrived on,
   which is the only stack there is. */
.macro TRAP_ENTRY name, epc, cause, tval, status, ret, is_m
    .balign 4
    .globl \name
\name:
    addi sp, sp, -FRAME_SIZE
    REG_S x1, (1 * REGBYTES)(sp)
    REG_S x3, (3 * REGBYTES)(sp)
    REG_S x4, (4 * REGBYTES)(sp)
    REG_S x5, (5 * REGBYTES)(sp)
    REG_S x6, (6 * REGBYTES)(sp)
    REG_S x7, (7 * REGBYTES)(sp)
    REG_S x8, (8 * REGBYTES)(sp)
    REG_S x9, (9 * REGBYTES)(sp)
    REG_S x10, (10 * REGBYTES)(sp)
    REG_S x11, (11 * REGBYTES)(sp)
    REG_S x12, (12 * REGBYTES)(sp)
    REG_S x13, (13 * REGBYTES)(sp)
    REG_S x14, (14 * REGBYTES)(sp)
    REG_S x15, (15 * REGBYTES)(sp)
    REG_S x16, (16 * REGBYTES)(sp)
    REG_S x17, (17 * REGBYTES)(sp)
    REG_S x18, (18 * REGBYTES)(sp)
    REG_S x19, (19 * REGBYTES)(sp)
    REG_S x20, (20 * REGBYTES)(sp)
    REG_S x21, (21 * REGBYTES)(sp)
    REG_S x22, (22 * REGBYTES)(sp)
    REG_S x23, (23 * REGBYTES)(sp)
    REG_S x24, (24 * REGBYTES)(sp)
    REG_S x25, (25 * REGBYTES)(sp)
    REG_S x26, (26 * REGBYTES)(sp)
    REG_S x27, (27 * REGBYTES)(sp)
    REG_S x28, (28 * REGBYTES)(sp)
    REG_S x29, (29 * REGBYTES)(sp)
    REG_S x30, (30 * REGBYTES)(sp)
    REG_S x31, (31 * REGBYTES)(sp)
    addi t0, sp, FRAME_SIZE
    REG_S t0, (2 * REGBYTES)(sp)
    csrr t0, \epc
    REG_S t0, (FRAME_EPC * REGBYTES)(sp)
    csrr t0, \cause
    REG_S t0, (FRAME_CAUSE * REGBYTES)(sp)
    csrr t0, \tval
    REG_S t0, (FRAME_TVAL * REGBYTES)(sp)
    csrr t0, \status
    REG_S t0, (FRAME_STATUS * REGBYTES)(sp)

    mv a0, sp
    li a1, \is_m
    call riscv_trap

    REG_L t0, (FRAME_EPC * REGBYTES)(sp)
    csrw \epc, t0
    REG_L x1, (1 * REGBYTES)(sp)
    REG_L x3, (3 * REGBYTES)(sp)
    REG_L x4, (4 * REGBYTES)(sp)
    REG_L x5, (5 * REGBYTES)(sp)
    REG_L x6, (6 * REGBYTES)(sp)
    REG_L x7, (7 * REGBYTES)(sp)
    REG_L x8, (8 * REGBYTES)(sp)
    REG_L x9, (9 * REGBYTES)(sp)
    REG_L x10, (10 * REGBYTES)(sp)
    REG_L x11, (11 * REGBYTES)(sp)
    REG_L x12, (12 * REGBYTES)(sp)
    REG_L x13, (13 * REGBYTES)(sp)
    REG_L x14, (14 * REGBYTES)(sp)
    REG_L x15, (15 * REGBYTES)(sp)
    REG_L x16, (16 * REGBYTES)(sp)
    REG_L x17, (17 * REGBYTES)(sp)
    REG_L x18, (18 * REGBYTES)(sp)
    REG_L x19, (19 * REGBYTES)(sp)
    REG_L x20, (20 * REGBYTES)(sp)
    REG_L x21, (21 * REGBYTES)(sp)
    REG_L x22, (22 * REGBYTES)(sp)
    REG_L x23, (23 * REGBYTES)(sp)
    REG_L x24, (24 * REGBYTES)(sp)
    REG_L x25, (25 * REGBYTES)(sp)
    REG_L x26, (26 * REGBYTES)(sp)
    REG_L x27, (27 * REGBYTES)(sp)
    REG_L x28, (28 * REGBYTES)(sp)
    REG_L x29, (29 * REGBYTES)(sp)
    REG_L x30, (30 * REGBYTES)(sp)
    REG_L x31, (31 * REGBYTES)(sp)
    addi sp, sp, FRAME_SIZE
    \ret
.endm

    .text
    TRAP_ENTRY riscv_trap_entry_m, mepc, mcause, mtval, mstatus, mret, 1
    TRAP_ENTRY riscv_trap_entry_s, sepc, scause, stval, sstatus, sret, 0


    .data
    .balign 8
/* In .data, not .bss: the boot hart clears the BSS while the others may
   still be on their way here. */
boot_lottery:
    .word 0
    .balign 8
    .globl riscv_hart_entry, riscv_hart_stack, riscv_hart_entry_s
riscv_hart_entry:
    .skip MAX_HARTS * REGBYTES
riscv_hart_stack:
    .skip MAX_HARTS * REGBYTES
riscv_hart_entry_s:
    .skip REGBYTES
