/*
 * USB mass storage tests
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
#include "test.h"
#include "test_control.h"
#include "testctl.h"
#include "usb.h"

/* The Bulk-Only Transport of the USB Mass Storage Class, revision 1.0: the
   class requests (3), the command and status wrappers (5) and the host and
   device expectations they reconcile (6.7). What the SCSI units do with the
   commands is the SCSI tests' business; they run over this transport too. */

#define BLOCK 512

namespace {

UsbBot sBot;

void open_bot(int index = 0)
{
    if (!sBot.Open(index))
        t_skip("no USB mass storage device %d", index);
}

const uint8_t kInquiry[6] = {0x12, 0, 0, 0, 36, 0};
const uint8_t kTestUnitReady[6] = {0, 0, 0, 0, 0, 0};
const uint8_t kReadCapacity[10] = {0x25};

void rw10(uint8_t *cdb, uint8_t op, uint32_t lba, uint16_t blocks)
{
    memset(cdb, 0, 10);
    cdb[0] = op;
    put_be32(cdb + 2, lba);
    cdb[7] = blocks >> 8;
    cdb[8] = blocks;
}

uint32_t pattern_errors(const uint8_t *buf, uint64_t lba, uint32_t blocks)
{
    uint32_t bad = 0;
    for (uint32_t b = 0; b < blocks; b++) {
        for (uint32_t i = 0; i < BLOCK; i++)
            bad += buf[b * BLOCK + i] != TEST_BLOCK_PATTERN(lba + b, i);
    }
    return bad;
}

/* The unit's blocks, past a unit attention. */
uint32_t capacity(uint8_t lun)
{
    uint8_t *cap = (uint8_t *)usb_alloc(8);
    for (int i = 0; i < 2; i++) {
        if (sBot.Command(lun, kReadCapacity, 10, true, cap, 8) == BOT_PASS)
            return be32(cap) + 1;
    }
    return 0;
}

/* The sense key and ASC/ASCQ of the last failure. */
uint32_t sense(uint8_t lun)
{
    static const uint8_t kRequestSense[6] = {0x03, 0, 0, 0, 18, 0};
    uint8_t *buf = (uint8_t *)usb_alloc(18);
    if (sBot.Command(lun, kRequestSense, 6, true, buf, 18) != BOT_PASS)
        return ~0u;
    return (buf[2] & 0xf) << 16 | buf[12] << 8 | buf[13];
}

/* Select the test disk behind a unit. */
void select_disk(uint8_t lun)
{
    uint32_t blocks = capacity(lun);
    ASSERT_NE(blocks, 0u);
    if (tc_block_find(blocks) < 0)
        t_skip("the unit's disk is not a test disk");
}

}

TEST(usb_storage, descriptors)
{
    open_bot();
    const uint8_t *id = usb_find_interface(&sBot.dev, sBot.iface);
    ASSERT_TRUE(id != nullptr);
    /* SCSI transparent command set, Bulk-Only (Overview 2, 3) */
    EXPECT_EQ(id[5], USB_CLASS_STORAGE);
    EXPECT_EQ(id[6], 0x06);
    EXPECT_EQ(id[7], 0x50);
    EXPECT_EQ(id[4], 2);
    /* a serial number is required (4.1.1) */
    EXPECT_NE(sBot.dev.desc[16], 0);
    UsbEndpoint ep;
    for (int n = 0; sBot.dev.FindEndpoint(0, n, &ep); n++)
        EXPECT_EQ(ep.attributes & 3, 2);
}

