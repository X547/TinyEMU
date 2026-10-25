/*
 * BusLogic BT-958 tests
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
#include "heap.h"
#include "pci.h"
#include "test.h"
#include "test_control.h"
#include "testctl.h"

/* The BusLogic MultiMaster host adapter, a BT-958 on PCI, as its drivers
   use it: the four registers, the host adapter commands, and 32 bit
   mailboxes and CCBs. There is no public specification to hold it to, so
   most of what it says is recorded rather than asserted; what the drivers
   depend on is asserted. */

#define CONTROL 0
#define STATUS  0
#define COMMAND 1
#define DATA    1
#define INTR    2
#define GEOM    3

#define RHARD 0x80
#define RSOFT 0x40
#define RINT  0x20
#define RSBUS 0x10

#define DACT   0x80
#define INREQ  0x20
#define HARDY  0x10
#define DIRRDY 0x04
#define CMDINV 0x01

#define INTV 0x80
#define RSTS 0x08
#define CMDC 0x04
#define MBOR 0x02
#define IMBL 0x01

#define MAILBOXES 4
#define LIMIT_4G 0x100000000ull

namespace {

PciFunc *sFn;
int sBar;
uint8_t *sMbox;
int sOut;
int sIn;

uint8_t rd(int reg) {return pci_bar_read(sFn, sBar, reg, 0);}
void wr(int reg, uint8_t v) {pci_bar_write(sFn, sBar, reg, v, 0);}

void cleanup()
{
    wr(CONTROL, RHARD);
    t_wait_until([] {return (rd(STATUS) & (DACT | HARDY)) == HARDY;});
    pci_restore_all();
}

void *alloc(size_t size, uint8_t fill = 0)
{
    void *p = heap_alloc(size ? size : 1, 16, LIMIT_4G);
    if (p == nullptr)
        t_skip("no memory");
    memset(p, fill, size);
    return p;
}

/* After a hard reset, waiting for the self test. */
void need_adapter()
{
    pci_enumerate();
    sFn = pci_require(0x104b, 0x1040);
    sBar = sFn->bars[0].size &&
           pci_reachable(sFn->host, sFn->bars[0].addr, 4, true) ? 0 : 1;
    t_on_cleanup(cleanup);
    wr(CONTROL, RHARD);
    ASSERT_TRUE(t_wait_until([] {
        return (rd(STATUS) & (DACT | HARDY)) == HARDY;
    }));
}

/* A host adapter command: its reply, and whether CMDC came. */
bool command(uint8_t cmd, const uint8_t *params, int n, uint8_t *reply,
             int reply_len)
{
    wr(COMMAND, cmd);
    for (int i = 0; i < n; i++)
        wr(COMMAND, params[i]);
    for (int i = 0; i < reply_len; i++) {
        if (!t_wait_until([] {return (rd(STATUS) & DIRRDY) != 0;}))
            return false;
        reply[i] = rd(DATA);
    }
    bool done = t_wait_until([] {return (rd(INTR) & CMDC) != 0;});
    wr(CONTROL, RINT);
    return done;
}

void init_mailboxes()
{
    sMbox = (uint8_t *)alloc(2 * MAILBOXES * 8);
    uint8_t p[5] = {MAILBOXES};
    put_le32(p + 1, (uint32_t)(uintptr_t)sMbox);
    ASSERT_TRUE(command(0x81, p, 5, nullptr, 0));
    EXPECT_EQ(rd(STATUS) & INREQ, 0);
    sOut = sIn = 0;
}

/* A 32 bit CCB for the target's LUN 0. */
uint8_t *make_ccb(uint8_t op, int target, const uint8_t *cdb, int cdb_len,
                  uint8_t dir, void *data, uint32_t len)
{
    uint8_t *ccb = (uint8_t *)alloc(40);
    uint8_t *sense = (uint8_t *)alloc(32, 0xee);
    ccb[0] = op;
    ccb[1] = dir;
    ccb[2] = cdb_len;
    ccb[3] = 32;
    put_le32(ccb + 4, len);
    put_le32(ccb + 8, (uint32_t)(uintptr_t)data);
    ccb[16] = target;
    memcpy(ccb + 18, cdb, cdb_len);
    put_le32(ccb + 36, (uint32_t)(uintptr_t)sense);
    return ccb;
}

void post(uint8_t *ccb, uint8_t action = 0x01)
{
    uint8_t *mbo = sMbox + sOut * 8;
    put_le32(mbo, (uint32_t)(uintptr_t)ccb);
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    mbo[7] = action;
    sOut = (sOut + 1) % MAILBOXES;
    wr(COMMAND, 0x02);
}

/* The next incoming mailbox: the CCB, host adapter status, SCSI status
   and completion code, taken. */
struct InMail {
    uint32_t ccb;
    uint8_t btstat;
    uint8_t sdstat;
    uint8_t code;
};

bool take(InMail *m, uint64_t timeout = 1000000)
{
    uint8_t *mbi = sMbox + (MAILBOXES + sIn) * 8;
    if (!t_wait_until([mbi] {return *(volatile uint8_t *)(mbi + 7) != 0;},
                      timeout))
        return false;
    *m = InMail {le32(mbi), mbi[4], mbi[5], mbi[7]};
    mbi[7] = 0;
    sIn = (sIn + 1) % MAILBOXES;
    wr(CONTROL, RINT);
    return true;
}

/* The target of the first disk on a test disk, by READ CAPACITY. */
int disk_target()
{
    for (int t = 0; t < 16; t++) {
        uint8_t cap[10] = {0x25};
        uint8_t *buf = (uint8_t *)alloc(8);
        uint8_t *ccb = make_ccb(0x00, t, cap, 10, 0x08, buf, 8);
        post(ccb);
        InMail m;
        if (!take(&m) || m.code != 1)
            continue;
        if (tc_block_find(be32(buf) + 1) >= 0)
            return t;
    }
    t_skip("no disk on a test disk");
}

}


