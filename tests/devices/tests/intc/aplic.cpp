/*
 * APLIC tests
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

#include "aplic.h"
#include "console.h"
#include "format.h"
#include "imsic.h"
#include "mmio.h"
#include "riscv.h"
#include "test.h"
#include "testctl.h"

/* The domain of the privilege the image runs at, with the test-control
   lines as sources. */

namespace {

struct Setup {
    Aplic *a;
    Aplic::Domain d;
    int hart;
    bool msi;
    uint32_t src[4];

    uint32_t rd(uint32_t off) const {return a->Read(d, off);}
    void wr(uint32_t off, uint32_t v) const {a->Write(d, off, v);}
    bool pending(uint32_t s) const
        {return (rd(APLIC_SETIP(s / 32)) >> (s % 32)) & 1;}
    bool rectified(uint32_t s) const
        {return (rd(APLIC_IN_CLRIP(s / 32)) >> (s % 32)) & 1;}
    bool enabled(uint32_t s) const
        {return (rd(APLIC_SETIE(s / 32)) >> (s % 32)) & 1;}
    uint32_t idc(uint32_t reg) const {return a->Idc(d, reg);}
    void set_idc(uint32_t reg, uint32_t v) const {a->SetIdc(d, reg, v);}
};

Setup need_aplic(int lines)
{
    Setup s {};
    s.a = Aplic::First();
    if (s.a == nullptr)
        t_skip("no APLIC");
    s.d = s.a->Mine();
    if (s.d.node == nullptr)
        t_skip("no APLIC domain at this privilege");
    s.hart = s.a->HartIndex(s.d);
    if (s.hart < 0)
        t_skip("the domain does not reach this hart");
    s.msi = s.a->MsiMode();
    t_require_tc_irqs(lines);
    const HwDevice *tc = test_control_device();
    for (int i = 0; i < lines; i++) {
        IntController *c;
        if (!intc_resolve(tc, i, &c, &s.src[i]) || c != s.a)
            t_skip("test-control line %d is not on the APLIC", i);
    }
    return s;
}

Setup need_direct(int lines)
{
    Setup s = need_aplic(lines);
    if (s.msi)
        t_skip("MSI delivery");
    return s;
}

Setup need_msi(int lines)
{
    Setup s = need_aplic(lines);
    if (!s.msi)
        t_skip("direct delivery");
    if (Imsic::Mine() == nullptr)
        t_skip("no interrupt file at this privilege");
    return s;
}

bool ext_pending()
{
    return (riscv_ip() >> riscv_ext_irq()) & 1;
}

/* a source in a mode, enabled, at priority 'prio' or identity 'eiid' */
void configure(const Setup &s, uint32_t src, uint32_t mode, uint32_t low)
{
    s.wr(APLIC_SOURCECFG(src), mode);
    s.wr(APLIC_TARGET(src), APLIC_TARGET_HART(s.hart) | low);
    s.wr(APLIC_SETIENUM, src);
}

void deliver(const Setup &s)
{
    s.wr(APLIC_DOMAINCFG, s.rd(APLIC_DOMAINCFG) | APLIC_DOMAINCFG_IE);
    if (!s.msi) {
        s.set_idc(APLIC_IDELIVERY, 1);
        s.set_idc(APLIC_ITHRESHOLD, 0);
    }
}

}


FRESH_TEST(aplic, reset_state)
{
    Setup s = need_aplic(1);
    SNAP("domaincfg", "%#x", s.rd(APLIC_DOMAINCFG));
    SNAP("sourcecfg", "%#x", s.rd(APLIC_SOURCECFG(s.src[0])));
    SNAP("target", "%#x", s.rd(APLIC_TARGET(s.src[0])));
    SNAP("setip0", "%#x", s.rd(APLIC_SETIP(0)));
    SNAP("setie0", "%#x", s.rd(APLIC_SETIE(0)));
    SNAP("sources", "%u", s.a->SourceCount());
    if (!s.msi) {
        SNAP("idelivery", "%#x", s.idc(APLIC_IDELIVERY));
        SNAP("iforce", "%#x", s.idc(APLIC_IFORCE));
        SNAP("ithreshold", "%#x", s.idc(APLIC_ITHRESHOLD));
        SNAP("topi", "%#x", s.idc(APLIC_TOPI));
    }
    if (s.a->HasRoot() && s.a->Child().node) {
        SNAP("child_domaincfg", "%#x",
             s.a->Read(s.a->Child(), APLIC_DOMAINCFG));
        SNAP("child_sourcecfg", "%#x",
             s.a->Read(s.a->Child(), APLIC_SOURCECFG(s.src[0])));
    }
}

