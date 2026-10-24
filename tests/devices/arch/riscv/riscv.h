/*
 * RISC-V specifics
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

#include <stdint.h>

#define CSR_READ(csr) ({ \
    unsigned long v_; \
    __asm__ volatile("csrr %0, " #csr : "=r"(v_)); \
    v_; })
#define CSR_WRITE(csr, val) \
    __asm__ volatile("csrw " #csr ", %0" : : "r"((unsigned long)(val)))

#define CSR_SET(csr, bits) \
    __asm__ volatile("csrs " #csr ", %0" : : "r"((unsigned long)(bits)))
#define CSR_CLEAR(csr, bits) \
    __asm__ volatile("csrc " #csr ", %0" : : "r"((unsigned long)(bits)))

/* Whether the image runs in M-mode, as the machine's firmware, rather than
   in S-mode under an SBI. */
bool riscv_machine_mode();

/* Interrupt causes, which are also the bits of mie/mip and sie/sip. */
#define IRQ_S_SOFT  1
#define IRQ_M_SOFT  3
#define IRQ_S_TIMER 5
#define IRQ_M_TIMER 7
#define IRQ_S_EXT   9
#define IRQ_M_EXT   11

/* The pending and enable registers of the mode the image runs in: mip and
   mie in M-mode, sip and sie in S-mode. */
unsigned long riscv_ip();
void riscv_ie_set(unsigned long bits);
void riscv_ie_clear(unsigned long bits);
/* The cause the mode's own external, timer and software interrupts use. */
unsigned riscv_ext_irq();
unsigned riscv_timer_irq();
unsigned riscv_soft_irq();

#define SBI_EXT_BASE 0x10
#define SBI_EXT_SRST 0x53525354
#define SBI_EXT_DBCN 0x4442434e

#define SBI_SRST_SHUTDOWN       0
#define SBI_SRST_REASON_NONE    0
#define SBI_SRST_REASON_FAILURE 1

struct SbiRet {
    long error;
    long value;
};

/* S-mode only. */
SbiRet sbi_call(unsigned long ext, unsigned long fid, unsigned long a0 = 0,
                unsigned long a1 = 0, unsigned long a2 = 0);
bool sbi_probe(unsigned long ext);
