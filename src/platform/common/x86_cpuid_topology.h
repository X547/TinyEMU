/*
 * The processor topology x86 hypervisor guests are shown
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

/* The "AuthenticAMD" of CPUID 0 EBX: AMD's topology leaves, not Intel's. */
bool x86_cpuid_is_amd(uint32_t leaf0_ebx);

/* Rewrites the topology fields of CPUID 'leaf'/'subleaf' in 'regs' (eax, ebx,
   ecx, edx) for processor 'index' of 'cpu_count': one package of that many
   cores of one thread each, APIC IDs being the indices. A hypervisor's own
   topology may count each processor as a package, of which a client
   edition of Windows uses one. */
void x86_cpuid_topology(uint32_t leaf, uint32_t subleaf, int cpu_count,
                        int index, bool amd, uint32_t regs[4]);
