/*
 * virtio-block tests
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

/* The block device of the virtio specification (1.2, 5.2) on temu's test
   disks: a request is a header, the data, and a status byte in the last
   writable byte. The machines give each disk a size of its own, which is
   how a test finds the test disk behind the device. */

#define VIRTIO_BLK_F_SIZE_MAX (1u << 1)
#define VIRTIO_BLK_F_SEG_MAX  (1u << 2)
#define VIRTIO_BLK_F_RO       (1u << 5)

#define VIRTIO_BLK_T_IN     0
#define VIRTIO_BLK_T_OUT    1
#define VIRTIO_BLK_T_FLUSH  4
#define VIRTIO_BLK_T_GET_ID 8
#define VIRTIO_BLK_T_DISCARD 11

#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

#define CFG_CAPACITY 0
#define CFG_SIZE_MAX 8
#define CFG_SEG_MAX  12

#define QUEUE_SIZE 16
#define SECTOR 512

namespace {

struct Header {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector;
};

VirtioDev sDev;
Virtq sVq;
uint64_t sCapacity;
int sDisk;

void cleanup()
{
    virtio_reset(&sDev);
    if (sDev.pci)
        pci_restore_all();
}

uint64_t capacity(const VirtioDev *dev)
{
    return virtio_config_read(dev, CFG_CAPACITY, 2) |
           (uint64_t)virtio_config_read(dev, CFG_CAPACITY + 4, 2) << 32;
}

/* The first device that is writable or read only, as asked, with its test
   disk selected. */
void find(bool read_only)
{
    for (int i = 0; virtio_find(VIRTIO_ID_BLOCK, i, &sDev); i++) {
        bool ro = virtio_device_features(&sDev, 0) & VIRTIO_BLK_F_RO;
        if (ro != read_only)
            continue;
        sCapacity = capacity(&sDev);
        sDisk = tc_block_find(sCapacity);
        if (sDisk < 0)
            t_skip("the device's disk is not a test disk");
        t_on_cleanup(cleanup);
        return;
    }
    t_skip("no %s virtio block device", read_only ? "read only" : "writable");
}

void start(bool read_only = false, uint64_t features = VIRTIO_F_VERSION_1)
{
    find(read_only);
    ASSERT_TRUE(virtio_negotiate(&sDev, features));
    ASSERT_TRUE(virtq_setup(&sDev, 0, QUEUE_SIZE, &sVq));
    virtio_set_status(&sDev, virtio_status(&sDev) | VIRTIO_STATUS_DRIVER_OK);
}

void select_disk()
{
    tc_block_find(sCapacity);
}

void *alloc(size_t size, uint8_t fill = 0)
{
    void *p = heap_alloc(size ? size : 1, 16);
    if (p == nullptr)
        t_skip("no memory");
    memset(p, fill, size);
    return p;
}

/* A request in descriptors base, base+1 and base+2, made available: the
   header, the data unless 'len' is 0, the status. */
uint8_t *queue_req(uint16_t base, uint32_t type, uint64_t sector, void *data,
                   uint32_t len)
{
    Header *h = (Header *)alloc(sizeof(Header));
    h->type = type;
    h->sector = sector;
    uint8_t *status = (uint8_t *)alloc(1, 0xee);
    bool in = type == VIRTIO_BLK_T_IN || type == VIRTIO_BLK_T_GET_ID;
    VirtqDesc *d = sVq.desc;
    d[base] = VirtqDesc {(uintptr_t)h, sizeof(*h), VIRTQ_DESC_F_NEXT,
                         (uint16_t)(base + 1)};
    uint16_t last = base + 1;
    if (len) {
        d[base + 1] = VirtqDesc {(uintptr_t)data, len,
                                 (uint16_t)(VIRTQ_DESC_F_NEXT |
                                            (in ? VIRTQ_DESC_F_WRITE : 0)),
                                 (uint16_t)(base + 2)};
        last = base + 2;
    }
    d[last] = VirtqDesc {(uintptr_t)status, 1, VIRTQ_DESC_F_WRITE, 0};
    virtq_publish(&sVq, base);
    return status;
}

/* One request, waited for: its status, and the used length. */
uint8_t request(uint32_t type, uint64_t sector, void *data, uint32_t len,
                uint32_t *used_len = nullptr)
{
    uint8_t *status = queue_req(0, type, sector, data, len);
    virtq_kick(&sVq);
    VirtqUsedElem e;
    if (!virtq_get_used(&sVq, &e)) {
        t_fail(__FILE__, __LINE__, "no answer to a request of type %u", type);
        t_abort();
    }
    EXPECT_EQ(e.id, 0u);
    if (used_len)
        *used_len = e.len;
    return *status;
}

/* How many bytes of 'n' sectors from 'first' differ from the test pattern,
   0 when it is all there. */
uint32_t pattern_errors(const uint8_t *buf, uint64_t first, uint32_t n)
{
    uint32_t bad = 0;
    for (uint32_t s = 0; s < n; s++) {
        for (uint32_t i = 0; i < SECTOR; i++)
            bad += buf[s * SECTOR + i] != TEST_BLOCK_PATTERN(first + s, i);
    }
    return bad;
}

}


