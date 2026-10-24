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
#include "x86_cpuid_topology.h"

#include <algorithm>

#include "bits.h"

/* CPUID 1 EDX: more than one logical processor in the package */
#define CPUID_HTT 28
/* CPUID 0x80000001 ECX: the count of leaf 1 is of cores */
#define CPUID_EXT_CMP_LEGACY 1

/* CPUID 0xb and 0x1f level types */
#define TOPOLOGY_LEVEL_SMT 1
#define TOPOLOGY_LEVEL_CORE 2

/* CPUID 4 EAX: the cache level */
#define CACHE_LEVEL(eax) get_bits(eax, 5, 3)

bool x86_cpuid_is_amd(uint32_t leaf0_ebx)
{
    return leaf0_ebx == 0x68747541; /* "Auth" */
}

void x86_cpuid_topology(uint32_t leaf, uint32_t subleaf, int cpu_count,
                        int index, bool amd, uint32_t regs[4])
{
    /* the APIC ID bits that number the cores */
    int core_bits = 0;
    while ((1 << core_bits) < cpu_count)
        core_bits++;
    uint32_t core_ids = 1u << core_bits;

    switch (leaf) {
    case 1:
        /* AMD counts the cores there are, Intel the IDs they may take */
        regs[1] = set_bits(regs[1], 16, 8,
                           amd ? cpu_count : std::min(core_ids, 0xffu));
        regs[1] = set_bits(regs[1], 24, 8, (uint32_t)index);
        regs[3] = set_bit(regs[3], CPUID_HTT, cpu_count > 1);
        break;
    case 4:
        /* Intel's caches: a core's own, but for the package's L3 */
        if (amd || get_bits(regs[0], 0, 5) == 0)
            break;
        regs[0] = set_bits(regs[0], 26, 6, core_ids - 1);
        regs[0] = set_bits(regs[0], 14, 12,
                           CACHE_LEVEL(regs[0]) == 3 ? core_ids - 1 : 0);
        break;
    case 0xb:
    case 0x1f:
        /* the thread level, then the core level, which is the package */
        if (subleaf == 0) {
            regs[0] = 0;
            regs[1] = 1;
            regs[2] = TOPOLOGY_LEVEL_SMT << 8;
        } else if (subleaf == 1) {
            regs[0] = core_bits;
            regs[1] = cpu_count;
            regs[2] = TOPOLOGY_LEVEL_CORE << 8 | 1;
        } else {
            regs[0] = regs[1] = 0;
            regs[2] = subleaf;
        }
        regs[3] = index;
        break;
    case 0x80000001:
        if (amd)
            regs[2] = set_bit(regs[2], CPUID_EXT_CMP_LEGACY, cpu_count > 1);
        break;
    case 0x80000008:
        /* the cores and the APIC ID bits that number them */
        if (amd) {
            regs[2] = set_bits(regs[2], 0, 8, (uint32_t)cpu_count - 1);
            regs[2] = set_bits(regs[2], 12, 4, (uint32_t)core_bits);
        }
        break;
    case 0x8000001e:
        /* the extended APIC ID and the core, of one thread on node 0 */
        if (amd) {
            regs[0] = index;
            regs[1] = index;
            regs[2] = 0;
        }
        break;
    }
}
