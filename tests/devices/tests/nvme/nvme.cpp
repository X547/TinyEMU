/*
 * NVMe tests
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
#include "pci.h"
#include "test.h"
#include "test_control.h"
#include "testctl.h"

/* An NVMe controller as the NVM Express Base Specification 1.4 has it:
   the controller registers (3.1), the admin and I/O queues (4), and the
   admin (5) and NVM (6) commands, on test disks as namespaces. A status
   below is SCT << 8 | SC. */

#define CAP_LO 0x00
#define CAP_HI 0x04
#define VS     0x08
#define INTMS  0x0c
#define INTMC  0x10
#define CC     0x14
#define CSTS   0x1c
#define AQA    0x24
#define ASQ    0x28
#define ACQ    0x30
#define DOORBELL 0x1000

#define CC_EN      0x1
#define CC_IOSQES  (6 << 16)
#define CC_IOCQES  (4 << 20)
#define CSTS_RDY   0x1
#define CSTS_CFS   0x2

#define ADM_DELETE_SQ 0x00
#define ADM_CREATE_SQ 0x01
#define ADM_GET_LOG   0x02
#define ADM_DELETE_CQ 0x04
#define ADM_CREATE_CQ 0x05
#define ADM_IDENTIFY  0x06
#define ADM_ABORT     0x08
#define ADM_SET_FEAT  0x09
#define ADM_GET_FEAT  0x0a
#define ADM_AER       0x0c

#define IO_FLUSH 0x00
#define IO_WRITE 0x01
#define IO_READ  0x02

#define SC_OK              0x000
#define SC_INVALID_OPCODE  0x001
#define SC_INVALID_FIELD   0x002
#define SC_ABORT_REQUESTED 0x007
#define SC_ABORT_SQ_DELETE 0x008
#define SC_INVALID_NS      0x00b
#define SC_PRP_OFFSET      0x013
#define SC_NS_WRITE_PROT   0x020
#define SC_LBA_RANGE       0x080
#define SC_CQ_INVALID      0x100
#define SC_QID_INVALID     0x101
#define SC_QSIZE_INVALID   0x102
#define SC_AER_LIMIT       0x105
#define SC_INVALID_LOG     0x109
#define SC_QUEUE_DELETION  0x10c
#define SC_WRITE_FAULT     0x280
#define SC_UNRECOVERED_READ 0x281

#define PAGE 4096
#define QSIZE 16
#define SECTOR 512
#define NO_STATUS 0xffff

