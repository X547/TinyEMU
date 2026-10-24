/*
 * x86 application processor start
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

/* The STARTUP IPI's real mode code, copied to a page below 1 MB: it loads
   the image's GDT and goes on in 32 bit protected mode in the image, then
   in long mode for x86_64, on the stack x86_ap_stack gives, into the
   function x86_ap_entry names. One processor starts at a time. */

    .text
    .code16
    .globl x86_ap_trampoline, x86_ap_trampoline_end
x86_ap_trampoline:
    cli
    mov %cs, %ax
    mov %ax, %ds
    lgdtl ap_gdtr - x86_ap_trampoline
    mov %cr0, %eax
    or $CR0_PE, %eax
    mov %eax, %cr0
    ljmpl $SEL_CODE32, $x86_ap_entry32
    .balign 4
ap_gdtr:
    .word GDT_LIMIT
    .long x86_gdt
x86_ap_trampoline_end:

    .code32
x86_ap_entry32:
    mov $SEL_DATA, %eax
    mov %eax, %ds
    mov %eax, %es
    mov %eax, %ss
    mov %eax, %fs
    mov %eax, %gs
#ifdef __x86_64__
    /* the boot processor's page tables */
    mov %cr4, %eax
    or $CR4_PAE, %eax
    mov %eax, %cr4
    mov $boot_pml4, %eax
    mov %eax, %cr3
    mov $MSR_EFER, %ecx
    rdmsr
    or $EFER_LME, %eax
    wrmsr
    mov %cr0, %eax
    or $CR0_PG + CR0_PE, %eax
    mov %eax, %cr0
    ljmp $SEL_CODE64, $1f

    .code64
1:  mov $SEL_DATA, %eax
    mov %eax, %ds
    mov %eax, %es
    mov %eax, %ss
    xor %eax, %eax
    mov %eax, %fs
    mov %eax, %gs
    mov x86_ap_stack, %rsp
    call *x86_ap_entry
#else
    mov x86_ap_stack, %esp
    call *x86_ap_entry
#endif
2:  hlt
    jmp 2b
