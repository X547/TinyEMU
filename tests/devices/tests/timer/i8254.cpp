/*
 * PIT tests
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
#if defined(__i386__) || defined(__x86_64__)

#include "intc.h"
#include "mmio.h"
#include "test.h"

/* The 8254 at the ports the DSDT gives it, with channel 2's gate and output
   in the speaker port. Channel 2 is used where only the counting matters,
   channel 0 where the interrupt does. */

#define PIT_HZ 1193182

namespace {

struct Pit {
    uint16_t base;    /* channel 0; the control word at base + 3 */
    uint16_t speaker; /* port 0x61 on a PC */
    uint32_t irq;
};

Pit sPit;

Pit need_pit()
{
    const HwDevice *d = t_require_device("PNP0100");
    const HwRegion *r = hw_region(d, HW_IO);
    if (r == nullptr)
        t_skip("no PIT ports");
    Pit p {(uint16_t)r->base, 0, d->nirqs ? d->irqs[0].line : 0};
    const HwDevice *spk = registry_find("PNP0800");
    const HwRegion *s = spk ? hw_region(spk, HW_IO) : nullptr;
    p.speaker = s ? s->base : 0;
    sPit = p;
    /* channel 0 back to quiet, as the boot left it, after the one edge
       that takes, so that it falls before the controllers are reset and
       not into the next test */
    t_on_cleanup([] {
        outb(sPit.base + 3, 0x30);
        outb(sPit.base, 1);
        outb(sPit.base, 0);
        t_delay_us(5000);
    });
    return p;
}

void program(const Pit &p, int ch, int mode, uint16_t count)
{
    outb(p.base + 3, ch << 6 | 0x30 | mode << 1);
    outb(p.base + ch, count & 0xff);
    outb(p.base + ch, count >> 8);
}

uint16_t latch(const Pit &p, int ch)
{
    outb(p.base + 3, ch << 6);
    uint8_t lo = inb(p.base + ch);
    uint8_t hi = inb(p.base + ch);
    return lo | hi << 8;
}

bool out2(const Pit &p)
{
    return (inb(p.speaker) >> 5) & 1;
}

void gate2(const Pit &p, bool on)
{
    outb(p.speaker, (inb(p.speaker) & ~1) | on);
}

volatile int sCount;

}


FRESH_TEST(pit, reset_state)
{
    Pit p = need_pit();
    if (p.speaker)
        /* without channel 2's output, which runs from reset */
        SNAP("speaker", "%#x", inb(p.speaker) & ~0x20);
    SNAP("table_irq", "%d", registry_find("PNP0100")->nirqs);
}

TEST(pit, counts_down)
{
    Pit p = need_pit();
    if (!p.speaker)
        t_skip("no speaker port for channel 2's gate");
    gate2(p, true);
    program(p, 2, 0, 0xffff);
    uint16_t a = latch(p, 2);
    t_delay_us(10000);
    uint16_t b = latch(p, 2);
    EXPECT_LT(b, a);
    /* about 11932 counts in 10 ms */
    uint32_t delta = a - b;
    EXPECT_GT(delta, 9000u);
    EXPECT_LT(delta, 16000u);
}

TEST(pit, latch_holds)
{
    Pit p = need_pit();
    /* one 55 ms run, so nothing wraps between the two */
    program(p, 0, 0, 0xffff);
    outb(p.base + 3, 0x00); /* latch channel 0 */
    t_delay_us(2000);
    uint8_t lo = inb(p.base);
    uint8_t hi = inb(p.base);
    uint16_t held = lo | hi << 8;
    uint16_t now = latch(p, 0);
    /* the latched value is older, so larger, than one taken now */
    EXPECT_GT(held, now);
    EXPECT_GT((uint32_t)(held - now), 1500u);
}

TEST(pit, byte_access_modes)
{
    Pit p = need_pit();
    if (!p.speaker)
        t_skip("no speaker port for channel 2's gate");
    /* periodic, so the count stays between 1 and what was loaded */
    /* low byte only: a count of 0x40 */
    outb(p.base + 3, 0x80 | 0x10 | 2 << 1);
    outb(p.base + 2, 0x40);
    uint8_t lo = inb(p.base + 2);
    EXPECT_LE(lo, 0x40);
    EXPECT_GE(lo, 1);
    /* high byte only: a count of 0x4000, read back by its high byte */
    outb(p.base + 3, 0x80 | 0x20 | 2 << 1);
    outb(p.base + 2, 0x40);
    uint8_t hi = inb(p.base + 2);
    EXPECT_LE(hi, 0x40);
}

