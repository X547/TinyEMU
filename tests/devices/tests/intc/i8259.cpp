/*
 * 8259 tests
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

#include "console.h"
#include "format.h"
#include "i8259.h"
#include "mmio.h"
#include "test.h"
#include "testctl.h"
#include "vectors.h"

/* The sources are test-control lines that are ISA lines, which reach the
   8259s whether or not there is an IOAPIC as well. */

namespace {

struct Setup {
    I8259 *pic;
    int count;          /* lines found */
    uint32_t line[4];   /* ISA line numbers */
    int index[4];       /* their test-control line */
};

/* 'want' lines, on the master or the slave as asked: -1 either */
Setup need_pic(int want, int slave = -1)
{
    Setup s {};
    s.pic = I8259::Get();
    if (s.pic == nullptr)
        t_skip("no 8259s");
    const HwDevice *tc = test_control_device();
    int n = tc_irq_count();
    for (int i = 0; tc && i < n && i < tc->nirqs && s.count < want; i++) {
        uint32_t l = tc->irqs[i].line;
        if (l >= 16 || l == 2)
            continue;
        if (slave >= 0 && (l >= 8) != (slave == 1))
            continue;
        s.line[s.count] = l;
        s.index[s.count] = i;
        s.count++;
    }
    if (s.count < want)
        t_skip("needs %d test-control ISA lines%s", want,
               slave == 1 ? " on the slave" : slave == 0 ? " on the master"
                                                      : "");
    s.pic->Reset();
    return s;
}

int pic_of(uint32_t line)
{
    return line >= 8;
}

uint8_t bit(uint32_t line)
{
    return 1 << (line & 7);
}

uint8_t vector(uint32_t line)
{
    return (line >= 8 ? VEC_PIC_SLAVE : VEC_PIC_MASTER) + (line & 7);
}

volatile int sOrder[8];
volatile int sCount;
Setup sSetup;

}


extern uint8_t gPicResetState[8];

FRESH_TEST(i8259, reset_state)
{
    if (I8259::Get() == nullptr)
        t_skip("no 8259s");
    /* as the boot found them, before setting them up; the PIT had fired
       once on line 0 by then */
    static const char *const names[] = {"irr", "isr", "imr", "elcr"};
    for (int i = 0; i < 8; i++) {
        char key[16];
        snprintf(key, sizeof(key), "%s%d", names[i % 4], i / 4);
        SNAP(key, "%#x", gPicResetState[i]);
    }
}

TEST(i8259, init_clears_mask)
{
    Setup s = need_pic(1);
    s.pic->SetImr(0, 0x5a);
    s.pic->SetImr(1, 0xa5);
    EXPECT_EQ(s.pic->Imr(0), 0x5a);
    EXPECT_EQ(s.pic->Imr(1), 0xa5);
    s.pic->Init(VEC_PIC_MASTER, VEC_PIC_SLAVE);
    SNAP("imr_after_init", "%#x %#x", s.pic->Imr(0), s.pic->Imr(1));
}

TEST(i8259, elcr_mask)
{
    Setup s = need_pic(1);
    s.pic->SetElcr(0, 0xff);
    s.pic->SetElcr(1, 0xff);
    /* the timer, the keyboard, the cascade, the RTC and the FPU cannot be
       level triggered */
    SNAP("elcr", "%#x %#x", s.pic->Elcr(0), s.pic->Elcr(1));
}

TEST(i8259, edge_latched)
{
    Setup s = need_pic(1);
    uint32_t l = s.line[0];
    /* masked, and still requested: the mask only holds back INTR */
    tc_irq_raise(s.index[0]);
    EXPECT_TRUE(s.pic->Irr(pic_of(l)) & bit(l));
    tc_irq_lower(s.index[0]);
    EXPECT_TRUE(s.pic->Irr(pic_of(l)) & bit(l));
}

TEST(i8259, level_follows_line)
{
    Setup s = need_pic(1);
    uint32_t l = s.line[0];
    if (!((0xdef8 >> l) & 1))
        t_skip("line %u cannot be level triggered", l);
    s.pic->SetElcr(pic_of(l), bit(l));
    tc_irq_raise(s.index[0]);
    EXPECT_TRUE(s.pic->Irr(pic_of(l)) & bit(l));
    tc_irq_lower(s.index[0]);
    EXPECT_FALSE(s.pic->Irr(pic_of(l)) & bit(l));
}