TEST(usb_storage, get_max_lun)
{
    UsbBot *bots = (UsbBot *)usb_alloc(4 * sizeof(UsbBot));
    int n = 0;
    for (; n < 4; n++) {
        new (&bots[n]) UsbBot();
        if (!bots[n].Open(n))
            break;
    }
    if (n == 0)
        t_skip("no USB mass storage device");
    for (int i = 0; i < n; i++) {
        /* the highest LUN, one byte (3.2) */
        int max_lun = bots[i].MaxLun();
        EXPECT_GE(max_lun, 0);
        EXPECT_LE(max_lun, 15);
        char k[16];
        snprintf(k, sizeof(k), "dev%d", i);
        SNAP(k, "%d", max_lun);
        /* every unit up to it answers INQUIRY */
        uint8_t *buf = (uint8_t *)usb_alloc(36);
        for (int lun = 0; lun <= max_lun; lun++)
            EXPECT_EQ(bots[i].Command(lun, kInquiry, 6, true, buf, 36),
                      (uint32_t)BOT_PASS);
    }
    /* wValue 0, the interface, wLength 1, or a Request Error (3.2) */
    UsbDevice &d = bots[0].dev;
    uint8_t *buf = (uint8_t *)usb_alloc(8);
    const uint8_t type = USB_DIR_IN | USB_TYPE_CLASS | USB_RECIP_INTERFACE;
    EXPECT_EQ(d.Control(type, 0xfe, 1, bots[0].iface, 1, buf),
              (uint32_t)CC_STALL);
    d.Recover(1);
    EXPECT_EQ(d.Control(type, 0xfe, 0, bots[0].iface + 1, 1, buf),
              (uint32_t)CC_STALL);
    d.Recover(1);
    EXPECT_EQ(d.Control(type, 0xfe, 0, bots[0].iface, 2, buf),
              (uint32_t)CC_STALL);
    d.Recover(1);
    /* the reset likewise (3.1) */
    EXPECT_EQ(d.Control(USB_TYPE_CLASS | USB_RECIP_INTERFACE, 0xff, 1,
                        bots[0].iface, 0, nullptr), (uint32_t)CC_STALL);
    d.Recover(1);
}

TEST(usb_storage, command_and_status)
{
    open_bot();
    uint8_t *buf = (uint8_t *)usb_alloc(64);
    uint32_t residue = ~0u;
    /* the status wrapper echoes the tag; nothing left over (5.2) */
    ASSERT_EQ(sBot.Command(0, kInquiry, 6, true, buf, 36, &residue),
              (uint32_t)BOT_PASS);
    EXPECT_EQ(residue, 0u);
    EXPECT_EQ(le32(sBot.csw), 0x53425355u);
    EXPECT_EQ(le32(sBot.csw + 4), sBot.tag);
    EXPECT_EQ(buf[0] & 0x1f, 0);
    ASSERT_EQ(sBot.Command(0, kTestUnitReady, 6, false, nullptr, 0,
                           &residue), (uint32_t)BOT_PASS);
    EXPECT_EQ(residue, 0u);
    /* a failed command: status 1, the sense by REQUEST SENSE */
    static const uint8_t kBad[6] = {0xff, 0, 0, 0, 0, 0};
    EXPECT_EQ(sBot.Command(0, kBad, 6, false, nullptr, 0),
              (uint32_t)BOT_FAIL);
    EXPECT_EQ(sense(0), 0x52000u);
    /* a LUN past GET_MAX_LUN (snapshot) */
    int max_lun = sBot.MaxLun();
    uint32_t st = sBot.Command(max_lun + 1, kInquiry, 6, true, buf, 36,
                               &residue);
    SNAP("lun_out_of_range", "%u", st);
    if (st == BOT_TRANSPORT)
        ASSERT_TRUE(sBot.ResetRecovery());
}