namespace {

struct Queue {
    uint8_t *sq;
    uint8_t *cq;
    uint16_t qid;
    uint16_t size;
    uint16_t tail;
    uint16_t head;
    bool phase;
};

PciFunc *sFn;
Queue sAdmin;
Queue sIo;
uint16_t sCid;

uint32_t rd(uint32_t off) {return pci_bar_read(sFn, 0, off, 2);}
void wr(uint32_t off, uint32_t v) {pci_bar_write(sFn, 0, off, v, 2);}

void *alloc(size_t size, uint8_t fill = 0, size_t align = PAGE)
{
    void *p = heap_alloc(size ? size : 1, align);
    if (p == nullptr)
        t_skip("no memory");
    memset(p, fill, size);
    return p;
}

void cleanup()
{
    wr(CC, 0);
    t_wait_until([] {return (rd(CSTS) & CSTS_RDY) == 0;});
    pci_restore_all();
}

struct Cmd {
    uint8_t op;
    uint32_t nsid = 0;
    uint64_t prp1 = 0;
    uint64_t prp2 = 0;
    uint32_t cdw10 = 0, cdw11 = 0, cdw12 = 0, cdw13 = 0;
};

struct Cqe {
    uint32_t dw0;
    uint16_t sqhd;
    uint16_t sqid;
    uint16_t cid;
    uint16_t status;  /* SCT << 8 | SC */
};

/* Queue a command; its command ID. */
uint16_t submit(Queue &q, const Cmd &c, bool ring = true)
{
    uint8_t *e = q.sq + 64 * q.tail;
    memset(e, 0, 64);
    uint16_t cid = ++sCid;
    put_le32(e, c.op | (uint32_t)cid << 16);
    put_le32(e + 4, c.nsid);
    put_le32(e + 24, (uint32_t)c.prp1);
    put_le32(e + 28, c.prp1 >> 32);
    put_le32(e + 32, (uint32_t)c.prp2);
    put_le32(e + 36, c.prp2 >> 32);
    put_le32(e + 40, c.cdw10);
    put_le32(e + 44, c.cdw11);
    put_le32(e + 48, c.cdw12);
    put_le32(e + 52, c.cdw13);
    q.tail = (q.tail + 1) % q.size;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    if (ring)
        wr(DOORBELL + 8 * q.qid, q.tail);
    return cid;
}

/* The next completion, waited for; false when none came. */
bool reap(Queue &q, Cqe *out, uint64_t timeout = 1000000)
{
    volatile uint8_t *e = q.cq + 16 * q.head;
    if (!t_wait_until([&] {
            return ((le16((const void *)(e + 14)) & 1) != 0) == q.phase;
        }, timeout))
        return false;
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
    uint32_t dw3 = le32((const void *)(e + 12));
    out->dw0 = le32((const void *)e);
    out->sqhd = le16((const void *)(e + 8));
    out->sqid = le16((const void *)(e + 10));
    out->cid = dw3 & 0xffff;
    out->status = (dw3 >> 17) & 0x7ff;
    q.head = (q.head + 1) % q.size;
    if (q.head == 0)
        q.phase = !q.phase;
    wr(DOORBELL + 8 * q.qid + 4, q.head);
    return true;
}

/* An admin command, waited for: its status, dw0 in 'dw0'. */
uint16_t admin(const Cmd &c, uint32_t *dw0 = nullptr)
{
    uint16_t cid = submit(sAdmin, c);
    Cqe e;
    if (!reap(sAdmin, &e)) {
        t_fail(__FILE__, __LINE__, "no answer to admin opcode %#x", c.op);
        return NO_STATUS;
    }
    EXPECT_EQ(e.cid, cid);
    if (dw0)
        *dw0 = e.dw0;
    return e.status;
}

uint16_t io(const Cmd &c)
{
    uint16_t cid = submit(sIo, c);
    Cqe e;
    if (!reap(sIo, &e)) {
        t_fail(__FILE__, __LINE__, "no answer to I/O opcode %#x", c.op);
        return NO_STATUS;
    }
    EXPECT_EQ(e.cid, cid);
    return e.status;
}

void make_queue(Queue &q, uint16_t qid, uint16_t size)
{
    q = Queue {};
    q.sq = (uint8_t *)alloc(PAGE);
    q.cq = (uint8_t *)alloc(PAGE);
    q.qid = qid;
    q.size = size;
    q.phase = true;
}

/* Reset, then the admin queues, enabled, with 4 KB pages. */
void need_controller()
{
    pci_enumerate();
    sFn = pci_require(0x1b36, 0x0010);
    t_on_cleanup(cleanup);
    wr(CC, 0);
    ASSERT_TRUE(t_wait_until([] {return (rd(CSTS) & CSTS_RDY) == 0;}));
    make_queue(sAdmin, 0, QSIZE);
    sCid = 0;
    wr(AQA, (QSIZE - 1) | (QSIZE - 1) << 16);
    wr(ASQ, (uint32_t)(uintptr_t)sAdmin.sq);
    wr(ASQ + 4, (uint64_t)(uintptr_t)sAdmin.sq >> 32);
    wr(ACQ, (uint32_t)(uintptr_t)sAdmin.cq);
    wr(ACQ + 4, (uint64_t)(uintptr_t)sAdmin.cq >> 32);
    wr(CC, CC_EN | CC_IOSQES | CC_IOCQES);
    ASSERT_TRUE(t_wait_until([] {return (rd(CSTS) & CSTS_RDY) != 0;}));
}

uint16_t create_cq(uint16_t qid, uint16_t size, void *base, bool ien = false,
                   uint16_t vector = 0)
{
    Cmd c {ADM_CREATE_CQ};
    c.prp1 = (uintptr_t)base;
    c.cdw10 = qid | (uint32_t)(size - 1) << 16;
    c.cdw11 = 1 | (ien ? 2 : 0) | (uint32_t)vector << 16;
    return admin(c);
}

uint16_t create_sq(uint16_t qid, uint16_t size, void *base, uint16_t cqid)
{
    Cmd c {ADM_CREATE_SQ};
    c.prp1 = (uintptr_t)base;
    c.cdw10 = qid | (uint32_t)(size - 1) << 16;
    c.cdw11 = 1 | (uint32_t)cqid << 16;
    return admin(c);
}

/* An I/O queue pair 1 as well. */
void need_io(bool ien = false, uint16_t vector = 0)
{
    need_controller();
    make_queue(sIo, 1, QSIZE);
    ASSERT_EQ(create_cq(1, QSIZE, sIo.cq, ien, vector), SC_OK);
    ASSERT_EQ(create_sq(1, QSIZE, sIo.sq, 1), SC_OK);
}

/* The namespace that is a test disk of the given write protection. */
uint32_t find_ns(bool read_only)
{
    uint8_t *id = (uint8_t *)alloc(PAGE);
    for (uint32_t nsid = 1; nsid <= 8; nsid++) {
        Cmd c {ADM_IDENTIFY};
        c.nsid = nsid;
        c.prp1 = (uintptr_t)id;
        if (admin(c) != SC_OK)
            continue;
        uint64_t size = le64(id);
        if (size == 0 || ((id[99] & 1) != 0) != read_only)
            continue;
        if (tc_block_find(size) >= 0)
            return nsid;
    }
    t_skip("no %s namespace on a test disk", read_only ? "write protected" :
           "writable");
}

Cmd rw(uint8_t op, uint32_t nsid, uint64_t lba, uint32_t blocks, void *buf)
{
    Cmd c {op};
    c.nsid = nsid;
    c.prp1 = (uintptr_t)buf;
    c.cdw10 = (uint32_t)lba;
    c.cdw11 = lba >> 32;
    c.cdw12 = blocks - 1;
    return c;
}

uint32_t pattern_errors(const uint8_t *buf, uint64_t lba, uint32_t n)
{
    uint32_t bad = 0;
    for (uint32_t s = 0; s < n; s++) {
        for (uint32_t i = 0; i < SECTOR; i++)
            bad += buf[s * SECTOR + i] != TEST_BLOCK_PATTERN(lba + s, i);
    }
    return bad;
}

}


