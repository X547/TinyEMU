/*
 * PLIC tests
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

#include "console.h"
#include "mmio.h"
#include "plic.h"
#include "riscv.h"
#include "test.h"
#include "testctl.h"

/* The sources are the test-control device's lines, which the guest raises
   and lowers itself. */

namespace {

struct Setup {
    Plic *plic;
    int ctx;
    uint32_t src[4];
};

Setup need_plic(int lines)
{
    Setup s {};
    s.plic = Plic::First();
    if (s.plic == nullptr)
        t_skip("no PLIC");
    s.ctx = s.plic->MyContext();
    if (s.ctx < 0)
        t_skip("no PLIC context for this hart and mode");
    t_require_tc_irqs(lines);
    const HwDevice *tc = test_control_device();
    for (int i = 0; i < lines; i++) {
        IntController *c;
        if (!intc_resolve(tc, i, &c, &s.src[i]) || c != s.plic)
            t_skip("test-control line %d is not on the PLIC", i);
    }
    return s;
}

bool pending(const Setup &s, uint32_t src)
{
    return (s.plic->Read(PLIC_PENDING(src / 32)) >> (src % 32)) & 1;
}

bool ext_pending()
{
    return (riscv_ip() >> riscv_ext_irq()) & 1;
}

}


FRESH_TEST(plic, reset_state)
{
    Setup s = need_plic(1);
    SNAP("priority", "%#x", s.plic->Read(PLIC_PRIORITY(s.src[0])));
    SNAP("enable0", "%#x", s.plic->Read(PLIC_ENABLE(s.ctx, 0)));
    SNAP("threshold", "%#x", s.plic->Read(PLIC_THRESHOLD(s.ctx)));
    SNAP("pending0", "%#x", s.plic->Read(PLIC_PENDING(0)));
    SNAP("claim", "%#x", s.plic->Claim(s.ctx));
    SNAP("sources", "%u", s.plic->SourceCount());
    SNAP("contexts", "%d", s.plic->ContextCount());
}

TEST(plic, priority_mask)
{
    Setup s = need_plic(1);
    s.plic->Write(PLIC_PRIORITY(s.src[0]), 0xffffffff);
    SNAP("all_ones", "%#x", s.plic->Read(PLIC_PRIORITY(s.src[0])));
    s.plic->Write(PLIC_PRIORITY(s.src[0]), 0);
    EXPECT_EQ(s.plic->Read(PLIC_PRIORITY(s.src[0])), 0u);

    /* source 0 does not exist */
    s.plic->Write(PLIC_PRIORITY(0), 7);
    EXPECT_EQ(s.plic->Read(PLIC_PRIORITY(0)), 0u);

    uint32_t last = s.plic->SourceCount();
    s.plic->Write(PLIC_PRIORITY(last), 7);
    EXPECT_EQ(s.plic->Read(PLIC_PRIORITY(last)), 7u);
    s.plic->Write(PLIC_PRIORITY(last + 1), 7);
    SNAP("past_last", "%#x", s.plic->Read(PLIC_PRIORITY(last + 1)));
}

TEST(plic, enable_mask)
{
    Setup s = need_plic(1);
    s.plic->Write(PLIC_ENABLE(s.ctx, 0), 0xffffffff);
    SNAP("word0", "%#x", s.plic->Read(PLIC_ENABLE(s.ctx, 0)));
    s.plic->Write(PLIC_ENABLE(s.ctx, 1), 0xffffffff);
    SNAP("word1", "%#x", s.plic->Read(PLIC_ENABLE(s.ctx, 1)));
    /* source 0 can never be enabled */
    EXPECT_EQ(s.plic->Read(PLIC_ENABLE(s.ctx, 0)) & 1, 0u);
}

TEST(plic, pending_follows_level)
{
    Setup s = need_plic(1);
    /* neither enabled nor given a priority: pending all the same */
    EXPECT_FALSE(pending(s, s.src[0]));
    tc_irq_raise(0);
    EXPECT_TRUE(pending(s, s.src[0]));
    tc_irq_lower(0);
    /* a level source: nothing is latched */
    EXPECT_FALSE(pending(s, s.src[0]));
}

TEST(plic, pending_read_only)
{
    Setup s = need_plic(1);
    s.plic->Write(PLIC_PENDING(0), 0xffffffff);
    EXPECT_EQ(s.plic->Read(PLIC_PENDING(0)), 0u);
    tc_irq_raise(0);
    s.plic->Write(PLIC_PENDING(0), 0);
    EXPECT_TRUE(pending(s, s.src[0]));
}