/* The size is the test disk's; the device offers size_max and seg_max,
   and only a read only disk VIRTIO_BLK_F_RO (5.2.3). */
TEST(virtio_block, config)
{
    find(false);
    uint32_t f = virtio_device_features(&sDev, 0);
    EXPECT_EQ(sCapacity, (uint64_t)tc_block_read(TEST_CONTROL_BLOCK_SECTORS));
    EXPECT_FALSE(f & VIRTIO_BLK_F_RO);
    SNAP("features", "%#x", f);
    if (f & VIRTIO_BLK_F_SIZE_MAX)
        SNAP("size_max", "%u", virtio_config_read(&sDev, CFG_SIZE_MAX, 2));
    if (f & VIRTIO_BLK_F_SEG_MAX)
        SNAP("seg_max", "%u", virtio_config_read(&sDev, CFG_SEG_MAX, 2));
}

/* Reads give the disk's contents; the used length counts the data and the
   status byte (2.7.8). */
TEST(virtio_block, read)
{
    start();
    uint8_t *buf = (uint8_t *)alloc(8 * SECTOR, 0xaa);
    uint32_t len;
    EXPECT_EQ(request(VIRTIO_BLK_T_IN, 3, buf, 8 * SECTOR, &len),
              VIRTIO_BLK_S_OK);
    EXPECT_EQ(len, 8u * SECTOR + 1);
    EXPECT_EQ(pattern_errors(buf, 3, 8), 0u);
    /* the last sector */
    EXPECT_EQ(request(VIRTIO_BLK_T_IN, sCapacity - 1, buf, SECTOR),
              VIRTIO_BLK_S_OK);
    EXPECT_EQ(pattern_errors(buf, sCapacity - 1, 1), 0u);
}

/* What is written reads back, and only where it was written. */
TEST(virtio_block, write)
{
    start();
    uint8_t *w = (uint8_t *)alloc(2 * SECTOR);
    for (int i = 0; i < 2 * SECTOR; i++)
        w[i] = i * 7 + 1;
    EXPECT_EQ(request(VIRTIO_BLK_T_OUT, 100, w, 2 * SECTOR),
              VIRTIO_BLK_S_OK);
    EXPECT_EQ(tc_block_read(TEST_CONTROL_BLOCK_WRITES) > 0, true);
    uint8_t *r = (uint8_t *)alloc(4 * SECTOR, 0x55);
    EXPECT_EQ(request(VIRTIO_BLK_T_IN, 99, r, 4 * SECTOR), VIRTIO_BLK_S_OK);
    EXPECT_EQ(pattern_errors(r, 99, 1), 0u);
    EXPECT_EQ(memcmp(r + SECTOR, w, 2 * SECTOR), 0);
    EXPECT_EQ(pattern_errors(r + 3 * SECTOR, 102, 1), 0u);
}

/* A request past the end of the disk fails with IOERR, and nothing of it
   is written. */