/* The capabilities (3.1.1): queues of up to MQES + 1 entries, contiguous
   ones required here, the NVM command set, and page sizes from MPSMIN to
   MPSMAX; the version is at least 1.0. */
TEST(nvme, registers)
{
    pci_enumerate();
    sFn = pci_require(0x1b36, 0x0010);
    t_on_cleanup(cleanup);
    uint64_t cap = rd(CAP_LO) | (uint64_t)rd(CAP_HI) << 32;
    EXPECT_GE(cap & 0xffff, 1u);
    EXPECT_TRUE(cap >> 37 & 1);
    EXPECT_LE((cap >> 48) & 0xf, (cap >> 52) & 0xf);
    EXPECT_GE(rd(VS), 0x00010000u);
    SNAP("cap", "mqes %u cqr %u to %u dstrd %u mps %u-%u",
         (unsigned)(cap & 0xffff), (unsigned)(cap >> 16 & 1),
         (unsigned)(cap >> 24 & 0xff), (unsigned)(cap >> 32 & 0xf),
         (unsigned)(cap >> 48 & 0xf), (unsigned)(cap >> 52 & 0xf));
    SNAP("vs", "%#x", rd(VS));
    wr(AQA, 0x00030007);
    EXPECT_EQ(rd(AQA), 0x00030007u);
    wr(ASQ, 0x12345000);
    wr(ASQ + 4, 0x1);
    EXPECT_EQ(rd(ASQ), 0x12345000u);
    EXPECT_EQ(rd(ASQ + 4), 1u);
    SNAP("doorbell_read", "%#x", rd(DOORBELL));
}

/* Enabling with a page size past MPSMAX cannot be honoured; then a proper
   enable sets RDY, and a shutdown request reports itself complete in
   CSTS.SHST (3.1.5, 3.1.6, 7.6.2). Clearing EN takes RDY away. */
TEST(nvme, enable_and_shutdown)
{
    need_controller();
    wr(CC, 0);
    EXPECT_TRUE(t_wait_until([] {return (rd(CSTS) & CSTS_RDY) == 0;}));
    wr(CC, CC_EN | CC_IOSQES | CC_IOCQES | 15 << 7);
    t_delay_us(1000);
    SNAP("bad_mps", "%#x", rd(CSTS));
    wr(CC, 0);
    wr(CC, CC_EN | CC_IOSQES | CC_IOCQES);
    EXPECT_TRUE(t_wait_until([] {return (rd(CSTS) & CSTS_RDY) != 0;}));
    EXPECT_EQ(rd(CSTS) & CSTS_CFS, 0u);
    wr(CC, CC_EN | CC_IOSQES | CC_IOCQES | 1 << 14);
    EXPECT_TRUE(t_wait_until([] {return (rd(CSTS) >> 2 & 3) == 2;}));
    wr(CC, 0);
    EXPECT_TRUE(t_wait_until([] {return (rd(CSTS) & CSTS_RDY) == 0;}));
}

