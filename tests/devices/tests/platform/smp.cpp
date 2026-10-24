/*
 * Starting the other processors
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
#include "arch.h"
#include "smp.h"
#include "test.h"

/* Every processor the tables list starts, by INIT and STARTUP on x86, by
   its software interrupt in M-mode and by the SBI in S-mode, and runs what
   it is given. */

namespace {

volatile uint32_t sSeen[16];
volatile int sRuns;

}


TEST(smp, start_all)
{
    int n = smp_cpu_count();
    if (n < 2)
        t_skip("one processor");
    SNAP("cpus", "%d", n);
    for (int i = 0; i < n; i++) {
        if (i == smp_boot_index())
            continue;
        sSeen[i] = ~0u;
        bool ran = smp_run(i, [](void *p) {
            sSeen[(int)(uintptr_t)p] = arch_this_cpu_hw_id();
        }, (void *)(uintptr_t)i);
        if (!ran)
            t_fail(__FILE__, __LINE__, "processor %d did not run", i);
        else
            EXPECT_EQ(sSeen[i], arch_cpu_hw_id(i));
    }
}

TEST(smp, run_repeatedly)
{
    int other = t_require_other_cpu();
    sRuns = 0;
    for (int i = 0; i < 100; i++) {
        if (!smp_run(other, [](void *) { sRuns = sRuns + 1; }, nullptr))
            break;
    }
    EXPECT_EQ(sRuns, 100);
}

TEST(smp, concurrent)
{
    int other = t_require_other_cpu();
    /* both at once: the other one spins until this one lets it go */
    static volatile int flag;
    flag = 0;
    ASSERT_TRUE(smp_post(other, [](void *) {
        while (__atomic_load_n(&flag, __ATOMIC_ACQUIRE) == 0)
            arch_cpu_relax();
        __atomic_store_n(&flag, 2, __ATOMIC_RELEASE);
    }, nullptr));
    t_delay_us(1000);
    __atomic_store_n(&flag, 1, __ATOMIC_RELEASE);
    EXPECT_TRUE(smp_wait(other));
    EXPECT_EQ(flag, 2);
}