TEST(aplic, domaincfg)
{
    Setup s = need_aplic(1);
    uint32_t dm = s.rd(APLIC_DOMAINCFG) & APLIC_DOMAINCFG_DM;
    s.wr(APLIC_DOMAINCFG, 0xffffffff);
    SNAP("all_ones", "%#x", s.rd(APLIC_DOMAINCFG));
    /* the delivery mode and the byte order are the machine's */
    EXPECT_EQ(s.rd(APLIC_DOMAINCFG) & APLIC_DOMAINCFG_DM, dm);
    EXPECT_EQ(s.rd(APLIC_DOMAINCFG) & APLIC_DOMAINCFG_BE, 0u);
    s.wr(APLIC_DOMAINCFG, 0);
    SNAP("zero", "%#x", s.rd(APLIC_DOMAINCFG));
}

TEST(aplic, sourcecfg_modes)
{
    Setup s = need_aplic(1);
    char buf[128];
    size_t n = 0;
    for (uint32_t mode = 0; mode < 8; mode++) {
        s.wr(APLIC_SOURCECFG(s.src[0]), mode);
        n += snprintf(buf + n, sizeof(buf) - n, "%s%u", mode ? " " : "",
                      s.rd(APLIC_SOURCECFG(s.src[0])));
    }
    SNAP("modes", "%s", buf);
    s.wr(APLIC_SOURCECFG(s.src[0]), 0xffffffff);
    SNAP("all_ones", "%#x", s.rd(APLIC_SOURCECFG(s.src[0])));
    s.wr(APLIC_SOURCECFG(s.src[0]), 0);
}

TEST(aplic, inactive_source)
{
    Setup s = need_aplic(1);
    uint32_t src = s.src[0];
    s.wr(APLIC_SOURCECFG(src), APLIC_SM_INACTIVE);
    s.wr(APLIC_SETIENUM, src);
    s.wr(APLIC_TARGET(src), APLIC_TARGET_HART(s.hart) | 5);
    s.wr(APLIC_SETIPNUM, src);
    tc_irq_raise(0);
    EXPECT_FALSE(s.enabled(src));
    EXPECT_EQ(s.rd(APLIC_TARGET(src)), 0u);
    EXPECT_FALSE(s.pending(src));
    EXPECT_FALSE(s.rectified(src));
}

TEST(aplic, target_fields)
{
    Setup s = need_aplic(1);
    uint32_t src = s.src[0];
    s.wr(APLIC_SOURCECFG(src), APLIC_SM_EDGE1);
    s.wr(APLIC_TARGET(src), 0xffffffff);
    SNAP("all_ones", "%#x", s.rd(APLIC_TARGET(src)));
    s.wr(APLIC_TARGET(src), 0);
    SNAP("zero", "%#x", s.rd(APLIC_TARGET(src)));
    /* going inactive clears it */
    s.wr(APLIC_TARGET(src), APLIC_TARGET_HART(s.hart) | 3);
    s.wr(APLIC_SOURCECFG(src), APLIC_SM_INACTIVE);
    s.wr(APLIC_SOURCECFG(src), APLIC_SM_EDGE1);
    SNAP("after_inactive", "%#x", s.rd(APLIC_TARGET(src)));
}

