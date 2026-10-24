/*
 * CLINT tests
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
#ifdef __riscv

#include "intc.h"
#include "mmio.h"
#include "riscv.h"
#include "smp.h"
#include "test.h"

/* The core local interruptor: a software interrupt and a timer per hart,
   which reach machine mode only, so the tests run in M-mode. */

#define CLINT_MSIP(i)     (4 * (i))
#define CLINT_MTIMECMP(i) (0x4000 + 8 * (i))
#define CLINT_MTIME       0xbff8

namespace {

struct Clint {
    uint64_t base;
    int index; /* the boot hart's slot */
    int harts;
};

Clint need_clint()
{
    t_require_mode("M");
    const HwDevice *d = t_require_device("riscv,clint0");
    Clint c {d->regions[0].base, -1, 0};
    /* a software and a timer interrupt per hart, in slot order */
    const FdtProp *ext = fdt_prop(d->fdt, "interrupts-extended");
    uint32_t intc = hart_intc_phandle(gBoot.boot_cpu);
    for (uint32_t off = 0; ext && off + 16 <= ext->len; off += 16) {
        if (be32(ext->data + off) == intc)
            c.index = c.harts;
        c.harts++;
    }
    if (c.index < 0)
        t_skip("the CLINT does not reach this hart");
    return c;
}

uint64_t mtime(const Clint &c)
{
    for (;;) {
        uint32_t hi = mmio_read32(c.base + CLINT_MTIME + 4);
        uint32_t lo = mmio_read32(c.base + CLINT_MTIME);
        if (mmio_read32(c.base + CLINT_MTIME + 4) == hi)
            return (uint64_t)hi << 32 | lo;
    }
}

uint64_t mtimecmp(const Clint &c)
{
    return mmio_read32(c.base + CLINT_MTIMECMP(c.index)) |
           (uint64_t)mmio_read32(c.base + CLINT_MTIMECMP(c.index) + 4) << 32;
}

void set_mtimecmp(const Clint &c, uint64_t v)
{
    /* high word first, through a value that cannot fire on the way */
    mmio_write32(c.base + CLINT_MTIMECMP(c.index) + 4, 0xffffffff);
    mmio_write32(c.base + CLINT_MTIMECMP(c.index), (uint32_t)v);
    mmio_write32(c.base + CLINT_MTIMECMP(c.index) + 4, v >> 32);
}

bool mip(int bit)
{
    return (CSR_READ(mip) >> bit) & 1;
}

uint64_t us_to_ticks(uint64_t us)
{
    return us * arch_tick_hz() / 1000000;
}

Clint sClint;
volatile int sCount;

}


FRESH_TEST(clint, reset_state)
{
    Clint c = need_clint();
    SNAP("msip", "%#x", mmio_read32(c.base + CLINT_MSIP(c.index)));
    SNAP("mtimecmp", "%#llx", T_FMT_VAL(mtimecmp(c)));
    /* temu raises MTIP when its timer loop next runs, which a read right
       after reset may come before: only what it settles to is recorded */
    t_log("mip_mtip at first read: %d", mip(IRQ_M_TIMER));
    t_wait_until([] { return mip(IRQ_M_TIMER); }, 10000);
    SNAP("mip_mtip", "%d", mip(IRQ_M_TIMER));
    SNAP("mip_msip", "%d", mip(IRQ_M_SOFT));
    SNAP("harts", "%d", c.harts);
}

TEST(clint, mtime_is_time)
{
    Clint c = need_clint();
    uint64_t before = arch_ticks();
    uint64_t t = mtime(c);
    uint64_t after = arch_ticks();
    EXPECT_LE(before, t);
    EXPECT_LE(t, after);
}

/* mtime is read-write by the privileged specification, and the time CSR
   shadows it. */
TEST(clint, mtime_write)
{
    Clint c = need_clint();
    const uint64_t jump = 1ull << 40;
    uint64_t slack = us_to_ticks(100000);
    uint64_t before = mtime(c);
    /* the high word first, so that the count never goes back */
    mmio_write32(c.base + CLINT_MTIME + 4, (before + jump) >> 32);
    mmio_write32(c.base + CLINT_MTIME, (uint32_t)(before + jump));
    uint64_t after = mtime(c);
    uint64_t csr = arch_ticks();
    /* put it back where it would have been */
    uint64_t now = mtime(c);
    mmio_write32(c.base + CLINT_MTIME, (uint32_t)(now - jump));
    mmio_write32(c.base + CLINT_MTIME + 4, (now - jump) >> 32);
    EXPECT_GE(after, before + jump);
    EXPECT_LT(after, before + jump + slack);
    EXPECT_GE(csr, after);
    EXPECT_LT(mtime(c), now);
}