/* After a hard reset: diagnostics, then ready for a command with the
   mailboxes still to be set up. */
FRESH_TEST(buslogic, reset_state)
{
    pci_enumerate();
    sFn = pci_require(0x104b, 0x1040);
    sBar = sFn->bars[0].size &&
           pci_reachable(sFn->host, sFn->bars[0].addr, 4, true) ? 0 : 1;
    t_on_cleanup(cleanup);
    SNAP("status", "%#x", rd(STATUS));
    SNAP("intr", "%#x", rd(INTR));
    SNAP("geometry", "%#x", rd(GEOM));
    wr(CONTROL, RHARD);
    uint8_t first = rd(STATUS);
    SNAP("after_hard_reset", "%#x", first);
    EXPECT_TRUE(t_wait_until([] {return rd(STATUS) == (HARDY | INREQ);}));
    SNAP("bars", "io %llu mem %llu", T_FMT_VAL(sFn->bars[0].size),
         T_FMT_VAL(sFn->bars[1].size));
}

/* What the adapter says of itself: the board, model and firmware, its
   configuration and setup. */
TEST(buslogic, inquiries)
{
    need_adapter();
    uint8_t r[64];
    ASSERT_TRUE(command(0x04, nullptr, 0, r, 4));
    SNAP("board_id", "%c%c%c%c", r[0], r[1], r[2], r[3]);
    ASSERT_TRUE(command(0x84, nullptr, 0, r, 1));
    uint8_t digit = r[0];
    ASSERT_TRUE(command(0x85, nullptr, 0, r, 1));
    SNAP("firmware", "%c%c", digit, r[0]);
    uint8_t len = 5;
    ASSERT_TRUE(command(0x8b, &len, 1, r, 5));
    SNAP("model", "%c%c%c%c%c", r[0], r[1], r[2], r[3], r[4]);
    ASSERT_TRUE(command(0x0b, nullptr, 0, r, 3));
    SNAP("config", "%02x %02x %02x", r[0], r[1], r[2]);
    EXPECT_EQ(r[2], 7);
    len = 16;
    ASSERT_TRUE(command(0x8d, &len, 1, r, 16));
    SNAP("ext_setup", "%02x %02x %04x %02x %02x %c%c%c %02x", r[0], r[1],
         le16(r + 2), r[4], r[9], r[10], r[11], r[12], r[13]);
    ASSERT_TRUE(command(0x86, nullptr, 0, r, 4));
    SNAP("pci_info", "%02x %02x %02x", r[0], r[2], r[3]);
    len = 8;
    ASSERT_TRUE(command(0x0d, &len, 1, r, 8));
    SNAP("setup", "%02x %02x%02x%02x", r[4], r[5], r[6], r[7]);
    uint8_t echo = 0xa5;
    ASSERT_TRUE(command(0x1f, &echo, 1, r, 1));
    EXPECT_EQ(r[0], 0xa5);
}