TEST(usb_storage, read_write)
{
    open_bot();
    select_disk(0);
    uint32_t blocks = capacity(0);
    uint8_t *buf = (uint8_t *)usb_alloc(16 * BLOCK);
    uint8_t cdb[10];
    uint32_t residue;
    rw10(cdb, 0x28, 3, 8);
    ASSERT_EQ(sBot.Command(0, cdb, 10, true, buf, 8 * BLOCK, &residue),
              (uint32_t)BOT_PASS);
    EXPECT_EQ(residue, 0u);
    EXPECT_EQ(pattern_errors(buf, 3, 8), 0u);
    rw10(cdb, 0x28, blocks - 1, 1);
    ASSERT_EQ(sBot.Command(0, cdb, 10, true, buf, BLOCK), (uint32_t)BOT_PASS);
    EXPECT_EQ(pattern_errors(buf, blocks - 1, 1), 0u);
    /* written and read back */
    for (int i = 0; i < 2 * BLOCK; i++)
        buf[i] = i * 7 + 1;
    rw10(cdb, 0x2a, 40, 2);
    ASSERT_EQ(sBot.Command(0, cdb, 10, false, buf, 2 * BLOCK, &residue),
              (uint32_t)BOT_PASS);
    EXPECT_EQ(residue, 0u);
    memset(buf, 0, 2 * BLOCK);
    rw10(cdb, 0x28, 40, 2);
    ASSERT_EQ(sBot.Command(0, cdb, 10, true, buf, 2 * BLOCK),
              (uint32_t)BOT_PASS);
    uint32_t bad = 0;
    for (int i = 0; i < 2 * BLOCK; i++)
        bad += buf[i] != (uint8_t)(i * 7 + 1);
    EXPECT_EQ(bad, 0u);
    /* past the end: status 1, ILLEGAL REQUEST, LBA OUT OF RANGE */
    rw10(cdb, 0x28, blocks, 1);
    EXPECT_EQ(sBot.Command(0, cdb, 10, true, buf, BLOCK),
              (uint32_t)BOT_FAIL);
    EXPECT_EQ(sense(0), 0x52100u);
}

TEST(usb_storage, write_protected_unit)
{
    open_bot();
    if (sBot.MaxLun() < 1)
        t_skip("one unit only");
    uint8_t *buf = (uint8_t *)usb_alloc(BLOCK);
    uint8_t cdb[10];
    capacity(1);
    rw10(cdb, 0x2a, 0, 1);
    EXPECT_EQ(sBot.Command(1, cdb, 10, false, buf, BLOCK),
              (uint32_t)BOT_FAIL);
    EXPECT_EQ(sense(1), 0x72700u);
    rw10(cdb, 0x28, 0, 1);
    EXPECT_EQ(sBot.Command(1, cdb, 10, true, buf, BLOCK), (uint32_t)BOT_PASS);
    EXPECT_EQ(pattern_errors(buf, 0, 1), 0u);
}

/* The thirteen cases of host and device expectations (6.7): Hn, Hi, Ho
   what the wrapper asks for, Dn, Di, Do what the command does. */
