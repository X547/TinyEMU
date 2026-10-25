/*
 * Tests of the framework itself
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
#include "heap.h"
#include "power.h"
#include "test.h"

/* Each ends a way run.py --selftest expects; they run only with meta=1.
   The names sort in the order the runner has to meet them. */

META_TEST(framework, a_pass)
{
    EXPECT_EQ(1 + 1, 2);
    SNAP("value", "%#x", 0x1234);
}

META_TEST(framework, b_fail_expect)
{
    /* both are reported: an EXPECT does not end the test */
    EXPECT_EQ(1, 2);
    EXPECT_TRUE(false);
}

META_TEST(framework, c_fail_assert)
{
    ASSERT_EQ(1, 2);
    EXPECT_TRUE(false); /* never reached, so one failure only */
}

META_TEST(framework, d_skip)
{
    t_skip("on purpose");
}

META_TEST(framework, e_crash)
{
    /* recovered in the guest: the next test runs in the same machine */
    arch_illegal_instruction();
}

META_TEST(framework, f_heap_release)
{
    /* the crash above left nothing allocated */
    uint64_t before = heap_free_bytes();
    void *p = heap_alloc(1 << 20);
    EXPECT_NE((uintptr_t)p, 0u);
    EXPECT_EQ(heap_free_bytes() <= before - (1 << 20), true);
}

META_TEST(framework, g_hang)
{
    /* the runner's timeout ends this one and starts over after it */
    for (;;)
        arch_cpu_relax();
}

META_TEST(framework, h_exit_status)
{
    /* said so beforehand: a pass when the status is the one named */
    t_expect_exit(5);
    power_off(5);
}

META_TEST(framework, h_power_off)
{
    /* the machine stops mid-test: a crash, then a restart after it */
    power_off(7);
}

META_TEST(framework, i_last)
{
    t_log("reached");
}