/* The installed devices: a LUN bit for each unit, by target; the
   adapter's own ID 7 is never one. */
TEST(buslogic, installed_devices)
{
    need_adapter();
    uint8_t r[8], hi[8], map[2];
    ASSERT_TRUE(command(0x0a, nullptr, 0, r, 8));
    ASSERT_TRUE(command(0x23, nullptr, 0, hi, 8));
    ASSERT_TRUE(command(0x24, nullptr, 0, map, 2));
    EXPECT_EQ(r[7], 0);
    uint16_t targets = 0;
    for (int t = 0; t < 8; t++) {
        if (r[t])
            targets |= 1 << t;
        if (hi[t])
            targets |= 1 << (t + 8);
    }
    EXPECT_EQ(le16(map), targets);
    SNAP("devices", "%02x%02x%02x%02x%02x%02x%02x%02x %04x", r[0], r[1], r[2],
         r[3], r[4], r[5], r[6], r[7], targets);
}

/* A command it does not know sets CMDINV, with CMDC; TEST CMDC INTERRUPT
   raises CMDC alone. Interrupts can be turned off for the line, and the
   register still shows them. */
TEST(buslogic, adapter_commands)
{
    need_adapter();
    wr(COMMAND, 0xf7);
    EXPECT_TRUE(t_wait_until([] {return (rd(INTR) & CMDC) != 0;}));
    EXPECT_EQ(rd(STATUS) & CMDINV, CMDINV);
    EXPECT_EQ(rd(INTR), INTV | CMDC);
    wr(CONTROL, RINT);
    EXPECT_EQ(rd(INTR), 0);
    wr(COMMAND, 0x00);
    EXPECT_EQ(rd(INTR), INTV | CMDC);
    EXPECT_EQ(rd(STATUS) & CMDINV, 0);
    wr(CONTROL, RINT);
    uint8_t off = 0;
    EXPECT_TRUE(command(0x25, &off, 1, nullptr, 0));
    uint8_t zero = 0;
    uint8_t *mb = (uint8_t *)alloc(8);
    uint8_t bad[5] = {zero};
    put_le32(bad + 1, (uint32_t)(uintptr_t)mb);
    EXPECT_TRUE(command(0x81, bad, 5, nullptr, 0));
    EXPECT_EQ(rd(STATUS) & (CMDINV | INREQ), CMDINV | INREQ);
}

/* A CCB started through a mailbox comes back in an incoming one with its
   status, the data moved, auto sense on CHECK CONDITION, and with
   opcode 3 the residual in its data length. */
TEST(buslogic, ccb)
{
    need_adapter();
    init_mailboxes();
    int t = disk_target();
    uint8_t *buf = (uint8_t *)alloc(1024, 0xee);
    uint8_t rd10[10] = {0x28, 0, 0, 0, 0, 4, 0, 0, 1, 0};
    uint8_t *ccb = make_ccb(0x03, t, rd10, 10, 0x08, buf, 1024);
    post(ccb);
    InMail m;
    ASSERT_TRUE(take(&m));
    EXPECT_EQ(m.ccb, (uint32_t)(uintptr_t)ccb);
    EXPECT_EQ(m.code, 1);
    EXPECT_EQ(m.btstat, 0);
    EXPECT_EQ(m.sdstat, 0);
    EXPECT_EQ(le32(ccb + 4), 512u);
    EXPECT_EQ(ccb[14], 0);
    EXPECT_EQ(buf[0], TEST_BLOCK_PATTERN(4, 0));
    EXPECT_EQ(buf[512], 0xee);

    uint8_t bad[6] = {0x1d};
    ccb = make_ccb(0x00, t, bad, 6, 0x18, nullptr, 0);
    post(ccb);
    ASSERT_TRUE(take(&m));
    EXPECT_EQ(m.code, 4);
    EXPECT_EQ(m.sdstat, 2);
    EXPECT_EQ(ccb[15], 2);
    uint8_t *sense = (uint8_t *)(uintptr_t)le32(ccb + 36);
    EXPECT_EQ(sense[2] & 0x0f, 5);
    EXPECT_EQ(sense[12], 0x20);

    /* a sense length of 1 asks for none */
    ccb = make_ccb(0x00, t, bad, 6, 0x18, nullptr, 0);
    ccb[3] = 1;
    post(ccb);
    ASSERT_TRUE(take(&m));
    sense = (uint8_t *)(uintptr_t)le32(ccb + 36);
    EXPECT_EQ(sense[0], 0xee);
}