TEST(aplic, level_follows_wire)
{
    Setup s = need_direct(1);
    uint32_t src = s.src[0];
    s.wr(APLIC_SOURCECFG(src), APLIC_SM_LEVEL1);
    EXPECT_FALSE(s.pending(src));
    tc_irq_raise(0);
    EXPECT_TRUE(s.pending(src));
    EXPECT_TRUE(s.rectified(src));
    /* in direct delivery a level source's pending bit is its wire */
    s.wr(APLIC_CLRIPNUM, src);
    EXPECT_TRUE(s.pending(src));
    tc_irq_lower(0);
    EXPECT_FALSE(s.pending(src));
    s.wr(APLIC_SETIPNUM, src);
    EXPECT_FALSE(s.pending(src));
}

TEST(aplic, level_low)
{
    Setup s = need_aplic(1);
    uint32_t src = s.src[0];
    s.wr(APLIC_SOURCECFG(src), APLIC_SM_LEVEL0);
    EXPECT_TRUE(s.rectified(src));
    if (!s.msi)
        EXPECT_TRUE(s.pending(src));
    tc_irq_raise(0);
    EXPECT_FALSE(s.rectified(src));
    EXPECT_FALSE(s.pending(src));
}

TEST(aplic, edge_latched)
{
    Setup s = need_aplic(1);
    uint32_t src = s.src[0];
    s.wr(APLIC_SOURCECFG(src), APLIC_SM_EDGE1);
    EXPECT_FALSE(s.pending(src));
    tc_irq_raise(0);
    EXPECT_TRUE(s.pending(src));
    EXPECT_TRUE(s.rectified(src));
    tc_irq_lower(0);
    EXPECT_TRUE(s.pending(src));
    EXPECT_FALSE(s.rectified(src));
    s.wr(APLIC_CLRIPNUM, src);
    EXPECT_FALSE(s.pending(src));
    s.wr(APLIC_SETIPNUM, src);
    EXPECT_TRUE(s.pending(src));
    s.wr(APLIC_IN_CLRIP(src / 32), 1u << (src % 32));
    EXPECT_FALSE(s.pending(src));
    s.wr(APLIC_SETIP(src / 32), 1u << (src % 32));
    EXPECT_TRUE(s.pending(src));
    s.wr(APLIC_CLRIPNUM, src);
    s.wr(APLIC_SETIPNUM_LE, src);
    EXPECT_TRUE(s.pending(src));
}

TEST(aplic, edge_falling)
{
    Setup s = need_aplic(1);
    uint32_t src = s.src[0];
    s.wr(APLIC_SOURCECFG(src), APLIC_SM_EDGE0);
    /* a low wire is a high rectified input, but no edge yet */
    EXPECT_TRUE(s.rectified(src));
    SNAP("pending_at_mode_change", "%d", s.pending(src));
    s.wr(APLIC_CLRIPNUM, src);
    tc_irq_raise(0);
    EXPECT_FALSE(s.pending(src));
    tc_irq_lower(0);
    EXPECT_TRUE(s.pending(src));
}

TEST(aplic, detached)
{
    Setup s = need_aplic(1);
    uint32_t src = s.src[0];
    s.wr(APLIC_SOURCECFG(src), APLIC_SM_DETACHED);
    tc_irq_raise(0);
    EXPECT_FALSE(s.pending(src));
    EXPECT_FALSE(s.rectified(src));
    s.wr(APLIC_SETIPNUM, src);
    EXPECT_TRUE(s.pending(src));
    s.wr(APLIC_CLRIPNUM, src);
    EXPECT_FALSE(s.pending(src));
}

TEST(aplic, enable_bitmaps)
{
    Setup s = need_aplic(1);
    uint32_t src = s.src[0];
    s.wr(APLIC_SOURCECFG(src), APLIC_SM_EDGE1);
    s.wr(APLIC_SETIENUM, src);
    EXPECT_TRUE(s.enabled(src));
    s.wr(APLIC_CLRIENUM, src);
    EXPECT_FALSE(s.enabled(src));
    s.wr(APLIC_SETIE(src / 32), 1u << (src % 32));
    EXPECT_TRUE(s.enabled(src));
    s.wr(APLIC_CLRIE(src / 32), 1u << (src % 32));
    EXPECT_FALSE(s.enabled(src));
    /* the write-only registers read zero */
    s.wr(APLIC_SETIENUM, src);
    SNAP("clrie_read", "%#x", s.rd(APLIC_CLRIE(src / 32)));
    SNAP("setienum_read", "%#x", s.rd(APLIC_SETIENUM));
    SNAP("setipnum_read", "%#x", s.rd(APLIC_SETIPNUM));
}

