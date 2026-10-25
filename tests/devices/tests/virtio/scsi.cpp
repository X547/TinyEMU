/*
 * virtio-scsi tests
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
#include "test.h"
#include "test_control.h"
#include "testctl.h"
#include "virtio.h"

/* The SCSI host device of the virtio specification (1.2, 5.6): its
   configuration, how it addresses targets and units, its responses, and
   task management on the control queue. The commands the units answer are
   the SCSI tests' business. */

#define CFG_NUM_QUEUES  0
#define CFG_SEG_MAX     4
#define CFG_MAX_SECTORS 8
#define CFG_CMD_PER_LUN 12
#define CFG_EVENT_INFO  16
#define CFG_SENSE_SIZE  20
#define CFG_CDB_SIZE    24
#define CFG_MAX_CHANNEL 28
#define CFG_MAX_TARGET  30
#define CFG_MAX_LUN     32

#define S_OK               0
#define S_ABORTED          2
#define S_BAD_TARGET       3
#define S_RESET            4
#define S_FAILURE          9
#define S_FUNCTION_SUCCEEDED 10
#define S_FUNCTION_REJECTED  11
#define S_INCORRECT_LUN    12

#define T_TMF       0
#define T_AN_QUERY  1
#define TMF_ABORT_TASK     0
#define TMF_ABORT_TASK_SET 1
#define TMF_I_T_NEXUS_RESET 4
#define TMF_LUN_RESET      5
#define TMF_QUERY_TASK     6

#define QSIZE 16