TEST(clint, mtimecmp_readback)
{
    Clint c = need_clint();
    set_mtimecmp(c, 0x9abcdef012345678ull);
    EXPECT_EQ(mtimecmp(c), 0x9abcdef012345678ull);
    set_mtimecmp(c, UINT64_MAX);
    EXPECT_EQ(mtimecmp(c), UINT64_MAX);
}

TEST(clint, timer_pending)
{
    Clint c = need_clint();
    set_mtimecmp(c, UINT64_MAX);
    EXPECT_FALSE(mip(IRQ_M_TIMER));
    uint64_t deadline = mtime(c) + us_to_ticks(5000);
    set_mtimecmp(c, deadline);
    EXPECT_FALSE(mip(IRQ_M_TIMER));
    EXPECT_TRUE(t_wait_until([] { return mip(IRQ_M_TIMER); }, 100000));
    /* not before its time */
    EXPECT_GE(mtime(c), deadline);
    /* a compare value in the future takes it back */
    set_mtimecmp(c, UINT64_MAX);
    EXPECT_FALSE(mip(IRQ_M_TIMER));
    /* and one in the past raises it, when temu's timer loop next runs
       rather than at once */
    set_mtimecmp(c, 0);
    EXPECT_TRUE(t_wait_until([] { return mip(IRQ_M_TIMER); }, 100000));
    set_mtimecmp(c, UINT64_MAX);
}

TEST(clint, timer_interrupt)
{
    sClint = need_clint();
    sCount = 0;
    set_mtimecmp(sClint, UINT64_MAX);
    arch_set_irq_handler(IRQ_M_TIMER, [](void *) {
        sCount = sCount + 1;
        set_mtimecmp(sClint, UINT64_MAX);
    }, nullptr);
    CSR_SET(mie, 1ul << IRQ_M_TIMER);
    set_mtimecmp(sClint, mtime(sClint) + us_to_ticks(1000));
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }, 100000));
    t_delay_us(2000);
    arch_irq_disable();
    EXPECT_EQ(sCount, 1);
}

TEST(clint, msip)
{
    Clint c = need_clint();
    uint64_t msip = c.base + CLINT_MSIP(c.index);
    mmio_write32(msip, 1);
    EXPECT_EQ(mmio_read32(msip), 1u);
    EXPECT_TRUE(mip(IRQ_M_SOFT));
    mmio_write32(msip, 0);
    EXPECT_EQ(mmio_read32(msip), 0u);
    EXPECT_FALSE(mip(IRQ_M_SOFT));
    /* only bit 0 is there */
    mmio_write32(msip, 0xfffffffe);
    SNAP("upper_bits", "%#x", mmio_read32(msip));
    mmio_write32(msip, 3);
    SNAP("bit0_and_1", "%#x", mmio_read32(msip));
    mmio_write32(msip, 0);
}

TEST(clint, software_interrupt)
{
    sClint = need_clint();
    sCount = 0;
    arch_set_irq_handler(IRQ_M_SOFT, [](void *) {
        sCount = sCount + 1;
        mmio_write32(sClint.base + CLINT_MSIP(sClint.index), 0);
    }, nullptr);
    CSR_SET(mie, 1ul << IRQ_M_SOFT);
    arch_irq_enable();
    mmio_write32(sClint.base + CLINT_MSIP(sClint.index), 1);
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }, 100000));
    arch_irq_disable();
    EXPECT_EQ(sCount, 1);
}

namespace {

volatile int sApReady;
uint64_t sOtherMsip;

/* the slot a hart has in the CLINT */
int slot_of(const Clint &c, uint32_t hart)
{
    const HwDevice *d = registry_find("riscv,clint0");
    const FdtProp *ext = fdt_prop(d->fdt, "interrupts-extended");
    uint32_t intc = hart_intc_phandle(hart);
    for (uint32_t i = 0; ext && 16 * i + 16 <= ext->len; i++) {
        if (be32(ext->data + 16 * i) == intc)
            return i;
    }
    (void)c;
    return -1;
}

/* On the other hart: one interrupt of 'bit', or a while */
void ap_wait_irq(void *bit)
{
    CSR_SET(mie, 1ul << (uintptr_t)bit);
    __atomic_store_n(&sApReady, 1, __ATOMIC_RELEASE);
    arch_irq_enable();
    uint64_t end = t_now_us() + 200000;
    while (sCount == 0 && t_now_us() < end)
        arch_cpu_relax();
    arch_irq_disable();
    CSR_CLEAR(mie, 1ul << (uintptr_t)bit);
}

}

