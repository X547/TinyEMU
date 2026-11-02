/*
 * xHCI host controller tests
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
#include "format.h"
#include "heap.h"
#include "test.h"
#include "usb.h"

/* The host controller itself, by the eXtensible Host Controller Interface
   specification, revision 1.2: its registers (5), rings (4.9), commands
   (4.6) and root hub ports (4.19), with the devices behind it used only as
   something to address. */

namespace {

int require_hosts()
{
    int n = xhci_count();
    if (n == 0)
        t_skip("no xHCI host");
    return n;
}

void snap_host(Xhci *hc, const char *key, const char *fmt, uint32_t v)
{
    char k[48];
    snprintf(k, sizeof(k), "%s.%s", hc->Name(), key);
    t_snap(k, fmt, v);
}

/* The ports with something attached, by the change they reported. */
bool connected(Xhci *hc, uint32_t port)
{
    return (hc->Portsc(port) & PORTSC_CCS) != 0;
}

/* Every register the specification gives a default (5.4, 5.5), with the
   ports as 4.19.1 leaves them. */
void check_defaults(Xhci *hc)
{
    EXPECT_EQ(hc->OpRd(XHCI_USBCMD), 0u);
    EXPECT_EQ(hc->OpRd(XHCI_USBSTS) & ~(uint32_t)(USBSTS_PCD | USBSTS_CNR),
              (uint32_t)USBSTS_HCH);
    EXPECT_EQ(hc->OpRd(XHCI_DNCTRL), 0u);
    EXPECT_EQ(hc->OpRd(XHCI_CRCR), 0u);
    EXPECT_EQ(hc->OpRd(XHCI_CRCR + 4), 0u);
    EXPECT_EQ(hc->OpRd(XHCI_DCBAAP), 0u);
    EXPECT_EQ(hc->OpRd(XHCI_DCBAAP + 4), 0u);
    EXPECT_EQ(hc->OpRd(XHCI_CONFIG), 0u);
    EXPECT_EQ(hc->RtRd(XHCI_IMAN), 0u);
    EXPECT_EQ(hc->RtRd(XHCI_IMOD) & 0xffff, 4000u);
    EXPECT_EQ(hc->RtRd(XHCI_ERSTSZ), 0u);
    EXPECT_EQ(hc->RtRd(XHCI_ERSTBA), 0u);
    EXPECT_EQ(hc->RtRd(XHCI_ERSTBA + 4), 0u);
    EXPECT_EQ(hc->RtRd(XHCI_ERDP), 0u);
    EXPECT_EQ(hc->RtRd(XHCI_ERDP + 4), 0u);

    bool any_change = false;
    for (uint32_t p = 1; p <= hc->max_ports; p++) {
        uint32_t v = hc->Portsc(p);
        int major = hc->PortMajor(p);
        char k[16];
        snprintf(k, sizeof(k), "portsc%u", p);
        snap_host(hc, k, "%#x", v);
        /* no port power switches: powered (5.4.8) */
        EXPECT_TRUE(v & PORTSC_PP);
        EXPECT_EQ(v & (PORTSC_PR | PORTSC_LWS | PORTSC_WPR | PORTSC_OCA), 0u);
        if (!(v & PORTSC_CCS)) {
            /* Disconnected, and never connected: no change */
            EXPECT_EQ(v & (PORTSC_PED | PORTSC_CHANGES), 0u);
            EXPECT_EQ(PORTSC_PLS(v), (uint32_t)PLS_RXDETECT);
            continue;
        }
        any_change |= (v & PORTSC_CSC) != 0;
        EXPECT_TRUE(v & PORTSC_CSC);
        if (major == 2) {
            /* Disabled until software resets it (4.19.1.1) */
            EXPECT_EQ(v & PORTSC_PED, 0u);
            EXPECT_EQ(PORTSC_PLS(v), (uint32_t)PLS_POLLING);
        } else {
            /* Enabled once the link trained (4.19.1.2) */
            EXPECT_TRUE(v & PORTSC_PED);
            EXPECT_EQ(PORTSC_PLS(v), (uint32_t)PLS_U0);
        }
    }
    if (any_change)
        EXPECT_TRUE(hc->OpRd(XHCI_USBSTS) & USBSTS_PCD);
}

}


//#pragma mark - capability registers

TEST(xhci, capabilities)
{
    int n = require_hosts();
    for (int h = 0; h < n; h++) {
        Xhci *hc = xhci_host(h);
        hc->ReadCaps();
        uint32_t cap = hc->Rd(XHCI_CAPLENGTH);
        uint32_t version = cap >> 16;
        /* the capability registers end at 1Ch (5.3) */
        EXPECT_GE(cap & 0xff, 0x20u);
        EXPECT_EQ(cap & 0x3, 0u);
        /* BCD (5.3.2) */
        EXPECT_GE(version, 0x0090u);
        EXPECT_LE(version, 0x0120u);
        EXPECT_LE(version & 0xf, 9u);
        EXPECT_LE((version >> 4) & 0xf, 9u);
        EXPECT_GE(hc->max_slots, 1u);
        EXPECT_GE(hc->max_intrs, 1u);
        EXPECT_LE(hc->max_intrs, 0x400u);
        EXPECT_GE(hc->max_ports, 1u);
        /* the doorbell array dword aligned, the runtime registers 32 byte
           aligned (5.3.7, 5.3.8), both past the operational ones */
        EXPECT_EQ(hc->Rd(XHCI_DBOFF) & 0x3, 0u);
        EXPECT_EQ(hc->Rd(XHCI_RTSOFF) & 0x1f, 0u);
        EXPECT_GE(hc->db, hc->op + 0x400 + 0x10 * hc->max_ports);
        EXPECT_GE(hc->rt, hc->op + 0x400 + 0x10 * hc->max_ports);
        uint32_t hcs2 = hc->Rd(XHCI_HCSPARAMS2);
        /* no scratchpad, so nothing to restore (5.3.4) */
        if (((hcs2 >> 21) & 0x1f) == 0 && (hcs2 >> 27) == 0)
            EXPECT_EQ(hcs2 & (1u << 26), 0u);
        /* at least one Supported Protocol capability (7.2) */
        EXPECT_NE(hc->FindExtCap(XECP_PROTOCOL), 0u);

        snap_host(hc, "caplength", "%#x", cap);
        snap_host(hc, "hcsparams1", "%#x", hc->Rd(XHCI_HCSPARAMS1));
        snap_host(hc, "hcsparams2", "%#x", hcs2);
        snap_host(hc, "hcsparams3", "%#x", hc->Rd(XHCI_HCSPARAMS3));
        snap_host(hc, "hccparams1", "%#x", hc->Rd(XHCI_HCCPARAMS1));
        snap_host(hc, "hccparams2", "%#x", hc->Rd(XHCI_HCCPARAMS2));
        snap_host(hc, "dboff", "%#x", hc->Rd(XHCI_DBOFF));
        snap_host(hc, "rtsoff", "%#x", hc->Rd(XHCI_RTSOFF));
        snap_host(hc, "pagesize", "%#x", hc->OpRd(XHCI_PAGESIZE));
        /* 4 KB pages at least (5.4.3) */
        EXPECT_NE(hc->OpRd(XHCI_PAGESIZE) & 0xffff, 0u);
    }
}

TEST(xhci, supported_protocols)
{
    int n = require_hosts();
    for (int h = 0; h < n; h++) {
        Xhci *hc = xhci_host(h);
        hc->ReadCaps();
        uint8_t owner[256] = {};
        uint32_t highest = 0;
        for (int i = 0;; i++) {
            uint32_t off = hc->FindExtCap(XECP_PROTOCOL, i);
            if (off == 0)
                break;
            uint32_t d0 = hc->Rd(off), d2 = hc->Rd(off + 8);
            uint32_t major = d0 >> 24, minor = (d0 >> 16) & 0xff;
            uint32_t first = d2 & 0xff, count = (d2 >> 8) & 0xff;
            /* "USB " (7.2.2) */
            EXPECT_EQ(hc->Rd(off + 4), 0x20425355u);
            EXPECT_TRUE(major == 2 || major == 3);
            EXPECT_GE(first, 1u);
            EXPECT_GE(count, 1u);
            EXPECT_LE(first + count - 1, hc->max_ports);
            /* no port in two of them (7.2) */
            for (uint32_t p = first; p < first + count && p < 256; p++) {
                EXPECT_EQ(owner[p], 0);
                owner[p] = major;
            }
            highest = max(highest, first + count - 1);
            char k[24];
            snprintf(k, sizeof(k), "protocol%d", i);
            snap_host(hc, k, "%#x", major << 8 | minor);
            snprintf(k, sizeof(k), "protocol%d.ports", i);
            snap_host(hc, k, "%#x", d2);
            snprintf(k, sizeof(k), "protocol%d.slot", i);
            snap_host(hc, k, "%#x", hc->Rd(off + 12));
        }
        /* MaxPorts is the highest port a capability assigns (5.3.3) */
        EXPECT_EQ(highest, hc->max_ports);
    }
}

