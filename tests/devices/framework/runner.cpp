/*
 * Running tests
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
#include "test.h"

#include "arch.h"
#include "console.h"
#include "format.h"
#include "heap.h"
#include "intc.h"
#include "power.h"
#include "smp.h"
#include "testctl.h"

extern "C" TestCase __devtests_start[], __devtests_end[];

/* The protocol, one line each, "@@T " first:

     BEGIN <name>
     FAIL <file>:<line> <message>
     LOG <message>
     SNAP <key> <value>
     CRASH <what>
     INPUT <hex bytes>
     EXIT <status>
     END <name> PASS|FAIL|SKIP|CRASH|RERUN <microseconds> [<reason>]
     DONE pass=N fail=N skip=N crash=N

   with "TEST <name>" lines instead when only listing. The runner treats a
   BEGIN without its END as a test that killed the machine, unless the test
   said with EXIT that it would and temu exited with that status, and
   RERUN as a test that needs a machine of its own. INPUT asks the runner
   to write the bytes to temu's standard input.

   A test that resets the machine goes on in the same run: HELLO comes
   again, then the rest of that test, with no BEGIN of its own, and the
   tests after it. */

namespace {

enum Outcome {
    OUTCOME_RUN = 0,
    OUTCOME_ABORT,
    OUTCOME_SKIP,
    OUTCOME_CRASH,
};

struct Options {
    const char *tests = nullptr;
    size_t tests_len = 0;
    const char *skip = nullptr;
    size_t skip_len = 0;
    /* the tests a machine is for, whatever 'tests' asks for */
    const char *only = nullptr;
    size_t only_len = 0;
    const char *after = nullptr;
    size_t after_len = 0;
    bool list = false;
    bool meta = false;
};

JmpBuf sTestJmp;
/* set while t_trap() expects one */
JmpBuf *sTrapJmp;
TrapInfo sTrapInfo;
const TestCase *sCurrent;
bool sInTrap;
int sFailures;
uint64_t sTestStart;
char sSkipReason[128];
/* The current test's place in the run, and whether the machine was reset
   in it. Test-control's KEEP holds RESUME_MAGIC with the place plus one
   across the reset. */
int sCurrentIndex;
bool sAfterReset;
#define RESUME_MAGIC 0x52000000u
#define RESUME_MASK  0xff000000u

/* The value of 'key=' in the command line: start and length. */
bool option(const char *cmdline, const char *key, const char **val,
            size_t *len)
{
    size_t klen = strlen(key);
    for (const char *p = cmdline; p && *p;) {
        while (*p == ' ')
            p++;
        const char *end = p;
        while (*end && *end != ' ')
            end++;
        if ((size_t)(end - p) > klen && strncmp(p, key, klen) == 0 &&
            p[klen] == '=') {
            *val = p + klen + 1;
            *len = end - *val;
            return true;
        }
        p = end;
    }
    return false;
}

/* Whether 'name' matches one of the comma separated patterns. */
bool matches_any(const char *patterns, size_t len, const char *name)
{
    char pat[96];
    for (size_t i = 0; i < len;) {
        size_t j = i;
        while (j < len && patterns[j] != ',')
            j++;
        size_t n = min(j - i, sizeof(pat) - 1);
        memcpy(pat, patterns + i, n);
        pat[n] = '\0';
        if (n && glob_match(pat, name))
            return true;
        i = j + 1;
    }
    return false;
}

bool selected(const TestCase *tc, const Options &opt)
{
    if ((tc->flags & TEST_META) && !opt.meta)
        return false;
    if (opt.tests && !matches_any(opt.tests, opt.tests_len, tc->name))
        return false;
    if (opt.skip && matches_any(opt.skip, opt.skip_len, tc->name))
        return false;
    if (opt.only && !matches_any(opt.only, opt.only_len, tc->name))
        return false;
    if (opt.after) {
        /* strictly after, in the order the tests run */
        size_t n = strlen(tc->name);
        int c = strncmp(tc->name, opt.after, min(n, opt.after_len));
        if (c < 0 || (c == 0 && n <= opt.after_len))
            return false;
    }
    return true;
}

uint64_t now_us()
{
    uint64_t hz = arch_tick_hz();
    uint64_t t = arch_ticks();
    return hz ? t / hz * 1000000 + t % hz * 1000000 / hz : 0;
}

const char *basename(const char *path)
{
    const char *b = path;
    for (const char *p = path; *p; p++) {
        if (*p == '/')
            b = p + 1;
    }
    return b;
}

}