TEST(clint, software_interrupt_to_other_hart)
{
    Clint c = need_clint();
    int other = t_require_other_cpu();
    int slot = slot_of(c, arch_cpu_hw_id(other));
    ASSERT_GE(slot, 0);
    sOtherMsip = c.base + CLINT_MSIP(slot);
    sCount = 0;
    sApReady = 0;
    arch_set_irq_handler(IRQ_M_SOFT, [](void *) {
        sCount = sCount + 1;
        mmio_write32(sOtherMsip, 0);
    }, nullptr);
    ASSERT_TRUE(smp_post(other, ap_wait_irq, (void *)(uintptr_t)IRQ_M_SOFT));
    t_wait_until([] { return __atomic_load_n(&sApReady, __ATOMIC_ACQUIRE); });
    mmio_write32(sOtherMsip, 1);
    /* this hart's own is untouched */
    EXPECT_FALSE(mip(IRQ_M_SOFT));
    EXPECT_TRUE(smp_wait(other));
    EXPECT_EQ(sCount, 1);
}

TEST(clint, timer_on_other_hart)
{
    sClint = need_clint();
    int other = t_require_other_cpu();
    int slot = slot_of(sClint, arch_cpu_hw_id(other));
    ASSERT_GE(slot, 0);
    static Clint theirs;
    theirs = sClint;
    theirs.index = slot;
    sCount = 0;
    sApReady = 0;
    set_mtimecmp(theirs, UINT64_MAX);
    arch_set_irq_handler(IRQ_M_TIMER, [](void *) {
        sCount = sCount + 1;
        set_mtimecmp(theirs, UINT64_MAX);
    }, nullptr);
    ASSERT_TRUE(smp_post(other, ap_wait_irq, (void *)(uintptr_t)IRQ_M_TIMER));
    t_wait_until([] { return __atomic_load_n(&sApReady, __ATOMIC_ACQUIRE); });
    set_mtimecmp(theirs, mtime(theirs) + us_to_ticks(1000));
    EXPECT_TRUE(smp_wait(other));
    EXPECT_EQ(sCount, 1);
    set_mtimecmp(sClint, UINT64_MAX);
    EXPECT_FALSE(mip(IRQ_M_TIMER));
}

TEST(clint, past_last_hart)
{
    Clint c = need_clint();
    mmio_write32(c.base + CLINT_MSIP(c.harts), 1);
    SNAP("msip", "%#x", mmio_read32(c.base + CLINT_MSIP(c.harts)));
    mmio_write32(c.base + CLINT_MTIMECMP(c.harts), 0x1234);
    SNAP("mtimecmp", "%#x", mmio_read32(c.base + CLINT_MTIMECMP(c.harts)));
    EXPECT_FALSE(mip(IRQ_M_SOFT));
}

TEST(clint, wide_access)
{
#if __riscv_xlen == 64
    Clint c = need_clint();
    volatile uint64_t *cmp = (volatile uint64_t *)(uintptr_t)
        (c.base + CLINT_MTIMECMP(c.index));
    *cmp = 0x1122334455667788ull;
    EXPECT_EQ(mtimecmp(c), 0x1122334455667788ull);
    EXPECT_EQ(*cmp, 0x1122334455667788ull);
    volatile uint64_t *mt = (volatile uint64_t *)(uintptr_t)
        (c.base + CLINT_MTIME);
    uint64_t t = *mt;
    EXPECT_LE(t, arch_ticks());
    /* and written whole */
    const uint64_t jump = 1ull << 40;
    *mt = t + jump;
    uint64_t moved = *mt;
    *mt = *mt - jump;
    EXPECT_GE(moved, t + jump);
    EXPECT_LT(moved, t + jump + us_to_ticks(100000));
    set_mtimecmp(c, UINT64_MAX);
#else
    t_skip("no 64 bit accesses");
#endif
}

TEST(clint, narrow_access)
{
    Clint c = need_clint();
    uint64_t msip = c.base + CLINT_MSIP(c.index);
    mmio_write8(msip, 1);
    SNAP("after_write8", "%#x", mmio_read32(msip));
    mmio_write32(msip, 1);
    SNAP("read8", "%#x", mmio_read8(msip));
    mmio_write32(msip, 0);
}

#endif