/* Identify Controller (5.15.2.2): the identity in ASCII, the entry sizes
   (SQES and CQES give 64 and 16 bytes), the number of namespaces, and
   for a 1.4 controller its version and type. */
TEST(nvme, identify_controller)
{
    need_controller();
    uint8_t *id = (uint8_t *)alloc(PAGE);
    Cmd c {ADM_IDENTIFY};
    c.prp1 = (uintptr_t)id;
    c.cdw10 = 1;
    ASSERT_EQ(admin(c), SC_OK);
    for (int i = 4; i < 72; i++)
        EXPECT_TRUE(id[i] >= 0x20 && id[i] < 0x7f);
    EXPECT_EQ(id[512] & 0x0f, 6);
    EXPECT_GE(id[512] >> 4, 6);
    EXPECT_EQ(id[513] & 0x0f, 4);
    EXPECT_GE(le32(id + 516), 2u);
    uint32_t ver = le32(id + 80);
    EXPECT_EQ(ver, rd(VS));
    if (ver >= 0x00010400)
        EXPECT_EQ(id[111], 1);
    char mn[41] = {};
    memcpy(mn, id + 24, 40);
    SNAP("model", "%s", mn);
    SNAP("limits", "mdts %u acl %u aerl %u nn %u oncs %#x vwc %u", id[77],
         id[258], id[259], le32(id + 516), le16(id + 520), id[525]);
}

/* Identify Namespace (5.15.2.1): the size of the test disk in 512 byte
   blocks, write protection in NSATTR; an inactive namespace ID within NN
   reads as zeros; the active namespace list, and IDs past the last are
   refused. */
TEST(nvme, identify_namespace)
{
    need_controller();
    uint32_t nsid = find_ns(false);
    uint8_t *id = (uint8_t *)alloc(PAGE, 0xee);
    Cmd c {ADM_IDENTIFY};
    c.nsid = nsid;
    c.prp1 = (uintptr_t)id;
    ASSERT_EQ(admin(c), SC_OK);
    EXPECT_EQ(le64(id), (uint64_t)tc_block_read(TEST_CONTROL_BLOCK_SECTORS));
    EXPECT_EQ(le64(id + 8), le64(id));
    EXPECT_EQ((le32(id + 128 + 4 * (id[26] & 0xf)) >> 16) & 0xff, 9u);
    EXPECT_EQ(id[99] & 1, 0);
    SNAP("formats", "nlbaf %u flbas %u", id[25], id[26]);

    uint32_t nn;
    {
        uint8_t *ctrl = (uint8_t *)alloc(PAGE);
        Cmd i {ADM_IDENTIFY};
        i.prp1 = (uintptr_t)ctrl;
        i.cdw10 = 1;
        admin(i);
        nn = le32(ctrl + 516);
    }
    c.cdw10 = 2;
    c.nsid = 0;
    ASSERT_EQ(admin(c), SC_OK);
    char list[64];
    size_t p = 0;
    for (int i = 0; i < 8 && le32(id + 4 * i); i++)
        p += snprintf(list + p, sizeof(list) - p, "%u ", le32(id + 4 * i));
    SNAP("active", "%s", list);
    c.nsid = 0xfffffffe;
    EXPECT_EQ(admin(c), SC_INVALID_NS);
    c.cdw10 = 0;
    c.nsid = nn + 1;
    EXPECT_EQ(admin(c), SC_INVALID_NS);
    c.nsid = 0;
    EXPECT_EQ(admin(c), SC_INVALID_NS);
    c.cdw10 = 3;
    c.nsid = nsid;
    SNAP("descriptors", "%#x", admin(c));
    c.cdw10 = 0x55;
    EXPECT_EQ(admin(c), SC_INVALID_FIELD);
}

/* Reads and writes through PRPs: PRP1 alone, PRP1 part way into a page
   with PRP2 for the next, and a PRP list for more (4.3). */
