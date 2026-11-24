/*
 * The world switch, and the accesses of the hypervisor's that may trap.
 *
 * The guest runs with stvec at tvmm_riscv_exit and sscratch at its context.
 * A trap from the guest, a host interrupt included, lands there; with
 * interrupts disabled on the way in, the host takes the interrupt itself once
 * they are enabled again, as it is still pending.
 */

#include "RiscVContext.h"
#include "RiscVCsr.h"


	.text


	.align 2
	.globl tvmm_riscv_run
	.type tvmm_riscv_run, @function
tvmm_riscv_run:
	sd ra, TVMM_CTX_HOST_RA(a0)
	sd sp, TVMM_CTX_HOST_SP(a0)
	sd gp, TVMM_CTX_HOST_GP(a0)
	sd tp, TVMM_CTX_HOST_TP(a0)
	sd s0, TVMM_CTX_HOST_S(0)(a0)
	sd s1, TVMM_CTX_HOST_S(1)(a0)
	sd s2, TVMM_CTX_HOST_S(2)(a0)
	sd s3, TVMM_CTX_HOST_S(3)(a0)
	sd s4, TVMM_CTX_HOST_S(4)(a0)
	sd s5, TVMM_CTX_HOST_S(5)(a0)
	sd s6, TVMM_CTX_HOST_S(6)(a0)
	sd s7, TVMM_CTX_HOST_S(7)(a0)
	sd s8, TVMM_CTX_HOST_S(8)(a0)
	sd s9, TVMM_CTX_HOST_S(9)(a0)
	sd s10, TVMM_CTX_HOST_S(10)(a0)
	sd s11, TVMM_CTX_HOST_S(11)(a0)

	csrr t0, TVMM_CSR_SSTATUS
	sd t0, TVMM_CTX_HOST_SSTATUS(a0)
	csrr t0, TVMM_CSR_HSTATUS
	sd t0, TVMM_CTX_HOST_HSTATUS(a0)
	csrr t0, TVMM_CSR_STVEC
	sd t0, TVMM_CTX_HOST_STVEC(a0)
	csrr t0, TVMM_CSR_SSCRATCH
	sd t0, TVMM_CTX_HOST_SSCRATCH(a0)

	lla t0, tvmm_riscv_exit
	csrw TVMM_CSR_STVEC, t0
	csrw TVMM_CSR_SSCRATCH, a0

	ld t0, TVMM_CTX_GUEST_SSTATUS(a0)
	csrw TVMM_CSR_SSTATUS, t0
	ld t0, TVMM_CTX_GUEST_HSTATUS(a0)
	csrw TVMM_CSR_HSTATUS, t0
	ld t0, TVMM_CTX_GUEST_PC(a0)
	csrw TVMM_CSR_SEPC, t0

	ld x1, TVMM_CTX_GUEST_X(1)(a0)
	ld x2, TVMM_CTX_GUEST_X(2)(a0)
	ld x3, TVMM_CTX_GUEST_X(3)(a0)
	ld x4, TVMM_CTX_GUEST_X(4)(a0)
	ld x5, TVMM_CTX_GUEST_X(5)(a0)
	ld x6, TVMM_CTX_GUEST_X(6)(a0)
	ld x7, TVMM_CTX_GUEST_X(7)(a0)
	ld x8, TVMM_CTX_GUEST_X(8)(a0)
	ld x9, TVMM_CTX_GUEST_X(9)(a0)
	ld x11, TVMM_CTX_GUEST_X(11)(a0)
	ld x12, TVMM_CTX_GUEST_X(12)(a0)
	ld x13, TVMM_CTX_GUEST_X(13)(a0)
	ld x14, TVMM_CTX_GUEST_X(14)(a0)
	ld x15, TVMM_CTX_GUEST_X(15)(a0)
	ld x16, TVMM_CTX_GUEST_X(16)(a0)
	ld x17, TVMM_CTX_GUEST_X(17)(a0)
	ld x18, TVMM_CTX_GUEST_X(18)(a0)
	ld x19, TVMM_CTX_GUEST_X(19)(a0)
	ld x20, TVMM_CTX_GUEST_X(20)(a0)
	ld x21, TVMM_CTX_GUEST_X(21)(a0)
	ld x22, TVMM_CTX_GUEST_X(22)(a0)
	ld x23, TVMM_CTX_GUEST_X(23)(a0)
	ld x24, TVMM_CTX_GUEST_X(24)(a0)
	ld x25, TVMM_CTX_GUEST_X(25)(a0)
	ld x26, TVMM_CTX_GUEST_X(26)(a0)
	ld x27, TVMM_CTX_GUEST_X(27)(a0)
	ld x28, TVMM_CTX_GUEST_X(28)(a0)
	ld x29, TVMM_CTX_GUEST_X(29)(a0)
	ld x30, TVMM_CTX_GUEST_X(30)(a0)
	ld x31, TVMM_CTX_GUEST_X(31)(a0)
	ld x10, TVMM_CTX_GUEST_X(10)(a0)
	sret
	.size tvmm_riscv_run, . - tvmm_riscv_run


	.align 2
