/*
 * RISC-V assembler helpers
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

#if __riscv_xlen == 64
#define REG_L ld
#define REG_S sd
#define REGBYTES 8
#else
#define REG_L lw
#define REG_S sw
#define REGBYTES 4
#endif

/* Trap frame layout, shared with arch.cpp: x1..x31 at their index, then
   the trap CSRs. */
#define FRAME_EPC    32
#define FRAME_CAUSE  33
#define FRAME_TVAL   34
#define FRAME_STATUS 35
#define FRAME_WORDS  36
#define FRAME_SIZE   (FRAME_WORDS * REGBYTES)

#define R_RISCV_RELATIVE 3