TEST(nvme, read_write)
{
    need_io();
    uint32_t nsid = find_ns(false);
    uint8_t *page = (uint8_t *)alloc(PAGE, 0xee);
    EXPECT_EQ(io(rw(IO_READ, nsid, 10, 8, page)), SC_OK);
    EXPECT_EQ(pattern_errors(page, 10, 8), 0u);

    uint8_t *a = (uint8_t *)alloc(2 * PAGE, 0xee);
    Cmd c = rw(IO_READ, nsid, 30, 4, a + PAGE - 1024);
    c.prp2 = (uintptr_t)(a + PAGE);
    EXPECT_EQ(io(c), SC_OK);
    EXPECT_EQ(pattern_errors(a + PAGE - 1024, 30, 4), 0u);

    uint8_t *pages[4];
    uint64_t *list = (uint64_t *)alloc(PAGE);
    for (int i = 0; i < 4; i++) {
        pages[i] = (uint8_t *)alloc(PAGE, 0xee);
        if (i)
            list[i - 1] = (uintptr_t)pages[i];
    }
    c = rw(IO_READ, nsid, 100, 32, pages[0]);
    c.prp2 = (uintptr_t)list;
    EXPECT_EQ(io(c), SC_OK);
    for (int i = 0; i < 4; i++)
        EXPECT_EQ(pattern_errors(pages[i], 100 + 8 * i, 8), 0u);

    for (int i = 0; i < PAGE; i++)
        page[i] = i * 3 + 1;
    EXPECT_EQ(io(rw(IO_WRITE, nsid, 200, 8, page)), SC_OK);
    memset(a, 0, PAGE);
    EXPECT_EQ(io(rw(IO_READ, nsid, 200, 8, a)), SC_OK);
    EXPECT_EQ(memcmp(a, page, PAGE), 0);
    Cmd f {IO_FLUSH};
    f.nsid = nsid;
    EXPECT_EQ(io(f), SC_OK);
    f.nsid = 0xffffffff;
    EXPECT_EQ(io(f), SC_OK);
}

/* The errors of the NVM command set: a range past the namespace (LBA Out
   of Range), a write to a write protected one, a namespace that is not
   there, an opcode not supported, more than MDTS allows (Invalid Field),
   and the disk failing: Unrecovered Read Error and Write Fault (6.1, 4.6).
   */
TEST(nvme, io_errors)
{
    need_io();
    uint32_t nsid = find_ns(false);
    uint64_t blocks = tc_block_read(TEST_CONTROL_BLOCK_SECTORS);
    uint8_t *buf = (uint8_t *)alloc(2 * PAGE);
    EXPECT_EQ(io(rw(IO_READ, nsid, blocks, 1, buf)), SC_LBA_RANGE);
    EXPECT_EQ(io(rw(IO_READ, nsid, blocks - 1, 2, buf)), SC_LBA_RANGE);
    EXPECT_EQ(io(rw(IO_READ, 0, 0, 1, buf)), SC_INVALID_NS);
    EXPECT_EQ(io(rw(IO_READ, 9, 0, 1, buf)), SC_INVALID_NS);
    Cmd bad = rw(0x7f, nsid, 0, 1, buf);
    EXPECT_EQ(io(bad), SC_INVALID_OPCODE);
    uint8_t *id = (uint8_t *)alloc(PAGE);
    Cmd i {ADM_IDENTIFY};
    i.prp1 = (uintptr_t)id;
    i.cdw10 = 1;
    admin(i);
    if (id[77] && id[77] < 8) {
        uint32_t max_blocks = (PAGE << id[77]) / SECTOR;
        EXPECT_EQ(io(rw(IO_READ, nsid, 0, max_blocks + 8, buf)),
                  SC_INVALID_FIELD);
    }
    tc_block_write(TEST_CONTROL_BLOCK_FAIL, 1);
    EXPECT_EQ(io(rw(IO_READ, nsid, 3, 1, buf)), SC_UNRECOVERED_READ);
    tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_ASYNC);
    tc_block_write(TEST_CONTROL_BLOCK_FAIL, 1);
    EXPECT_EQ(io(rw(IO_WRITE, nsid, 3, 1, buf)), SC_WRITE_FAULT);
    EXPECT_EQ(io(rw(IO_READ, nsid, 3, 1, buf)), SC_OK);

    uint32_t ro = find_ns(true);
    EXPECT_EQ(io(rw(IO_WRITE, ro, 0, 1, buf)), SC_NS_WRITE_PROT);
    EXPECT_EQ(io(rw(IO_READ, ro, 0, 1, buf)), SC_OK);
}