/* Scatter/gather: the data length is that of a list of length and address
   pairs. */
TEST(buslogic, scatter_gather)
{
    need_adapter();
    init_mailboxes();
    int t = disk_target();
    uint8_t *a = (uint8_t *)alloc(300, 0xee);
    uint8_t *b = (uint8_t *)alloc(724, 0xee);
    uint8_t *list = (uint8_t *)alloc(16);
    put_le32(list, 300);
    put_le32(list + 4, (uint32_t)(uintptr_t)a);
    put_le32(list + 8, 724);
    put_le32(list + 12, (uint32_t)(uintptr_t)b);
    uint8_t rd10[10] = {0x28, 0, 0, 0, 0, 8, 0, 0, 2, 0};
    uint8_t *ccb = make_ccb(0x04, t, rd10, 10, 0x08, list, 16);
    post(ccb);
    InMail m;
    ASSERT_TRUE(take(&m));
    EXPECT_EQ(m.code, 1);
    EXPECT_EQ(le32(ccb + 4), 0u);
    EXPECT_EQ(a[0], TEST_BLOCK_PATTERN(8, 0));
    EXPECT_EQ(a[299], TEST_BLOCK_PATTERN(8, 299));
    EXPECT_EQ(b[0], TEST_BLOCK_PATTERN(8, 300));
    EXPECT_EQ(b[723], TEST_BLOCK_PATTERN(9, 511));
}

/* Errors the adapter reports itself: a target nobody answers for times
   out (11h), as does its own ID; an opcode it does not have is 16h; a CDB
   of no length is 1Ah. */
TEST(buslogic, adapter_errors)
{
    need_adapter();
    init_mailboxes();
    uint8_t tur[6] = {0};
    InMail m;
    uint8_t *ccb = make_ccb(0x00, 15, tur, 6, 0x18, nullptr, 0);
    post(ccb);
    ASSERT_TRUE(take(&m));
    EXPECT_EQ(m.code, 4);
    EXPECT_EQ(m.btstat, 0x11);
    ccb = make_ccb(0x00, 7, tur, 6, 0x18, nullptr, 0);
    post(ccb);
    ASSERT_TRUE(take(&m));
    EXPECT_EQ(m.btstat, 0x11);
    ccb = make_ccb(0x42, 0, tur, 6, 0x18, nullptr, 0);
    post(ccb);
    ASSERT_TRUE(take(&m));
    EXPECT_EQ(m.btstat, 0x16);
    ccb = make_ccb(0x00, 0, tur, 0, 0x18, nullptr, 0);
    post(ccb);
    ASSERT_TRUE(take(&m));
    EXPECT_EQ(m.btstat, 0x1a);
}

/* A CCB held by the disk is aborted through a mailbox (completion code 2
   for the abort, the CCB never coming back), and one not there is not
   found (3). A bus device reset ends the target's CCBs with 23h, a SCSI
   bus reset all of them with 22h and RSTS. */
