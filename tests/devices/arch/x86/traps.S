/*
 * x86 exception stubs
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
#include "x86_asm.h"

/* One stub per vector pushes a dummy error code where the processor does
   not, then the vector, so that every frame has the same shape; x86_trap()
   sees it as struct X86TrapFrame. The stubs are 16 bytes apart, so vector
   n's is at x86_trap_stubs + 16 * n. */

    .text
    .balign 16
    .globl x86_trap_stubs
x86_trap_stubs:
.set i, 0
.rept 256
    .balign 16
    .if X86_HAS_ERROR_CODE(i) == 0
    push $0
    .endif
    push $i
    jmp trap_common
.set i, i + 1
.endr

#ifdef __x86_64__
trap_common:
    push %rax
    push %rcx
    push %rdx
    push %rbx
    push %rbp
    push %rsi
    push %rdi
    push %r8
    push %r9
    push %r10
    push %r11
    push %r12
    push %r13
    push %r14
    push %r15
    mov %rsp, %rdi
    mov %rsp, %rbx
    and $-16, %rsp
    call x86_trap
    mov %rbx, %rsp
    pop %r15
    pop %r14
    pop %r13
    pop %r12
    pop %r11
    pop %r10
    pop %r9
    pop %r8
    pop %rdi
    pop %rsi
    pop %rbp
    pop %rbx
    pop %rdx
    pop %rcx
    pop %rax
    add $16, %rsp
    iretq
#else
trap_common:
    pushal
    push %esp
    call x86_trap
    add $4, %esp
    popal
    add $8, %esp
    iret
#endif
