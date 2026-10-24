/*
 * x86_64 PVH entry
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
   at the hvm_start_info. The low 4 GB are identity mapped with 2 MB pages,
   which every backend has (the interpreter has no 1 GB ones), and the image
   runs in long mode from there. */

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

    /* the page tables are in the BSS, so it is cleared first */
    mov $__bss_start, %edi
    mov $__bss_end, %ecx
    sub %edi, %ecx
    xor %eax, %eax
    rep stosb

    /* PML4[0] -> PDPT, PDPT[0..3] -> four directories of 2 MB pages */
    mov $boot_pdpt + PTE_P + PTE_W, %eax
    mov %eax, boot_pml4
    mov $boot_pdpt, %edi
    mov $boot_pd + PTE_P + PTE_W, %eax
    mov $4, %ecx
2:  mov %eax, (%edi)
    add $8, %edi
    add $0x1000, %eax
    loop 2b
    mov $boot_pd, %edi
    mov $PTE_P + PTE_W + PTE_PS, %eax
    xor %edx, %edx
    mov $2048, %ecx
3:  mov %eax, (%edi)
    mov %edx, 4(%edi)
    add $8, %edi
    add $0x200000, %eax
    adc $0, %edx
    loop 3b

    mov $boot_pml4, %eax
    mov %eax, %cr3
    mov %cr4, %eax
    or $CR4_PAE, %eax
    mov %eax, %cr4
    mov $MSR_EFER, %ecx
    rdmsr
    or $EFER_LME, %eax
    wrmsr
    mov %cr0, %eax
    or $CR0_PG + CR0_PE, %eax
    mov %eax, %cr0
    ljmp $SEL_CODE64, $4f

    .code64
4:  mov $SEL_DATA, %eax
    mov %eax, %ds
    mov %eax, %es
    mov %eax, %ss
    xor %eax, %eax
    mov %eax, %fs
    mov %eax, %gs
    mov $__stack_top, %rsp
    mov %esi, %edi
    call x86_boot
5:  hlt
    jmp 5b

    .section .rodata
    .balign 8
gdt:
    .quad 0
    .quad GDT_CODE32
    .quad GDT_DATA
    .quad GDT_CODE64
gdt_end:
gdtr:
    .word gdt_end - gdt - 1
    .long gdt

    .section .bss.pagetables, "aw", @nobits
    .balign 4096
    .globl boot_pml4
boot_pml4:
    .skip 4096
boot_pdpt:
    .skip 4096
boot_pd:
    .skip 4 * 4096
