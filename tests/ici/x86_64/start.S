/* ICI benchmark: PVH entry, long mode, AP trampoline and interrupt stubs.
 *
 * The PVH entry is flat 32 bit protected mode with paging off and EBX
 * pointing at the hvm_start_info. The first 4 GB are identity mapped with
 * 2 MB pages, the top gigabyte (local APIC, IOAPIC) uncached.
 */
    .intel_syntax noprefix

#define SEL_CODE32 0x08
#define SEL_DATA 0x10
#define SEL_CODE64 0x18

#define MSR_EFER 0xc0000080
#define EFER_LME 0x100

#define LAPIC_BASE 0xfee00000
#define LAPIC_EOI 0xb0
#define LAPIC_ICR_LO 0x300
#define LAPIC_ICR_HI 0x310

/* keep in sync with ici.c */
#define VECTOR_PING 0x40
#define VECTOR_PONG 0x41
#define VECTOR_ACK 0x42
#define VECTOR_WAKE 0x43
#define AP_TRAMPOLINE 0x8000
#define STACK_SIZE 0x4000

    .pushsection .note.Xen, "a", @note
    .align 4
    .long 4                 /* namesz */
    .long 4                 /* descsz */
    .long 18                /* XEN_ELFNOTE_PHYS32_ENTRY */
    .asciz "Xen"
    .long pvh_entry
    .popsection

    .text
    .code32
    .globl pvh_entry
pvh_entry:
    cli
    mov esi, ebx
    lgdt [gdtr]
    mov eax, SEL_DATA
    mov ds, eax
    mov es, eax
    mov ss, eax
    mov esp, offset bsp_stack + STACK_SIZE

    /* PML4[0] -> PDPT, PDPT[0..3] -> four page directories of 2 MB pages */
    lea eax, [pdpt + 3]
    mov [pml4], eax
    lea edi, [pdpt]
    lea eax, [pd + 3]
    mov ecx, 4
1:  mov [edi], eax
    add edi, 8
    add eax, 0x1000
    loop 1b
    lea edi, [pd]
    mov eax, 0x83           /* present, writable, 2 MB */
    xor edx, edx
    mov ecx, 4 * 512
2:  mov ebx, eax
    cmp ecx, 512
    ja 3f
    or ebx, 0x18            /* PWT | PCD on the last gigabyte */
3:  mov [edi], ebx
    mov [edi + 4], edx
    add edi, 8
    add eax, 0x200000
    loop 2b

    mov edi, offset bsp_entry64
    jmp long_mode_on

/* Paging on, IA-32e mode, then a far return to the 64 bit code at EDI.
   ESI passes through. */
long_mode_on:
    mov eax, cr4
    or eax, 0x20            /* PAE */
    mov cr4, eax
    lea eax, [pml4]
    mov cr3, eax
    mov ecx, MSR_EFER
    rdmsr
    or eax, EFER_LME
    wrmsr
    mov eax, cr0
    or eax, 0x80000001      /* PG | PE */
    mov cr0, eax
    push SEL_CODE64
    push edi
    retf

/* An AP arrives here from the trampoline, in 32 bit mode on this GDT. */
ap_entry32:
    mov eax, SEL_DATA
    mov ds, eax
    mov es, eax
    mov ss, eax
    mov esp, [ap_stack_top]
    mov edi, offset ap_entry64
    jmp long_mode_on

    .code64
bsp_entry64:
    mov edi, esi            /* the hvm_start_info */
    call bsp_main
    jmp halt_forever

ap_entry64:
    call ap_main
halt_forever:
    cli
    hlt
    jmp halt_forever

/* Copied to AP_TRAMPOLINE; STARTUP vector AP_TRAMPOLINE >> 12. */
    .code16
    .globl ap_trampoline, ap_trampoline_end
ap_trampoline:
    cli
    mov ax, cs
    mov ds, ax
    .att_syntax
    lgdtl ap_gdtr - ap_trampoline
    .intel_syntax noprefix
    mov eax, cr0
    or eax, 1
    mov cr0, eax
    .byte 0x66, 0xea        /* ljmp SEL_CODE32:ap_entry32 */
    .long ap_entry32
    .word SEL_CODE32
    .align 4
ap_gdtr:
    .word gdt_end - gdt - 1
    .long gdt
ap_trampoline_end:

    .code64
/* Interrupt stubs. Each handler does its one thing and returns: they are
   what is being measured. */

/* On the target: answer CPU 0. */
    .globl ping_handler
ping_handler:
    push rax
    push rcx
    mov ecx, LAPIC_BASE
    mov eax, [bsp_apic_id]
    shl eax, 24
    mov dword ptr [rcx + LAPIC_ICR_HI], eax
    mov dword ptr [rcx + LAPIC_ICR_LO], VECTOR_PONG
    mov dword ptr [rcx + LAPIC_EOI], 0
    pop rcx
    pop rax
    iretq

/* On CPU 0: the answer is in. */
    .globl pong_handler
pong_handler:
    push rcx
    mov dword ptr [pong_flag], 1
    mov ecx, LAPIC_BASE
    mov dword ptr [rcx + LAPIC_EOI], 0
    pop rcx
    iretq

/* On every target of a broadcast. */
    .globl ack_handler
ack_handler:
    push rcx
    lock inc dword ptr [ack_count]
    mov ecx, LAPIC_BASE
    mov dword ptr [rcx + LAPIC_EOI], 0
    pop rcx
    iretq

/* Wakes a halted processor to look at its mode again. */
    .globl wake_handler
wake_handler:
    push rcx
    mov ecx, LAPIC_BASE
    mov dword ptr [rcx + LAPIC_EOI], 0
    pop rcx
    iretq

/* Spurious vector: no EOI. */
    .globl spurious_handler
spurious_handler:
    iretq

/* Exceptions: vector and error code to fault() in ici.c, which does not
   return. */
.macro exception vec, has_error
    .globl exception_\vec
exception_\vec:
    .if \has_error == 0
    push 0
    .endif
    push \vec
    jmp exception_common
.endm

    exception 0, 0
    exception 1, 0
    exception 2, 0
    exception 3, 0
    exception 4, 0
    exception 5, 0
    exception 6, 0
    exception 7, 0
    exception 8, 1
    exception 9, 0
    exception 10, 1
    exception 11, 1
    exception 12, 1
    exception 13, 1
    exception 14, 1
    exception 15, 0
    exception 16, 0
    exception 17, 1
    exception 18, 0
    exception 19, 0
    exception 20, 0
    exception 21, 1

    .globl exception_table
    .section .rodata
    .align 8
exception_table:
    .irp vec, 0,1,2,3,4,5,6,7,8,9,10,11,12,13,14,15,16,17,18,19,20,21
    .quad exception_\vec
    .endr
    .text

exception_common:
    mov rdi, rsp            /* vector, error code, then the CPU's frame */
    and rsp, -16
    call fault
    jmp halt_forever

    .section .rodata
    .align 8
gdt:
    .quad 0
    .quad 0x00cf9a000000ffff    /* 32 bit code */
    .quad 0x00cf92000000ffff    /* data */
    .quad 0x00af9a000000ffff    /* 64 bit code */
gdt_end:
    .align 4
    .word 0
gdtr:
    .word gdt_end - gdt - 1
    .long gdt

    .bss
    .align 4096
pml4:
    .space 4096
pdpt:
    .space 4096
pd:
    .space 4 * 4096
    .align 16
bsp_stack:
    .space STACK_SIZE