TEST(buslogic, abort_and_reset)
{
    need_adapter();
    init_mailboxes();
    int t = disk_target();
    tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_HOLD);
    uint32_t cancels = tc_block_read(TEST_CONTROL_BLOCK_CANCELS);
    uint8_t *buf = (uint8_t *)alloc(512);
    uint8_t rd10[10] = {0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0};
    uint8_t *ccb = make_ccb(0x00, t, rd10, 10, 0x08, buf, 512);
    post(ccb);
    EXPECT_TRUE(t_wait_until([] {
        return tc_block_read(TEST_CONTROL_BLOCK_HELD) == 1;
    }));
    post(ccb, 0x02);
    InMail m;
    ASSERT_TRUE(take(&m));
    EXPECT_EQ(m.code, 2);
    EXPECT_EQ(tc_block_read(TEST_CONTROL_BLOCK_CANCELS) - cancels, 1u);
    post(ccb, 0x02);
    ASSERT_TRUE(take(&m));
    EXPECT_EQ(m.code, 3);

    ccb = make_ccb(0x00, t, rd10, 10, 0x08, buf, 512);
    post(ccb);
    EXPECT_TRUE(t_wait_until([] {
        return tc_block_read(TEST_CONTROL_BLOCK_HELD) == 1;
    }));
    uint8_t *bdr = make_ccb(0x81, t, rd10, 0, 0x18, nullptr, 0);
    post(bdr);
    InMail first, second;
    ASSERT_TRUE(take(&first));
    ASSERT_TRUE(take(&second));
    InMail held = first.ccb == (uint32_t)(uintptr_t)ccb ? first : second;
    InMail reset = first.ccb == (uint32_t)(uintptr_t)ccb ? second : first;
    EXPECT_EQ(held.btstat, 0x23);
    EXPECT_EQ(reset.code, 1);

    ccb = make_ccb(0x00, t, rd10, 10, 0x08, buf, 512);
    post(ccb);
    EXPECT_TRUE(t_wait_until([] {
        return tc_block_read(TEST_CONTROL_BLOCK_HELD) == 1;
    }));
    wr(CONTROL, RSBUS);
    ASSERT_TRUE(take(&m));
    EXPECT_EQ(m.btstat, 0x22);
    SNAP("intr_after_bus_reset", "%#x", rd(INTR));
}

/* A completion waits for its incoming mailbox to be emptied, and the
   adapter interrupts again for it. MBOR, once enabled, says an outgoing
   mailbox was taken. */
TEST(buslogic, mailbox_flow)
{
    need_adapter();
    init_mailboxes();
    uint8_t on = 1;
    ASSERT_TRUE(command(0x05, &on, 1, nullptr, 0));
    uint8_t tur[6] = {0};
    uint8_t *ccbs[MAILBOXES + 2];
    for (int i = 0; i < MAILBOXES + 2; i++)
        ccbs[i] = make_ccb(0x00, 15, tur, 6, 0x18, nullptr, 0);
    for (int i = 0; i < MAILBOXES; i++)
        post(ccbs[i]);
    SNAP("intr_after_starts", "%#x", rd(INTR));
    /* the incoming ring is full; two more wait */
    post(ccbs[MAILBOXES]);
    post(ccbs[MAILBOXES + 1]);
    for (int i = 0; i < MAILBOXES + 2; i++) {
        InMail m;
        ASSERT_TRUE(take(&m));
        EXPECT_EQ(m.ccb, (uint32_t)(uintptr_t)ccbs[i]);
    }
}

/* The adapter's interrupt reaches the controller the tables give for
   its INTx pin, and stays until acknowledged. */
TEST(buslogic, interrupt_line)
{
    need_adapter();
    static volatile int count;
    count = 0;
    if (!pci_intx_route(sFn, [](void *) {
            count = count + 1;
            wr(CONTROL, RINT);
        }, nullptr))
        t_skip("its interrupt reaches no controller the suite drives");
    t_on_cleanup([] {pci_intx_unroute(sFn);});
    arch_irq_enable();
    wr(COMMAND, 0x00);
    EXPECT_TRUE(t_wait_until([] {return count > 0;}));
    arch_irq_disable();
    EXPECT_EQ(count, 1);
    uint8_t off = 0;
    EXPECT_TRUE(command(0x25, &off, 1, nullptr, 0));
    count = 0;
    arch_irq_enable();
    wr(COMMAND, 0x00);
    t_delay_us(2000);
    arch_irq_disable();
    EXPECT_EQ(count, 0);
    EXPECT_EQ(rd(INTR), INTV | CMDC);
}