FRESH_TEST(xhci, reset_state)
{
    int n = require_hosts();
    for (int h = 0; h < n; h++) {
        Xhci *hc = xhci_host(h);
        hc->ReadCaps();
        ASSERT_TRUE(t_wait_until([&] {
            return !(hc->OpRd(XHCI_USBSTS) & USBSTS_CNR);
        }, 1000000));
        check_defaults(hc);
    }
}

TEST(xhci, hcrst)
{
    int n = require_hosts();
    for (int h = 0; h < n; h++) {
        Xhci *hc = xhci_host(h);
        ASSERT_TRUE(hc->Init());
        hc->OpWr(XHCI_DNCTRL, 0x2);
        hc->RtWr(XHCI_IMAN, IMAN_IE);
        hc->RtWr(XHCI_IMOD, 100);
        for (uint32_t p = 1; p <= hc->max_ports; p++) {
            if (connected(hc, p) && hc->PortMajor(p) == 2)
                hc->PortReset(p);
        }
        ASSERT_TRUE(hc->Halt());
        /* HCRST clears itself once done, and puts everything back
           (5.4.1) */
        hc->OpWr(XHCI_USBCMD, USBCMD_HCRST);
        ASSERT_TRUE(t_wait_until([&] {
            return !(hc->OpRd(XHCI_USBCMD) & USBCMD_HCRST) &&
                   !(hc->OpRd(XHCI_USBSTS) & USBSTS_CNR);
        }, 1000000));
        check_defaults(hc);
        hc->dcbaa = nullptr;
    }
}

TEST(xhci, register_writes)
{
    int n = require_hosts();
    for (int h = 0; h < n; h++) {
        Xhci *hc = xhci_host(h);
        hc->ReadCaps();
        ASSERT_TRUE(hc->Reset());
        hc->dcbaa = (uint64_t *)1; /* reset it after */

        /* halted: every writable bit but Run/Stop and the resets */
        hc->OpWr(XHCI_USBCMD, ~(uint32_t)(USBCMD_RS | USBCMD_HCRST |
                                          1u << 7 | 1u << 8 | 1u << 9));
        uint32_t cmd = hc->OpRd(XHCI_USBCMD);
        snap_host(hc, "usbcmd", "%#x", cmd);
        EXPECT_EQ(cmd & (USBCMD_INTE | USBCMD_HSEE),
                  (uint32_t)(USBCMD_INTE | USBCMD_HSEE));
        /* still halted */
        EXPECT_TRUE(hc->OpRd(XHCI_USBSTS) & USBSTS_HCH);
        hc->OpWr(XHCI_USBCMD, 0);
        EXPECT_EQ(hc->OpRd(XHCI_USBCMD), 0u);

        /* RO */
        uint32_t pagesize = hc->OpRd(XHCI_PAGESIZE);
        hc->OpWr(XHCI_PAGESIZE, ~pagesize);
        EXPECT_EQ(hc->OpRd(XHCI_PAGESIZE), pagesize);

        /* N0-N15 (5.4.4) */
        hc->OpWr(XHCI_DNCTRL, 0xffffffff);
        EXPECT_EQ(hc->OpRd(XHCI_DNCTRL), 0xffffu);
        hc->OpWr(XHCI_DNCTRL, 0);

        /* MaxSlotsEn (5.4.7) */
        hc->OpWr(XHCI_CONFIG, hc->max_slots);
        EXPECT_EQ(hc->OpRd(XHCI_CONFIG) & 0xff, hc->max_slots);
        hc->OpWr(XHCI_CONFIG, 0xffffffff);
        snap_host(hc, "config", "%#x", hc->OpRd(XHCI_CONFIG));
        hc->OpWr(XHCI_CONFIG, 0);

        /* the pointers: CRCR reads as zero (5.4.5), DCBAAP keeps its
           address bits (5.4.6) */
        hc->OpWr(XHCI_CRCR, 0x12345671);
        hc->OpWr(XHCI_CRCR + 4, 0);
        EXPECT_EQ(hc->OpRd(XHCI_CRCR), 0u);
        EXPECT_EQ(hc->OpRd(XHCI_CRCR + 4), 0u);
        hc->OpWr(XHCI_DCBAAP, 0x1234567f);
        hc->OpWr(XHCI_DCBAAP + 4, 0);
        EXPECT_EQ(hc->OpRd(XHCI_DCBAAP), 0x12345640u);

        /* the interrupter (5.5.2) */
        hc->RtWr(XHCI_IMAN, 0xffffffff);
        EXPECT_EQ(hc->RtRd(XHCI_IMAN), (uint32_t)IMAN_IE);
        hc->RtWr(XHCI_IMAN, 0);
        hc->RtWr(XHCI_IMOD, 0x12345678);
        EXPECT_EQ(hc->RtRd(XHCI_IMOD) & 0xffff, 0x5678u);
        snap_host(hc, "imod", "%#x", hc->RtRd(XHCI_IMOD));
        hc->RtWr(XHCI_ERSTSZ, 0xffffffff);
        EXPECT_EQ(hc->RtRd(XHCI_ERSTSZ), 0xffffu);
        hc->RtWr(XHCI_ERSTSZ, 0);
        hc->RtWr(XHCI_ERDP, 0x12345670);
        hc->RtWr(XHCI_ERDP + 4, 0);
        snap_host(hc, "erdp", "%#x", hc->RtRd(XHCI_ERDP));
        EXPECT_EQ(hc->RtRd(XHCI_ERDP) & ~0xfu, 0x12345670u);
        EXPECT_EQ(hc->RtRd(XHCI_ERDP) & ERDP_EHB, 0u);

        /* the doorbells read as zero (5.6) */
        EXPECT_EQ(hc->Rd(hc->db), 0u);
        EXPECT_EQ(hc->Rd(hc->db + 4), 0u);
        /* the capability registers are RO */
        uint32_t hcs1 = hc->Rd(XHCI_HCSPARAMS1);
        hc->Wr(XHCI_HCSPARAMS1, ~hcs1);
        EXPECT_EQ(hc->Rd(XHCI_HCSPARAMS1), hcs1);
    }
}

TEST(xhci, mfindex)
{
    Xhci *hc = xhci_require();
    /* 125 us microframes, 14 bits (5.5.1); bracketed against the clock */
    for (int attempt = 0; attempt < 5; attempt++) {
        uint64_t t0 = t_now_us();
        uint32_t m0 = hc->RtRd(XHCI_MFINDEX);
        uint64_t t1 = t_now_us();
        t_delay_us(4000);
        uint64_t t2 = t_now_us();
        uint32_t m1 = hc->RtRd(XHCI_MFINDEX);
        uint64_t t3 = t_now_us();
        if (t1 - t0 > 500 || t3 - t2 > 500)
            continue;
        EXPECT_EQ(m0 & ~0x3fffu, 0u);
        uint32_t d = (m1 - m0) & 0x3fff;
        uint64_t lo = (t2 - t1) / 125, hi = (t3 - t0) / 125 + 1;
        EXPECT_GE(d + 1, lo);
        EXPECT_LE(d, hi);
        return;
    }
    t_skip("the machine is too slow to tell");
}


//#pragma mark - the command ring

TEST(xhci, noop_command)
{
    Xhci *hc = xhci_require();
    /* more than the ring holds, so that the link TRB is followed and the
       cycle toggles (4.9.2) */
    for (int i = 0; i < 70; i++) {
        Trb ev;
        uint64_t addr = hc->cmd.Enqueue();
        uint32_t cc = hc->Command(0, 0, 0, TRB_TYPE(TRB_NOOP_CMD), &ev);
        ASSERT_EQ(cc, (uint32_t)CC_SUCCESS);
        /* Command Completion Event (6.4.2.2) */
        EXPECT_EQ(ev.Param(), addr);
        EXPECT_EQ(ev.Type(), (uint32_t)TRB_EV_COMMAND);
        EXPECT_EQ(ev.Slot(), 0u);
        EXPECT_EQ(ev.d[2] & 0xffffff, 0u);
        if (i == 0) {
            SNAP("event.status", "%#x", ev.d[2]);
            SNAP("event.control", "%#x", ev.d[3] & ~TRB_C);
        }
    }
}

