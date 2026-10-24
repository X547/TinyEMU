/*
 * i686 PVH entry
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

/* Entered in flat 32 bit protected mode with paging off and EBX pointing
   at the hvm_start_info. The image stays in that mode. */

    .pushsection .note.Xen, "a", @note
    .balign 4
    .long 4
    .long 4
    .long XEN_ELFNOTE_PHYS32_ENTRY
    .asciz "Xen"
    .long pvh_entry
    .popsection

    .text
    .code32
    .globl pvh_entry
pvh_entry:
    cli
    cld
    lgdt gdtr
    ljmp $SEL_CODE32, $1f
1:  mov $SEL_DATA, %eax
    mov %eax, %ds
    mov %eax, %es
    mov %eax, %ss
    mov %eax, %fs
    mov %eax, %gs
    mov %ebx, %esi

    mov $__bss_start, %edi
    mov $__bss_end, %ecx
    sub %edi, %ecx
    xor %eax, %eax
    rep stosb

    mov $__stack_top, %esp
    push %esi
    call x86_boot
2:  hlt
    jmp 2b

    .section .rodata
    .balign 8
    .globl x86_gdt, x86_gdt_end
x86_gdt:
    .quad 0
    .quad GDT_CODE32
    .quad GDT_DATA
x86_gdt_end:
gdtr:
    .word GDT_LIMIT
    .long x86_gdt