TEST(virtio_block, past_capacity)
{
    start();
    uint8_t *buf = (uint8_t *)alloc(2 * SECTOR);
    EXPECT_EQ(request(VIRTIO_BLK_T_IN, sCapacity, buf, SECTOR),
              VIRTIO_BLK_S_IOERR);
    EXPECT_EQ(request(VIRTIO_BLK_T_IN, sCapacity - 1, buf, 2 * SECTOR),
              VIRTIO_BLK_S_IOERR);
    EXPECT_EQ(request(VIRTIO_BLK_T_OUT, sCapacity - 1, buf, 2 * SECTOR),
              VIRTIO_BLK_S_IOERR);
    EXPECT_EQ(request(VIRTIO_BLK_T_IN, ~0ull, buf, SECTOR),
              VIRTIO_BLK_S_IOERR);
    EXPECT_EQ(request(VIRTIO_BLK_T_IN, sCapacity - 1, buf, SECTOR),
              VIRTIO_BLK_S_OK);
    EXPECT_EQ(pattern_errors(buf, sCapacity - 1, 1), 0u);
}

/* A read only disk fails writes with IOERR and writes no data (5.2.6.2),
   and still reads. */
TEST(virtio_block, read_only)
{
    start(true);
    EXPECT_TRUE(virtio_device_features(&sDev, 0) & VIRTIO_BLK_F_RO);
    uint8_t *buf = (uint8_t *)alloc(SECTOR, 0x11);
    EXPECT_EQ(request(VIRTIO_BLK_T_OUT, 5, buf, SECTOR), VIRTIO_BLK_S_IOERR);
    EXPECT_EQ(request(VIRTIO_BLK_T_IN, 5, buf, SECTOR), VIRTIO_BLK_S_OK);
    EXPECT_EQ(pattern_errors(buf, 5, 1), 0u);
}

/* Flush succeeds; the device ID is a string of at most 20 bytes, padded
   with NULs (5.2.6); a type the device does not know is UNSUPP. */
TEST(virtio_block, other_requests)
{
    start();
    uint32_t len;
    EXPECT_EQ(request(VIRTIO_BLK_T_FLUSH, 0, nullptr, 0, &len),
              VIRTIO_BLK_S_OK);
    EXPECT_EQ(len, 1u);
    char *id = (char *)alloc(20, 0x7f);
    EXPECT_EQ(request(VIRTIO_BLK_T_GET_ID, 0, id, 20, &len), VIRTIO_BLK_S_OK);
    EXPECT_EQ(len, 21u);
    char shown[21] = {};
    memcpy(shown, id, 20);
    for (int i = (int)strlen(shown); i < 20; i++)
        EXPECT_EQ(id[i], 0);
    SNAP("id", "%s", shown);
    EXPECT_EQ(request(VIRTIO_BLK_T_DISCARD, 0, id, 16, &len),
              VIRTIO_BLK_S_UNSUPP);
    SNAP("unsupp_len", "%u", len);
    EXPECT_EQ(request(0x1234, 0, nullptr, 0), VIRTIO_BLK_S_UNSUPP);
}

/* The disk failing a request, at once or later, is IOERR. A read that
   failed hands over no data. */
TEST(virtio_block, io_error)
{
    start();
    select_disk();
    uint8_t *buf = (uint8_t *)alloc(SECTOR, 0xaa);
    tc_block_write(TEST_CONTROL_BLOCK_FAIL, 1);
    EXPECT_EQ(request(VIRTIO_BLK_T_IN, 1, buf, SECTOR), VIRTIO_BLK_S_IOERR);
    uint32_t untouched = 0;
    for (int i = 0; i < SECTOR; i++)
        untouched += buf[i] == 0xaa;
    SNAP("data_after_error", "%s", untouched == SECTOR ? "untouched" :
         "written");
    tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_ASYNC);
    tc_block_write(TEST_CONTROL_BLOCK_FAIL, 1);
    EXPECT_EQ(request(VIRTIO_BLK_T_OUT, 1, buf, SECTOR), VIRTIO_BLK_S_IOERR);
    EXPECT_EQ(request(VIRTIO_BLK_T_IN, 1, buf, SECTOR), VIRTIO_BLK_S_OK);
    EXPECT_EQ(pattern_errors(buf, 1, 1), 0u);
}