TEST(xhci, command_ring_state)
{
    Xhci *hc = xhci_require();
    /* CRR is set by the doorbell and stays set while the ring is running,
       idle or not, until a stop (5.4.5) */
    EXPECT_EQ(hc->OpRd(XHCI_CRCR), 0u);
    ASSERT_EQ(hc->Command(0, 0, 0, TRB_TYPE(TRB_NOOP_CMD)),
              (uint32_t)CC_SUCCESS);
    uint32_t crcr = hc->OpRd(XHCI_CRCR);
    EXPECT_EQ(crcr, (uint32_t)CRCR_CRR);

    /* writes to the pointer are ignored while it runs */
    hc->OpWr(XHCI_CRCR, 0x1000 | CRCR_RCS);
    hc->OpWr(XHCI_CRCR + 4, 0);

    /* Command Stop: a Command Ring Stopped completion at the dequeue
       pointer (4.6.1.1) */
    uint64_t deq = hc->cmd.Enqueue();
    hc->OpWr(XHCI_CRCR, CRCR_CS);
    hc->OpWr(XHCI_CRCR + 4, 0);
    Trb ev;
    ASSERT_TRUE(hc->WaitEvent([](const Trb &t) {
        return t.Type() == TRB_EV_COMMAND;
    }, &ev));
    EXPECT_EQ(ev.Cc(), (uint32_t)CC_CMD_RING_STOPPED);
    EXPECT_EQ(ev.Param(), deq);
    EXPECT_EQ(hc->OpRd(XHCI_CRCR), 0u);

    /* the doorbell starts it again where it stopped */
    ASSERT_EQ(hc->Command(0, 0, 0, TRB_TYPE(TRB_NOOP_CMD)),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(hc->OpRd(XHCI_CRCR), (uint32_t)CRCR_CRR);

    /* Command Abort with nothing executing stops it the same way
       (4.6.1.2) */
    hc->OpWr(XHCI_CRCR, CRCR_CA);
    hc->OpWr(XHCI_CRCR + 4, 0);
    ASSERT_TRUE(hc->WaitEvent([](const Trb &t) {
        return t.Type() == TRB_EV_COMMAND;
    }, &ev));
    EXPECT_EQ(ev.Cc(), (uint32_t)CC_CMD_RING_STOPPED);
    EXPECT_EQ(hc->OpRd(XHCI_CRCR) & CRCR_CRR, 0u);

    /* stopped, the pointer may move: a new ring */
    ASSERT_TRUE(hc->cmd.Init(16));
    hc->OpWr(XHCI_CRCR, (uint32_t)hc->cmd.Base() | CRCR_RCS);
    hc->OpWr(XHCI_CRCR + 4, 0);
    ASSERT_EQ(hc->Command(0, 0, 0, TRB_TYPE(TRB_NOOP_CMD), &ev),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(ev.Param(), hc->cmd.Base());

    /* a stop with the ring stopped does nothing; written to the low half
       alone, as a stop is, it does not move the pointer either */
    hc->OpWr(XHCI_CRCR, CRCR_CS);
    ASSERT_TRUE(hc->WaitEvent([](const Trb &t) {
        return t.Type() == TRB_EV_COMMAND;
    }, &ev));
    hc->OpWr(XHCI_CRCR, CRCR_CS);
    EXPECT_FALSE(hc->WaitEvent([](const Trb &t) {
        return t.Type() == TRB_EV_COMMAND;
    }, &ev, 20000));

    /* halting the controller stops the ring too */
    ASSERT_EQ(hc->Command(0, 0, 0, TRB_TYPE(TRB_NOOP_CMD)),
              (uint32_t)CC_SUCCESS);
    ASSERT_TRUE(hc->Halt());
    EXPECT_EQ(hc->OpRd(XHCI_CRCR) & CRCR_CRR, 0u);
}

TEST(xhci, unknown_commands)
{
    Xhci *hc = xhci_require();
    /* every command gets a completion; one the controller does not do is a
       TRB Error (6.4.5) */
    static const uint32_t kTypes[] = {TRB_NORMAL,
                                      TRB_FORCE_EVENT, TRB_NEGOTIATE_BW,
                                      TRB_SET_LT, TRB_GET_BW,
                                      TRB_FORCE_HEADER, 24, 31, 40};
    for (uint32_t type : kTypes) {
        uint32_t cc = hc->Command(0, 0, 0, TRB_TYPE(type));
        char k[24];
        snprintf(k, sizeof(k), "type%u", type);
        SNAP(k, "%u", cc);
        EXPECT_NE(cc, (uint32_t)CC_TIMEOUT);
    }
}


//#pragma mark - root hub ports

TEST(xhci, port_events_at_start)
{
    int n = require_hosts();
    for (int h = 0; h < n; h++) {
        Xhci *hc = xhci_host(h);
        ASSERT_TRUE(hc->Init(false));
        uint32_t pending = 0;
        for (uint32_t p = 1; p <= hc->max_ports; p++) {
            if (hc->Portsc(p) & PORTSC_CHANGES)
                pending |= 1u << p;
        }
        /* none while halted (5.4.8) */
        Trb ev;
        EXPECT_FALSE(hc->NextEvent(&ev, 20000));
        ASSERT_TRUE(hc->Run());
        /* then one for every port with a change bit set (4.19.2) */
        uint32_t seen = 0;
        while (hc->NextEvent(&ev, 50000)) {
            EXPECT_EQ(ev.Type(), (uint32_t)TRB_EV_PORT);
            EXPECT_EQ(ev.Cc(), (uint32_t)CC_SUCCESS);
            uint32_t port = ev.d[0] >> 24;
            EXPECT_EQ(seen & (1u << port), 0u);
            seen |= 1u << port;
        }
        EXPECT_EQ(seen, pending);
        snap_host(hc, "ports", "%#x", seen);

        /* clearing the change bits posts nothing new; PCD is the
           controller's own (5.4.2) */
        for (uint32_t p = 1; p <= hc->max_ports; p++)
            hc->PortSet(p, hc->Portsc(p) & PORTSC_CHANGES);
        for (uint32_t p = 1; p <= hc->max_ports; p++)
            EXPECT_EQ(hc->Portsc(p) & PORTSC_CHANGES, 0u);
        EXPECT_FALSE(hc->NextEvent(&ev, 20000));
        if (pending) {
            EXPECT_TRUE(hc->OpRd(XHCI_USBSTS) & USBSTS_PCD);
            hc->OpWr(XHCI_USBSTS, USBSTS_PCD);
        }
        EXPECT_EQ(hc->OpRd(XHCI_USBSTS) & USBSTS_PCD, 0u);
    }
}

TEST(xhci, port_reset)
{
    int n = require_hosts();
    bool any = false;
    for (int h = 0; h < n; h++) {
        Xhci *hc = xhci_host(h);
        ASSERT_TRUE(hc->Init());
        hc->DrainEvents();
        for (uint32_t p = 1; p <= hc->max_ports; p++) {
            if (hc->PortMajor(p) != 2)
                continue;
            hc->PortSet(p, hc->Portsc(p) & PORTSC_CHANGES);
            if (!connected(hc, p)) {
                /* nothing to reset: no change (snapshot) */
                hc->PortSet(p, PORTSC_PR);
                t_delay_us(1000);
                char k[24];
                snprintf(k, sizeof(k), "empty%u", p);
                snap_host(hc, k, "%#x", hc->Portsc(p));
                hc->PortSet(p, hc->Portsc(p) & PORTSC_CHANGES);
                hc->TakePortEvents(p);
                continue;
            }
            any = true;
            /* PR then PRC, with the port enabled in U0 (4.19.1.1) */
            hc->PortSet(p, PORTSC_PR);
            ASSERT_TRUE(t_wait_until([&] {
                uint32_t v = hc->Portsc(p);
                return !(v & PORTSC_PR) && (v & PORTSC_PRC);
            }, 500000));
            uint32_t v = hc->Portsc(p);
            EXPECT_TRUE(v & PORTSC_PED);
            EXPECT_EQ(PORTSC_PLS(v), (uint32_t)PLS_U0);
            EXPECT_NE(PORTSC_SPEED(v), 0u);
            EXPECT_EQ(v & PORTSC_WRC, 0u);
            Trb ev;
            ASSERT_TRUE(hc->NextEvent(&ev));
            EXPECT_EQ(ev.Type(), (uint32_t)TRB_EV_PORT);
            EXPECT_EQ(ev.d[0] >> 24, p);
            hc->PortSet(p, PORTSC_PRC);
            EXPECT_EQ(hc->Portsc(p) & PORTSC_PRC, 0u);

            /* writing PED disables the port (5.4.8) */
            hc->PortSet(p, PORTSC_PED);
            v = hc->Portsc(p);
            EXPECT_EQ(v & PORTSC_PED, 0u);
            EXPECT_TRUE(v & PORTSC_CCS);
            char k[24];
            snprintf(k, sizeof(k), "disabled%u", p);
            snap_host(hc, k, "%#x", v);
            hc->PortSet(p, hc->Portsc(p) & PORTSC_CHANGES);
            hc->TakePortEvents(p);
            /* and a reset enables it again */
            EXPECT_TRUE(hc->PortReset(p));
        }
    }
    if (!any)
        t_skip("nothing attached to a USB 2 port");
}

TEST(xhci, port_power_and_wake_bits)
{
    Xhci *hc = xhci_require();
    hc->DrainEvents();
    for (uint32_t p = 1; p <= hc->max_ports; p++) {
        hc->PortSet(p, hc->Portsc(p) & PORTSC_CHANGES);
        /* no port power switches: PP stays set (5.4.8) */
        if (!(hc->Rd(XHCI_HCCPARAMS1) & (1u << 3))) {
            hc->OpWr(XHCI_PORTSC(p), hc->Portsc(p) & PORTSC_KEEP &
                     ~PORTSC_PP);
            EXPECT_TRUE(hc->Portsc(p) & PORTSC_PP);
        }
        /* WCE, WDE and WOE are RWS */
        hc->PortSet(p, PORTSC_WCE | PORTSC_WDE | PORTSC_WOE);
        uint32_t wake = PORTSC_WCE | PORTSC_WDE | PORTSC_WOE;
        EXPECT_EQ(hc->Portsc(p) & wake, wake);
        hc->OpWr(XHCI_PORTSC(p), (hc->Portsc(p) & PORTSC_KEEP) & ~wake);
        EXPECT_EQ(hc->Portsc(p) & wake, 0u);
        /* LWS reads 0; PLS is ignored without it */
        uint32_t before = hc->Portsc(p);
        hc->PortSet(p, 3u << 5);
        EXPECT_EQ(PORTSC_PLS(hc->Portsc(p)), PORTSC_PLS(before));
        EXPECT_EQ(hc->Portsc(p) & PORTSC_LWS, 0u);
        /* the other registers of the set (5.4.9-5.4.11) */
        char k[24];
        snprintf(k, sizeof(k), "pmsc%u", p);
        snap_host(hc, k, "%#x", hc->OpRd(XHCI_PORTPMSC(p)));
        snprintf(k, sizeof(k), "li%u", p);
        snap_host(hc, k, "%#x", hc->OpRd(XHCI_PORTLI(p)));
        snprintf(k, sizeof(k), "hlpmc%u", p);
        snap_host(hc, k, "%#x", hc->OpRd(XHCI_PORTHLPMC(p)));
    }
    hc->DrainEvents();
}

TEST(xhci, port_suspend_resume)
{
    UsbDevice d;
    usb_require_device(0, 0, &d);
    Xhci *hc = d.hc;
    uint32_t p = d.root_port;
    hc->DrainEvents();
    /* U0 to U3 and back, by the link state write strobe (4.15) */
    hc->PortSet(p, PORTSC_LWS | PLS_U3 << 5);
    ASSERT_TRUE(t_wait_until([&] {
        return PORTSC_PLS(hc->Portsc(p)) == PLS_U3;
    }));
    /* U3E is 0: no PLC on entry */
    EXPECT_EQ(hc->Portsc(p) & PORTSC_PLC, 0u);
    hc->PortSet(p, PORTSC_LWS | PLS_U0 << 5);
    ASSERT_TRUE(t_wait_until([&] {
        return PORTSC_PLS(hc->Portsc(p)) == PLS_U0;
    }));
    /* software resume complete: PLC (5.4.8) */
    EXPECT_TRUE(hc->Portsc(p) & PORTSC_PLC);
    EXPECT_EQ(hc->TakePortEvents(p), 1);
    hc->PortSet(p, PORTSC_PLC);
    /* the device answers again */
    uint8_t *buf = (uint8_t *)usb_alloc(18);
    EXPECT_EQ(d.GetDescriptor(USB_DT_DEVICE, 0, buf, 18),
              (uint32_t)CC_SUCCESS);
}


//#pragma mark - device slots

namespace {

/* A device on a root port, enabled but not yet addressed. */
void root_device(UsbDevice *d, uint8_t cls = USB_CLASS_STORAGE)
{
    UsbDevice found;
    usb_require_device(cls, 0, &found);
    *d = UsbDevice {};
    d->hc = found.hc;
    d->root_port = found.root_port;
    d->route = found.route;
    d->speed = found.speed;
    d->tt_slot = found.tt_slot;
    d->tt_port = found.tt_port;
}

uint32_t cmd(Xhci *hc, uint32_t type, uint32_t slot, uint32_t ep = 0,
             uint64_t param = 0, uint32_t flags = 0)
{
    return hc->Command((uint32_t)param, (uint32_t)(param >> 32), 0,
                       TRB_TYPE(type) | TRB_SLOT(slot) | TRB_EP(ep) | flags);
}

}

TEST(xhci, enable_disable_slot)
{
    Xhci *hc = xhci_require();
    /* MaxSlotsEn bounds the slots handed out (5.4.7) */
    ASSERT_TRUE(hc->Halt());
    hc->OpWr(XHCI_CONFIG, 2);
    ASSERT_TRUE(hc->Run());
    uint32_t a, b, c;
    ASSERT_EQ(hc->EnableSlot(&a), (uint32_t)CC_SUCCESS);
    ASSERT_EQ(hc->EnableSlot(&b), (uint32_t)CC_SUCCESS);
    EXPECT_NE(a, b);
    EXPECT_GE(a, 1u);
    EXPECT_LE(a, 2u);
    EXPECT_GE(b, 1u);
    EXPECT_LE(b, 2u);
    Trb ev;
    EXPECT_EQ(hc->Command(0, 0, 0, TRB_TYPE(TRB_ENABLE_SLOT), &ev),
              (uint32_t)CC_NO_SLOTS);
    EXPECT_EQ(ev.Slot(), 0u);
    /* Disable Slot frees one, and its completion names it (4.6.4) */
    EXPECT_EQ(hc->Command(0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(a),
                          &ev), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(ev.Slot(), a);
    ASSERT_EQ(hc->EnableSlot(&c), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(c, a);
    /* a slot not enabled */
    EXPECT_EQ(hc->Command(0, 0, 0, TRB_TYPE(TRB_DISABLE_SLOT) | TRB_SLOT(3),
                          &ev), (uint32_t)CC_SLOT_NOT_ENABLED);
    EXPECT_EQ(ev.Slot(), 3u);
    EXPECT_EQ(hc->DisableSlot(c), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(hc->DisableSlot(c), (uint32_t)CC_SLOT_NOT_ENABLED);
    EXPECT_EQ(hc->DisableSlot(0), (uint32_t)CC_SLOT_NOT_ENABLED);
    EXPECT_EQ(hc->DisableSlot(hc->max_slots + 1),
              (uint32_t)CC_SLOT_NOT_ENABLED);
}

TEST(xhci, commands_on_disabled_slot)
{
    Xhci *hc = xhci_require();
    uint64_t in = (uintptr_t)usb_alloc(33 * 64);
    /* every slot command names a slot that is not enabled (4.6) */
    static const uint32_t kTypes[] = {TRB_ADDRESS_DEVICE, TRB_CONFIGURE_EP,
                                      TRB_EVALUATE_CTX, TRB_RESET_EP,
                                      TRB_STOP_EP, TRB_SET_TR_DEQ,
                                      TRB_RESET_DEVICE};
    for (uint32_t type : kTypes) {
        uint32_t cc = cmd(hc, type, 5, 1, in);
        char k[16];
        snprintf(k, sizeof(k), "type%u", type);
        SNAP(k, "%u", cc);
        EXPECT_EQ(cc, (uint32_t)CC_SLOT_NOT_ENABLED);
    }
}

TEST(xhci, address_device)
{
    UsbDevice d;
    root_device(&d);
    Xhci *hc = d.hc;

    /* with BSR: Default, address 0, endpoint 0 running (4.6.5) */
    ASSERT_EQ(d.Address(true), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(d.SlotState(), (uint32_t)SLOT_DEFAULT);
    EXPECT_EQ(le32(d.OutSlot() + 12) & 0xff, 0u);
    EXPECT_EQ(d.EpState(1), (uint32_t)EP_RUNNING);
    /* the input slot context copied */
    EXPECT_EQ(le32(d.OutSlot() + 4) >> 16 & 0xff, d.root_port);
    EXPECT_EQ(le32(d.OutSlot()) >> 27, 1u);
    EXPECT_EQ(le32(d.OutEp(1) + 4) >> 16, 64u);
    EXPECT_EQ(le32(d.OutEp(1) + 8) & ~0xfu, (uint32_t)d.rings[1].Base());
    /* the device answers at address 0 */
    uint8_t *buf = (uint8_t *)usb_alloc(18);
    EXPECT_EQ(d.GetDescriptor(USB_DT_DEVICE, 0, buf, 8),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(buf[1], USB_DT_DEVICE);
    /* BSR again in Default: Context State Error */
    uint64_t in = (uintptr_t)d.in;
    EXPECT_EQ(cmd(hc, TRB_ADDRESS_DEVICE, d.slot, 0, in, TRB_BSR),
              (uint32_t)CC_CONTEXT_STATE);

    /* without: Addressed, with an address of the controller's choosing */
    put_le32(d.InEp(1) + 8, (uint32_t)d.rings[1].Enqueue() |
             (d.rings[1].cycle ? 1 : 0));
    ASSERT_EQ(cmd(hc, TRB_ADDRESS_DEVICE, d.slot, 0, in),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(d.SlotState(), (uint32_t)SLOT_ADDRESSED);
    uint32_t address = le32(d.OutSlot() + 12) & 0xff;
    EXPECT_NE(address, 0u);
    EXPECT_LT(address, 128u);
    SNAP("address", "%u", address == d.slot ? 0 : address);
    EXPECT_EQ(d.GetDescriptor(USB_DT_DEVICE, 0, buf, 18),
              (uint32_t)CC_SUCCESS);
    /* and never again once Addressed */
    EXPECT_EQ(cmd(hc, TRB_ADDRESS_DEVICE, d.slot, 0, in),
              (uint32_t)CC_CONTEXT_STATE);
    EXPECT_EQ(cmd(hc, TRB_ADDRESS_DEVICE, d.slot, 0, in, TRB_BSR),
              (uint32_t)CC_CONTEXT_STATE);
}

TEST(xhci, address_device_errors)
{
    UsbDevice d;
    root_device(&d);
    Xhci *hc = d.hc;
    uint32_t slot;
    ASSERT_EQ(hc->EnableSlot(&slot), (uint32_t)CC_SUCCESS);
    uint8_t *out = (uint8_t *)usb_alloc(32 * hc->ctx_size);
    hc->dcbaa[slot] = (uintptr_t)out;
    uint8_t *in = (uint8_t *)usb_alloc(33 * hc->ctx_size);
    XhciRing ring;
    ring.Init(16);
    uint8_t *islot = in + hc->ctx_size, *iep0 = in + 2 * hc->ctx_size;
    put_le32(islot, d.speed << 20 | 1u << 27);
    put_le32(islot + 4, d.root_port << 16);
    put_le32(iep0 + 4, 3 << 1 | EPT_CONTROL << 3 | 64 << 16);
    put_le32(iep0 + 8, (uint32_t)ring.Base() | 1);

    /* A0 and A1 must both be set (4.6.5): snapshot what comes of it */
    put_le32(in + 4, 0x1);
    SNAP("a0_only", "%u", cmd(hc, TRB_ADDRESS_DEVICE, slot, 0,
                              (uintptr_t)in));
    put_le32(in + 4, 0x3);
    /* a port with nothing behind it: a USB Transaction Error, and the slot
       stays where it was */
    uint32_t empty = 0;
    for (uint32_t p = 1; p <= hc->max_ports && !empty; p++) {
        if (!(hc->Portsc(p) & PORTSC_CCS))
            empty = p;
    }
    if (empty) {
        put_le32(islot + 4, empty << 16);
        EXPECT_EQ(cmd(hc, TRB_ADDRESS_DEVICE, slot, 0, (uintptr_t)in),
                  (uint32_t)CC_TRANSACTION);
        EXPECT_NE(le32(out + 12) >> 27, (uint32_t)SLOT_ADDRESSED);
        put_le32(islot + 4, d.root_port << 16);
    }
    /* a port number past MaxPorts */
    put_le32(islot + 4, (hc->max_ports + 1) << 16);
    SNAP("bad_port", "%u", cmd(hc, TRB_ADDRESS_DEVICE, slot, 0,
                               (uintptr_t)in));
    put_le32(islot + 4, d.root_port << 16);
    EXPECT_EQ(cmd(hc, TRB_ADDRESS_DEVICE, slot, 0, (uintptr_t)in),
              (uint32_t)CC_SUCCESS);
}

TEST(xhci, set_address_on_ring)
{
    UsbDevice d;
    usb_require_device(USB_CLASS_STORAGE, 0, &d);
    /* the controller never forwards SET_ADDRESS; the TD is a TRB Error
       (4.6.5) */
    EXPECT_EQ(d.Control(0, USB_REQ_SET_ADDRESS, 42, 0, 0, nullptr),
              (uint32_t)CC_TRB);
    if (d.EpState(1) == EP_HALTED)
        ASSERT_TRUE(d.Recover(1));
    uint8_t *buf = (uint8_t *)usb_alloc(18);
    EXPECT_EQ(d.GetDescriptor(USB_DT_DEVICE, 0, buf, 18),
              (uint32_t)CC_SUCCESS);
}

TEST(xhci, evaluate_context)
{
    UsbDevice d;
    root_device(&d);
    Xhci *hc = d.hc;
    uint32_t slot;
    ASSERT_EQ(hc->EnableSlot(&slot), (uint32_t)CC_SUCCESS);
    uint8_t *in = (uint8_t *)usb_alloc(33 * hc->ctx_size);
    put_le32(in + 4, 0x3);
    /* a slot only Enabled: Context State Error (4.6.7) */
    hc->dcbaa[slot] = (uintptr_t)usb_alloc(32 * hc->ctx_size);
    EXPECT_EQ(cmd(hc, TRB_EVALUATE_CTX, slot, 0, (uintptr_t)in),
              (uint32_t)CC_CONTEXT_STATE);
    hc->DisableSlot(slot);

    ASSERT_EQ(d.Address(), (uint32_t)CC_SUCCESS);
    /* the slot context's Max Exit Latency and Interrupter Target, and
       endpoint 0's Max Packet Size (6.2.2.3, 6.2.3.3) */
    memset(d.in, 0, 33 * hc->ctx_size);
    put_le32(d.InCtl() + 4, 0x3);
    memcpy(d.InSlot(), d.OutSlot(), hc->ctx_size);
    memcpy(d.InEp(1), d.OutEp(1), hc->ctx_size);
    put_le32(d.InSlot() + 4, (le32(d.OutSlot() + 4) & ~0xffffu) | 1234);
    put_le32(d.InSlot() + 8, le32(d.OutSlot() + 8) & 0x3fffff);
    put_le32(d.InEp(1) + 4, (le32(d.OutEp(1) + 4) & 0xffff) | 8u << 16);
    /* what is not evaluated stays */
    put_le32(d.InSlot(), le32(d.OutSlot()) ^ 0x5);
    put_le32(d.InEp(1) + 16, 0x77);
    uint32_t slot0 = le32(d.OutSlot()), ep4 = le32(d.OutEp(1) + 16);
    ASSERT_EQ(cmd(hc, TRB_EVALUATE_CTX, d.slot, 0, (uintptr_t)d.in),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(le32(d.OutSlot() + 4) & 0xffff, 1234u);
    EXPECT_EQ(le32(d.OutEp(1) + 4) >> 16, 8u);
    EXPECT_EQ(le32(d.OutSlot()), slot0);
    EXPECT_EQ(le32(d.OutEp(1) + 16), ep4);
    EXPECT_EQ(d.SlotState(), (uint32_t)SLOT_ADDRESSED);
    EXPECT_EQ(d.EpState(1), (uint32_t)EP_RUNNING);
    /* endpoint 0 still moves a descriptor with 8 byte packets */
    uint8_t *buf = (uint8_t *)usb_alloc(18);
    uint32_t got;
    EXPECT_EQ(d.GetDescriptor(USB_DT_DEVICE, 0, buf, 18, &got),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(got, 18u);
}

TEST(xhci, configure_endpoint)
{
    UsbDevice d;
    usb_require_device(USB_CLASS_STORAGE, 0, &d);
    Xhci *hc = d.hc;
    ASSERT_EQ(d.Configure(), (uint32_t)CC_SUCCESS);
    UsbEndpoint in_ep, out_ep;
    ASSERT_TRUE(d.FindEndpoint(0, 0, &in_ep));
    ASSERT_TRUE(d.FindEndpoint(0, 1, &out_ep));
    int a = usb_dci(in_ep.address), b = usb_dci(out_ep.address);
    int last = max(a, b);
    /* Configured, with the last valid context (4.6.6) */
    EXPECT_EQ(d.SlotState(), (uint32_t)SLOT_CONFIGURED);
    EXPECT_EQ(le32(d.OutSlot()) >> 27, (uint32_t)last);
    EXPECT_EQ(d.EpState(a), (uint32_t)EP_RUNNING);
    EXPECT_EQ(d.EpState(b), (uint32_t)EP_RUNNING);
    EXPECT_EQ((le32(d.OutEp(a) + 4) >> 3) & 7, (uint32_t)EPT_BULK_IN);
    EXPECT_EQ((le32(d.OutEp(b) + 4) >> 3) & 7, (uint32_t)EPT_BULK_OUT);
    EXPECT_EQ(le32(d.OutEp(a) + 4) >> 16, (uint32_t)in_ep.max_packet);

    /* drop one: it is Disabled and the slot stays Configured */
    memset(d.in, 0, 33 * hc->ctx_size);
    memcpy(d.InSlot(), d.OutSlot(), hc->ctx_size);
    put_le32(d.InCtl(), 1u << b);
    put_le32(d.InCtl() + 4, 1);
    ASSERT_EQ(cmd(hc, TRB_CONFIGURE_EP, d.slot, 0, (uintptr_t)d.in),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(d.EpState(b), (uint32_t)EP_DISABLED);
    EXPECT_EQ(d.EpState(a), (uint32_t)EP_RUNNING);
    EXPECT_EQ(d.SlotState(), (uint32_t)SLOT_CONFIGURED);
    /* dropping the slot or endpoint 0 is not for this command (6.2.5.1) */
    put_le32(d.InCtl(), 0x3);
    put_le32(d.InCtl() + 4, 0);
    SNAP("drop_a0_a1", "%u", cmd(hc, TRB_CONFIGURE_EP, d.slot, 0,
                                 (uintptr_t)d.in));
    /* an added context past the input slot's Context Entries is invalid:
       Parameter Error, nothing changes */
    put_le32(d.InCtl(), 0);
    d.SetupEpContext(out_ep);
    put_le32(d.InCtl() + 4, 1u << b);
    put_le32(d.InSlot(), (le32(d.InSlot()) & ~(0x1fu << 27)) | 1u << 27);
    EXPECT_EQ(cmd(hc, TRB_CONFIGURE_EP, d.slot, 0, (uintptr_t)d.in),
              (uint32_t)CC_PARAMETER);
    EXPECT_EQ(d.EpState(b), (uint32_t)EP_DISABLED);
    /* drop the rest: back to Addressed with one context (4.6.6) */
    put_le32(d.InCtl(), 1u << a);
    put_le32(d.InCtl() + 4, 1);
    ASSERT_EQ(cmd(hc, TRB_CONFIGURE_EP, d.slot, 0, (uintptr_t)d.in),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(d.EpState(a), (uint32_t)EP_DISABLED);
    EXPECT_EQ(d.SlotState(), (uint32_t)SLOT_ADDRESSED);
    EXPECT_EQ(le32(d.OutSlot()) >> 27, 1u);

    /* Deconfigure only from Configured */
    EXPECT_EQ(cmd(hc, TRB_CONFIGURE_EP, d.slot, 0, 0, TRB_DC),
              (uint32_t)CC_CONTEXT_STATE);
    ASSERT_EQ(d.Configure(), (uint32_t)CC_SUCCESS);
    ASSERT_EQ(cmd(hc, TRB_CONFIGURE_EP, d.slot, 0, 0, TRB_DC),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(d.SlotState(), (uint32_t)SLOT_ADDRESSED);
    EXPECT_EQ(d.EpState(a), (uint32_t)EP_DISABLED);
    EXPECT_EQ(d.EpState(b), (uint32_t)EP_DISABLED);
    EXPECT_EQ(d.EpState(1), (uint32_t)EP_RUNNING);

    /* not before the device is addressed */
    uint32_t slot;
    ASSERT_EQ(hc->EnableSlot(&slot), (uint32_t)CC_SUCCESS);
    hc->dcbaa[slot] = (uintptr_t)usb_alloc(32 * hc->ctx_size);
    EXPECT_EQ(cmd(hc, TRB_CONFIGURE_EP, slot, 0, (uintptr_t)d.in),
              (uint32_t)CC_CONTEXT_STATE);
}

TEST(xhci, reset_device)
{
    UsbDevice d;
    usb_require_device(USB_CLASS_STORAGE, 0, &d);
    Xhci *hc = d.hc;
    ASSERT_EQ(d.Configure(), (uint32_t)CC_SUCCESS);
    UsbEndpoint ep;
    ASSERT_TRUE(d.FindEndpoint(0, 0, &ep));
    int dci = usb_dci(ep.address);
    /* Default, address 0, one context, every endpoint but 0 Disabled
       (4.6.11) */
    hc->PortReset(d.root_port);
    ASSERT_EQ(cmd(hc, TRB_RESET_DEVICE, d.slot), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(d.SlotState(), (uint32_t)SLOT_DEFAULT);
    EXPECT_EQ(le32(d.OutSlot() + 12) & 0xff, 0u);
    EXPECT_EQ(le32(d.OutSlot()) >> 27, 1u);
    EXPECT_EQ(d.EpState(dci), (uint32_t)EP_DISABLED);
    EXPECT_EQ(d.EpState(1), (uint32_t)EP_RUNNING);
    /* only from Addressed or Configured */
    EXPECT_EQ(cmd(hc, TRB_RESET_DEVICE, d.slot),
              (uint32_t)CC_CONTEXT_STATE);
    /* and addressed again from Default */
    memset(d.in, 0, 33 * hc->ctx_size);
    put_le32(d.InCtl() + 4, 0x3);
    memcpy(d.InSlot(), d.OutSlot(), hc->ctx_size);
    put_le32(d.InSlot() + 12, 0);
    memcpy(d.InEp(1), d.OutEp(1), hc->ctx_size);
    put_le32(d.InEp(1), 0);
    put_le32(d.InEp(1) + 8, (uint32_t)d.rings[1].Enqueue() |
             (d.rings[1].cycle ? 1 : 0));
    ASSERT_EQ(cmd(hc, TRB_ADDRESS_DEVICE, d.slot, 0, (uintptr_t)d.in),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(d.SlotState(), (uint32_t)SLOT_ADDRESSED);
    uint8_t *buf = (uint8_t *)usb_alloc(18);
    EXPECT_EQ(d.GetDescriptor(USB_DT_DEVICE, 0, buf, 18),
              (uint32_t)CC_SUCCESS);
}


//#pragma mark - transfers

TEST(xhci, control_transfer_events)
{
    UsbDevice d;
    usb_require_device(USB_CLASS_STORAGE, 0, &d);
    Xhci *hc = d.hc;
    XhciRing &r = d.rings[1];
    uint8_t *buf = (uint8_t *)usb_alloc(64);

    /* a short IN data stage with ISP: a Short Packet event on the Data TRB
       with the residual, then the Status TRB's (4.10.1.1) */
    r.Push(USB_DIR_IN | USB_REQ_GET_DESCRIPTOR << 8 | USB_DT_DEVICE << 24,
           64u << 16, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | TRB_TRT_IN);
    uint64_t data = r.Push((uint32_t)(uintptr_t)buf, 0, 64,
                           TRB_TYPE(TRB_DATA) | TRB_ISP | TRB_DIR_IN);
    uint64_t status = r.Push(0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC);
    hc->Doorbell(d.slot, 1);
    Trb ev;
    ASSERT_EQ(d.WaitTransfer(1, nullptr, 2000000, &ev),
              (uint32_t)CC_SHORT_PACKET);
    EXPECT_EQ(ev.Param(), data);
    EXPECT_EQ(ev.Residual(), 64u - 18u);
    EXPECT_EQ(ev.d[3] & (1u << 2), 0u); /* ED */
    ASSERT_EQ(d.WaitTransfer(1, nullptr, 2000000, &ev),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(ev.Param(), status);
    EXPECT_EQ(ev.Residual(), 0u);
    EXPECT_EQ(buf[0], 18);

    /* without ISP or IOC on the Data TRB only the Status TRB reports */
    r.Push(USB_DIR_IN | USB_REQ_GET_DESCRIPTOR << 8 | USB_DT_DEVICE << 24,
           64u << 16, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | TRB_TRT_IN);
    r.Push((uint32_t)(uintptr_t)buf, 0, 64,
           TRB_TYPE(TRB_DATA) | TRB_DIR_IN);
    status = r.Push(0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC);
    hc->Doorbell(d.slot, 1);
    ASSERT_EQ(d.WaitTransfer(1, nullptr, 2000000, &ev),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(ev.Param(), status);
    EXPECT_EQ(d.WaitTransfer(1, nullptr, 20000, &ev), (uint32_t)CC_TIMEOUT);

    /* an Event Data TRB after the Data TRB: ED set, its own parameter
       back, and the bytes moved since the last one (4.11.5.2) */
    r.Push(USB_DIR_IN | USB_REQ_GET_DESCRIPTOR << 8 | USB_DT_DEVICE << 24,
           64u << 16, 8, TRB_TYPE(TRB_SETUP) | TRB_IDT | TRB_TRT_IN);
    r.Push((uint32_t)(uintptr_t)buf, 0, 64,
           TRB_TYPE(TRB_DATA) | TRB_DIR_IN | TRB_CH);
    r.Push(0x89abcdef, 0x01234567, 0,
           TRB_TYPE(TRB_EVENT_DATA) | TRB_IOC);
    status = r.Push(0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC);
    hc->Doorbell(d.slot, 1);
    ASSERT_NE(d.WaitTransfer(1, nullptr, 2000000, &ev),
              (uint32_t)CC_TIMEOUT);
    EXPECT_TRUE(ev.d[3] & (1u << 2));
    EXPECT_EQ(ev.Param(), 0x0123456789abcdefull);
    EXPECT_EQ(ev.Residual(), 18u);
    SNAP("event_data.cc", "%u", ev.Cc());
    ASSERT_EQ(d.WaitTransfer(1, nullptr, 2000000, &ev),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(ev.Param(), status);

    /* no data stage, and an IN data stage of one byte */
    uint32_t got;
    EXPECT_EQ(d.Control(0, USB_REQ_SET_CONFIGURATION, 1, 0, 0, nullptr,
                        &got), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(d.Control(USB_DIR_IN, USB_REQ_GET_CONFIGURATION, 0, 0, 1, buf,
                        &got), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(got, 1u);
    EXPECT_EQ(buf[0], 1);
}

TEST(xhci, control_stall)
{
    UsbDevice d;
    usb_require_device(USB_CLASS_STORAGE, 0, &d);
    uint8_t *buf = (uint8_t *)usb_alloc(64);
    /* a request the device refuses halts endpoint 0 (4.10.2.1) */
    EXPECT_EQ(d.GetDescriptor(0x42, 0, buf, 64), (uint32_t)CC_STALL);
    EXPECT_EQ(d.EpState(1), (uint32_t)EP_HALTED);
    /* the dequeue pointer saved where it stopped */
    SNAP("deq_offset", "%#x",
         (uint32_t)((le32(d.OutEp(1) + 8) & ~0xfu) - d.rings[1].Base()));
    /* a halted ring ignores its doorbell */
    d.rings[1].Push(USB_DIR_IN | USB_REQ_GET_DESCRIPTOR << 8 |
                    USB_DT_DEVICE << 24, 18u << 16, 8,
                    TRB_TYPE(TRB_SETUP) | TRB_IDT | TRB_TRT_IN);
    d.rings[1].Push((uint32_t)(uintptr_t)buf, 0, 18,
                    TRB_TYPE(TRB_DATA) | TRB_DIR_IN);
    d.rings[1].Push(0, 0, 0, TRB_TYPE(TRB_STATUS) | TRB_IOC);
    d.hc->Doorbell(d.slot, 1);
    EXPECT_EQ(d.WaitTransfer(1, nullptr, 20000), (uint32_t)CC_TIMEOUT);
    /* Set TR Dequeue needs Stopped or Error (4.6.10) */
    EXPECT_EQ(d.SkipQueued(1), (uint32_t)CC_CONTEXT_STATE);
    /* Reset Endpoint: Stopped (4.6.8) */
    ASSERT_EQ(d.ResetEndpoint(1), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(d.EpState(1), (uint32_t)EP_STOPPED);
    ASSERT_EQ(d.SkipQueued(1), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(le32(d.OutEp(1) + 8), (uint32_t)d.rings[1].Enqueue() |
              (d.rings[1].cycle ? 1 : 0));
    /* the doorbell runs it again */
    EXPECT_EQ(d.GetDescriptor(USB_DT_DEVICE, 0, buf, 18),
              (uint32_t)CC_SUCCESS);
    EXPECT_EQ(d.EpState(1), (uint32_t)EP_RUNNING);
    /* Reset Endpoint only on a halted endpoint */
    EXPECT_EQ(d.ResetEndpoint(1), (uint32_t)CC_CONTEXT_STATE);
}

TEST(xhci, stop_endpoint)
{
    UsbDevice d;
    usb_require_device(USB_CLASS_HID, 0, &d);
    ASSERT_EQ(d.Configure(), (uint32_t)CC_SUCCESS);
    UsbEndpoint ep;
    ASSERT_TRUE(d.FindEndpoint(0, 0, &ep));
    int dci = usb_dci(ep.address);
    uint8_t *buf = (uint8_t *)usb_alloc(64);

    /* the interrupt IN TD waits for a report that does not come */
    uint64_t trb = d.Queue(dci, buf, ep.max_packet);
    EXPECT_EQ(d.WaitTransfer(dci, nullptr, 20000), (uint32_t)CC_TIMEOUT);
    /* Stop Endpoint: a Stopped Transfer Event for the TD in progress, then
       the completion; the context saves the dequeue pointer (4.6.9) */
    Trb ev;
    ASSERT_EQ(d.StopEndpoint(dci), (uint32_t)CC_SUCCESS);
    ASSERT_EQ(d.WaitTransfer(dci, nullptr, 20000, &ev),
              (uint32_t)CC_STOPPED);
    /* nothing moved: all of it left */
    EXPECT_EQ(ev.Residual(), (uint32_t)ep.max_packet);
    EXPECT_EQ(ev.Param(), trb);
    EXPECT_EQ(d.EpState(dci), (uint32_t)EP_STOPPED);
    EXPECT_EQ(le32(d.OutEp(dci) + 8) & ~0xfu, (uint32_t)trb);
    /* only a running endpoint stops */
    EXPECT_EQ(d.StopEndpoint(dci), (uint32_t)CC_CONTEXT_STATE);
    /* Set TR Dequeue on the stopped one */
    EXPECT_EQ(d.SkipQueued(dci), (uint32_t)CC_SUCCESS);
    /* ringing the doorbell restarts it, here with nothing queued */
    d.hc->Doorbell(d.slot, dci);
    EXPECT_EQ(d.EpState(dci), (uint32_t)EP_RUNNING);
    EXPECT_EQ(d.WaitTransfer(dci, nullptr, 20000), (uint32_t)CC_TIMEOUT);
    /* Set TR Dequeue on a running endpoint */
    EXPECT_EQ(d.SkipQueued(dci), (uint32_t)CC_CONTEXT_STATE);
    /* and stopping it idle (snapshot: xHCI 1.1 forces an event) */
    ASSERT_EQ(d.StopEndpoint(dci), (uint32_t)CC_SUCCESS);
    SNAP("idle_stop", "%u", d.WaitTransfer(dci, nullptr, 20000, &ev));
    /* a disabled endpoint */
    EXPECT_EQ(d.StopEndpoint(30), (uint32_t)CC_CONTEXT_STATE);
    EXPECT_EQ(d.ResetEndpoint(30), (uint32_t)CC_CONTEXT_STATE);
}

TEST(xhci, doorbells_ignored)
{
    UsbDevice d;
    usb_require_device(USB_CLASS_STORAGE, 0, &d);
    Xhci *hc = d.hc;
    uint8_t *buf = (uint8_t *)usb_alloc(64);
    hc->DrainEvents();
    /* a disabled endpoint, a slot not enabled, a reserved target: nothing
       happens (5.6) */
    d.rings[3].Init(16);
    d.rings[3].Push((uint32_t)(uintptr_t)buf, 0, 64,
                    TRB_TYPE(TRB_NORMAL) | TRB_IOC);
    hc->Doorbell(d.slot, 3);
    hc->Doorbell(d.slot + 1, 1);
    hc->Doorbell(d.slot, 0);
    hc->Doorbell(d.slot, 32);
    hc->Doorbell(hc->max_slots + 1, 1);
    hc->Doorbell(0, 1);
    Trb ev;
    while (hc->NextEvent(&ev, 20000))
        t_fail(__FILE__, __LINE__, "event %#x %#x %#x %#x", ev.d[0], ev.d[1],
               ev.d[2], ev.d[3]);
    EXPECT_EQ(d.GetDescriptor(USB_DT_DEVICE, 0, buf, 18),
              (uint32_t)CC_SUCCESS);
}

TEST(xhci, td_not_starting_with_setup)
{
    UsbDevice d;
    usb_require_device(USB_CLASS_STORAGE, 0, &d);
    uint8_t *buf = (uint8_t *)usb_alloc(64);
    /* a Normal TRB on the default control endpoint */
    uint64_t trb = d.Queue(1, buf, 8);
    Trb ev;
    uint32_t cc = d.WaitTransfer(1, nullptr, 200000, &ev);
    SNAP("cc", "%u", cc);
    EXPECT_NE(cc, (uint32_t)CC_SUCCESS);
    if (cc != CC_TIMEOUT)
        EXPECT_EQ(ev.Param(), trb);
    if (d.EpState(1) == EP_HALTED)
        ASSERT_TRUE(d.Recover(1));
    EXPECT_EQ(d.GetDescriptor(USB_DT_DEVICE, 0, buf, 18),
              (uint32_t)CC_SUCCESS);
}


//#pragma mark - events and interrupts

TEST(xhci, event_ring_wrap)
{
    Xhci *hc = xhci_require();
    /* the producer cycle toggles at the end of the segment (4.9.4), over
       three times round the ring */
    for (int i = 0; i < 3 * XHCI_EVENTS + 5; i++)
        ASSERT_EQ(hc->Command(0, 0, 0, TRB_TYPE(TRB_NOOP_CMD)),
                  (uint32_t)CC_SUCCESS);
}

TEST(xhci, event_ring_full)
{
    Xhci *hc = xhci_require();
    ASSERT_TRUE(hc->Init(true, 4 * XHCI_EVENTS));
    hc->DrainEvents();
    /* more completions than the ring has room for, none taken: none may
       be lost or overwritten, and they come in order (4.9.4) */
    const int n = XHCI_EVENTS + 8;
    uint64_t expect = hc->cmd.Enqueue();
    for (int i = 0; i < n; i++)
        hc->cmd.Push(0, 0, 0, TRB_TYPE(TRB_NOOP_CMD));
    hc->Doorbell(0, 0);
    t_delay_us(10000);
    int got = 0, full = 0;
    Trb ev;
    while (got < n && hc->NextEvent(&ev, 200000)) {
        if (ev.Type() == TRB_EV_HOST) {
            full++;
            continue;
        }
        if (ev.Type() != TRB_EV_COMMAND || ev.Cc() != CC_SUCCESS)
            continue;
        EXPECT_EQ(ev.Param(), expect);
        expect += 16;
        if ((expect - hc->cmd.Base()) / 16 == hc->cmd.size - 1)
            expect = hc->cmd.Base();
        got++;
    }
    EXPECT_EQ(got, n);
    SNAP("full_events", "%d", full);
}

namespace {

volatile int sIrqs;
Xhci *sIrqHost;

void xhci_irq(void *)
{
    sIrqs = sIrqs + 1;
    /* EINT before IP (5.4.2) */
    sIrqHost->OpWr(XHCI_USBSTS, USBSTS_EINT);
    sIrqHost->RtWr(XHCI_IMAN, IMAN_IP | IMAN_IE);
}

void unroute()
{
    if (sIrqHost->fn != nullptr)
        pci_intx_unroute(sIrqHost->fn);
    else
        irq_unroute(sIrqHost->dev, 0);
}

}

TEST(xhci, interrupter)
{
    int n = require_hosts();
    bool any = false;
    for (int h = 0; h < n; h++) {
        Xhci *hc = xhci_host(h);
        ASSERT_TRUE(hc->Init());
        hc->DrainEvents();
        hc->RtWr(XHCI_IMOD, 0);
        sIrqHost = hc;
        sIrqs = 0;
        bool routed = hc->fn != nullptr
                          ? pci_intx_route(hc->fn, xhci_irq, nullptr)
                          : irq_route(hc->dev, 0, xhci_irq, nullptr);
        if (!routed)
            continue;
        any = true;
        t_on_cleanup(unroute);

        /* polled: IP and EINT still set by an event (4.17.2, 5.4.2) */
        ASSERT_EQ(hc->Command(0, 0, 0, TRB_TYPE(TRB_NOOP_CMD)),
                  (uint32_t)CC_SUCCESS);
        EXPECT_TRUE(hc->RtRd(XHCI_IMAN) & IMAN_IP);
        EXPECT_TRUE(hc->OpRd(XHCI_USBSTS) & USBSTS_EINT);
        hc->OpWr(XHCI_USBSTS, USBSTS_EINT);
        EXPECT_EQ(hc->OpRd(XHCI_USBSTS) & USBSTS_EINT, 0u);
        hc->RtWr(XHCI_IMAN, IMAN_IP);
        EXPECT_EQ(hc->RtRd(XHCI_IMAN) & IMAN_IP, 0u);
        arch_irq_enable();
        t_delay_us(2000);
        arch_irq_disable();
        EXPECT_EQ(sIrqs, 0);

        /* IE alone is not enough: INTE too (5.4.1) */
        hc->RtWr(XHCI_IMAN, IMAN_IE);
        arch_irq_enable();
        hc->Command(0, 0, 0, TRB_TYPE(TRB_NOOP_CMD));
        t_delay_us(2000);
        arch_irq_disable();
        EXPECT_EQ(sIrqs, 0);
        /* setting INTE with IP pending interrupts */
        hc->OpWr(XHCI_USBCMD, hc->OpRd(XHCI_USBCMD) | USBCMD_INTE);
        arch_irq_enable();
        EXPECT_TRUE(t_wait_until([] {return sIrqs > 0;}));
        arch_irq_disable();
        EXPECT_EQ(sIrqs, 1);

        /* one per event while each is taken */
        for (int i = 0; i < 3; i++) {
            sIrqs = 0;
            arch_irq_enable();
            hc->Command(0, 0, 0, TRB_TYPE(TRB_NOOP_CMD));
            EXPECT_TRUE(t_wait_until([] {return sIrqs > 0;}));
            arch_irq_disable();
        }
        /* EHB set with IP (4.17.2); an event left on the ring interrupts
           again once ERDP is written with EHB (5.5.2.3.3) */
        hc->DrainEvents();
        sIrqs = 0;
        uint64_t a = hc->cmd.Push(0, 0, 0, TRB_TYPE(TRB_NOOP_CMD));
        hc->cmd.Push(0, 0, 0, TRB_TYPE(TRB_NOOP_CMD));
        hc->Doorbell(0, 0);
        arch_irq_enable();
        EXPECT_TRUE(t_wait_until([] {return sIrqs > 0;}));
        arch_irq_disable();
        EXPECT_TRUE(hc->RtRd(XHCI_ERDP) & ERDP_EHB);
        Trb ev;
        ASSERT_TRUE(hc->PollRing(&ev));
        EXPECT_EQ(ev.Param(), a);
        sIrqs = 0;
        arch_irq_enable();
        EXPECT_TRUE(t_wait_until([] {return sIrqs > 0;}));
        arch_irq_disable();
        hc->DrainEvents();

        hc->OpWr(XHCI_USBCMD, hc->OpRd(XHCI_USBCMD) & ~USBCMD_INTE);
        hc->RtWr(XHCI_IMAN, IMAN_IP);
        unroute();
    }
    if (!any)
        t_skip("no interrupt reaches a controller the suite drives");
}

TEST(xhci, msix)
{
    Xhci *hc = nullptr;
    for (int h = 0; h < xhci_count() && hc == nullptr; h++) {
        Xhci *x = xhci_host(h);
        if (x->fn != nullptr && x->fn->host->msi != nullptr &&
            pci_find_cap(x->fn, PCI_CAP_MSIX))
            hc = x;
    }
    if (hc == nullptr)
        t_skip("no xHCI with MSI-X, or nothing to receive it");
    PciFunc *fn = hc->fn;
    static volatile int count;
    count = 0;
    uint64_t addr;
    uint32_t data;
    ASSERT_TRUE(fn->host->msi->Message(5, [](void *) {count = count + 1;},
                                       nullptr, &addr, &data));
    uint32_t cap = pci_find_cap(fn, PCI_CAP_MSIX);
    uint32_t table = pci_read32(fn, cap + 4);
    uint32_t pba = pci_read32(fn, cap + 8);
    SNAP("table_size", "%u", (pci_read16(fn, cap + 2) & 0x7ff) + 1u);
    SNAP("table", "%#x", table);
    SNAP("pba", "%#x", pba);
    uint32_t entry = table & ~7u;
    pci_bar_write(fn, table & 7, entry, (uint32_t)addr, 2);
    pci_bar_write(fn, table & 7, entry + 4, addr >> 32, 2);
    pci_bar_write(fn, table & 7, entry + 8, data, 2);
    pci_bar_write(fn, table & 7, entry + 12, 0, 2);
    uint16_t ctrl = pci_read16(fn, cap + 2);
    pci_write16(fn, cap + 2, (ctrl & 0x7ff) | 0x8000);
    ASSERT_TRUE(hc->Init());
    hc->DrainEvents();
    hc->RtWr(XHCI_IMOD, 0);
    hc->RtWr(XHCI_IMAN, IMAN_IP | IMAN_IE);
    hc->OpWr(XHCI_USBCMD, hc->OpRd(XHCI_USBCMD) | USBCMD_INTE);
    arch_irq_enable();
    ASSERT_EQ(hc->Command(0, 0, 0, TRB_TYPE(TRB_NOOP_CMD)),
              (uint32_t)CC_SUCCESS);
    EXPECT_TRUE(t_wait_until([] {return count > 0;}));
    arch_irq_disable();
    /* with MSI-X, IP clears as the message goes (4.17.5) */
    EXPECT_EQ(hc->RtRd(XHCI_IMAN) & IMAN_IP, 0u);
    /* a second event, a second message */
    count = 0;
    arch_irq_enable();
    hc->Command(0, 0, 0, TRB_TYPE(TRB_NOOP_CMD));
    EXPECT_TRUE(t_wait_until([] {return count > 0;}));
    arch_irq_disable();
    /* masked by the vector control: held in the PBA until unmasked */
    pci_bar_write(fn, table & 7, entry + 12, 1, 2);
    count = 0;
    hc->Command(0, 0, 0, TRB_TYPE(TRB_NOOP_CMD));
    arch_irq_enable();
    t_delay_us(2000);
    arch_irq_disable();
    EXPECT_EQ(count, 0);
    EXPECT_EQ(pci_bar_read(fn, pba & 7, pba & ~7u, 2) & 1, 1u);
    pci_bar_write(fn, table & 7, entry + 12, 0, 2);
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([] {return count > 0;}));
    arch_irq_disable();
    hc->OpWr(XHCI_USBCMD, hc->OpRd(XHCI_USBCMD) & ~USBCMD_INTE);
}