TEST(aplic, topi_priority)
{
    Setup s = need_direct(2);
    configure(s, s.src[0], APLIC_SM_LEVEL1, 5);
    configure(s, s.src[1], APLIC_SM_LEVEL1, 2);
    deliver(s);
    tc_irq_set_levels(3);
    /* the lower priority number wins */
    EXPECT_EQ(s.idc(APLIC_TOPI), s.src[1] << 16 | 2);
    EXPECT_TRUE(ext_pending());
    EXPECT_EQ(s.idc(APLIC_CLAIMI), s.src[1] << 16 | 2);
    /* a level source stays pending while its wire is high */
    EXPECT_EQ(s.idc(APLIC_TOPI), s.src[1] << 16 | 2);
    tc_irq_lower(1);
    EXPECT_EQ(s.idc(APLIC_TOPI), s.src[0] << 16 | 5);
    tc_irq_lower(0);
    EXPECT_EQ(s.idc(APLIC_TOPI), 0u);
    EXPECT_FALSE(ext_pending());
}

TEST(aplic, equal_priority)
{
    Setup s = need_direct(2);
    configure(s, s.src[0], APLIC_SM_LEVEL1, 4);
    configure(s, s.src[1], APLIC_SM_LEVEL1, 4);
    deliver(s);
    tc_irq_set_levels(3);
    EXPECT_EQ(s.idc(APLIC_TOPI) >> 16, min(s.src[0], s.src[1]));
}

TEST(aplic, other_hart_target)
{
    Setup s = need_direct(1);
    const FdtProp *ext = fdt_prop(s.d.node, "interrupts-extended");
    if (ext == nullptr || ext->len < 16)
        t_skip("one hart");
    int other = s.hart == 0 ? 1 : 0;
    s.wr(APLIC_SOURCECFG(s.src[0]), APLIC_SM_LEVEL1);
    s.wr(APLIC_TARGET(s.src[0]), APLIC_TARGET_HART(other) | 1);
    s.wr(APLIC_SETIENUM, s.src[0]);
    deliver(s);
    tc_irq_raise(0);
    EXPECT_EQ(s.idc(APLIC_TOPI), 0u);
    EXPECT_FALSE(ext_pending());
    EXPECT_EQ(s.a->Read(s.d, APLIC_IDC(other) + APLIC_TOPI),
              s.src[0] << 16 | 1);
}

TEST(aplic, idc_read_only)
{
    Setup s = need_direct(1);
    configure(s, s.src[0], APLIC_SM_EDGE1, 1);
    deliver(s);
    tc_irq_raise(0);
    /* topi and claimi are read-only: writing claims nothing */
    s.set_idc(APLIC_TOPI, 0);
    s.set_idc(APLIC_CLAIMI, 0);
    EXPECT_TRUE(s.pending(s.src[0]));
    EXPECT_EQ(s.idc(APLIC_TOPI), s.src[0] << 16 | 1);
}

TEST(aplic, setipnum_be)
{
    Setup s = need_aplic(1);
    s.wr(APLIC_SOURCECFG(s.src[0]), APLIC_SM_EDGE1);
    /* the big endian register, given the source's number byte swapped */
    uint32_t v = s.src[0];
    s.wr(APLIC_SETIPNUM_BE, __builtin_bswap32(v));
    SNAP("be", "%d", s.pending(s.src[0]));
    s.wr(APLIC_CLRIPNUM, s.src[0]);
    s.wr(APLIC_SETIPNUM_BE, v);
    SNAP("native", "%d", s.pending(s.src[0]));
}

