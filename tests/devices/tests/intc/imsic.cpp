/*
 * IMSIC tests
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

#include "imsic.h"
#include "mmio.h"
#include "riscv.h"
#include "smp.h"
#include "test.h"

/* The boot hart's interrupt file at the privilege the image runs at, fed
   through its own MSI page. */

namespace {

Imsic *need_imsic()
{
    Imsic *f = Imsic::Mine();
    if (f == nullptr)
        t_skip("no interrupt file at this privilege");
    return f;
}

bool ext_pending()
{
    return (riscv_ip() >> riscv_ext_irq()) & 1;
}

void send(const Imsic *f, uint32_t id)
{
    mmio_write32(f->MyPage(), id);
}

unsigned sSelect;

void read_select(void *)
{
    Imsic::Mine()->ReadReg(sSelect);
}

}


FRESH_TEST(imsic, reset_state)
{
    Imsic *f = need_imsic();
    SNAP("eidelivery", "%#lx", f->ReadReg(ISEL_EIDELIVERY));
    SNAP("eithreshold", "%#lx", f->ReadReg(ISEL_EITHRESHOLD));
    SNAP("eip0", "%#lx", f->ReadReg(ISEL_EIP0));
    SNAP("eie0", "%#lx", f->ReadReg(ISEL_EIE0));
    SNAP("topei", "%#lx", f->Topei());
    SNAP("ids", "%u", f->IdCount());
}

TEST(imsic, eidelivery)
{
    Imsic *f = need_imsic();
    f->WriteReg(ISEL_EIDELIVERY, ~0ul);
    SNAP("all_ones", "%#lx", f->ReadReg(ISEL_EIDELIVERY));
    f->WriteReg(ISEL_EIDELIVERY, 0);
    EXPECT_EQ(f->ReadReg(ISEL_EIDELIVERY), 0ul);
}

TEST(imsic, eithreshold)
{
    Imsic *f = need_imsic();
    f->WriteReg(ISEL_EITHRESHOLD, 5);
    EXPECT_EQ(f->ReadReg(ISEL_EITHRESHOLD), 5ul);
    f->WriteReg(ISEL_EITHRESHOLD, f->IdCount() + 5);
    SNAP("past_ids", "%#lx", f->ReadReg(ISEL_EITHRESHOLD));
    f->WriteReg(ISEL_EITHRESHOLD, ~0ul);
    SNAP("all_ones", "%#lx", f->ReadReg(ISEL_EITHRESHOLD));
}

TEST(imsic, arrays)
{
    Imsic *f = need_imsic();
    f->WriteReg(ISEL_EIE0, ~0ul);
    SNAP("eie0", "%#lx", f->ReadReg(ISEL_EIE0));
    f->WriteReg(ISEL_EIP0, ~0ul);
    SNAP("eip0", "%#lx", f->ReadReg(ISEL_EIP0));
    /* identity 0 does not exist */
    EXPECT_EQ(f->ReadReg(ISEL_EIP0) & 1, 0ul);
    /* past the last identity: a null register */
    unsigned past = Imsic::SelectFor(ISEL_EIE0, f->IdCount() + 1 + 64);
    if (past <= 0xff) {
        f->WriteReg(past, ~0ul);
        SNAP("past_ids", "%#lx", f->ReadReg(past));
    }
}

TEST(imsic, msi_sets_pending)
{
    Imsic *f = need_imsic();
    send(f, 5);
    EXPECT_TRUE(f->Pending(5));
    /* not enabled: neither top nor pending at the hart */
    EXPECT_EQ(f->Topei(), 0ul);
    f->SetEnabled(5, true);
    EXPECT_EQ(f->Topei(), 5ul << 16 | 5);
    EXPECT_FALSE(ext_pending());
    f->WriteReg(ISEL_EIDELIVERY, 1);
    EXPECT_TRUE(ext_pending());
    f->ClaimTop();
    EXPECT_FALSE(f->Pending(5));
    EXPECT_FALSE(ext_pending());
}

TEST(imsic, lowest_identity_first)
{
    Imsic *f = need_imsic();
    f->SetEnabled(3, true);
    f->SetEnabled(9, true);
    send(f, 9);
    send(f, 3);
    EXPECT_EQ(f->Topei() >> 16, 3ul);
    f->ClaimTop();
    EXPECT_EQ(f->Topei() >> 16, 9ul);
    f->ClaimTop();
    EXPECT_EQ(f->Topei(), 0ul);
}

TEST(imsic, threshold)
{
    Imsic *f = need_imsic();
    f->SetEnabled(3, true);
    f->SetEnabled(5, true);
    f->WriteReg(ISEL_EIDELIVERY, 1);
    send(f, 5);
    f->WriteReg(ISEL_EITHRESHOLD, 4);
    EXPECT_EQ(f->Topei(), 0ul);
    EXPECT_FALSE(ext_pending());
    send(f, 3);
    EXPECT_EQ(f->Topei() >> 16, 3ul);
    /* a lower identity under the threshold hides a higher one */
    f->ClaimTop();
    EXPECT_EQ(f->Topei(), 0ul);
    f->WriteReg(ISEL_EITHRESHOLD, 0);
    EXPECT_EQ(f->Topei() >> 16, 5ul);
}

TEST(imsic, set_pending_by_csr)
{
    Imsic *f = need_imsic();
    f->SetEnabled(7, true);
    f->SetPending(7, true);
    EXPECT_EQ(f->Topei() >> 16, 7ul);
    f->SetPending(7, false);
    EXPECT_EQ(f->Topei(), 0ul);
}