TEST(i8259, interrupt)
{
    Setup s = need_pic(1);
    sSetup = s;
    sCount = 0;
    ASSERT_TRUE(s.pic->Route(s.line[0], [](void *) {
        /* in service while its handler runs */
        uint32_t l = sSetup.line[0];
        sOrder[0] = (sSetup.pic->Isr(pic_of(l)) & bit(l)) != 0;
        sCount = sCount + 1;
        tc_irq_lower(sSetup.index[0]);
    }, nullptr, false));
    arch_irq_enable();
    tc_irq_raise(s.index[0]);
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
    EXPECT_EQ(sCount, 1);
    EXPECT_EQ(sOrder[0], 1);
    /* the EOI after the handler ended it */
    EXPECT_FALSE(s.pic->Isr(pic_of(s.line[0])) & bit(s.line[0]));
}

TEST(i8259, slave_interrupt)
{
    Setup s = need_pic(1, 1);
    sSetup = s;
    sCount = 0;
    ASSERT_TRUE(s.pic->Route(s.line[0], [](void *) {
        sCount = sCount + 1;
        /* the cascade input is in service on the master meanwhile */
        sOrder[0] = (sSetup.pic->Isr(0) & 4) != 0;
        tc_irq_lower(sSetup.index[0]);
    }, nullptr, false));
    arch_irq_enable();
    tc_irq_raise(s.index[0]);
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
    EXPECT_EQ(sOrder[0], 1);
    EXPECT_EQ(s.pic->Isr(0) | s.pic->Isr(1), 0);
}

TEST(i8259, priority_order)
{
    Setup s = need_pic(2, 0);
    sSetup = s;
    sCount = 0;
    for (int i = 0; i < 2; i++) {
        arch_set_irq_handler(vector(s.line[i]), [](void *p) {
            int i = (int)(uintptr_t)p;
            sOrder[sCount] = sSetup.line[i];
            sCount = sCount + 1;
            tc_irq_lower(sSetup.index[i]);
            outb(sSetup.pic->CommandPort(0), 0x20);
        }, (void *)(uintptr_t)i);
        s.pic->SetMasked(s.line[i], false);
    }
    /* both requested before either is taken: the lower line first */
    tc_irq_raise(s.index[0]);
    tc_irq_raise(s.index[1]);
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount >= 2; }));
    arch_irq_disable();
    EXPECT_EQ(sOrder[0], (int)min(s.line[0], s.line[1]));
    EXPECT_EQ(sOrder[1], (int)max(s.line[0], s.line[1]));
}

TEST(i8259, rotation)
{
    Setup s = need_pic(2, 0);
    uint32_t lo = min(s.line[0], s.line[1]), hi = max(s.line[0], s.line[1]);
    /* set priority: 'lo' becomes the lowest, so 'hi' wins */
    outb(s.pic->CommandPort(0), 0xc0 | lo);
    sSetup = s;
    sCount = 0;
    for (int i = 0; i < 2; i++) {
        arch_set_irq_handler(vector(s.line[i]), [](void *p) {
            int i = (int)(uintptr_t)p;
            sOrder[sCount] = sSetup.line[i];
            sCount = sCount + 1;
            tc_irq_lower(sSetup.index[i]);
            outb(sSetup.pic->CommandPort(0), 0x20);
        }, (void *)(uintptr_t)i);
        s.pic->SetMasked(s.line[i], false);
    }
    tc_irq_raise(s.index[0]);
    tc_irq_raise(s.index[1]);
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount >= 2; }));
    arch_irq_disable();
    EXPECT_EQ(sOrder[0], (int)hi);
    EXPECT_EQ(sOrder[1], (int)lo);
}

TEST(i8259, specific_eoi)
{
    Setup s = need_pic(1, 0);
    uint32_t l = s.line[0];
    sSetup = s;
    sCount = 0;
    arch_set_irq_handler(vector(l), [](void *) {
        sCount = sCount + 1;
        tc_irq_lower(sSetup.index[0]);
        /* no EOI here */
    }, nullptr);
    s.pic->SetMasked(l, false);
    arch_irq_enable();
    tc_irq_raise(s.index[0]);
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
    EXPECT_TRUE(s.pic->Isr(0) & bit(l));
    /* a specific EOI for another line leaves it */
    outb(s.pic->CommandPort(0), 0x60 | ((l + 1) & 7));
    EXPECT_TRUE(s.pic->Isr(0) & bit(l));
    outb(s.pic->CommandPort(0), 0x60 | l);
    EXPECT_FALSE(s.pic->Isr(0) & bit(l));
}

