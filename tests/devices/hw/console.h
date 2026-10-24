/*
 * Where the report goes
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

#include "registry.h"

/* The best output the machine has, in this order: the test-control
   device, which leaves the consoles to the tests; the UART /chosen names,
   or else the first 16550; HTIF; the SBI debug console. */
void console_init();
bool console_ready();
/* "test-control", "uart", "htif" or "sbi" */
const char *console_kind();
/* The device carrying the output, which a test must leave alone; nullptr
   for the SBI. */
const HwDevice *console_device();

void console_write(const char *s, size_t len);

/* One line of the protocol the runner reads: "@@T " and the text. */
void report(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
void vreport(const char *fmt, va_list ap);

/* The test-control device, when there is one. */
const HwDevice *test_control_device();