namespace {

VirtioDev sDev;
Virtq sCtrl;
Virtq sEvent;
Virtq sReq;
uint32_t sCdbSize;
uint32_t sSenseSize;
/* the target of the first disk, which has a test disk behind it */
int sDiskTarget = -1;

struct Cmd {
    uint8_t *req;
    uint8_t *resp;
    uint8_t *data;
};

void cleanup()
{
    virtio_reset(&sDev);
    if (sDev.pci)
        pci_restore_all();
}

void *alloc(size_t size, uint8_t fill = 0)
{
    void *p = heap_alloc(size ? size : 1, 16);
    if (p == nullptr)
        t_skip("no memory");
    memset(p, fill, size);
    return p;
}

void lun_bytes(uint8_t *p, int target, int lun)
{
    memset(p, 0, 8);
    p[0] = 1;
    p[1] = target;
    p[2] = 0x40 | (lun >> 8);
    p[3] = lun;
}

/* A command in descriptors base to base+2: the request with 'cdb', the
   response, and 'len' bytes of data in, made available. */
Cmd queue_cmd(uint16_t base, int target, int lun, const uint8_t *cdb,
              int cdb_len, uint32_t len, uint64_t id = 0)
{
    Cmd c;
    uint32_t req_len = 19 + sCdbSize, resp_len = 12 + sSenseSize;
    c.req = (uint8_t *)alloc(req_len);
    c.resp = (uint8_t *)alloc(resp_len, 0xee);
    c.data = (uint8_t *)alloc(len, 0xee);
    lun_bytes(c.req, target, lun);
    memcpy(c.req + 8, &id, 8);
    memcpy(c.req + 19, cdb, cdb_len);
    VirtqDesc *d = sReq.desc;
    d[base] = VirtqDesc {(uintptr_t)c.req, req_len, VIRTQ_DESC_F_NEXT,
                         (uint16_t)(base + 1)};
    d[base + 1] = VirtqDesc {(uintptr_t)c.resp, resp_len,
                             (uint16_t)(VIRTQ_DESC_F_WRITE |
                                        (len ? VIRTQ_DESC_F_NEXT : 0)),
                             (uint16_t)(base + 2)};
    if (len)
        d[base + 2] = VirtqDesc {(uintptr_t)c.data, len, VIRTQ_DESC_F_WRITE,
                                 0};
    virtq_publish(&sReq, base);
    return c;
}

uint8_t response(const Cmd &c) {return c.resp[11];}
uint8_t status(const Cmd &c) {return c.resp[10];}

/* One command, waited for. */
Cmd run_cmd(int target, int lun, const uint8_t *cdb, int cdb_len,
            uint32_t len)
{
    Cmd c = queue_cmd(0, target, lun, cdb, cdb_len, len);
    virtq_kick(&sReq);
    VirtqUsedElem e;
    if (!virtq_get_used(&sReq, &e)) {
        t_fail(__FILE__, __LINE__, "no answer");
        t_abort();
    }
    return c;
}

/* A task management function: its response. */
uint8_t tmf(uint32_t subtype, int target, int lun, uint64_t id = 0)
{
    uint8_t *req = (uint8_t *)alloc(24);
    uint8_t *resp = (uint8_t *)alloc(1, 0xee);
    put_le32(req, T_TMF);
    put_le32(req + 4, subtype);
    lun_bytes(req + 8, target, lun);
    memcpy(req + 16, &id, 8);
    sCtrl.desc[0] = VirtqDesc {(uintptr_t)req, 24, VIRTQ_DESC_F_NEXT, 1};
    sCtrl.desc[1] = VirtqDesc {(uintptr_t)resp, 1, VIRTQ_DESC_F_WRITE, 0};
    virtq_publish(&sCtrl, 0);
    virtq_kick(&sCtrl);
    VirtqUsedElem e;
    if (!virtq_get_used(&sCtrl, &e)) {
        t_fail(__FILE__, __LINE__, "no answer to a TMF");
        t_abort();
    }
    return *resp;
}

void start()
{
    virtio_require(VIRTIO_ID_SCSI, &sDev);
    t_on_cleanup(cleanup);
    ASSERT_TRUE(virtio_negotiate(&sDev, VIRTIO_F_VERSION_1));
    ASSERT_TRUE(virtq_setup(&sDev, 0, 8, &sCtrl));
    ASSERT_TRUE(virtq_setup(&sDev, 1, 8, &sEvent));
    ASSERT_TRUE(virtq_setup(&sDev, 2, QSIZE, &sReq));
    virtio_set_status(&sDev, virtio_status(&sDev) | VIRTIO_STATUS_DRIVER_OK);
    sCdbSize = virtio_config_read(&sDev, CFG_CDB_SIZE, 2);
    sSenseSize = virtio_config_read(&sDev, CFG_SENSE_SIZE, 2);
    /* find the disk with a test disk, by its capacity */
    sDiskTarget = -1;
    for (int t = 0; t < 16 && sDiskTarget < 0; t++) {
        uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
        Cmd c = run_cmd(t, 0, inq, 6, 36);
        if (response(c) != S_OK || status(c) != 0 || c.data[0] != 0)
            continue;
        uint8_t cap[10] = {0x25};
        for (int k = 0; k < 2; k++) {
            c = run_cmd(t, 0, cap, 10, 8);
            if (status(c) == 0)
                break;
        }
        if (status(c) == 0 && tc_block_find(be32(c.data) + 1) >= 0)
            sDiskTarget = t;
    }
}

void need_disk()
{
    if (sDiskTarget < 0)
        t_skip("no SCSI disk on a test disk");
}

}


/* The configuration (5.6.4): one request queue, and the limits. */
TEST(virtio_scsi, config)
{
    virtio_require(VIRTIO_ID_SCSI, &sDev);
    EXPECT_EQ(virtio_config_read(&sDev, CFG_NUM_QUEUES, 2), 1u);
    EXPECT_GT(virtio_config_read(&sDev, CFG_CMD_PER_LUN, 2), 0u);
    EXPECT_GE(virtio_config_read(&sDev, CFG_CDB_SIZE, 2), 16u);
    SNAP("limits", "seg_max %u max_sectors %u cmd_per_lun %u",
         virtio_config_read(&sDev, CFG_SEG_MAX, 2),
         virtio_config_read(&sDev, CFG_MAX_SECTORS, 2),
         virtio_config_read(&sDev, CFG_CMD_PER_LUN, 2));
    SNAP("sizes", "event_info %u sense %u cdb %u",
         virtio_config_read(&sDev, CFG_EVENT_INFO, 2),
         virtio_config_read(&sDev, CFG_SENSE_SIZE, 2),
         virtio_config_read(&sDev, CFG_CDB_SIZE, 2));
    SNAP("addresses", "channel %u target %u lun %u",
         virtio_config_read(&sDev, CFG_MAX_CHANNEL, 1),
         virtio_config_read(&sDev, CFG_MAX_TARGET, 1),
         virtio_config_read(&sDev, CFG_MAX_LUN, 2));
    SNAP("features", "%#x", virtio_device_features(&sDev, 0));
}