TEST(aplic, genmsi_direct)
{
    Setup s = need_direct(1);
    s.wr(APLIC_GENMSI, APLIC_TARGET_HART(s.hart) | 33);
    SNAP("read", "%#x", s.rd(APLIC_GENMSI));
}

TEST(aplic, claimi_clears_edge)
{
    Setup s = need_direct(1);
    uint32_t src = s.src[0];
    configure(s, src, APLIC_SM_EDGE1, 1);
    deliver(s);
    tc_irq_raise(0);
    EXPECT_EQ(s.idc(APLIC_CLAIMI), src << 16 | 1);
    EXPECT_FALSE(s.pending(src));
    EXPECT_EQ(s.idc(APLIC_TOPI), 0u);
    EXPECT_EQ(s.idc(APLIC_CLAIMI), 0u);
}

TEST(aplic, ithreshold)
{
    Setup s = need_direct(1);
    uint32_t src = s.src[0];
    configure(s, src, APLIC_SM_LEVEL1, 3);
    deliver(s);
    tc_irq_raise(0);
    s.set_idc(APLIC_ITHRESHOLD, 3);
    EXPECT_EQ(s.idc(APLIC_TOPI), 0u);
    EXPECT_FALSE(ext_pending());
    s.set_idc(APLIC_ITHRESHOLD, 4);
    EXPECT_EQ(s.idc(APLIC_TOPI), src << 16 | 3);
    s.set_idc(APLIC_ITHRESHOLD, 0);
    EXPECT_EQ(s.idc(APLIC_TOPI), src << 16 | 3);
    s.set_idc(APLIC_ITHRESHOLD, 0xffffffff);
    SNAP("mask", "%#x", s.idc(APLIC_ITHRESHOLD));
}

TEST(aplic, idelivery_and_iforce)
{
    Setup s = need_direct(1);
    uint32_t src = s.src[0];
    configure(s, src, APLIC_SM_LEVEL1, 1);
    deliver(s);
    tc_irq_raise(0);
    EXPECT_TRUE(ext_pending());
    s.set_idc(APLIC_IDELIVERY, 0);
    EXPECT_FALSE(ext_pending());
    s.set_idc(APLIC_IDELIVERY, 0xffffffff);
    SNAP("idelivery_mask", "%#x", s.idc(APLIC_IDELIVERY));
    tc_irq_lower(0);
    EXPECT_FALSE(ext_pending());
    /* iforce raises the line with nothing pending */
    s.set_idc(APLIC_IFORCE, 1);
    EXPECT_TRUE(ext_pending());
    /* and a claim of nothing clears it */
    EXPECT_EQ(s.idc(APLIC_CLAIMI), 0u);
    EXPECT_EQ(s.idc(APLIC_IFORCE), 0u);
    EXPECT_FALSE(ext_pending());
}

TEST(aplic, domain_ie)
{
    Setup s = need_direct(1);
    configure(s, s.src[0], APLIC_SM_LEVEL1, 1);
    s.set_idc(APLIC_IDELIVERY, 1);
    tc_irq_raise(0);
    EXPECT_FALSE(ext_pending());
    s.wr(APLIC_DOMAINCFG, APLIC_DOMAINCFG_IE);
    EXPECT_TRUE(ext_pending());
    s.wr(APLIC_DOMAINCFG, 0);
    EXPECT_FALSE(ext_pending());
}

TEST(aplic, interrupt)
{
    Setup s = need_aplic(1);
    static volatile int count;
    count = 0;
    ASSERT_TRUE(s.a->Route(s.src[0], [](void *) {
        count = count + 1;
        tc_irq_lower(0);
    }, nullptr));
    arch_irq_enable();
    tc_irq_raise(0);
    EXPECT_TRUE(t_wait_until([] { return count > 0; }));
    t_delay_us(1000);
    arch_irq_disable();
    EXPECT_EQ(count, 1);
}