TEST(i8259, in_service_blocks_lower)
{
    Setup s = need_pic(2, 0);
    uint32_t lo = min(s.line[0], s.line[1]), hi = max(s.line[0], s.line[1]);
    int lo_i = s.line[0] == lo ? s.index[0] : s.index[1];
    int hi_i = s.line[0] == lo ? s.index[1] : s.index[0];
    sSetup = s;
    sCount = 0;
    arch_set_irq_handler(vector(lo), [](void *) {
        sCount = sCount + 1;
    }, nullptr);
    arch_set_irq_handler(vector(hi), [](void *) {
        sCount = sCount + 100;
    }, nullptr);
    s.pic->SetMasked(lo, false);
    s.pic->SetMasked(hi, false);
    tc_irq_raise(lo_i);
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    /* 'lo' in service and not ended: 'hi' waits */
    tc_irq_raise(hi_i);
    t_delay_us(1000);
    EXPECT_EQ(sCount, 1);
    outb(s.pic->CommandPort(0), 0x20);
    EXPECT_TRUE(t_wait_until([] { return sCount > 100; }));
    arch_irq_disable();
    outb(s.pic->CommandPort(0), 0x20);
}

TEST(i8259, auto_eoi)
{
    Setup s = need_pic(1, 0);
    uint32_t l = s.line[0];
    s.pic->Init(VEC_PIC_MASTER, VEC_PIC_SLAVE, true);
    s.pic->SetImr(0, 0xff & ~bit(l));
    s.pic->SetImr(1, 0xff);
    sSetup = s;
    sCount = 0;
    arch_set_irq_handler(vector(l), [](void *) {
        sOrder[0] = sSetup.pic->Isr(0);
        sCount = sCount + 1;
        tc_irq_lower(sSetup.index[0]);
    }, nullptr);
    arch_irq_enable();
    tc_irq_raise(s.index[0]);
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
    /* nothing put in service */
    EXPECT_EQ(sOrder[0], 0);
}

namespace {

/* Raise two master lines and record the order they are taken in, ending
   each by 'eoi'. */
void take_both(const Setup &s, uint8_t eoi)
{
    sSetup = s;
    sCount = 0;
    sOrder[7] = eoi;
    for (int i = 0; i < 2; i++) {
        arch_set_irq_handler(vector(s.line[i]), [](void *p) {
            int i = (int)(uintptr_t)p;
            sOrder[sCount] = sSetup.line[i];
            sCount = sCount + 1;
            tc_irq_lower(sSetup.index[i]);
            if (sOrder[7])
                outb(sSetup.pic->CommandPort(0), sOrder[7]);
        }, (void *)(uintptr_t)i);
        s.pic->SetMasked(s.line[i], false);
    }
    tc_irq_raise(s.index[0]);
    tc_irq_raise(s.index[1]);
    arch_irq_enable();
    t_wait_until([] { return sCount >= 2; });
    arch_irq_disable();
}

}

TEST(i8259, rotate_on_eoi)
{
    Setup s = need_pic(2, 0);
    uint32_t lo = min(s.line[0], s.line[1]);
    /* a rotating EOI after 'lo' makes it the lowest priority */
    take_both(s, 0xa0);
    EXPECT_EQ(sCount, 2);
    sCount = 0;
    take_both(s, 0x20);
    SNAP("second_round_first", "%s", sOrder[0] == (int)lo ? "lo" : "hi");
}

TEST(i8259, rotate_on_specific_eoi)
{
    Setup s = need_pic(1, 0);
    uint32_t l = s.line[0];
    sSetup = s;
    sCount = 0;
    arch_set_irq_handler(vector(l), [](void *) {
        sCount = sCount + 1;
        tc_irq_lower(sSetup.index[0]);
        outb(sSetup.pic->CommandPort(0), 0xe0 | (sSetup.line[0] & 7));
    }, nullptr);
    s.pic->SetMasked(l, false);
    arch_irq_enable();
    tc_irq_raise(s.index[0]);
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
    EXPECT_FALSE(s.pic->Isr(0) & bit(l));
    /* the commands that only set modes */
    outb(s.pic->CommandPort(0), 0x60);
    outb(s.pic->CommandPort(0), 0xc0);
    outb(s.pic->CommandPort(0), 0x00);
}

TEST(i8259, rotate_in_auto_eoi)
{
    Setup s = need_pic(2, 0);
    uint32_t lo = min(s.line[0], s.line[1]);
    s.pic->Init(VEC_PIC_MASTER, VEC_PIC_SLAVE, true);
    s.pic->SetImr(0, 0xff);
    s.pic->SetImr(1, 0xff);
    outb(s.pic->CommandPort(0), 0x80); /* rotate in automatic EOI mode */
    take_both(s, 0);
    sCount = 0;
    take_both(s, 0);
    SNAP("second_round_first", "%s", sOrder[0] == (int)lo ? "lo" : "hi");
    outb(s.pic->CommandPort(0), 0x00);
}