TEST(pit, mode0_output)
{
    Pit p = need_pit();
    if (!p.speaker)
        t_skip("no speaker port");
    gate2(p, true);
    program(p, 2, 0, 1193); /* 1 ms */
    EXPECT_FALSE(out2(p));
    EXPECT_TRUE(t_wait_until([] { return out2(sPit); }, 20000));
    /* and stays up */
    t_delay_us(2000);
    EXPECT_TRUE(out2(p));
}

TEST(pit, mode3_square_wave)
{
    Pit p = need_pit();
    if (!p.speaker)
        t_skip("no speaker port");
    gate2(p, true);
    program(p, 2, 3, 1193 * 2); /* 2 ms period */
    bool seen[2] = {};
    uint64_t end = t_now_us() + 10000;
    while (t_now_us() < end)
        seen[out2(p)] = true;
    EXPECT_TRUE(seen[0]);
    EXPECT_TRUE(seen[1]);
}

TEST(pit, other_modes)
{
    Pit p = need_pit();
    if (!p.speaker)
        t_skip("no speaker port");
    gate2(p, true);
    /* mode 1, a one-shot the gate starts: the output while counting */
    program(p, 2, 1, 0xffff);
    SNAP("mode1_counting", "%d", out2(p));
    /* modes 6 and 7, which the 8254 takes as 2 and 3 */
    program(p, 2, 6, 0xffff);
    SNAP("mode6_counting", "%d", out2(p));
    program(p, 2, 7, 1193 * 2);
    bool seen[2] = {};
    uint64_t end = t_now_us() + 10000;
    while (t_now_us() < end)
        seen[out2(p)] = true;
    SNAP("mode7_toggles", "%d", seen[0] && seen[1]);
    /* and on channel 0, whose interrupt they then decide */
    sCount = 0;
    ASSERT_TRUE(irq_route_isa(p.irq, [](void *) {
        sCount = sCount + 1;
    }, nullptr));
    program(p, 0, 6, 1193);
    arch_irq_enable();
    t_delay_us(10000);
    arch_irq_disable();
    SNAP("mode6_interrupts", "%s", sCount == 0 ? "none" : sCount == 1 ?
         "one" : "several");
}

TEST(pit, speaker_port)
{
    Pit p = need_pit();
    if (!p.speaker)
        t_skip("no speaker port");
    outb(p.speaker, 0x03);
    SNAP("on", "%#x", inb(p.speaker) & ~0x20);
    outb(p.speaker, 0x00);
    SNAP("off", "%#x", inb(p.speaker) & ~0x20);
}

TEST(pit, control_port)
{
    Pit p = need_pit();
    SNAP("read", "%#x", inb(p.base + 3));
    /* a read-back command for channel 0's count and status */
    outb(p.base + 3, 0xc2);
    SNAP("readback", "%s", inb(p.base) == 0xff ? "ff" : "other");
}

TEST(pit, periodic_interrupt)
{
    Pit p = need_pit();
    sCount = 0;
    ASSERT_TRUE(irq_route_isa(p.irq, [](void *) {
        sCount = sCount + 1;
    }, nullptr));
    program(p, 0, 2, 1193); /* 1 kHz */
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount >= 5; }, 200000));
    arch_irq_disable();
}

TEST(pit, one_shot_interrupt)
{
    Pit p = need_pit();
    sCount = 0;
    ASSERT_TRUE(irq_route_isa(p.irq, [](void *) {
        sCount = sCount + 1;
    }, nullptr));
    program(p, 0, 0, 1193);
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }, 100000));
    t_delay_us(10000);
    arch_irq_disable();
    EXPECT_EQ(sCount, 1);
}

TEST(pit, bcd)
{
    Pit p = need_pit();
    if (!p.speaker)
        t_skip("no speaker port");
    /* BCD counting, periodic: in BCD the count stays a valid decimal
       number, in binary any value up to 0x1000 */
    outb(p.base + 3, 0x80 | 0x30 | 2 << 1 | 1);
    outb(p.base + 2, 0x00);
    outb(p.base + 2, 0x10);
    bool decimal = true;
    for (int i = 0; i < 200; i++) {
        uint16_t c = latch(p, 2);
        for (int d = 0; d < 4; d++)
            decimal &= ((c >> (4 * d)) & 0xf) <= 9;
    }
    SNAP("decimal_counts", "%d", decimal);
}

#endif