/* The driver sets the sense and CDB sizes (5.6.4); the device keeps them
   to what it can do, and a reset puts them back. */
TEST(virtio_scsi, sizes)
{
    start();
    virtio_config_write(&sDev, CFG_SENSE_SIZE, 32, 2);
    virtio_config_write(&sDev, CFG_CDB_SIZE, 16, 2);
    EXPECT_EQ(virtio_config_read(&sDev, CFG_SENSE_SIZE, 2), 32u);
    EXPECT_EQ(virtio_config_read(&sDev, CFG_CDB_SIZE, 2), 16u);
    sSenseSize = 32;
    sCdbSize = 16;
    uint8_t tur[6] = {0x00};
    Cmd c = run_cmd(0, 0, tur, 6, 0);
    EXPECT_EQ(response(c), S_OK);
    virtio_config_write(&sDev, CFG_SENSE_SIZE, 100000, 2);
    SNAP("sense_clamped", "%u", virtio_config_read(&sDev, CFG_SENSE_SIZE, 2));
    virtio_config_write(&sDev, CFG_MAX_TARGET, 3, 1);
    SNAP("max_target_after_write", "%u",
         virtio_config_read(&sDev, CFG_MAX_TARGET, 1));
    virtio_reset(&sDev);
    SNAP("after_reset", "%u %u", virtio_config_read(&sDev, CFG_SENSE_SIZE, 2),
         virtio_config_read(&sDev, CFG_CDB_SIZE, 2));
}

/* A target nothing is attached to is VIRTIO_SCSI_S_BAD_TARGET, as is a
   LUN not in the single level format (5.6.6.1). A unit not there on a
   target that is answers INQUIRY with peripheral qualifier 3 (SPC-3), and
   REPORT LUNS lists the units that are. */
TEST(virtio_scsi, addressing)
{
    start();
    uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
    Cmd c = run_cmd(15, 0, inq, 6, 36);
    EXPECT_EQ(response(c), S_BAD_TARGET);
    c = queue_cmd(0, 0, 0, inq, 6, 36);
    c.req[0] = 2;
    virtq_kick(&sReq);
    VirtqUsedElem e;
    ASSERT_TRUE(virtq_get_used(&sReq, &e));
    EXPECT_EQ(response(c), S_BAD_TARGET);

    c = run_cmd(0, 5, inq, 6, 36);
    EXPECT_EQ(response(c), S_OK);
    EXPECT_EQ(status(c), 0);
    EXPECT_EQ(c.data[0], 0x7f);
    uint8_t tur[6] = {0x00};
    c = run_cmd(0, 5, tur, 6, 0);
    EXPECT_EQ(status(c), 2);
    EXPECT_EQ(c.resp[12 + 12], 0x25);

    uint8_t luns[12] = {0xa0, 0, 0, 0, 0, 0, 0, 0, 0, 64, 0, 0};
    c = run_cmd(0, 0, luns, 12, 64);
    EXPECT_EQ(response(c), S_OK);
    EXPECT_EQ(be32(c.data), 8u);
    EXPECT_EQ(c.data[9], 0);
}

/* The residual is what the data in did not fill; sense comes with the
   response, no longer than the sense size. */
