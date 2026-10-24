/*
 * x86 assembler definitions
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
#pragma once

#define XEN_ELFNOTE_PHYS32_ENTRY 18

#define SEL_CODE32 0x08
#define SEL_DATA   0x10
#define SEL_CODE64 0x18

/* flat 4 GB descriptors */
#define GDT_CODE32 0x00cf9a000000ffff
#define GDT_DATA   0x00cf92000000ffff
#define GDT_CODE64 0x00af9a000000ffff

#define PTE_P  0x1
#define PTE_W  0x2
#define PTE_PS 0x80

#define CR0_PE   0x1
#define CR0_PG   0x80000000
#define CR4_PAE  0x20
#define MSR_EFER 0xc0000080
#define EFER_LME 0x100

/* exceptions that push an error code */
#define X86_HAS_ERROR_CODE(v) \
    ((v) == 8 || ((v) >= 10 && (v) <= 14) || (v) == 17 || (v) == 21 || \
     (v) == 29 || (v) == 30)
