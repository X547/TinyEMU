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
#include "smp.h"

#include "arch.h"
#include "heap.h"
#include "imsic.h"
#include "lapic.h"
#include "test.h"

#define SMP_MAX 16
#define STACK_SIZE 0x4000

namespace {

enum {
    CPU_OFF,
    CPU_IDLE,
    CPU_DEAD,
};

struct Cpu {
    int state;
    void (*fn)(void *);
    void *ctx;
    uint32_t posted;
    uint32_t done;
};

Cpu sCpus[SMP_MAX];

template <typename T> T load(const T &v)
{
    return __atomic_load_n(&v, __ATOMIC_ACQUIRE);
}

template <typename T> void store(T &v, T x)
{
    __atomic_store_n(&v, x, __ATOMIC_RELEASE);
}

/* Where a started processor lives. */
[[noreturn]] void ap_loop()
{
    arch_ap_init();
    Cpu &me = sCpus[smp_current()];
    store(me.state, (int)CPU_IDLE);
    for (;;) {
        uint32_t posted = load(me.posted);
        if (posted != me.done) {
            me.fn(me.ctx);
            store(me.done, posted);
        } else {
            arch_cpu_relax();
        }
    }
}

bool start(int index)
{
    Cpu &c = sCpus[index];
    if (load(c.state) == CPU_IDLE)
        return true;
    if (load(c.state) == CPU_DEAD)
        return false;
    uint8_t *stack = (uint8_t *)boot_alloc(STACK_SIZE, 16);
    if (!arch_cpu_start(index, ap_loop, (uintptr_t)(stack + STACK_SIZE))) {
        store(c.state, (int)CPU_DEAD);
        return false;
    }
    uint64_t end = t_now_us() + 1000000;
    while (load(c.state) != CPU_IDLE && t_now_us() < end)
        arch_cpu_relax();
    if (load(c.state) != CPU_IDLE) {
        store(c.state, (int)CPU_DEAD);
        return false;
    }
    return true;
}

/* Each processor's own interrupt state, as the boot one resets its own. */
void ap_quiesce(void *)
{
    arch_irq_reset();
#if defined(__i386__) || defined(__x86_64__)
    if (Lapic *l = Lapic::Get())
        l->Reset();
#endif
#ifdef __riscv
    if (Imsic *f = Imsic::Mine())
        f->Reset();
#endif
}

}


int smp_cpu_count()
{
    return min(arch_cpu_count(), SMP_MAX);
}


int smp_boot_index()
{
    for (int i = 0; i < smp_cpu_count(); i++) {
        if (arch_cpu_hw_id(i) == gBoot.boot_cpu)
            return i;
    }
    return 0;
}


int smp_current()
{
    uint32_t id = arch_this_cpu_hw_id();
    for (int i = 0; i < smp_cpu_count(); i++) {
        if (arch_cpu_hw_id(i) == id)
            return i;
    }
    return 0;
}


bool smp_started(int index)
{
    return load(sCpus[index].state) == CPU_IDLE;
}


bool smp_post(int index, void (*fn)(void *), void *ctx)
{
    if (index < 0 || index >= smp_cpu_count() || index == smp_boot_index() ||
        !start(index))
        return false;
    Cpu &c = sCpus[index];
    if (load(c.done) != c.posted)
        return false; /* still busy */
    c.fn = fn;
    c.ctx = ctx;
    store(c.posted, c.posted + 1);
    return true;
}


bool smp_wait(int index, uint64_t timeout_us)
{
    Cpu &c = sCpus[index];
    uint64_t end = t_now_us() + timeout_us;
    while (load(c.done) != c.posted) {
        if (t_now_us() >= end || load(c.state) == CPU_DEAD)
            return false;
        arch_cpu_relax();
    }
    return true;
}


bool smp_run(int index, void (*fn)(void *), void *ctx, uint64_t timeout_us)
{
    return smp_post(index, fn, ctx) && smp_wait(index, timeout_us);
}


void smp_quiesce()
{
    for (int i = 0; i < smp_cpu_count(); i++) {
        if (i == smp_boot_index() || !smp_started(i))
            continue;
        /* one still running the test's code is left to it */
        if (!smp_wait(i, 100000))
            continue;
        smp_run(i, ap_quiesce, nullptr, 100000);
    }
}


void smp_ap_trapped()
{
    store(sCpus[smp_current()].state, (int)CPU_DEAD);
    arch_halt();
}


int t_require_other_cpu()
{
    for (int i = 0; i < smp_cpu_count(); i++) {
        if (i != smp_boot_index() && start(i))
            return i;
    }
    t_skip(smp_cpu_count() > 1 ? "no other processor would start"
                               : "one processor");
}
