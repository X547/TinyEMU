/*
 * x86 setjmp and longjmp
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

    .text
    .globl arch_setjmp
    .globl arch_longjmp

#ifdef __x86_64__
arch_setjmp:
    mov (%rsp), %rax
    mov %rax, 0(%rdi)
    lea 8(%rsp), %rax
    mov %rax, 8(%rdi)
    mov %rbx, 16(%rdi)
    mov %rbp, 24(%rdi)
    mov %r12, 32(%rdi)
    mov %r13, 40(%rdi)
    mov %r14, 48(%rdi)
    mov %r15, 56(%rdi)
    xor %eax, %eax
    ret

arch_longjmp:
    mov %esi, %eax
    test %eax, %eax
    jnz 1f
    inc %eax
1:  mov 16(%rdi), %rbx
    mov 24(%rdi), %rbp
    mov 32(%rdi), %r12
    mov 40(%rdi), %r13
    mov 48(%rdi), %r14
    mov 56(%rdi), %r15
    mov 8(%rdi), %rsp
    jmp *0(%rdi)
#else
arch_setjmp:
    mov 4(%esp), %ecx
    mov (%esp), %eax
    mov %eax, 0(%ecx)
    lea 4(%esp), %eax
    mov %eax, 4(%ecx)
    mov %ebx, 8(%ecx)
    mov %ebp, 12(%ecx)
    mov %esi, 16(%ecx)
    mov %edi, 20(%ecx)
    xor %eax, %eax
    ret

arch_longjmp:
    mov 4(%esp), %ecx
    mov 8(%esp), %eax
    test %eax, %eax
    jnz 1f
    inc %eax
1:  mov 8(%ecx), %ebx
    mov 12(%ecx), %ebp
    mov 16(%ecx), %esi
    mov 20(%ecx), %edi
    mov 4(%ecx), %esp
    jmp *0(%ecx)
#endif