/* Requests finish in whatever order the disk answers them, and the used
   ring follows that order, which virtio allows (2.7.8). */
TEST(virtio_block, out_of_order)
{
    start();
    select_disk();
    tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_HOLD);
    uint8_t *buf[3];
    uint8_t *status[3];
    for (int i = 0; i < 3; i++) {
        buf[i] = (uint8_t *)alloc(SECTOR);
        status[i] = queue_req(3 * i, VIRTIO_BLK_T_IN, 10 + i, buf[i], SECTOR);
    }
    virtq_kick(&sVq);
    EXPECT_TRUE(t_wait_until([] {
        return tc_block_read(TEST_CONTROL_BLOCK_HELD) == 3;
    }));
    EXPECT_EQ(virtq_used_idx(&sVq), 0);
    tc_block_write(TEST_CONTROL_BLOCK_RELEASE, 3 | TEST_BLOCK_RELEASE_NEWEST);
    for (int i = 2; i >= 0; i--) {
        VirtqUsedElem e;
        ASSERT_TRUE(virtq_get_used(&sVq, &e));
        EXPECT_EQ(e.id, 3u * i);
    }
    for (int i = 0; i < 3; i++) {
        EXPECT_EQ(*status[i], VIRTIO_BLK_S_OK);
        EXPECT_EQ(pattern_errors(buf[i], 10 + i, 1), 0u);
    }
}

/* A reset gives up the requests in flight: the disk is told, and nothing
   comes back afterwards (2.1.2, 4.2.2.1). */
TEST(virtio_block, reset_in_flight)
{
    start();
    select_disk();
    tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_HOLD);
    uint32_t cancels = tc_block_read(TEST_CONTROL_BLOCK_CANCELS);
    uint8_t *buf = (uint8_t *)alloc(2 * SECTOR, 0xaa);
    queue_req(0, VIRTIO_BLK_T_IN, 20, buf, SECTOR);
    queue_req(3, VIRTIO_BLK_T_OUT, 21, buf + SECTOR, SECTOR);
    virtq_kick(&sVq);
    EXPECT_TRUE(t_wait_until([] {
        return tc_block_read(TEST_CONTROL_BLOCK_HELD) == 2;
    }));
    virtio_reset(&sDev);
    EXPECT_EQ(tc_block_read(TEST_CONTROL_BLOCK_CANCELS) - cancels, 2u);
    EXPECT_EQ(tc_block_read(TEST_CONTROL_BLOCK_HELD), 0u);
    t_delay_us(2000);
    EXPECT_EQ(virtq_used_idx(&sVq), 0);
    EXPECT_EQ(buf[0], 0xaa);
}

/* The data may be split over descriptors, of the ring or of an indirect
   table. */
TEST(virtio_block, segments)
{
    start(false, VIRTIO_F_VERSION_1 | VIRTIO_F_INDIRECT_DESC);
    if (!(virtio_device_features(&sDev, 0) & VIRTIO_F_INDIRECT_DESC))
        t_skip("indirect descriptors are not offered");
    Header *h = (Header *)alloc(sizeof(Header));
    h->type = VIRTIO_BLK_T_IN;
    h->sector = 40;
    uint8_t *a = (uint8_t *)alloc(SECTOR + 100);
    uint8_t *b = (uint8_t *)alloc(SECTOR - 100);
    uint8_t *c = (uint8_t *)alloc(SECTOR);
    uint8_t *status = (uint8_t *)alloc(1, 0xee);
    VirtqDesc *t = (VirtqDesc *)alloc(5 * sizeof(VirtqDesc));
    t[0] = VirtqDesc {(uintptr_t)h, sizeof(*h), VIRTQ_DESC_F_NEXT, 1};
    t[1] = VirtqDesc {(uintptr_t)a, SECTOR + 100,
                      VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE, 2};
    t[2] = VirtqDesc {(uintptr_t)b, SECTOR - 100,
                      VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE, 3};
    t[3] = VirtqDesc {(uintptr_t)c, SECTOR,
                      VIRTQ_DESC_F_NEXT | VIRTQ_DESC_F_WRITE, 4};
    t[4] = VirtqDesc {(uintptr_t)status, 1, VIRTQ_DESC_F_WRITE, 0};
    sVq.desc[7] = VirtqDesc {(uintptr_t)t, 5 * sizeof(VirtqDesc),
                             VIRTQ_DESC_F_INDIRECT, 0};
    virtq_publish(&sVq, 7);
    virtq_kick(&sVq);
    VirtqUsedElem e;
    ASSERT_TRUE(virtq_get_used(&sVq, &e));
    EXPECT_EQ(e.id, 7u);
    EXPECT_EQ(*status, VIRTIO_BLK_S_OK);
    uint8_t *all = (uint8_t *)alloc(3 * SECTOR);
    memcpy(all, a, SECTOR + 100);
    memcpy(all + SECTOR + 100, b, SECTOR - 100);
    memcpy(all + 2 * SECTOR, c, SECTOR);
    EXPECT_EQ(pattern_errors(all, 40, 3), 0u);
}