void t_fail(const char *file, int line, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    sFailures++;
    report("FAIL %s:%d %s", basename(file), line, msg);
}


int t_failures()
{
    return sFailures;
}


void t_abort()
{
    if (sFailures == 0)
        sFailures = 1;
    arch_longjmp(&sTestJmp, OUTCOME_ABORT);
}


void t_skip(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(sSkipReason, sizeof(sSkipReason), fmt, ap);
    va_end(ap);
    arch_longjmp(&sTestJmp, OUTCOME_SKIP);
}


void t_log(const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    report("LOG %s", msg);
}


void t_snap(const char *key, const char *fmt, ...)
{
    char msg[256];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(msg, sizeof(msg), fmt, ap);
    va_end(ap);
    report("SNAP %s %s", key, msg);
}


void t_host_input(const void *buf, size_t len)
{
    static const char digits[] = "0123456789abcdef";
    char hex[2 * 96 + 1];
    const uint8_t *p = (const uint8_t *)buf;
    while (len > 0) {
        size_t n = min(len, sizeof(hex) / 2);
        for (size_t i = 0; i < n; i++) {
            hex[2 * i] = digits[p[i] >> 4];
            hex[2 * i + 1] = digits[p[i] & 15];
        }
        hex[2 * n] = '\0';
        report("INPUT %s", hex);
        p += n;
        len -= n;
    }
}


void t_expect_exit(int status)
{
    report("EXIT %d", status);
}


void t_reboot(void (*reset)())
{
    if (!tc_present())
        t_skip("no test-control device to go on with");
    /* the test starts over with no failures counted */
    if (sFailures)
        t_abort();
    tc_set_keep(RESUME_MAGIC | (sCurrentIndex + 1));
    if (reset != nullptr)
        reset();
    else
        tc_request_reset();
    /* the processors stop soon after the request */
    t_delay_us(1000000);
    tc_set_keep(0);
    t_fail(__FILE__, __LINE__, "the machine was not reset");
    t_abort();
}


bool t_after_reset()
{
    return sAfterReset;
}


const HwDevice *t_require_device(const char *compat)
{
    const HwDevice *d = registry_find(compat);
    if (d == nullptr)
        t_skip("no %s", compat);
    return d;
}


void t_require_tc_irqs(int count)
{
    int n = tc_irq_count();
    if (n < count)
        t_skip("needs %d test-control interrupt lines, has %d", count, n);
}


void t_require_mode(const char *mode)
{
    if (strcmp(arch_mode(), mode) != 0)
        t_skip("only in %s mode", mode);
}


uint64_t t_elapsed_us()
{
    return now_us() - sTestStart;
}


uint64_t t_now_us()
{
    return now_us();
}


void t_delay_us(uint64_t us)
{
    uint64_t end = now_us() + us;
    while (now_us() < end)
        arch_cpu_relax();
}


static void (*sCleanups[8])();
static int sCleanupCount;

void t_on_cleanup(void (*fn)())
{
    for (int i = 0; i < sCleanupCount; i++) {
        if (sCleanups[i] == fn)
            return;
    }
    if (sCleanupCount == (int)ARRAY_SIZE(sCleanups))
        panic("more than %d cleanups", (int)ARRAY_SIZE(sCleanups));
    sCleanups[sCleanupCount++] = fn;
}

/* What a test may have left behind that would reach the next one. */
static void quiesce()
{
    sTrapJmp = nullptr;
    smp_quiesce();
    arch_irq_reset();
    /* the test's own first, while its interrupts are off */
    for (int i = sCleanupCount - 1; i >= 0; i--)
        sCleanups[i]();
    sCleanupCount = 0;
    tc_irq_set_levels(0);
    tc_block_reset_all();
    intc_reset_all();
}


bool t_trap(void (*fn)(void *), void *ctx, TrapInfo *info)
{
    JmpBuf jb;
    JmpBuf *outer = sTrapJmp;
    sTrapJmp = &jb;
    if (arch_setjmp(&jb) == 0) {
        fn(ctx);
        sTrapJmp = outer;
        return false;
    }
    sTrapJmp = outer;
    if (info)
        *info = sTrapInfo;
    return true;
}