TEST(aplic, delegation)
{
    t_require_mode("M");
    Setup s = need_aplic(1);
    if (!s.a->HasRoot() || s.a->Child().node == nullptr)
        t_skip("no child domain");
    const Aplic::Domain &root = s.a->Root(), &child = s.a->Child();
    uint32_t src = s.src[0];
    s.a->Write(root, APLIC_SOURCECFG(src), APLIC_SOURCECFG_D | 7);
    SNAP("root_delegated", "%#x", s.a->Read(root, APLIC_SOURCECFG(src)));
    /* the source is now the child's */
    s.a->Write(child, APLIC_SOURCECFG(src), APLIC_SM_EDGE1);
    EXPECT_EQ(s.a->Read(child, APLIC_SOURCECFG(src)), (uint32_t)APLIC_SM_EDGE1);
    tc_irq_raise(0);
    EXPECT_TRUE((s.a->Read(child, APLIC_SETIP(src / 32)) >> (src % 32)) & 1);
    EXPECT_FALSE((s.a->Read(root, APLIC_SETIP(src / 32)) >> (src % 32)) & 1);
    /* and the root's own registers for it are inactive */
    s.a->Write(root, APLIC_SETIENUM, src);
    EXPECT_FALSE((s.a->Read(root, APLIC_SETIE(src / 32)) >> (src % 32)) & 1);
    /* a leaf domain cannot delegate */
    s.a->Write(child, APLIC_SOURCECFG(src), APLIC_SOURCECFG_D);
    SNAP("leaf_delegate", "%#x", s.a->Read(child, APLIC_SOURCECFG(src)));
    /* taking it back makes it vanish from the child */
    s.a->Write(child, APLIC_SOURCECFG(src), APLIC_SM_EDGE1);
    s.a->Write(root, APLIC_SOURCECFG(src), APLIC_SM_LEVEL1);
    EXPECT_EQ(s.a->Read(child, APLIC_SOURCECFG(src)), 0u);
    EXPECT_TRUE((s.a->Read(root, APLIC_SETIP(src / 32)) >> (src % 32)) & 1 ||
                s.msi);
}

TEST(aplic, undelegated_in_child)
{
    t_require_mode("M");
    Setup s = need_aplic(1);
    if (!s.a->HasRoot() || s.a->Child().node == nullptr)
        t_skip("no child domain");
    const Aplic::Domain &child = s.a->Child();
    uint32_t src = s.src[0];
    /* not delegated: looks unimplemented to the child */
    s.a->Write(child, APLIC_SOURCECFG(src), APLIC_SM_EDGE1);
    EXPECT_EQ(s.a->Read(child, APLIC_SOURCECFG(src)), 0u);
}

TEST(aplic, past_last_source)
{
    Setup s = need_aplic(1);
    uint32_t past = s.a->SourceCount() + 1;
    s.wr(APLIC_SOURCECFG(past), APLIC_SM_EDGE1);
    SNAP("sourcecfg", "%#x", s.rd(APLIC_SOURCECFG(past)));
    s.wr(APLIC_TARGET(past), 5);
    SNAP("target", "%#x", s.rd(APLIC_TARGET(past)));
    s.wr(APLIC_SOURCECFG(0), APLIC_SM_EDGE1);
    SNAP("source0", "%#x", s.rd(APLIC_SOURCECFG(0)));
}

TEST(aplic, past_last_idc)
{
    Setup s = need_direct(1);
    /* the IDC after the last hart's, inside the domain's region */
    const FdtProp *ext = fdt_prop(s.d.node, "interrupts-extended");
    uint32_t idc = APLIC_IDC(ext ? ext->len / 8 : 1);
    s.wr(idc + APLIC_IDELIVERY, 1);
    SNAP("idelivery", "%#x", s.rd(idc + APLIC_IDELIVERY));
}

