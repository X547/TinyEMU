/*
 * x86 interrupt vectors
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

/* Who uses which vectors, so that no two drivers or tests collide. */
#define VEC_PIC_MASTER  0x20 /* to 0x27 */
#define VEC_PIC_SLAVE   0x28 /* to 0x2f */
#define VEC_IOAPIC      0x40 /* plus the GSI, to 0x7f */
#define VEC_MSI         0x80 /* to 0xbf */
#define VEC_TEST        0xc0 /* to 0xdf, for tests that pick their own */
#define VEC_IPI         0xe0 /* to 0xef */
#define VEC_LAPIC_TIMER 0xf0
#define VEC_SPURIOUS    0xff