void trap_unexpected(const TrapInfo &info)
{
    if (smp_current() != smp_boot_index()) {
        report("CRASH cpu %d: %s (cause %#llx) at pc %#llx addr %#llx",
               smp_current(), info.name, T_FMT_VAL(info.cause),
               T_FMT_VAL(info.pc), T_FMT_VAL(info.addr));
        smp_ap_trapped();
    }
    if (sTrapJmp != nullptr) {
        JmpBuf *jb = sTrapJmp;
        sTrapJmp = nullptr;
        sTrapInfo = info;
        arch_longjmp(jb, 1);
    }
    if (sCurrent == nullptr || sInTrap)
        return;
    sInTrap = true;
    report("CRASH %s (cause %#llx) at pc %#llx addr %#llx error %#llx",
           info.name, T_FMT_VAL(info.cause), T_FMT_VAL(info.pc),
           T_FMT_VAL(info.addr), T_FMT_VAL(info.error));
    arch_longjmp(&sTestJmp, OUTCOME_CRASH);
}


void panic(const char *fmt, ...)
{
    static bool sPanicking;
    if (!sPanicking && console_ready()) {
        sPanicking = true;
        char msg[256];
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(msg, sizeof(msg), fmt, ap);
        va_end(ap);
        report("PANIC %s", msg);
        power_off(3);
    }
    arch_halt();
}


void run_tests(const char *cmdline)
{
    Options opt;
    const char *v;
    size_t len;
    if (option(cmdline, "tests", &opt.tests, &opt.tests_len) &&
        opt.tests_len == 0)
        opt.tests = nullptr;
    if (option(cmdline, "skip", &opt.skip, &opt.skip_len) &&
        opt.skip_len == 0)
        opt.skip = nullptr;
    if (option(cmdline, "only", &opt.only, &opt.only_len) &&
        opt.only_len == 0)
        opt.only = nullptr;
    option(cmdline, "after", &opt.after, &opt.after_len);
    opt.list = option(cmdline, "list", &v, &len) && len && *v == '1';
    opt.meta = option(cmdline, "meta", &v, &len) && len && *v == '1';

    /* name order, so that "after" means the same on every run */
    int count = __devtests_end - __devtests_start;
    TestCase **order = (TestCase **)heap_alloc(count * sizeof(TestCase *));
    if (count && order == nullptr)
        panic("no memory for the test list");
    for (int i = 0; i < count; i++) {
        TestCase *tc = &__devtests_start[i];
        int j = i;
        while (j > 0 && strcmp(order[j - 1]->name, tc->name) > 0) {
            order[j] = order[j - 1];
            j--;
        }
        order[j] = tc;
    }

    /* After a test reset the machine, the run goes on in that test; the
       counts start over, the runner having the earlier results. */
    int resume = -1;
    uint32_t keep = tc_keep();
    if ((keep & RESUME_MASK) == RESUME_MAGIC) {
        resume = (int)(keep & ~RESUME_MASK) - 1;
        tc_set_keep(0);
    }

    int pass = 0, fail = 0, skip = 0, crash = 0;
    bool touched = false;
    for (int i = 0; i < count; i++) {
        const TestCase *tc = order[i];
        if (i < resume || !selected(tc, opt))
            continue;
        if (opt.list) {
            report("TEST %s", tc->name);
            continue;
        }

        sAfterReset = i == resume;
        if (sAfterReset) {
            t_log("after reset %u", (unsigned)tc_resets());
        } else {
            report("BEGIN %s", tc->name);
            if ((tc->flags & TEST_FRESH) && touched) {
                report("END %s RERUN 0", tc->name);
                continue;
            }
        }
        touched = true;
        HeapMark mark = heap_mark();
        sFailures = 0;
        sSkipReason[0] = '\0';
        sInTrap = false;
        sTestStart = now_us();
        sCurrent = tc;
        sCurrentIndex = i;
        Outcome outcome = (Outcome)arch_setjmp(&sTestJmp);
        if (outcome == OUTCOME_RUN) {
            tc->fn();
        }
        sCurrent = nullptr;
        uint64_t us = now_us() - sTestStart;
        quiesce();
        heap_release(mark);

        if (outcome == OUTCOME_CRASH) {
            crash++;
            report("END %s CRASH %llu", tc->name, T_FMT_VAL(us));
        } else if (outcome == OUTCOME_SKIP) {
            skip++;
            report("END %s SKIP %llu %s", tc->name, T_FMT_VAL(us),
                   sSkipReason);
        } else if (sFailures) {
            fail++;
            report("END %s FAIL %llu", tc->name, T_FMT_VAL(us));
        } else {
            pass++;
            report("END %s PASS %llu", tc->name, T_FMT_VAL(us));
        }
    }

    report("DONE pass=%d fail=%d skip=%d crash=%d", pass, fail, skip, crash);
    power_off(fail || crash ? 1 : 0);
}