/* Requests a driver may not make (5.2.6.1): no status byte, a header
   short of 16 bytes, data that is not whole sectors, or more than the
   size_max and seg_max the device offers. */
TEST(virtio_block, malformed_requests)
{
    start();
    Header *h = (Header *)alloc(sizeof(Header));
    uint8_t *buf = (uint8_t *)alloc(2 * SECTOR, 0xaa);
    VirtqUsedElem e;

    /* no writable byte at all */
    sVq.desc[0] = VirtqDesc {(uintptr_t)h, sizeof(*h), 0, 0};
    virtq_publish(&sVq, 0);
    virtq_kick(&sVq);
    ASSERT_TRUE(virtq_get_used(&sVq, &e));
    SNAP("no_status_len", "%u", e.len);

    /* a short header */
    uint8_t *status = (uint8_t *)alloc(1, 0xee);
    sVq.desc[0] = VirtqDesc {(uintptr_t)h, 8, VIRTQ_DESC_F_NEXT, 1};
    sVq.desc[1] = VirtqDesc {(uintptr_t)status, 1, VIRTQ_DESC_F_WRITE, 0};
    virtq_publish(&sVq, 0);
    virtq_kick(&sVq);
    ASSERT_TRUE(virtq_get_used(&sVq, &e));
    SNAP("short_header", "len %u status %#x", e.len, *status);

    /* a sector and a byte: the byte past the data is not the host's */
    uint32_t len;
    uint8_t st = request(VIRTIO_BLK_T_IN, 50, buf, SECTOR + 1, &len);
    SNAP("odd_length", "status %u len %u tail %#x", st, len, buf[SECTOR]);
    EXPECT_EQ(pattern_errors(buf, 50, 1), 0u);

    /* past what the device offers, without it taking that much memory */
    uint32_t size_max = virtio_config_read(&sDev, CFG_SIZE_MAX, 2);
    uint32_t seg_max = virtio_config_read(&sDev, CFG_SEG_MAX, 2);
    uint64_t limit = (uint64_t)size_max * seg_max;
    if (limit && limit < 0x10000000) {
        h->type = VIRTIO_BLK_T_IN;
        h->sector = 0;
        /* the data descriptor says more than there is; the device must not
           get as far as filling it */
        sVq.desc[0] = VirtqDesc {(uintptr_t)h, sizeof(*h), VIRTQ_DESC_F_NEXT,
                                 1};
        sVq.desc[1] = VirtqDesc {(uintptr_t)buf, (uint32_t)(limit + SECTOR),
                                 VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT, 2};
        sVq.desc[2] = VirtqDesc {(uintptr_t)status, 1, VIRTQ_DESC_F_WRITE, 0};
        *status = 0xee;
        virtq_publish(&sVq, 0);
        virtq_kick(&sVq);
        ASSERT_TRUE(virtq_get_used(&sVq, &e));
        EXPECT_EQ(*status, VIRTIO_BLK_S_IOERR);
        EXPECT_EQ(buf[0], 0xaa);
    }
}