TEST(i8259, init_without_icw4)
{
    Setup s = need_pic(1);
    outb(s.pic->CommandPort(0), 0x10); /* ICW1 without IC4 */
    outb(s.pic->DataPort(0), VEC_PIC_MASTER);
    outb(s.pic->DataPort(0), 4);
    /* the next data write is OCW1 already */
    outb(s.pic->DataPort(0), 0x5a);
    EXPECT_EQ(s.pic->Imr(0), 0x5a);
}

TEST(i8259, spurious_slave)
{
    Setup s = need_pic(1, 1);
    uint32_t l = s.line[0];
    sCount = 0;
    arch_set_irq_handler(VEC_PIC_SLAVE + 7, [](void *) {
        /* a spurious interrupt is not in service: no EOI to the slave */
        sCount = sCount + 1;
        outb(I8259::Get()->CommandPort(0), 0x20);
    }, nullptr);
    s.pic->SetMasked(2, false);
    s.pic->SetMasked(l, false);
    tc_irq_raise(s.index[0]);
    /* the cascade input latched the request; the slave then withdraws */
    s.pic->SetMasked(l, true);
    arch_irq_enable();
    t_wait_until([] { return sCount > 0; }, 2000);
    arch_irq_disable();
    SNAP("spurious_taken", "%d", sCount);
}

TEST(i8259, special_mask)
{
    Setup s = need_pic(2, 0);
    uint32_t lo = min(s.line[0], s.line[1]), hi = max(s.line[0], s.line[1]);
    int lo_i = s.line[0] == lo ? s.index[0] : s.index[1];
    int hi_i = s.line[0] == lo ? s.index[1] : s.index[0];
    sCount = 0;
    arch_set_irq_handler(vector(lo), [](void *) {
        sCount = sCount + 1;
    }, nullptr);
    arch_set_irq_handler(vector(hi), [](void *) {
        sCount = sCount + 100;
    }, nullptr);
    s.pic->SetMasked(lo, false);
    s.pic->SetMasked(hi, false);
    tc_irq_raise(lo_i);
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
    /* with 'lo' in service and masked, special mask mode lets the lower
       priority 'hi' through, by the 8259A data sheet */
    s.pic->SetMasked(lo, true);
    outb(s.pic->CommandPort(0), 0x68);
    tc_irq_raise(hi_i);
    arch_irq_enable();
    t_wait_until([] { return sCount > 100; }, 2000);
    arch_irq_disable();
    SNAP("lower_delivered", "%d", sCount > 100);
    outb(s.pic->CommandPort(0), 0x48);
    outb(s.pic->CommandPort(0), 0x20);
    outb(s.pic->CommandPort(0), 0x20);
}

TEST(i8259, poll_command)
{
    Setup s = need_pic(1, 0);
    tc_irq_raise(s.index[0]);
    /* OCW3 poll: the next read gives the highest request, bit 7 set */
    outb(s.pic->CommandPort(0), 0x0c);
    SNAP("poll", "%#x", inb(s.pic->CommandPort(0)));
}

TEST(i8259, level_init_word)
{
    /* LTIM in ICW1 asks for level triggering of every input; a PIIX takes
       the ELCRs instead and ignores it */
    Setup s = need_pic(1, 0);
    uint32_t l = s.line[0];
    uint8_t elcr = s.pic->Elcr(0);
    outb(s.pic->CommandPort(0), 0x19);
    outb(s.pic->DataPort(0), VEC_PIC_MASTER);
    outb(s.pic->DataPort(0), 4);
    outb(s.pic->DataPort(0), 1);
    EXPECT_EQ(s.pic->Imr(0), 0);
    /* the input stays edge triggered: a request held up is taken once */
    s.pic->SetElcr(0, elcr & ~bit(l));
    s.pic->SetImr(0, ~bit(l) & ~4);
    sSetup = s;
    sCount = 0;
    arch_set_irq_handler(vector(l), [](void *) {
        sCount = sCount + 1;
        outb(sSetup.pic->CommandPort(0), 0x20);
    }, nullptr);
    tc_irq_raise(s.index[0]);
    arch_irq_enable();
    t_delay_us(2000);
    arch_irq_disable();
    tc_irq_lower(s.index[0]);
    EXPECT_EQ(sCount, 1);
}

TEST(i8259, single_mode_init_word)
{
    /* SNGL in ICW1: a lone 8259, which takes no ICW3, so the third byte is
       ICW4 and the fourth the mask */
    Setup s = need_pic(1);
    outb(s.pic->CommandPort(1), 0x13);
    outb(s.pic->DataPort(1), VEC_PIC_SLAVE);
    outb(s.pic->DataPort(1), 1);
    EXPECT_EQ(s.pic->Imr(1), 0);
    outb(s.pic->DataPort(1), 0x5a);
    EXPECT_EQ(s.pic->Imr(1), 0x5a);
}

#endif