TEST(virtio_scsi, response)
{
    start();
    need_disk();
    uint8_t inq[6] = {0x12, 0, 0, 0, 36, 0};
    Cmd c = run_cmd(sDiskTarget, 0, inq, 6, 100);
    EXPECT_EQ(response(c), S_OK);
    EXPECT_EQ(le32(c.resp + 4), 64u);
    EXPECT_EQ(le32(c.resp), 0u);
    uint8_t bad[10] = {0xe7};
    c = run_cmd(sDiskTarget, 0, bad, 10, 0);
    EXPECT_EQ(status(c), 2);
    EXPECT_GE(le32(c.resp), 18u);
    EXPECT_LE(le32(c.resp), sSenseSize);
    EXPECT_EQ(c.resp[12 + 2] & 0x0f, 0x5);
    EXPECT_EQ(c.resp[12 + 12], 0x20);
}

/* A command with data both ways needs VIRTIO_SCSI_F_INOUT, which is not
   offered, and one longer than the device takes fails as well. A request
   without room for its headers is handed back. */
TEST(virtio_scsi, malformed)
{
    start();
    need_disk();
    uint8_t *req = (uint8_t *)alloc(19 + sCdbSize);
    uint8_t *out = (uint8_t *)alloc(512);
    uint8_t *resp = (uint8_t *)alloc(12 + sSenseSize, 0xee);
    uint8_t *in = (uint8_t *)alloc(512);
    lun_bytes(req, sDiskTarget, 0);
    req[19] = 0x28;
    req[27] = 1;
    VirtqDesc *d = sReq.desc;
    d[0] = VirtqDesc {(uintptr_t)req, 19 + sCdbSize, VIRTQ_DESC_F_NEXT, 1};
    d[1] = VirtqDesc {(uintptr_t)out, 512, VIRTQ_DESC_F_NEXT, 2};
    d[2] = VirtqDesc {(uintptr_t)resp, 12 + sSenseSize,
                      VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT, 3};
    d[3] = VirtqDesc {(uintptr_t)in, 512, VIRTQ_DESC_F_WRITE, 0};
    virtq_publish(&sReq, 0);
    virtq_kick(&sReq);
    VirtqUsedElem e;
    ASSERT_TRUE(virtq_get_used(&sReq, &e));
    EXPECT_EQ(resp[11], S_FAILURE);

    uint32_t max = virtio_config_read(&sDev, CFG_MAX_SECTORS, 2) * 512;
    d[0].next = 2;
    d[2].len = 12 + sSenseSize;
    d[3].len = max + 512;
    resp[11] = 0xee;
    virtq_publish(&sReq, 0);
    virtq_kick(&sReq);
    ASSERT_TRUE(virtq_get_used(&sReq, &e));
    EXPECT_EQ(resp[11], S_FAILURE);

    d[0] = VirtqDesc {(uintptr_t)req, 10, VIRTQ_DESC_F_NEXT, 2};
    d[2] = VirtqDesc {(uintptr_t)resp, 12 + sSenseSize, VIRTQ_DESC_F_WRITE,
                      0};
    virtq_publish(&sReq, 0);
    virtq_kick(&sReq);
    ASSERT_TRUE(virtq_get_used(&sReq, &e));
    EXPECT_EQ(e.len, 0u);
}

/* Task management (5.6.6.2): commands held by the disk are aborted one by
   one or all together, or reset; each comes back with ABORTED or RESET,
   and the disk is told to drop it. QUERY TASK tells whether one is there. */
