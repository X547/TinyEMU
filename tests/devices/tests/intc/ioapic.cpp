/*
 * IOAPIC tests
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
#include "ioapic.h"
#include "lapic.h"
#include "mmio.h"
#include "test.h"
#include "testctl.h"
#include "vectors.h"

/* The sources are the test-control lines, at their GSIs. */

namespace {

struct Setup {
    IoApic *io;
    Lapic *lapic;
    uint32_t gsi[4];
    uint32_t pin[4];
};

Setup need_ioapic(int lines)
{
    Setup s {};
    s.io = IoApic::At(0);
    s.lapic = Lapic::Get();
    if (s.io == nullptr || s.lapic == nullptr)
        t_skip("no IOAPIC");
    t_require_tc_irqs(lines);
    const HwDevice *tc = test_control_device();
    for (int i = 0; i < lines; i++) {
        IntController *c;
        if (!intc_resolve(tc, i, &c, &s.gsi[i]) || c != s.io)
            t_skip("test-control line %d is not on the IOAPIC", i);
        s.pin[i] = s.gsi[i] - s.io->GsiBase();
    }
    s.lapic->Enable();
    return s;
}

uint64_t to_me(const Setup &s, uint8_t vector)
{
    return vector | RTE_DEST(s.lapic->Id());
}

volatile int sCount;
Setup sSetup;

}


FRESH_TEST(ioapic, reset_state)
{
    IoApic *io = IoApic::At(0);
    if (io == nullptr)
        t_skip("no IOAPIC");
    /* first: every register read goes through it */
    SNAP("regsel", "%#x", mmio_read32(io->Base() + IOREGSEL));
    SNAP("id", "%#x", io->Read(IOAPIC_REG_ID));
    SNAP("version", "%#x", io->Read(IOAPIC_REG_VER));
    SNAP("arbitration", "%#x", io->Read(IOAPIC_REG_ARB));
    SNAP("rte0", "%#llx", T_FMT_VAL(io->Rte(0)));
    SNAP("rte_last", "%#llx", T_FMT_VAL(io->Rte(io->Pins() - 1)));
}

TEST(ioapic, identification)
{
    Setup s = need_ioapic(1);
    s.io->Write(IOAPIC_REG_ID, 0xffffffff);
    SNAP("id_all_ones", "%#x", s.io->Read(IOAPIC_REG_ID));
    SNAP("arb_after_id", "%#x", s.io->Read(IOAPIC_REG_ARB));
    uint32_t ver = s.io->Read(IOAPIC_REG_VER);
    s.io->Write(IOAPIC_REG_VER, 0);
    EXPECT_EQ(s.io->Read(IOAPIC_REG_VER), ver);
    s.io->Write(IOAPIC_REG_ARB, 0x0f000000);
    SNAP("arb_written", "%#x", s.io->Read(IOAPIC_REG_ARB));
    /* a register that is not there */
    SNAP("reg_3", "%#x", s.io->Read(3));
    SNAP("past_last_rte", "%#x", s.io->Read(IOAPIC_REG_RTE(s.io->Pins())));
}

TEST(ioapic, window)
{
    Setup s = need_ioapic(1);
    mmio_write32(s.io->Base() + IOREGSEL, 0x12);
    SNAP("regsel", "%#x", mmio_read32(s.io->Base() + IOREGSEL));
    mmio_write32(s.io->Base() + IOREGSEL, 0x1ff);
    SNAP("regsel_wide", "%#x", mmio_read32(s.io->Base() + IOREGSEL));
    SNAP("offset_4", "%#x", mmio_read32(s.io->Base() + 4));
    SNAP("offset_20", "%#x", mmio_read32(s.io->Base() + 0x20));
    mmio_write32(s.io->Base() + 0x20, 1);
}

TEST(ioapic, rte_bits)
{
    Setup s = need_ioapic(1);
    uint32_t pin = s.pin[0];
    s.io->Write(IOAPIC_REG_RTE(pin), 0xffffffff);
    s.io->Write(IOAPIC_REG_RTE(pin) + 1, 0xffffffff);
    /* by the 82093AA data sheet: delivery status and remote IRR are read
       only, and bits 17-55 reserved */
    EXPECT_EQ(s.io->Rte(pin), 0xff0000000001afffull);
    s.io->SetRte(pin, RTE_MASKED);
}

TEST(ioapic, edge)
{
    Setup s = need_ioapic(1);
    sSetup = s;
    sCount = 0;
    uint8_t v = VEC_TEST;
    arch_set_irq_handler(v, [](void *) {
        sCount = sCount + 1;
        tc_irq_lower(0);
        Lapic::Get()->Eoi();
    }, nullptr);
    s.io->SetRte(s.pin[0], to_me(s, v));
    arch_irq_enable();
    tc_irq_raise(0);
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
    EXPECT_EQ(sCount, 1);
    EXPECT_FALSE(s.io->Rte(s.pin[0]) & RTE_REMOTE_IRR);
}

TEST(ioapic, edge_while_masked)
{
    Setup s = need_ioapic(1);
    sCount = 0;
    uint8_t v = VEC_TEST;
    arch_set_irq_handler(v, [](void *) {
        sCount = sCount + 1;
        Lapic::Get()->Eoi();
    }, nullptr);
    s.io->SetRte(s.pin[0], to_me(s, v) | RTE_MASKED);
    tc_irq_raise(0);
    s.io->SetRte(s.pin[0], to_me(s, v));
    arch_irq_enable();
    t_delay_us(1000);
    arch_irq_disable();
    /* an edge while masked is lost */
    EXPECT_EQ(sCount, 0);
}