tvmm_riscv_exit:
	csrrw a0, TVMM_CSR_SSCRATCH, a0

	sd x1, TVMM_CTX_GUEST_X(1)(a0)
	sd x2, TVMM_CTX_GUEST_X(2)(a0)
	sd x3, TVMM_CTX_GUEST_X(3)(a0)
	sd x4, TVMM_CTX_GUEST_X(4)(a0)
	sd x5, TVMM_CTX_GUEST_X(5)(a0)
	sd x6, TVMM_CTX_GUEST_X(6)(a0)
	sd x7, TVMM_CTX_GUEST_X(7)(a0)
	sd x8, TVMM_CTX_GUEST_X(8)(a0)
	sd x9, TVMM_CTX_GUEST_X(9)(a0)
	sd x11, TVMM_CTX_GUEST_X(11)(a0)
	sd x12, TVMM_CTX_GUEST_X(12)(a0)
	sd x13, TVMM_CTX_GUEST_X(13)(a0)
	sd x14, TVMM_CTX_GUEST_X(14)(a0)
	sd x15, TVMM_CTX_GUEST_X(15)(a0)
	sd x16, TVMM_CTX_GUEST_X(16)(a0)
	sd x17, TVMM_CTX_GUEST_X(17)(a0)
	sd x18, TVMM_CTX_GUEST_X(18)(a0)
	sd x19, TVMM_CTX_GUEST_X(19)(a0)
	sd x20, TVMM_CTX_GUEST_X(20)(a0)
	sd x21, TVMM_CTX_GUEST_X(21)(a0)
	sd x22, TVMM_CTX_GUEST_X(22)(a0)
	sd x23, TVMM_CTX_GUEST_X(23)(a0)
	sd x24, TVMM_CTX_GUEST_X(24)(a0)
	sd x25, TVMM_CTX_GUEST_X(25)(a0)
	sd x26, TVMM_CTX_GUEST_X(26)(a0)
	sd x27, TVMM_CTX_GUEST_X(27)(a0)
	sd x28, TVMM_CTX_GUEST_X(28)(a0)
	sd x29, TVMM_CTX_GUEST_X(29)(a0)
	sd x30, TVMM_CTX_GUEST_X(30)(a0)
	sd x31, TVMM_CTX_GUEST_X(31)(a0)
	csrr t0, TVMM_CSR_SSCRATCH
	sd t0, TVMM_CTX_GUEST_X(10)(a0)

	csrr t0, TVMM_CSR_SEPC
	sd t0, TVMM_CTX_GUEST_PC(a0)
	csrr t0, TVMM_CSR_SSTATUS
	sd t0, TVMM_CTX_GUEST_SSTATUS(a0)
	csrr t0, TVMM_CSR_HSTATUS
	sd t0, TVMM_CTX_GUEST_HSTATUS(a0)
	csrr t0, TVMM_CSR_SCAUSE
	sd t0, TVMM_CTX_SCAUSE(a0)
	csrr t0, TVMM_CSR_STVAL
	sd t0, TVMM_CTX_STVAL(a0)
	csrr t0, TVMM_CSR_HTVAL
	sd t0, TVMM_CTX_HTVAL(a0)
	csrr t0, TVMM_CSR_HTINST
	sd t0, TVMM_CTX_HTINST(a0)

	/* hstatus first: SPV must be clear before anything returns to the
	   host's userland with sret */
	ld t0, TVMM_CTX_HOST_HSTATUS(a0)
	csrw TVMM_CSR_HSTATUS, t0
	ld t0, TVMM_CTX_HOST_STVEC(a0)
	csrw TVMM_CSR_STVEC, t0
	ld t0, TVMM_CTX_HOST_SSCRATCH(a0)
	csrw TVMM_CSR_SSCRATCH, t0
	ld t0, TVMM_CTX_HOST_SSTATUS(a0)
	csrw TVMM_CSR_SSTATUS, t0

	ld ra, TVMM_CTX_HOST_RA(a0)
	ld sp, TVMM_CTX_HOST_SP(a0)
	ld gp, TVMM_CTX_HOST_GP(a0)
	ld tp, TVMM_CTX_HOST_TP(a0)
	ld s0, TVMM_CTX_HOST_S(0)(a0)
	ld s1, TVMM_CTX_HOST_S(1)(a0)
	ld s2, TVMM_CTX_HOST_S(2)(a0)
	ld s3, TVMM_CTX_HOST_S(3)(a0)
	ld s4, TVMM_CTX_HOST_S(4)(a0)
	ld s5, TVMM_CTX_HOST_S(5)(a0)
	ld s6, TVMM_CTX_HOST_S(6)(a0)
	ld s7, TVMM_CTX_HOST_S(7)(a0)
	ld s8, TVMM_CTX_HOST_S(8)(a0)
	ld s9, TVMM_CTX_HOST_S(9)(a0)
	ld s10, TVMM_CTX_HOST_S(10)(a0)
	ld s11, TVMM_CTX_HOST_S(11)(a0)
	ret