TEST(usb_storage, thirteen_cases)
{
    open_bot();
    select_disk(0);
    uint8_t *buf = (uint8_t *)usb_alloc(8 * BLOCK);
    uint8_t cdb[10];
    uint32_t residue;
    uint32_t st;

    /* 1: Hn = Dn: passed, no residue */
    st = sBot.Command(0, kTestUnitReady, 6, false, nullptr, 0, &residue);
    EXPECT_EQ(st, (uint32_t)BOT_PASS);
    EXPECT_EQ(residue, 0u);
    /* 2: Hn < Di: phase error */
    rw10(cdb, 0x28, 0, 1);
    st = sBot.Command(0, cdb, 10, true, nullptr, 0, &residue);
    EXPECT_EQ(st, (uint32_t)BOT_PHASE);
    ASSERT_TRUE(sBot.ResetRecovery());
    /* 3: Hn < Do: phase error */
    rw10(cdb, 0x2a, 50, 1);
    st = sBot.Command(0, cdb, 10, false, nullptr, 0, &residue);
    EXPECT_EQ(st, (uint32_t)BOT_PHASE);
    ASSERT_TRUE(sBot.ResetRecovery());
    /* 4: Hi > Dn: passed, all of it residue */
    st = sBot.Command(0, kTestUnitReady, 6, true, buf, 64, &residue);
    EXPECT_EQ(st, (uint32_t)BOT_PASS);
    EXPECT_EQ(residue, 64u);
    /* 5: Hi > Di: passed, the difference residue */
    static const uint8_t kInquiry96[6] = {0x12, 0, 0, 0, 36, 0};
    st = sBot.Command(0, kInquiry96, 6, true, buf, 96, &residue);
    EXPECT_EQ(st, (uint32_t)BOT_PASS);
    EXPECT_EQ(residue, 60u);
    /* 6: Hi = Di */
    st = sBot.Command(0, kInquiry, 6, true, buf, 36, &residue);
    EXPECT_EQ(st, (uint32_t)BOT_PASS);
    EXPECT_EQ(residue, 0u);
    /* 7: Hi < Di: phase error */
    rw10(cdb, 0x28, 0, 2);
    st = sBot.Command(0, cdb, 10, true, buf, BLOCK, &residue);
    EXPECT_EQ(st, (uint32_t)BOT_PHASE);
    ASSERT_TRUE(sBot.ResetRecovery());
    /* 8: Hi <> Do: phase error */
    rw10(cdb, 0x2a, 50, 1);
    st = sBot.Command(0, cdb, 10, true, buf, BLOCK, &residue);
    EXPECT_EQ(st, (uint32_t)BOT_PHASE);
    ASSERT_TRUE(sBot.ResetRecovery());
    /* 9: Ho > Dn: passed or failed, all of it residue */
    st = sBot.Command(0, kTestUnitReady, 6, false, buf, 64, &residue);
    EXPECT_TRUE(st == BOT_PASS || st == BOT_FAIL);
    EXPECT_EQ(residue, 64u);
    SNAP("case9", "%u", st);
    /* 10: Ho <> Di: phase error */
    rw10(cdb, 0x28, 0, 1);
    st = sBot.Command(0, cdb, 10, false, buf, BLOCK, &residue);
    EXPECT_EQ(st, (uint32_t)BOT_PHASE);
    ASSERT_TRUE(sBot.ResetRecovery());
    /* 11: Ho > Do: the difference residue */
    rw10(cdb, 0x2a, 50, 1);
    st = sBot.Command(0, cdb, 10, false, buf, 2 * BLOCK, &residue);
    EXPECT_TRUE(st == BOT_PASS || st == BOT_FAIL);
    EXPECT_EQ(residue, (uint32_t)BLOCK);
    SNAP("case11", "%u", st);
    /* 12: Ho = Do */
    st = sBot.Command(0, cdb, 10, false, buf, BLOCK, &residue);
    EXPECT_EQ(st, (uint32_t)BOT_PASS);
    EXPECT_EQ(residue, 0u);
    /* 13: Ho < Do: phase error */
    rw10(cdb, 0x2a, 50, 2);
    st = sBot.Command(0, cdb, 10, false, buf, BLOCK, &residue);
    EXPECT_EQ(st, (uint32_t)BOT_PHASE);
    ASSERT_TRUE(sBot.ResetRecovery());
    /* and after it all, a plain command */
    EXPECT_EQ(sBot.Command(0, kInquiry, 6, true, buf, 36),
              (uint32_t)BOT_PASS);
}

/* A wrapper that is not valid (6.6.1): both bulk endpoints stall until a
   Reset Recovery, clearing the halt alone is not enough (5.3.4). */
TEST(usb_storage, invalid_cbw)
{
    open_bot();
    UsbDevice &d = sBot.dev;
    uint8_t *cbw = (uint8_t *)usb_alloc(64);
    memset(cbw, 0, 64);
    put_le32(cbw, 0x12345678);
    EXPECT_EQ(d.Transfer(sBot.out, cbw, 31), (uint32_t)CC_STALL);
    ASSERT_TRUE(d.Recover(sBot.out));
    uint8_t *buf = (uint8_t *)usb_alloc(64);
    EXPECT_EQ(d.Transfer(sBot.in, buf, 13), (uint32_t)CC_STALL);
    ASSERT_TRUE(d.Recover(sBot.in));
    /* still stalled: a valid command is refused too */
    EXPECT_EQ(sBot.Command(0, kInquiry, 6, true, buf, 36),
              (uint32_t)BOT_TRANSPORT);
    ASSERT_TRUE(sBot.ResetRecovery());
    EXPECT_EQ(sBot.Command(0, kInquiry, 6, true, buf, 36),
              (uint32_t)BOT_PASS);

    /* a wrapper of the wrong length: not meaningful either */
    put_le32(cbw, 0x43425355);
    cbw[14] = 6;
    EXPECT_EQ(d.Transfer(sBot.out, cbw, 30), (uint32_t)CC_STALL);
    d.Recover(sBot.out);
    ASSERT_TRUE(sBot.ResetRecovery());
    /* a command block length of 0 or over 16 (6.2.2): snapshot */
    cbw[14] = 0;
    uint32_t cc = d.Transfer(sBot.out, cbw, 31);
    SNAP("cb_length_0", "%u", cc);
    if (cc == CC_STALL)
        d.Recover(sBot.out);
    ASSERT_TRUE(sBot.ResetRecovery());
    EXPECT_EQ(sBot.Command(0, kInquiry, 6, true, buf, 36),
              (uint32_t)BOT_PASS);
}