TEST(aplic, msi_delivery)
{
    Setup s = need_msi(1);
    Imsic *f = Imsic::Mine();
    uint32_t src = s.src[0], eiid = 40;
    f->SetEnabled(eiid, true);
    configure(s, src, APLIC_SM_EDGE1, eiid);
    tc_irq_raise(0);
    /* pending while the domain sends nothing */
    EXPECT_TRUE(s.pending(src));
    EXPECT_FALSE(f->Pending(eiid));
    deliver(s);
    /* sent, which clears it */
    EXPECT_FALSE(s.pending(src));
    EXPECT_TRUE(f->Pending(eiid));
    EXPECT_EQ(f->Topei() >> 16, eiid);
    f->ClaimTop();
    EXPECT_FALSE(f->Pending(eiid));
}

TEST(aplic, msi_level)
{
    Setup s = need_msi(1);
    uint32_t src = s.src[0];
    s.wr(APLIC_SOURCECFG(src), APLIC_SM_LEVEL1);
    /* only while the wire is asserted */
    s.wr(APLIC_SETIPNUM, src);
    EXPECT_FALSE(s.pending(src));
    tc_irq_raise(0);
    EXPECT_TRUE(s.pending(src));
    s.wr(APLIC_CLRIPNUM, src);
    EXPECT_FALSE(s.pending(src));
    s.wr(APLIC_SETIPNUM, src);
    EXPECT_TRUE(s.pending(src));
    tc_irq_lower(0);
    EXPECT_FALSE(s.pending(src));
}

TEST(aplic, genmsi)
{
    Setup s = need_msi(1);
    Imsic *f = Imsic::Mine();
    uint32_t eiid = 33;
    s.wr(APLIC_GENMSI, APLIC_TARGET_HART(s.hart) | eiid);
    EXPECT_TRUE(f->Pending(eiid));
    SNAP("read", "%#x", s.rd(APLIC_GENMSI) & ~APLIC_TARGET_HART(0x3fff));
}

TEST(aplic, msiaddrcfg)
{
    Setup s = need_aplic(1);
    if (!s.msi || !s.a->HasRoot())
        t_skip("needs the root domain in MSI delivery");
    const Aplic::Domain &root = s.a->Root();
    uint32_t h = s.a->Read(root, APLIC_MMSIADDRCFGH);
    /* locked, with the hart index width */
    SNAP("mmsiaddrcfgh_flags", "%#x", h & 0xfffff000);
    Imsic *f = Imsic::Mine();
    uint64_t ppn = s.a->Read(root, APLIC_MMSIADDRCFG) |
                   (uint64_t)(h & 0xfff) << 32;
    /* the first hart's page; the hart index selects the others */
    EXPECT_EQ(ppn << 12, f->MyPage() - 0x1000ull * s.hart);
    s.a->Write(root, APLIC_MMSIADDRCFG, 0);
    EXPECT_EQ(s.a->Read(root, APLIC_MMSIADDRCFG), (uint32_t)ppn);
    /* the supervisor files' page number, with the same hart numbering */
    uint64_t sppn = s.a->Read(root, APLIC_SMSIADDRCFG) |
                    (uint64_t)(s.a->Read(root, APLIC_SMSIADDRCFGH) & 0xfff)
                        << 32;
    SNAP("smsiaddrcfgh_flags", "%#x",
         s.a->Read(root, APLIC_SMSIADDRCFGH) & 0xfffff000);
    const FdtNode *child = s.a->Child().node;
    uint32_t msi;
    const FdtNode *simsic = child && fdt_prop_u32(child, "msi-parent", &msi)
        ? fdt_find_phandle(msi) : nullptr;
    uint64_t base, size;
    if (simsic && fdt_reg(simsic, 0, &base, &size))
        EXPECT_EQ(sppn << 12, base);
    if (s.a->Child().node)
        SNAP("child_mmsiaddrcfg", "%#x",
             s.a->Read(s.a->Child(), APLIC_MMSIADDRCFG));
}

TEST(aplic, narrow_access)
{
    Setup s = need_aplic(1);
    uint64_t cfg = s.d.base + APLIC_DOMAINCFG;
    SNAP("read8", "%#x", mmio_read8(cfg + 3));
    SNAP("read16", "%#x", mmio_read16(cfg + 2));
}

#endif
