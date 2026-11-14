/*
 * The SVM world switch.
 *
 * void tvmm_svm_run(SvmRunContext *context);
 *
 * Called with interrupts disabled, and returns with them disabled; they are
 * enabled only around VMRUN, under a clear GIF. Everything between the host's FPU state
 * going out and coming back is here, so no compiled code that may use SSE
 * runs while the guest's is loaded. GIF is clear throughout, so no NMI comes
 * while GS holds the guest's base.
 *
 * Both FPU states are saved and restored under the host's XCR0, so the
 * guest's SSE state is kept even while its own XCR0 leaves SSE out; its XCR0
 * is in force only around VMRUN.
 */

#define CTX_GUEST_VMCB	0
#define CTX_HOST_SAVE	8
#define CTX_GPRS		16
#define CTX_HOST_FPU	24
#define CTX_GUEST_FPU	32
#define CTX_HOST_XCR0	40
#define CTX_GUEST_XCR0	48

	.text
	.globl	tvmm_svm_run
	.type	tvmm_svm_run, @function
	.align	16
tvmm_svm_run:
	push	%rbp
	mov		%rsp, %rbp
	push	%rbx
	push	%r12
	push	%r13
	push	%r14
	push	%r15
	push	%rdi

	clgi

	mov		%rdi, %r12

	/* the host's FPU state out, the guest's in */
	mov		CTX_HOST_XCR0(%r12), %eax
	mov		CTX_HOST_XCR0+4(%r12), %edx
	mov		CTX_HOST_FPU(%r12), %rcx
	xsave64	(%rcx)
	mov		CTX_GUEST_FPU(%r12), %rcx
	xrstor64	(%rcx)

	mov		CTX_GUEST_XCR0(%r12), %rax
	cmp		CTX_HOST_XCR0(%r12), %rax
	je		1f
	mov		CTX_GUEST_XCR0(%r12), %eax
	mov		CTX_GUEST_XCR0+4(%r12), %edx
	xor		%ecx, %ecx
	xsetbv
1:
	/* the host's FS, GS, TR, LDTR and syscall MSRs out, the guest's in */
	mov		CTX_HOST_SAVE(%r12), %rax
	vmsave	%rax
	mov		CTX_GUEST_VMCB(%r12), %rax
	vmload	%rax

	mov		CTX_GPRS(%r12), %rsi
	mov		8*1(%rsi), %rcx
	mov		8*2(%rsi), %rdx
	mov		8*3(%rsi), %rbx
	mov		8*5(%rsi), %rbp
	mov		8*7(%rsi), %rdi
	mov		8*8(%rsi), %r8
	mov		8*9(%rsi), %r9
	mov		8*10(%rsi), %r10
	mov		8*11(%rsi), %r11
	mov		8*12(%rsi), %r12
	mov		8*13(%rsi), %r13
	mov		8*14(%rsi), %r14
	mov		8*15(%rsi), %r15
	mov		8*6(%rsi), %rsi

	/*
	 * With V_INTR_MASKING the host's IF, as VMRUN finds it, masks physical
	 * interrupts while the guest runs, so it must be set for an interrupt to
	 * make the guest exit. GIF holds them off until VMRUN sets it, and again
	 * from the exit on. RAX holds the guest's VMCB again after the exit.
	 */
	sti
	vmrun	%rax
	cli
	vmsave	%rax

	push	%rsi
	mov		8(%rsp), %rsi
	mov		CTX_GPRS(%rsi), %rsi
	mov		%rcx, 8*1(%rsi)
	mov		%rdx, 8*2(%rsi)
	mov		%rbx, 8*3(%rsi)
	mov		%rbp, 8*5(%rsi)
	mov		%rdi, 8*7(%rsi)
	mov		%r8, 8*8(%rsi)
	mov		%r9, 8*9(%rsi)
	mov		%r10, 8*10(%rsi)
	mov		%r11, 8*11(%rsi)
	mov		%r12, 8*12(%rsi)
	mov		%r13, 8*13(%rsi)
	mov		%r14, 8*14(%rsi)
	mov		%r15, 8*15(%rsi)
	pop		%rax
	mov		%rax, 8*6(%rsi)

	mov		(%rsp), %r12
	mov		CTX_HOST_SAVE(%r12), %rax
	vmload	%rax

	mov		CTX_GUEST_XCR0(%r12), %rax
	cmp		CTX_HOST_XCR0(%r12), %rax
	je		2f
	mov		CTX_HOST_XCR0(%r12), %eax
	mov		CTX_HOST_XCR0+4(%r12), %edx
	xor		%ecx, %ecx
	xsetbv
2:
	/* the guest's FPU state out, the host's back */
	mov		CTX_HOST_XCR0(%r12), %eax
	mov		CTX_HOST_XCR0+4(%r12), %edx
	mov		CTX_GUEST_FPU(%r12), %rcx
	xsave64	(%rcx)
	mov		CTX_HOST_FPU(%r12), %rcx
	xrstor64	(%rcx)

	stgi

	add		$8, %rsp
	pop		%r15
	pop		%r14
	pop		%r13
	pop		%r12
	pop		%rbx
	pop		%rbp
	ret
	.size	tvmm_svm_run, .-tvmm_svm_run

	.section	.note.GNU-stack, "", @progbits