/* A trap of an access below. a1 holds the RiscVTrap; the access is 4 bytes,
   which the return skips. Only t5 is used. */
	.align 2
tvmm_riscv_access_trap:
	li t5, 1
	sd t5, 0(a1)
	csrr t5, TVMM_CSR_SCAUSE
	sd t5, 8(a1)
	csrr t5, TVMM_CSR_STVAL
	sd t5, 16(a1)
	csrr t5, TVMM_CSR_SEPC
	addi t5, t5, 4
	csrw TVMM_CSR_SEPC, t5
	sret


/* hlvx.hu rd, (rs1) */
#define HLVX_HU(rd, rs1) .insn r 0x73, 4, 0x32, rd, rs1, x3


	.align 2
	.globl tvmm_riscv_read_insn
	.type tvmm_riscv_read_insn, @function
tvmm_riscv_read_insn:
	csrr t2, TVMM_CSR_STVEC
	lla t0, tvmm_riscv_access_trap
	csrw TVMM_CSR_STVEC, t0
	sd zero, 0(a1)
	li t0, 0
	mv t3, a0
	HLVX_HU(t0, t3)
	ld t1, 0(a1)
	bnez t1, 1f
	/* the low bits of a 32 bit instruction are both set */
	andi t1, t0, 3
	li t4, 3
	bne t1, t4, 1f
	addi t3, t3, 2
	li t1, 0
	HLVX_HU(t1, t3)
	ld t4, 0(a1)
	bnez t4, 1f
	slli t1, t1, 16
	or t0, t0, t1
1:
	csrw TVMM_CSR_STVEC, t2
	mv a0, t0
	ret
	.size tvmm_riscv_read_insn, . - tvmm_riscv_read_insn


	.align 2
	.globl tvmm_riscv_probe_henvcfg
	.type tvmm_riscv_probe_henvcfg, @function
