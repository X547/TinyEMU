/*
 * Running code on the other processors
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

#include "klib.h"

/* The other processors are started when a test first asks for them and
   then wait, interrupts off, for something to run. A trap on one of them
   is reported and that processor stops for the rest of the run. */

int smp_cpu_count();
int smp_boot_index();
/* the index of the processor running this */
int smp_current();
bool smp_started(int index);

/* Run 'fn' on processor 'index', starting it first if need be, and wait
   for it to return; false if it did not within 'timeout_us'. */
bool smp_run(int index, void (*fn)(void *), void *ctx,
             uint64_t timeout_us = 1000000);
/* the same, without waiting: smp_wait() then waits */
bool smp_post(int index, void (*fn)(void *), void *ctx);
bool smp_wait(int index, uint64_t timeout_us = 1000000);

/* For the runner: what each started processor resets of its own between
   tests, and a trap on one of them, which stops it. */
void smp_quiesce();
[[noreturn]] void smp_ap_trapped();

/* The first processor other than the boot one, started, or the test is
   skipped. */
int t_require_other_cpu();