TEST(plic, claim_order)
{
    Setup s = need_plic(3);
    uint32_t low = s.src[0], a = s.src[1], b = s.src[2];
    s.plic->Write(PLIC_PRIORITY(low), 3);
    s.plic->Write(PLIC_PRIORITY(a), 5);
    s.plic->Write(PLIC_PRIORITY(b), 5);
    for (int i = 0; i < 3; i++)
        s.plic->SetEnabled(s.ctx, s.src[i], true);
    tc_irq_set_levels(7);

    /* the highest priority first, ties to the lower source number */
    uint32_t first = min(a, b), second = max(a, b);
    EXPECT_EQ(s.plic->Claim(s.ctx), first);
    EXPECT_EQ(s.plic->Claim(s.ctx), second);
    EXPECT_EQ(s.plic->Claim(s.ctx), low);
    EXPECT_EQ(s.plic->Claim(s.ctx), 0u);
    tc_irq_set_levels(0);
    for (int i = 0; i < 3; i++)
        s.plic->Complete(s.ctx, s.src[i]);
    EXPECT_EQ(s.plic->Claim(s.ctx), 0u);
}

TEST(plic, priority_zero_never_claimed)
{
    Setup s = need_plic(1);
    s.plic->SetEnabled(s.ctx, s.src[0], true);
    tc_irq_raise(0);
    EXPECT_EQ(s.plic->Claim(s.ctx), 0u);
    EXPECT_FALSE(ext_pending());
}

TEST(plic, threshold)
{
    Setup s = need_plic(1);
    s.plic->Write(PLIC_PRIORITY(s.src[0]), 3);
    s.plic->SetEnabled(s.ctx, s.src[0], true);
    s.plic->Write(PLIC_THRESHOLD(s.ctx), 0xffffffff);
    SNAP("threshold_mask", "%#x", s.plic->Read(PLIC_THRESHOLD(s.ctx)));
    s.plic->Write(PLIC_THRESHOLD(s.ctx), 3);
    tc_irq_raise(0);
    /* only a priority above the threshold gets through */
    EXPECT_FALSE(ext_pending());
    EXPECT_EQ(s.plic->Claim(s.ctx), 0u);
    s.plic->Write(PLIC_THRESHOLD(s.ctx), 2);
    EXPECT_TRUE(ext_pending());
    EXPECT_EQ(s.plic->Claim(s.ctx), s.src[0]);
    s.plic->Complete(s.ctx, s.src[0]);
}

TEST(plic, claim_complete_cycle)
{
    Setup s = need_plic(1);
    uint32_t src = s.src[0];
    s.plic->Write(PLIC_PRIORITY(src), 1);
    s.plic->SetEnabled(s.ctx, src, true);
    tc_irq_raise(0);
    EXPECT_TRUE(ext_pending());
    EXPECT_EQ(s.plic->Claim(s.ctx), src);
    /* in service: neither pending nor claimable, whatever the line does */
    EXPECT_FALSE(pending(s, src));
    EXPECT_FALSE(ext_pending());
    EXPECT_EQ(s.plic->Claim(s.ctx), 0u);
    /* completed with the line still high: pending again */
    s.plic->Complete(s.ctx, src);
    EXPECT_TRUE(pending(s, src));
    EXPECT_EQ(s.plic->Claim(s.ctx), src);
    tc_irq_lower(0);
    s.plic->Complete(s.ctx, src);
    EXPECT_FALSE(pending(s, src));
    EXPECT_FALSE(ext_pending());
}

TEST(plic, complete_ignores_others)
{
    Setup s = need_plic(2);
    uint32_t src = s.src[0];
    s.plic->Write(PLIC_PRIORITY(src), 1);
    s.plic->SetEnabled(s.ctx, src, true);
    tc_irq_raise(0);
    EXPECT_EQ(s.plic->Claim(s.ctx), src);
    s.plic->Complete(s.ctx, 0);
    s.plic->Complete(s.ctx, s.plic->SourceCount() + 1);
    s.plic->Complete(s.ctx, s.src[1]);
    EXPECT_FALSE(pending(s, src));
    s.plic->Complete(s.ctx, src);
    EXPECT_TRUE(pending(s, src));
}

TEST(plic, complete_while_disabled)
{
    Setup s = need_plic(1);
    uint32_t src = s.src[0];
    s.plic->Write(PLIC_PRIORITY(src), 1);
    s.plic->SetEnabled(s.ctx, src, true);
    tc_irq_raise(0);
    EXPECT_EQ(s.plic->Claim(s.ctx), src);
    tc_irq_lower(0);
    s.plic->SetEnabled(s.ctx, src, false);
    s.plic->Complete(s.ctx, src);
    /* whether a completion for a source no longer enabled counts */
    s.plic->SetEnabled(s.ctx, src, true);
    tc_irq_raise(0);
    SNAP("claim_after", "%s", s.plic->Claim(s.ctx) == src ? "src" : "none");
    s.plic->Complete(s.ctx, src);
}

