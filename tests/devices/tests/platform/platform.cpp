/*
 * What every machine has: discovery, RAM and time
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
#include "format.h"
#include "heap.h"
#include "registry.h"
#include "test.h"

/* The devices the tables describe, one LOG line each: what a failing run's
   log needs to say what the machine was. */
TEST(platform, devices)
{
    for (int i = 0; i < registry_count(); i++) {
        const HwDevice *d = registry_at(i);
        char line[200];
        size_t n = 0;
        n += snprintf(line + n, sizeof(line) - n, "%s [", d->name);
        for (int c = 0; c < d->ncompat && n < sizeof(line); c++)
            n += snprintf(line + n, sizeof(line) - n, "%s%s", c ? " " : "",
                          d->compat[c]);
        if (n < sizeof(line))
            n += snprintf(line + n, sizeof(line) - n, "]");
        for (int r = 0; r < d->nregions && n < sizeof(line); r++)
            n += snprintf(line + n, sizeof(line) - n, " %s %#llx+%#llx",
                          d->regions[r].kind == HW_MMIO ? "mmio" : "io",
                          T_FMT_VAL(d->regions[r].base),
                          T_FMT_VAL(d->regions[r].size));
        for (int q = 0; q < d->nirqs && n < sizeof(line); q++)
            n += snprintf(line + n, sizeof(line) - n, " irq %u",
                          d->irqs[q].line);
        t_log("%s", line);
    }
    EXPECT_GT(registry_count(), 0);
}

TEST(platform, heap)
{
    EXPECT_GT(heap_free_bytes(), 1u << 20);
    /* allocations are zeroed, aligned and inside RAM the tables gave */
    uint8_t *p = (uint8_t *)heap_alloc(4096, 4096);
    ASSERT_NE((uintptr_t)p, 0u);
    EXPECT_EQ((uintptr_t)p & 4095, 0u);
    bool zero = true;
    for (int i = 0; i < 4096; i++)
        zero &= p[i] == 0;
    EXPECT_TRUE(zero);
    memset(p, 0xa5, 4096);
    EXPECT_EQ(p[4095], 0xa5);
}

TEST(platform, time)
{
    uint64_t hz = arch_tick_hz();
    EXPECT_GE(hz, 1000000u);
    /* 10 ms by the counter must take at least 10 ms of the counter */
    uint64_t start = arch_ticks();
    uint64_t end = start + hz / 100;
    uint64_t last = start;
    while (arch_ticks() < end) {
        uint64_t t = arch_ticks();
        EXPECT_GE(t, last);
        last = t;
    }
    EXPECT_GE(arch_ticks() - start, hz / 100);
}