TEST(ioapic, level_remote_irr)
{
    Setup s = need_ioapic(1);
    sSetup = s;
    sCount = 0;
    uint8_t v = VEC_TEST;
    arch_set_irq_handler(v, [](void *) {
        sCount = sCount + 1;
        /* no EOI and the line left high */
    }, nullptr);
    s.io->SetRte(s.pin[0], to_me(s, v) | RTE_LEVEL);
    arch_irq_enable();
    tc_irq_raise(0);
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
    EXPECT_TRUE(s.io->Rte(s.pin[0]) & RTE_REMOTE_IRR);
    EXPECT_TRUE(s.lapic->Read(LAPIC_TMR(v / 32)) >> (v % 32) & 1);
    /* the EOI with the line still high sends it again */
    s.lapic->Eoi();
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] { return sCount > 1; }));
    arch_irq_disable();
    tc_irq_lower(0);
    s.lapic->Eoi();
    EXPECT_FALSE(s.io->Rte(s.pin[0]) & RTE_REMOTE_IRR);
    arch_irq_enable();
    t_delay_us(1000);
    arch_irq_disable();
    EXPECT_EQ(sCount, 2);
}

TEST(ioapic, level_unmask_sends)
{
    Setup s = need_ioapic(1);
    sCount = 0;
    uint8_t v = VEC_TEST;
    arch_set_irq_handler(v, [](void *) {
        sCount = sCount + 1;
        tc_irq_lower(0);
        Lapic::Get()->Eoi();
    }, nullptr);
    s.io->SetRte(s.pin[0], to_me(s, v) | RTE_LEVEL | RTE_MASKED);
    tc_irq_raise(0);
    arch_irq_enable();
    t_delay_us(500);
    EXPECT_EQ(sCount, 0);
    s.io->Write(IOAPIC_REG_RTE(s.pin[0]),
                (uint32_t)(to_me(s, v) | RTE_LEVEL));
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
}

TEST(ioapic, edge_clears_remote_irr)
{
    Setup s = need_ioapic(1);
    sCount = 0;
    uint8_t v = VEC_TEST;
    arch_set_irq_handler(v, [](void *) { sCount = sCount + 1; }, nullptr);
    s.io->SetRte(s.pin[0], to_me(s, v) | RTE_LEVEL);
    arch_irq_enable();
    tc_irq_raise(0);
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
    tc_irq_lower(0);
    s.io->Write(IOAPIC_REG_RTE(s.pin[0]), (uint32_t)to_me(s, v));
    EXPECT_FALSE(s.io->Rte(s.pin[0]) & RTE_REMOTE_IRR);
    s.lapic->Eoi();
}

TEST(ioapic, polarity)
{
    Setup s = need_ioapic(1);
    sCount = 0;
    uint8_t v = VEC_TEST;
    arch_set_irq_handler(v, [](void *) {
        sCount = sCount + 1;
        tc_irq_lower(0);
        Lapic::Get()->Eoi();
    }, nullptr);
    /* active low: by the data sheet a low line is the asserted one */
    s.io->SetRte(s.pin[0], to_me(s, v) | RTE_LEVEL | RTE_ACTIVE_LOW);
    arch_irq_enable();
    t_delay_us(500);
    SNAP("low_line_delivers", "%d", sCount > 0);
    tc_irq_raise(0);
    t_delay_us(500);
    arch_irq_disable();
    SNAP("high_line_delivers", "%d", sCount > 0);
}

TEST(ioapic, route)
{
    Setup s = need_ioapic(1);
    sCount = 0;
    ASSERT_TRUE(s.io->Route(s.gsi[0], [](void *) {
        sCount = sCount + 1;
        tc_irq_lower(0);
    }, nullptr, true));
    arch_irq_enable();
    tc_irq_raise(0);
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    t_delay_us(500);
    arch_irq_disable();
    EXPECT_EQ(sCount, 1);
}

TEST(ioapic, logical_destination)
{
    Setup s = need_ioapic(1);
    sCount = 0;
    uint8_t v = VEC_TEST;
    arch_set_irq_handler(v, [](void *) {
        sCount = sCount + 1;
        tc_irq_lower(0);
        Lapic::Get()->Eoi();
    }, nullptr);
    s.lapic->Write(LAPIC_DFR, 0xffffffff);
    s.lapic->Write(LAPIC_LDR, 0x04000000);
    /* a destination this APIC's logical ID is not in */
    s.io->SetRte(s.pin[0], v | RTE_LOGICAL | RTE_DEST(0x08));
    tc_irq_raise(0);
    arch_irq_enable();
    t_delay_us(500);
    arch_irq_disable();
    EXPECT_EQ(sCount, 0);
    tc_irq_lower(0);
    s.io->SetRte(s.pin[0], v | RTE_LOGICAL | RTE_DEST(0x04));
    arch_irq_enable();
    tc_irq_raise(0);
    EXPECT_TRUE(t_wait_until([] { return sCount > 0; }));
    arch_irq_disable();
}

#endif
