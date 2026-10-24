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

/* Whether the image runs in M-mode, as the machine's firmware, rather than
   in S-mode under an SBI. */
bool riscv_machine_mode();

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