TEST(virtio_scsi, task_management)
{
    start();
    need_disk();
    tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_HOLD);
    uint32_t cancels = tc_block_read(TEST_CONTROL_BLOCK_CANCELS);
    uint8_t rd[10] = {0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0};
    Cmd a = queue_cmd(0, sDiskTarget, 0, rd, 10, 512, 0x111);
    Cmd b = queue_cmd(3, sDiskTarget, 0, rd, 10, 512, 0x222);
    Cmd c = queue_cmd(6, sDiskTarget, 0, rd, 10, 512, 0x333);
    virtq_kick(&sReq);
    EXPECT_TRUE(t_wait_until([] {
        return tc_block_read(TEST_CONTROL_BLOCK_HELD) == 3;
    }));
    EXPECT_EQ(tmf(TMF_QUERY_TASK, sDiskTarget, 0, 0x222),
              S_FUNCTION_SUCCEEDED);
    EXPECT_EQ(tmf(TMF_ABORT_TASK, sDiskTarget, 0, 0x222), S_OK);
    EXPECT_EQ(response(b), S_ABORTED);
    EXPECT_EQ(tmf(TMF_QUERY_TASK, sDiskTarget, 0, 0x222), S_OK);
    EXPECT_EQ(tmf(TMF_ABORT_TASK_SET, sDiskTarget, 0), S_OK);
    EXPECT_EQ(response(a), S_ABORTED);
    EXPECT_EQ(response(c), S_ABORTED);
    EXPECT_EQ(tc_block_read(TEST_CONTROL_BLOCK_CANCELS) - cancels, 3u);
    EXPECT_EQ(virtq_used_idx(&sReq) - sReq.last_used, 3);
    sReq.last_used = virtq_used_idx(&sReq);

    Cmd d = queue_cmd(0, sDiskTarget, 0, rd, 10, 512, 0x444);
    virtq_kick(&sReq);
    EXPECT_TRUE(t_wait_until([] {
        return tc_block_read(TEST_CONTROL_BLOCK_HELD) == 1;
    }));
    EXPECT_EQ(tmf(TMF_LUN_RESET, sDiskTarget, 0), S_OK);
    EXPECT_EQ(response(d), S_RESET);
    Cmd f = queue_cmd(3, sDiskTarget, 0, rd, 10, 512, 0x555);
    virtq_kick(&sReq);
    EXPECT_TRUE(t_wait_until([] {
        return tc_block_read(TEST_CONTROL_BLOCK_HELD) == 1;
    }));
    EXPECT_EQ(tmf(TMF_I_T_NEXUS_RESET, sDiskTarget, 0), S_OK);
    EXPECT_EQ(response(f), S_RESET);
    EXPECT_EQ(tc_block_read(TEST_CONTROL_BLOCK_HELD), 0u);

    EXPECT_EQ(tmf(TMF_ABORT_TASK, 15, 0), S_BAD_TARGET);
    EXPECT_EQ(tmf(TMF_ABORT_TASK, sDiskTarget, 9), S_INCORRECT_LUN);
    EXPECT_EQ(tmf(99, sDiskTarget, 0), S_FUNCTION_REJECTED);
}

/* No asynchronous notification is offered: a query or subscription is
   granted no events (5.6.6.2). */
TEST(virtio_scsi, async_notification)
{
    start();
    uint8_t *req = (uint8_t *)alloc(16);
    uint8_t *resp = (uint8_t *)alloc(5, 0xee);
    put_le32(req, T_AN_QUERY);
    lun_bytes(req + 4, 0, 0);
    put_le32(req + 12, 0x7e);
    sCtrl.desc[0] = VirtqDesc {(uintptr_t)req, 16, VIRTQ_DESC_F_NEXT, 1};
    sCtrl.desc[1] = VirtqDesc {(uintptr_t)resp, 5, VIRTQ_DESC_F_WRITE, 0};
    virtq_publish(&sCtrl, 0);
    virtq_kick(&sCtrl);
    VirtqUsedElem e;
    ASSERT_TRUE(virtq_get_used(&sCtrl, &e));
    EXPECT_EQ(le32(resp), 0u);
    EXPECT_EQ(resp[4], S_OK);
}

/* A device reset gives up the commands in flight. */
TEST(virtio_scsi, reset_in_flight)
{
    start();
    need_disk();
    tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_HOLD);
    uint32_t cancels = tc_block_read(TEST_CONTROL_BLOCK_CANCELS);
    uint8_t rd[10] = {0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0};
    queue_cmd(0, sDiskTarget, 0, rd, 10, 512);
    virtq_kick(&sReq);
    EXPECT_TRUE(t_wait_until([] {
        return tc_block_read(TEST_CONTROL_BLOCK_HELD) == 1;
    }));
    virtio_reset(&sDev);
    EXPECT_EQ(tc_block_read(TEST_CONTROL_BLOCK_CANCELS) - cancels, 1u);
    t_delay_us(2000);
    EXPECT_EQ(virtq_used_idx(&sReq), sReq.last_used);
}