TEST(plic, external_interrupt)
{
    Setup s = need_plic(1);
    static volatile int count;
    count = 0;
    ASSERT_TRUE(s.plic->Route(s.src[0], [](void *) {
        count = count + 1;
        tc_irq_lower(0);
    }, nullptr));
    arch_irq_enable();
    tc_irq_raise(0);
    EXPECT_TRUE(t_wait_until([] { return count > 0; }));
    t_delay_us(1000);
    arch_irq_disable();
    EXPECT_EQ(count, 1);
    /* again, after the completion */
    arch_irq_enable();
    tc_irq_raise(0);
    EXPECT_TRUE(t_wait_until([] { return count > 1; }));
    arch_irq_disable();
    EXPECT_EQ(count, 2);
}

TEST(plic, supervisor_context_from_m)
{
    t_require_mode("M");
    Setup s = need_plic(1);
    int sctx = s.plic->ContextFor(gBoot.boot_cpu, false);
    if (sctx < 0)
        t_skip("no supervisor context");
    uint32_t src = s.src[0];
    s.plic->Write(PLIC_PRIORITY(src), 1);
    s.plic->SetEnabled(sctx, src, true);
    tc_irq_raise(0);
    /* the supervisor context drives mip.SEIP, not MEIP */
    EXPECT_EQ((CSR_READ(mip) >> IRQ_S_EXT) & 1, 1u);
    EXPECT_EQ((CSR_READ(mip) >> IRQ_M_EXT) & 1, 0u);
    EXPECT_EQ(s.plic->Claim(s.ctx), 0u);
    EXPECT_EQ(s.plic->Claim(sctx), src);
    EXPECT_EQ((CSR_READ(mip) >> IRQ_S_EXT) & 1, 0u);
    tc_irq_lower(0);
    s.plic->Complete(sctx, src);
}

TEST(plic, other_hart)
{
    Setup s = need_plic(1);
    int other = -1;
    for (uint32_t hart = 0; hart < 64 && other < 0; hart++) {
        if (hart != gBoot.boot_cpu)
            other = s.plic->ContextFor(hart, riscv_machine_mode());
    }
    if (other < 0)
        t_skip("one hart");
    s.plic->Write(PLIC_PRIORITY(s.src[0]), 1);
    s.plic->SetEnabled(other, s.src[0], true);
    tc_irq_raise(0);
    EXPECT_FALSE(ext_pending());
    EXPECT_EQ(s.plic->Claim(s.ctx), 0u);
    EXPECT_EQ(s.plic->Claim(other), s.src[0]);
    tc_irq_lower(0);
    s.plic->Complete(other, s.src[0]);
}

TEST(plic, past_last_context)
{
    Setup s = need_plic(1);
    int ctx = s.plic->ContextCount();
    s.plic->Write(PLIC_THRESHOLD(ctx), 5);
    SNAP("threshold", "%#x", s.plic->Read(PLIC_THRESHOLD(ctx)));
    s.plic->Write(PLIC_ENABLE(ctx, 0), 0xffffffff);
    SNAP("enable", "%#x", s.plic->Read(PLIC_ENABLE(ctx, 0)));
    SNAP("claim", "%#x", s.plic->Read(PLIC_CLAIM(ctx)));
}

TEST(plic, context_reserved)
{
    Setup s = need_plic(1);
    uint32_t reserved = PLIC_THRESHOLD(s.ctx) + 8;
    s.plic->Write(PLIC_THRESHOLD(s.ctx), 2);
    s.plic->Write(reserved, 5);
    SNAP("read", "%#x", s.plic->Read(reserved));
    EXPECT_EQ(s.plic->Read(PLIC_THRESHOLD(s.ctx)), 2u);
}

TEST(plic, narrow_access)
{
    Setup s = need_plic(1);
    uint64_t prio = s.plic->Base() + PLIC_PRIORITY(s.src[0]);
    mmio_write32(prio, 5);
    SNAP("read8", "%#x", mmio_read8(prio));
    SNAP("read16", "%#x", mmio_read16(prio));
    mmio_write8(prio, 3);
    SNAP("after_write8", "%#x", mmio_read32(prio));
    mmio_write16(prio, 2);
    SNAP("after_write16", "%#x", mmio_read32(prio));
}

#endif
