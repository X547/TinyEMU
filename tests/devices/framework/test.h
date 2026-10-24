/*
 * Writing tests
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

#include "arch.h"
#include "klib.h"
#include "registry.h"

/* A test is a function registered under "suite.name":

       TEST(uart, reset_values)
       {
           const HwDevice *uart = t_require_device("ns16550a");
           EXPECT_EQ(read_lsr(uart), 0x60);
           SNAP("lsr", "%#x", read_lsr(uart));
       }

   Tests run in name order, each with interrupts off and the heap at the
   same mark; what a test allocates is released after it. A test that
   traps is reported as crashed and the next one runs. */

#define TEST_META  0x1 /* a test of the framework itself, run on request */
#define TEST_FRESH 0x2 /* needs a machine no other test has touched */

struct TestCase {
    const char *name;
    void (*fn)();
    uint32_t flags;
    const char *file;
    int line;
};

#define TEST_DEFINE(suite, name, flags)                                    \
    static void test_##suite##_##name();                                   \
    __attribute__((used, section("devtests"), aligned(sizeof(void *))))    \
    static TestCase tc_##suite##_##name = {                                \
        #suite "." #name, test_##suite##_##name, flags, __FILE__, __LINE__ \
    };                                                                     \
    static void test_##suite##_##name()

#define TEST(suite, name) TEST_DEFINE(suite, name, 0)
#define META_TEST(suite, name) TEST_DEFINE(suite, name, TEST_META)
/* For reset values: run.py gives such a test a machine of its own when
   others ran before it. */
#define FRESH_TEST(suite, name) TEST_DEFINE(suite, name, TEST_FRESH)

/* A failure: reported, and the test goes on. */
void t_fail(const char *file, int line, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
/* End the test now, as failed if anything failed. */
[[noreturn]] void t_abort();
[[noreturn]] void t_skip(const char *fmt, ...)
    __attribute__((format(printf, 1, 2)));
void t_log(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
/* A characterization value: compared by the runner with the golden copy
   it keeps for this test. */
void t_snap(const char *key, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* The first device that is 'compat', or the test is skipped. */
const HwDevice *t_require_device(const char *compat);

/* Have 'fn' run once the test is over, however it ended: to put back
   what the runner does not reset by itself. */
void t_on_cleanup(void (*fn)());

/* Run 'fn' and report whether it trapped, and how in 'info'; a trap there
   is expected rather than a crash. */
bool t_trap(void (*fn)(void *), void *ctx = nullptr, TrapInfo *info = nullptr);

/* At least 'count' test-control interrupt lines, or the test is skipped. */
void t_require_tc_irqs(int count);
/* The privilege the image runs at ("M", "S", "pvh"), or the test is
   skipped. */
void t_require_mode(const char *mode);

/* Microseconds since the test began. */
uint64_t t_elapsed_us();
uint64_t t_now_us();
void t_delay_us(uint64_t us);

/* Poll until 'cond' holds; false after 'timeout_us'. */
template <typename F>
bool t_wait_until(F cond, uint64_t timeout_us = 100000)
{
    uint64_t end = t_now_us() + timeout_us;
    for (;;) {
        if (cond())
            return true;
        if (t_now_us() >= end)
            return cond();
    }
}

#define T_FMT_VAL(v) ((unsigned long long)(v))

#define EXPECT_TRUE(c)                                                   \
    do {                                                                 \
        if (!(c))                                                        \
            t_fail(__FILE__, __LINE__, "expected %s", #c);               \
    } while (0)

#define EXPECT_FALSE(c)                                                  \
    do {                                                                 \
        if (c)                                                           \
            t_fail(__FILE__, __LINE__, "expected !(%s)", #c);            \
    } while (0)

#define T_EXPECT_OP(a, b, op)                                            \
    do {                                                                 \
        auto t_a_ = (a);                                                 \
        auto t_b_ = (b);                                                 \
        if (!(t_a_ op t_b_))                                             \
            t_fail(__FILE__, __LINE__, "expected %s " #op " %s: %#llx, %#llx", \
                   #a, #b, T_FMT_VAL(t_a_), T_FMT_VAL(t_b_));            \
    } while (0)

#define EXPECT_EQ(a, b) T_EXPECT_OP(a, b, ==)
#define EXPECT_NE(a, b) T_EXPECT_OP(a, b, !=)
#define EXPECT_LT(a, b) T_EXPECT_OP(a, b, <)
#define EXPECT_LE(a, b) T_EXPECT_OP(a, b, <=)
#define EXPECT_GT(a, b) T_EXPECT_OP(a, b, >)
#define EXPECT_GE(a, b) T_EXPECT_OP(a, b, >=)

/* The same, ending the test on failure. */
int t_failures();
#define T_ASSERT(expect)                                                 \
    do {                                                                 \
        int t_before_ = t_failures();                                    \
        expect;                                                          \
        if (t_failures() != t_before_)                                   \
            t_abort();                                                   \
    } while (0)

#define ASSERT_TRUE(c) T_ASSERT(EXPECT_TRUE(c))
#define ASSERT_FALSE(c) T_ASSERT(EXPECT_FALSE(c))
#define ASSERT_EQ(a, b) T_ASSERT(EXPECT_EQ(a, b))
#define ASSERT_NE(a, b) T_ASSERT(EXPECT_NE(a, b))
#define ASSERT_LT(a, b) T_ASSERT(EXPECT_LT(a, b))
#define ASSERT_LE(a, b) T_ASSERT(EXPECT_LE(a, b))
#define ASSERT_GT(a, b) T_ASSERT(EXPECT_GT(a, b))
#define ASSERT_GE(a, b) T_ASSERT(EXPECT_GE(a, b))

#define SNAP(key, ...) t_snap(key, __VA_ARGS__)

/* Run what the command line selects, report, and power off. */
[[noreturn]] void run_tests(const char *cmdline);