/* Creating and deleting I/O queues (5.3, 5.4, 5.5, 5.6): an ID of 0, one
   in use or past what Set Features allocated is an Invalid Queue
   Identifier, a size past MQES an Invalid Queue Size, a submission queue
   without its completion queue Completion Queue Invalid, a base not page
   aligned an Invalid PRP Offset; a completion queue that a submission
   queue still uses cannot be deleted. */
TEST(nvme, queue_management)
{
    need_io();
    void *q = alloc(PAGE);
    EXPECT_EQ(create_cq(0, 4, q), SC_QID_INVALID);
    EXPECT_EQ(create_cq(1, 4, q), SC_QID_INVALID);
    EXPECT_EQ(create_sq(1, 4, q, 1), SC_QID_INVALID);
    uint64_t cap = rd(CAP_LO);
    if ((cap & 0xffff) < 0xfffe)
        EXPECT_EQ(create_cq(2, (cap & 0xffff) + 2, q), SC_QSIZE_INVALID);
    EXPECT_EQ(create_cq(2, 1, q), SC_QSIZE_INVALID);
    EXPECT_EQ(create_sq(2, 4, q, 5), SC_CQ_INVALID);
    EXPECT_EQ(create_cq(2, 4, (uint8_t *)q + 64), SC_PRP_OFFSET);
    Cmd nc {ADM_CREATE_CQ};
    nc.prp1 = (uintptr_t)q;
    nc.cdw10 = 2 | 3 << 16;
    EXPECT_EQ(admin(nc), SC_INVALID_FIELD);

    Cmd del {ADM_DELETE_CQ};
    del.cdw10 = 1;
    EXPECT_EQ(admin(del), SC_QUEUE_DELETION);
    del.op = ADM_DELETE_SQ;
    EXPECT_EQ(admin(del), SC_OK);
    del.op = ADM_DELETE_CQ;
    EXPECT_EQ(admin(del), SC_OK);
    EXPECT_EQ(admin(del), SC_QID_INVALID);
    del.cdw10 = 0;
    EXPECT_EQ(admin(del), SC_QID_INVALID);

    /* Number of Queues: what was allocated is the limit */
    Cmd nq {ADM_SET_FEAT};
    nq.cdw10 = 0x07;
    nq.cdw11 = 1 | 1 << 16;
    uint32_t dw0;
    uint16_t st = admin(nq, &dw0);
    SNAP("num_queues", "%#x %#x", st, dw0);
    if (st == SC_OK && (dw0 & 0xffff) == 1)
        EXPECT_EQ(create_cq(3, 4, q), SC_QID_INVALID);
}

/* Get and Set Features (5.21, 5.27): Number of Queues, and the features
   every controller has (arbitration, power management, temperature
   threshold, error recovery, interrupt coalescing, write atomicity, the
   asynchronous event configuration) take what is set and give it back;
   a feature the controller lacks is an Invalid Field. */
TEST(nvme, features)
{
    need_controller();
    uint32_t dw0;
    Cmd g {ADM_GET_FEAT};
    g.cdw10 = 0x07;
    EXPECT_EQ(admin(g, &dw0), SC_OK);
    SNAP("num_queues_default", "%#x", dw0);
    static const struct {
        uint8_t fid;
        uint32_t value;
    } features[] = {
        {0x01, 0x00000003}, {0x02, 0x00000000}, {0x04, 0x00000150},
        {0x05, 0x00000000}, {0x08, 0x00000101}, {0x0a, 0x00000001},
        {0x0b, 0x00000002},
    };
    char gets[96];
    size_t p = 0;
    for (const auto &f : features) {
        Cmd s {ADM_SET_FEAT};
        s.cdw10 = f.fid;
        s.cdw11 = f.value;
        EXPECT_EQ(admin(s), SC_OK);
        g.cdw10 = f.fid;
        EXPECT_EQ(admin(g, &dw0), SC_OK);
        EXPECT_EQ(dw0, f.value);
        p += snprintf(gets + p, sizeof(gets) - p, "%02x ", f.fid);
    }
    SNAP("round_trip", "%s", gets);
    g.cdw10 = 0x09; /* interrupt vector configuration, vector 0 */
    EXPECT_EQ(admin(g, &dw0), SC_OK);
    EXPECT_EQ(dw0 & 0xffff, 0u);
    g.cdw10 = 0x06; /* volatile write cache, which there is none of */
    EXPECT_EQ(admin(g), SC_INVALID_FIELD);
    g.cdw10 = 0x7e;
    EXPECT_EQ(admin(g), SC_INVALID_FIELD);
}