TEST(imsic, page)
{
    Imsic *f = need_imsic();
    SNAP("read", "%#x", mmio_read32(f->MyPage()));
    /* only the little endian register at offset 0 takes identities */
    mmio_write32(f->MyPage() + 4, 6);
    SNAP("be_write", "%d", f->Pending(6));
    send(f, f->IdCount() + 1);
    SNAP("past_ids_topei", "%#lx", f->Topei());
    mmio_write8(f->MyPage(), 8);
    SNAP("write8", "%d", f->Pending(8));
}

TEST(imsic, missing_registers)
{
    Imsic *f = need_imsic();
    (void)f;
    TrapInfo info;
    /* RV64 numbers only the even array registers */
    sSelect = ISEL_EIP0 + 1;
    SNAP("odd_eip", "%s", t_trap(read_select, nullptr, &info) ? "trap" : "ok");
    sSelect = 0x30;
    SNAP("iprio0", "%s", t_trap(read_select, nullptr, &info) ? "trap" : "ok");
    sSelect = 0x71;
    SNAP("reserved_71", "%s", t_trap(read_select) ? "trap" : "ok");
    sSelect = 0x10;
    SNAP("below_30", "%s", t_trap(read_select) ? "trap" : "ok");
}

TEST(imsic, interrupt)
{
    Imsic *f = need_imsic();
    static volatile int count;
    count = 0;
    ASSERT_TRUE(f->Route(12, [](void *) { count = count + 1; }, nullptr));
    arch_irq_enable();
    send(f, 12);
    EXPECT_TRUE(t_wait_until([] { return count > 0; }));
    arch_irq_disable();
    EXPECT_EQ(count, 1);
    EXPECT_FALSE(f->Pending(12));
}

TEST(imsic, other_hart)
{
    Imsic *f = need_imsic();
    uint64_t other = 0;
    for (uint32_t hart = 0; hart < 64 && !other; hart++) {
        if (hart != gBoot.boot_cpu)
            other = f->PageOf(hart);
    }
    if (other == 0)
        t_skip("one hart");
    mmio_write32(other, 11);
    EXPECT_FALSE(f->Pending(11));
}

namespace {

volatile int sApReady;
volatile int sApCount;

/* On the other hart, in its own file: identity 14 on, one interrupt of it
   or a while */
void ap_take_msi(void *)
{
    Imsic *f = Imsic::Mine();
    f->SetEnabled(14, true);
    f->WriteReg(ISEL_EITHRESHOLD, 0);
    f->WriteReg(ISEL_EIDELIVERY, 1);
    riscv_ie_set(1ul << riscv_ext_irq());
    __atomic_store_n(&sApReady, 1, __ATOMIC_RELEASE);
    arch_irq_enable();
    uint64_t end = t_now_us() + 200000;
    while (sApCount == 0 && t_now_us() < end)
        arch_cpu_relax();
    arch_irq_disable();
    riscv_ie_clear(1ul << riscv_ext_irq());
    f->WriteReg(ISEL_EIDELIVERY, 0);
    f->SetEnabled(14, false);
}

}

TEST(imsic, msi_to_other_hart)
{
    Imsic *f = need_imsic();
    int other = t_require_other_cpu();
    uint64_t page = f->PageOf(arch_cpu_hw_id(other));
    ASSERT_NE(page, 0u);
    sApReady = 0;
    sApCount = 0;
    arch_set_irq_handler(riscv_ext_irq(), [](void *) {
        Imsic *f = Imsic::Mine();
        if ((f->Topei() >> 16) == 14)
            sApCount = sApCount + 1;
        f->ClaimTop();
    }, nullptr);
    ASSERT_TRUE(smp_post(other, ap_take_msi, nullptr));
    t_wait_until([] { return __atomic_load_n(&sApReady, __ATOMIC_ACQUIRE); });
    mmio_write32(page, 14);
    EXPECT_TRUE(smp_wait(other));
    EXPECT_EQ(sApCount, 1);
    EXPECT_FALSE(f->Pending(14));
}

TEST(imsic, supervisor_file_from_m)
{
    t_require_mode("M");
    Imsic *f = need_imsic();
    (void)f;
    /* the supervisor's page for this hart */
    uint32_t intc = hart_intc_phandle(gBoot.boot_cpu);
    uint64_t page = 0;
    for (const FdtNode *n = fdt_find_compatible("riscv,imsics"); n && !page;
         n = fdt_find_compatible("riscv,imsics", n)) {
        const FdtProp *ext = fdt_prop(n, "interrupts-extended");
        uint64_t base, size;
        if (!fdt_reg(n, 0, &base, &size))
            continue;
        for (uint32_t i = 0; ext && 8 * i + 8 <= ext->len; i++) {
            if (be32(ext->data + 8 * i) == intc &&
                be32(ext->data + 8 * i + 4) == IRQ_S_EXT)
                page = base + 0x1000ull * i;
        }
    }
    if (page == 0)
        t_skip("no supervisor interrupt file");
    mmio_write32(page, 4);
    CSR_WRITE(0x150, Imsic::SelectFor(ISEL_EIP0, 4)); /* siselect */
    EXPECT_EQ((CSR_READ(0x151) >> Imsic::BitFor(4)) & 1, 1ul); /* sireg */
    CSR_WRITE(0x151, 0);
    /* and not the machine file */
    EXPECT_FALSE(Imsic::Mine()->Pending(4));
}

#endif
