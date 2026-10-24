/*
 * RISC-V setjmp and longjmp
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
#include "asm.h"

    .text
    .globl arch_setjmp
arch_setjmp:
    REG_S ra, (0 * REGBYTES)(a0)
    REG_S sp, (1 * REGBYTES)(a0)
    REG_S s0, (2 * REGBYTES)(a0)
    REG_S s1, (3 * REGBYTES)(a0)
    REG_S s2, (4 * REGBYTES)(a0)
    REG_S s3, (5 * REGBYTES)(a0)
    REG_S s4, (6 * REGBYTES)(a0)
    REG_S s5, (7 * REGBYTES)(a0)
    REG_S s6, (8 * REGBYTES)(a0)
    REG_S s7, (9 * REGBYTES)(a0)
    REG_S s8, (10 * REGBYTES)(a0)
    REG_S s9, (11 * REGBYTES)(a0)
    REG_S s10, (12 * REGBYTES)(a0)
    REG_S s11, (13 * REGBYTES)(a0)
    li a0, 0
    ret

    .globl arch_longjmp
arch_longjmp:
    REG_L ra, (0 * REGBYTES)(a0)
    REG_L sp, (1 * REGBYTES)(a0)
    REG_L s0, (2 * REGBYTES)(a0)
    REG_L s1, (3 * REGBYTES)(a0)
    REG_L s2, (4 * REGBYTES)(a0)
    REG_L s3, (5 * REGBYTES)(a0)
    REG_L s4, (6 * REGBYTES)(a0)
    REG_L s5, (7 * REGBYTES)(a0)
    REG_L s6, (8 * REGBYTES)(a0)
    REG_L s7, (9 * REGBYTES)(a0)
    REG_L s8, (10 * REGBYTES)(a0)
    REG_L s9, (11 * REGBYTES)(a0)
    REG_L s10, (12 * REGBYTES)(a0)
    REG_L s11, (13 * REGBYTES)(a0)
    seqz a0, a1
    add a0, a0, a1
    ret