/* Get Log Page (5.14): the error information, health and firmware slot
   logs every controller has; a log it does not have is an Invalid Log
   Page. */
TEST(nvme, log_pages)
{
    need_controller();
    uint8_t *buf = (uint8_t *)alloc(PAGE, 0xee);
    Cmd c {ADM_GET_LOG};
    c.prp1 = (uintptr_t)buf;
    for (uint8_t lid = 1; lid <= 3; lid++) {
        c.cdw10 = lid | (uint32_t)(128 - 1) << 16;
        EXPECT_EQ(admin(c), SC_OK);
    }
    SNAP("health", "%02x %02x%02x", buf[0], buf[1], buf[2]);
    c.cdw10 = 0x7f | (uint32_t)(128 - 1) << 16;
    EXPECT_EQ(admin(c), SC_INVALID_LOG);
}

/* Commands the disk holds: one is aborted by Abort (5.1), with dw0 bit 0
   clear to say so and the command completed as Command Abort Requested;
   one not found leaves bit 0 set. Deleting the submission queue aborts
   what it has (Command Aborted due to SQ Deletion). The disk finishing in
   another order is allowed. */
TEST(nvme, abort)
{
    need_io();
    uint32_t nsid = find_ns(false);
    tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_HOLD);
    uint32_t cancels = tc_block_read(TEST_CONTROL_BLOCK_CANCELS);
    uint8_t *buf = (uint8_t *)alloc(3 * PAGE);
    uint16_t a = submit(sIo, rw(IO_READ, nsid, 1, 1, buf));
    uint16_t b = submit(sIo, rw(IO_READ, nsid, 2, 1, buf + PAGE));
    uint16_t c = submit(sIo, rw(IO_READ, nsid, 3, 1, buf + 2 * PAGE));
    EXPECT_TRUE(t_wait_until([] {
        return tc_block_read(TEST_CONTROL_BLOCK_HELD) == 3;
    }));
    Cmd ab {ADM_ABORT};
    ab.cdw10 = 1 | (uint32_t)b << 16;
    uint32_t dw0;
    EXPECT_EQ(admin(ab, &dw0), SC_OK);
    EXPECT_EQ(dw0 & 1, 0u);
    Cqe e;
    ASSERT_TRUE(reap(sIo, &e));
    EXPECT_EQ(e.cid, b);
    EXPECT_EQ(e.status, SC_ABORT_REQUESTED);
    ab.cdw10 = 1 | 0x7777u << 16;
    EXPECT_EQ(admin(ab, &dw0), SC_OK);
    EXPECT_EQ(dw0 & 1, 1u);

    tc_block_write(TEST_CONTROL_BLOCK_RELEASE, 1 | TEST_BLOCK_RELEASE_NEWEST);
    ASSERT_TRUE(reap(sIo, &e));
    EXPECT_EQ(e.cid, c);
    EXPECT_EQ(e.status, SC_OK);
    EXPECT_EQ(e.sqid, 1);

    Cmd del {ADM_DELETE_SQ};
    del.cdw10 = 1;
    EXPECT_EQ(admin(del), SC_OK);
    ASSERT_TRUE(reap(sIo, &e));
    EXPECT_EQ(e.cid, a);
    EXPECT_EQ(e.status, SC_ABORT_SQ_DELETE);
    EXPECT_EQ(tc_block_read(TEST_CONTROL_BLOCK_CANCELS) - cancels, 2u);
}

/* Asynchronous Event Requests are held outstanding, up to AERL + 1; one
   more is AER Limit Exceeded (5.2). A controller reset drops them, and
   the commands the disk holds. */