/* A command the disk holds: the bulk IN pipe waits for it, and a reset
   abandons it. */
TEST(usb_storage, held_command)
{
    open_bot();
    select_disk(0);
    UsbDevice &d = sBot.dev;
    uint8_t *buf = (uint8_t *)usb_alloc(4 * BLOCK);
    uint8_t cdb[10];
    t_on_cleanup(tc_block_reset_all);
    tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_HOLD);

    /* the data stage NAKs until the disk answers */
    rw10(cdb, 0x28, 9, 4);
    memset(sBot.cbw, 0, 31);
    put_le32(sBot.cbw, 0x43425355);
    put_le32(sBot.cbw + 4, 0x777);
    put_le32(sBot.cbw + 8, 4 * BLOCK);
    sBot.cbw[12] = 0x80;
    sBot.cbw[14] = 10;
    memcpy(sBot.cbw + 15, cdb, 10);
    ASSERT_EQ(d.Transfer(sBot.out, sBot.cbw, 31), (uint32_t)CC_SUCCESS);
    d.Queue(sBot.in, buf, 4 * BLOCK);
    EXPECT_EQ(d.WaitTransfer(sBot.in, nullptr, 20000), (uint32_t)CC_TIMEOUT);
    EXPECT_EQ(tc_block_read(TEST_CONTROL_BLOCK_HELD), 1u);
    tc_block_write(TEST_CONTROL_BLOCK_RELEASE, 1);
    uint32_t residual;
    EXPECT_EQ(d.WaitTransfer(sBot.in, &residual), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(residual, 0u);
    EXPECT_EQ(pattern_errors(buf, 9, 4), 0u);
    uint32_t got;
    ASSERT_EQ(d.Transfer(sBot.in, sBot.csw, 13, &got), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(le32(sBot.csw + 4), 0x777u);
    EXPECT_EQ(sBot.csw[12], BOT_PASS);

    /* held again, then the Bulk-Only Mass Storage Reset: the disk is told
       to give it up (3.1) */
    uint32_t cancels = tc_block_read(TEST_CONTROL_BLOCK_CANCELS);
    ASSERT_EQ(d.Transfer(sBot.out, sBot.cbw, 31), (uint32_t)CC_SUCCESS);
    d.Queue(sBot.in, buf, 4 * BLOCK);
    EXPECT_EQ(d.WaitTransfer(sBot.in, nullptr, 20000), (uint32_t)CC_TIMEOUT);
    Trb ev;
    ASSERT_EQ(d.StopEndpoint(sBot.in), (uint32_t)CC_SUCCESS);
    d.WaitTransfer(sBot.in, nullptr, 20000, &ev);
    d.SkipQueued(sBot.in);
    ASSERT_TRUE(sBot.ResetRecovery());
    EXPECT_EQ(tc_block_read(TEST_CONTROL_BLOCK_CANCELS) - cancels, 1u);
    tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_SYNC);
    EXPECT_EQ(sBot.Command(0, kInquiry, 6, true, buf, 36),
              (uint32_t)BOT_PASS);
}

/* A data stage over many TRBs: chained, each within a 64 KB boundary
   (4.11.7.1), with an Event Data TRB reporting the total. */