tvmm_riscv_probe_henvcfg:
	csrr t2, TVMM_CSR_STVEC
	lla t0, tvmm_riscv_access_trap
	csrw TVMM_CSR_STVEC, t0
	sd zero, 0(a1)
	li t0, 0
	csrw TVMM_CSR_HENVCFG, a0
	csrr t0, TVMM_CSR_HENVCFG
	csrw TVMM_CSR_STVEC, t2
	mv a0, t0
	ret
	.size tvmm_riscv_probe_henvcfg, . - tvmm_riscv_probe_henvcfg


	.align 2
	.globl tvmm_riscv_save_fpu
	.type tvmm_riscv_save_fpu, @function
tvmm_riscv_save_fpu:
	fsd f0, 0*8(a0)
	fsd f1, 1*8(a0)
	fsd f2, 2*8(a0)
	fsd f3, 3*8(a0)
	fsd f4, 4*8(a0)
	fsd f5, 5*8(a0)
	fsd f6, 6*8(a0)
	fsd f7, 7*8(a0)
	fsd f8, 8*8(a0)
	fsd f9, 9*8(a0)
	fsd f10, 10*8(a0)
	fsd f11, 11*8(a0)
	fsd f12, 12*8(a0)
	fsd f13, 13*8(a0)
	fsd f14, 14*8(a0)
	fsd f15, 15*8(a0)
	fsd f16, 16*8(a0)
	fsd f17, 17*8(a0)
	fsd f18, 18*8(a0)
	fsd f19, 19*8(a0)
	fsd f20, 20*8(a0)
	fsd f21, 21*8(a0)
	fsd f22, 22*8(a0)
	fsd f23, 23*8(a0)
	fsd f24, 24*8(a0)
	fsd f25, 25*8(a0)
	fsd f26, 26*8(a0)
	fsd f27, 27*8(a0)
	fsd f28, 28*8(a0)
	fsd f29, 29*8(a0)
	fsd f30, 30*8(a0)
	fsd f31, 31*8(a0)
	frcsr t0
	sd t0, 32*8(a0)
	ret
	.size tvmm_riscv_save_fpu, . - tvmm_riscv_save_fpu


	.align 2
	.globl tvmm_riscv_load_fpu
	.type tvmm_riscv_load_fpu, @function
tvmm_riscv_load_fpu:
	fld f0, 0*8(a0)
	fld f1, 1*8(a0)
	fld f2, 2*8(a0)
	fld f3, 3*8(a0)
	fld f4, 4*8(a0)
	fld f5, 5*8(a0)
	fld f6, 6*8(a0)
	fld f7, 7*8(a0)
	fld f8, 8*8(a0)
	fld f9, 9*8(a0)
	fld f10, 10*8(a0)
	fld f11, 11*8(a0)
	fld f12, 12*8(a0)
	fld f13, 13*8(a0)
	fld f14, 14*8(a0)
	fld f15, 15*8(a0)
	fld f16, 16*8(a0)
	fld f17, 17*8(a0)
	fld f18, 18*8(a0)
	fld f19, 19*8(a0)
	fld f20, 20*8(a0)
	fld f21, 21*8(a0)
	fld f22, 22*8(a0)
	fld f23, 23*8(a0)
	fld f24, 24*8(a0)
	fld f25, 25*8(a0)
	fld f26, 26*8(a0)
	fld f27, 27*8(a0)
	fld f28, 28*8(a0)
	fld f29, 29*8(a0)
	fld f30, 30*8(a0)
	fld f31, 31*8(a0)
	ld t0, 32*8(a0)
	fscsr t0
	ret
	.size tvmm_riscv_load_fpu, . - tvmm_riscv_load_fpu


	.align 2
	.globl tvmm_riscv_hfence_all
	.type tvmm_riscv_hfence_all, @function
tvmm_riscv_hfence_all:
	/* hfence.vvma zero, zero, then hfence.gvma zero, zero */
	.insn r 0x73, 0, 0x11, x0, x0, x0
	.insn r 0x73, 0, 0x31, x0, x0, x0
	ret
	.size tvmm_riscv_hfence_all, . - tvmm_riscv_hfence_all


	.section .note.GNU-stack, "", @progbits