TEST(nvme, aer_and_reset)
{
    need_io();
    uint8_t *id = (uint8_t *)alloc(PAGE);
    Cmd i {ADM_IDENTIFY};
    i.prp1 = (uintptr_t)id;
    i.cdw10 = 1;
    admin(i);
    int limit = id[259] + 1;
    for (int k = 0; k < limit; k++)
        submit(sAdmin, Cmd {ADM_AER});
    Cqe e;
    EXPECT_FALSE(reap(sAdmin, &e, 2000));
    submit(sAdmin, Cmd {ADM_AER});
    ASSERT_TRUE(reap(sAdmin, &e));
    EXPECT_EQ(e.status, SC_AER_LIMIT);

    uint32_t nsid = find_ns(false);
    tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_HOLD);
    uint32_t cancels = tc_block_read(TEST_CONTROL_BLOCK_CANCELS);
    uint8_t *buf = (uint8_t *)alloc(PAGE);
    submit(sIo, rw(IO_READ, nsid, 1, 1, buf));
    EXPECT_TRUE(t_wait_until([] {
        return tc_block_read(TEST_CONTROL_BLOCK_HELD) == 1;
    }));
    wr(CC, 0);
    EXPECT_TRUE(t_wait_until([] {return (rd(CSTS) & CSTS_RDY) == 0;}));
    EXPECT_EQ(tc_block_read(TEST_CONTROL_BLOCK_CANCELS) - cancels, 1u);
}

/* The pin interrupt: asserted while a completion queue with interrupts
   enabled has entries not taken, and held off by INTMS for its vector
   (3.1.3, 3.1.4, 7.5.1). */
TEST(nvme, pin_interrupt)
{
    need_io(true, 0);
    uint32_t nsid = find_ns(false);
    uint8_t *buf = (uint8_t *)alloc(PAGE);
    static volatile int count;
    count = 0;
    /* takes what the queue has, without waiting for more */
    if (!pci_intx_route(sFn, [](void *) {
            count = count + 1;
            Cqe e;
            reap(sIo, &e, 0);
        }, nullptr))
        t_skip("the interrupt reaches no controller the suite drives");
    t_on_cleanup([] {pci_intx_unroute(sFn);});
    arch_irq_enable();
    submit(sIo, rw(IO_READ, nsid, 1, 1, buf));
    EXPECT_TRUE(t_wait_until([] {return count > 0;}));
    arch_irq_disable();
    EXPECT_EQ(count, 1);
    wr(INTMS, 1);
    EXPECT_EQ(rd(INTMS) & 1, 1u);
    count = 0;
    arch_irq_enable();
    submit(sIo, rw(IO_READ, nsid, 2, 1, buf));
    t_delay_us(2000);
    EXPECT_EQ(count, 0);
    wr(INTMC, 1);
    EXPECT_TRUE(t_wait_until([] {return count > 0;}));
    arch_irq_disable();
}

/* With MSI-X a completion queue's vector signals by message. */
TEST(nvme, msix)
{
    need_controller();
    if (sFn->host->msi == nullptr || !pci_find_cap(sFn, PCI_CAP_MSIX))
        t_skip("no MSI-X, or nothing to receive it");
    static volatile int count;
    count = 0;
    uint64_t addr;
    uint32_t data;
    ASSERT_TRUE(sFn->host->msi->Message(3, [](void *) {count = count + 1;},
                                        nullptr, &addr, &data));
    uint32_t cap = pci_find_cap(sFn, PCI_CAP_MSIX);
    uint32_t table = pci_read32(sFn, cap + 4);
    uint32_t entry = (table & ~7u) + 16 * 3;
    pci_bar_write(sFn, table & 7, entry, (uint32_t)addr, 2);
    pci_bar_write(sFn, table & 7, entry + 4, addr >> 32, 2);
    pci_bar_write(sFn, table & 7, entry + 8, data, 2);
    pci_bar_write(sFn, table & 7, entry + 12, 0, 2);
    uint16_t ctrl = pci_read16(sFn, cap + 2);
    pci_write16(sFn, cap + 2, (ctrl & 0x7ff) | 0x8000);
    make_queue(sIo, 1, QSIZE);
    ASSERT_EQ(create_cq(1, QSIZE, sIo.cq, true, 3), SC_OK);
    ASSERT_EQ(create_sq(1, QSIZE, sIo.sq, 1), SC_OK);
    uint32_t nsid = find_ns(false);
    uint8_t *buf = (uint8_t *)alloc(PAGE);
    arch_irq_enable();
    submit(sIo, rw(IO_READ, nsid, 1, 1, buf));
    EXPECT_TRUE(t_wait_until([] {return count > 0;}));
    arch_irq_disable();
    Cqe e;
    EXPECT_TRUE(reap(sIo, &e));
    EXPECT_EQ(create_cq(2, QSIZE, alloc(PAGE), true, 200), 0x108);
}