TEST(usb_storage, chained_transfer)
{
    open_bot();
    select_disk(0);
    UsbDevice &d = sBot.dev;
    const uint32_t blocks = 72, len = blocks * BLOCK;
    uint8_t *buf = (uint8_t *)usb_alloc(len, 4096);
    uint8_t cdb[10];
    rw10(cdb, 0x28, 100, blocks);
    memset(sBot.cbw, 0, 31);
    put_le32(sBot.cbw, 0x43425355);
    put_le32(sBot.cbw + 4, 0x999);
    put_le32(sBot.cbw + 8, len);
    sBot.cbw[12] = 0x80;
    sBot.cbw[14] = 10;
    memcpy(sBot.cbw + 15, cdb, 10);
    ASSERT_EQ(d.Transfer(sBot.out, sBot.cbw, 31), (uint32_t)CC_SUCCESS);
    /* in 4 KB pieces, a page each, as a scatter list would come */
    XhciRing &r = d.rings[sBot.in];
    for (uint32_t off = 0; off < len; off += 4096) {
        uint32_t n = min(4096u, len - off);
        r.Push((uint32_t)(uintptr_t)(buf + off), 0, n,
               TRB_TYPE(TRB_NORMAL) | TRB_CH);
    }
    r.Push(0x5555, 0x6666, 0, TRB_TYPE(TRB_EVENT_DATA) | TRB_IOC);
    d.hc->Doorbell(d.slot, sBot.in);
    Trb ev;
    ASSERT_EQ(d.WaitTransfer(sBot.in, nullptr, 2000000, &ev),
              (uint32_t)CC_SUCCESS);
    EXPECT_TRUE(ev.d[3] & (1u << 2));
    EXPECT_EQ(ev.Param(), 0x0000666600005555ull);
    EXPECT_EQ(ev.Residual(), len);
    EXPECT_EQ(pattern_errors(buf, 100, blocks), 0u);
    uint32_t got;
    ASSERT_EQ(d.Transfer(sBot.in, sBot.csw, 13, &got), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(sBot.csw[12], BOT_PASS);
    EXPECT_EQ(le32(sBot.csw + 8), 0u);

    /* the same short: the TD ends at the short packet, which ISP on every
       TRB reports with that TRB's residual (4.10.1.1) */
    rw10(cdb, 0x28, 100, 2);
    put_le32(sBot.cbw + 4, 0x99a);
    put_le32(sBot.cbw + 8, 2 * BLOCK);
    memcpy(sBot.cbw + 15, cdb, 10);
    ASSERT_EQ(d.Transfer(sBot.out, sBot.cbw, 31), (uint32_t)CC_SUCCESS);
    uint64_t first = r.Push((uint32_t)(uintptr_t)buf, 0, 4096,
                            TRB_TYPE(TRB_NORMAL) | TRB_CH | TRB_ISP);
    r.Push((uint32_t)(uintptr_t)(buf + 4096), 0, 4096,
           TRB_TYPE(TRB_NORMAL) | TRB_ISP | TRB_IOC);
    d.hc->Doorbell(d.slot, sBot.in);
    ASSERT_EQ(d.WaitTransfer(sBot.in, nullptr, 2000000, &ev),
              (uint32_t)CC_SHORT_PACKET);
    EXPECT_EQ(ev.Param(), first);
    EXPECT_EQ(ev.Residual(), 4096u - 2 * BLOCK);
    /* nothing more from the TD */
    EXPECT_EQ(d.WaitTransfer(sBot.in, nullptr, 20000), (uint32_t)CC_TIMEOUT);
    ASSERT_EQ(d.Transfer(sBot.in, sBot.csw, 13, &got), (uint32_t)CC_SUCCESS);
    EXPECT_EQ(sBot.csw[12], BOT_PASS);
}

/* The storage device behind the hub, by route string. */
TEST(usb_storage, behind_hub)
{
    bool found = false;
    for (int i = 0; i < 4; i++) {
        if (!sBot.Open(i))
            break;
        if (sBot.dev.route == 0)
            continue;
        found = true;
        uint8_t *buf = (uint8_t *)usb_alloc(BLOCK);
        select_disk(0);
        uint8_t cdb[10];
        rw10(cdb, 0x28, 5, 1);
        ASSERT_EQ(sBot.Command(0, cdb, 10, true, buf, BLOCK),
                  (uint32_t)BOT_PASS);
        EXPECT_EQ(pattern_errors(buf, 5, 1), 0u);
    }
    if (!found)
        t_skip("no mass storage device behind a hub");
}
