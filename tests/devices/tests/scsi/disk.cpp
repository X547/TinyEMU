/*
 * SCSI disk tests
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
#include "scsi.h"
#include "test.h"
#include "test_control.h"
#include "testctl.h"

/* A direct access block device as SPC-3 (SCSI Primary Commands - 3) and
   SBC-3 (SCSI Block Commands - 3) describe it, on whichever initiator
   reaches one, with a test disk behind it. "Fails with 5/24/00" below is
   CHECK CONDITION with that sense key, additional sense code and
   qualifier. */

#define TYPE_DISK 0x00
#define BLOCK 512

namespace {

ScsiUnit sUnit;

void *alloc(size_t size, uint8_t fill = 0)
{
    void *p = heap_alloc(size ? size : 1, 16);
    if (p == nullptr)
        t_skip("no memory");
    memset(p, fill, size);
    return p;
}

/* Write protected, by the device specific parameter of the mode
   parameter header (SBC-3). */
bool write_protected(const ScsiUnit &u)
{
    uint8_t cdb[6] = {0x1a, 0, 0x3f, 0, 255, 0};
    uint8_t *buf = (uint8_t *)alloc(255);
    ScsiResult r;
    return u.Command(cdb, 6, SCSI_IN, buf, 255, &r) && r.delivered &&
           r.status == SCSI_GOOD && (buf[2] & 0x80);
}

/* The first disk unit that is writable, or write protected, with its test
   disk selected. */
void need_disk(bool read_only = false)
{
    for (int i = 0; scsi_find_unit(TYPE_DISK, i, &sUnit); i++) {
        if (write_protected(sUnit) != read_only)
            continue;
        if (tc_block_find(sUnit.blocks * sUnit.block_size / 512) < 0)
            t_skip("the unit's disk is not a test disk");
        return;
    }
    t_skip("no %s SCSI disk", read_only ? "write protected" : "writable");
}

ScsiResult cmd(const uint8_t *cdb, int len, ScsiDir dir = SCSI_NONE,
               void *buf = nullptr, uint32_t buf_len = 0)
{
    ScsiResult r;
    if (!sUnit.Command(cdb, len, dir, buf, buf_len, &r)) {
        t_fail(__FILE__, __LINE__, "no answer to command %#x", cdb[0]);
        t_abort();
    }
    if (!r.delivered) {
        t_fail(__FILE__, __LINE__, "%s refused command %#x: %u",
               sUnit.init->Name(), cdb[0], r.adapter);
        t_abort();
    }
    return r;
}

#define EXPECT_GOOD(r) EXPECT_EQ((r).status, SCSI_GOOD)
#define EXPECT_SENSE(r, key, asc)                                    \
    do {                                                             \
        ScsiResult r_ = (r);                                         \
        EXPECT_EQ(r_.status, SCSI_CHECK_CONDITION);                  \
        EXPECT_EQ(r_.Key(), key);                                    \
        EXPECT_EQ(r_.Asc(), asc);                                    \
    } while (0)

uint32_t pattern_errors(const uint8_t *buf, uint64_t lba, uint32_t blocks)
{
    uint32_t bad = 0;
    for (uint32_t b = 0; b < blocks; b++) {
        for (uint32_t i = 0; i < BLOCK; i++)
            bad += buf[b * BLOCK + i] != TEST_BLOCK_PATTERN(lba + b, i);
    }
    return bad;
}

void rw10(uint8_t *cdb, uint8_t op, uint32_t lba, uint16_t blocks)
{
    memset(cdb, 0, 10);
    cdb[0] = op;
    put_be32(cdb + 2, lba);
    cdb[7] = blocks >> 8;
    cdb[8] = blocks;
}

}


/* Standard INQUIRY data (SPC-3): a direct access device, not
   removable, response data format 2, 36 bytes, and the identification in
   ASCII. */
TEST(scsi_disk, inquiry)
{
    need_disk();
    uint8_t cdb[6] = {0x12, 0, 0, 0, 96, 0};
    uint8_t *buf = (uint8_t *)alloc(96, 0xee);
    ScsiResult r = cmd(cdb, 6, SCSI_IN, buf, 96);
    EXPECT_GOOD(r);
    EXPECT_EQ(buf[0], TYPE_DISK);
    EXPECT_EQ(buf[1] & 0x80, 0);
    EXPECT_EQ(buf[3] & 0x0f, 2);
    EXPECT_GE(buf[4], 31);
    EXPECT_EQ(r.residual, 96u - (buf[4] + 5));
    for (int i = 8; i < 36; i++)
        EXPECT_TRUE(buf[i] >= 0x20 && buf[i] < 0x7f);
    SNAP("version", "%#x", buf[2]);
    char id[29] = {};
    memcpy(id, buf + 8, 28);
    SNAP("id", "%s", id);
}

/* No more than the allocation length is returned, however large the
   initiator's buffer (SPC-3). */
TEST(scsi_disk, allocation_length)
{
    need_disk();
    uint8_t *buf = (uint8_t *)alloc(96, 0xee);
    uint8_t inquiry[6] = {0x12, 0, 0, 0, 5, 0};
    ScsiResult r = cmd(inquiry, 6, SCSI_IN, buf, 96);
    EXPECT_GOOD(r);
    EXPECT_EQ(r.residual, 91u);
    EXPECT_EQ(buf[5], 0xee);

    memset(buf, 0xee, 96);
    uint8_t sense[6] = {0x03, 0, 0, 0, 4, 0};
    r = cmd(sense, 6, SCSI_IN, buf, 96);
    EXPECT_GOOD(r);
    EXPECT_EQ(r.residual, 92u);
    EXPECT_EQ(buf[4], 0xee);

    memset(buf, 0xee, 96);
    uint8_t mode[6] = {0x1a, 0, 0x3f, 0, 4, 0};
    r = cmd(mode, 6, SCSI_IN, buf, 96);
    EXPECT_GOOD(r);
    EXPECT_EQ(r.residual, 92u);
    EXPECT_EQ(buf[4], 0xee);

    if (sUnit.init->MaxCdb() < 16)
        return;
    memset(buf, 0xee, 96);
    uint8_t cap16[16] = {0x9e, 0x10};
    cap16[13] = 12;
    r = cmd(cap16, 16, SCSI_IN, buf, 96);
    EXPECT_GOOD(r);
    EXPECT_EQ(r.residual, 84u);
    EXPECT_EQ(buf[12], 0xee);
}

/* Vital product data (SPC-3): the supported pages, which list
   themselves, the serial number and the device identification. A page not
   supported, or a page code without EVPD, fails with 5/24/00. */
TEST(scsi_disk, vpd)
{
    need_disk();
    uint8_t *buf = (uint8_t *)alloc(255);
    uint8_t cdb[6] = {0x12, 1, 0x00, 0, 255, 0};
    ScsiResult r = cmd(cdb, 6, SCSI_IN, buf, 255);
    EXPECT_GOOD(r);
    EXPECT_EQ(buf[1], 0x00);
    int n = buf[3];
    bool has80 = false, has83 = false;
    for (int i = 0; i < n; i++) {
        has80 |= buf[4 + i] == 0x80;
        has83 |= buf[4 + i] == 0x83;
    }
    EXPECT_EQ(buf[4], 0x00);
    char pages[64];
    size_t p = 0;
    for (int i = 0; i < n && p + 4 < sizeof(pages); i++)
        p += snprintf(pages + p, sizeof(pages) - p, "%02x ", buf[4 + i]);
    SNAP("pages", "%s", pages);

    if (has80) {
        cdb[2] = 0x80;
        r = cmd(cdb, 6, SCSI_IN, buf, 255);
        EXPECT_GOOD(r);
        EXPECT_EQ(buf[1], 0x80);
        char serial[64] = {};
        memcpy(serial, buf + 4, buf[3] < 63 ? buf[3] : 63);
        SNAP("serial", "%s", serial);
    }
    if (has83) {
        cdb[2] = 0x83;
        r = cmd(cdb, 6, SCSI_IN, buf, 255);
        EXPECT_GOOD(r);
        EXPECT_EQ(buf[1], 0x83);
        /* each designator: 4 byte header and its length */
        uint32_t len = be16(buf + 2), off = 4;
        while (off + 4 <= len + 4) {
            SNAP("designator", "code set %u type %u length %u",
                 buf[off] & 0x0f, buf[off + 1] & 0x0f, buf[off + 3]);
            off += 4 + buf[off + 3];
        }
        EXPECT_EQ(off, len + 4);
    }
    cdb[2] = 0xb7;
    EXPECT_SENSE(cmd(cdb, 6, SCSI_IN, buf, 255), SCSI_KEY_ILLEGAL_REQUEST,
                 0x2400);
    uint8_t no_evpd[6] = {0x12, 0, 0x80, 0, 255, 0};
    EXPECT_SENSE(cmd(no_evpd, 6, SCSI_IN, buf, 255), SCSI_KEY_ILLEGAL_REQUEST,
                 0x2400);
}

/* READ CAPACITY (10) and (16) give the last block and the block length
   (SBC-3); with PMI clear the LBA field must be 0. */
TEST(scsi_disk, read_capacity)
{
    need_disk();
    uint64_t sectors = tc_block_read(TEST_CONTROL_BLOCK_SECTORS);
    uint8_t *buf = (uint8_t *)alloc(32, 0xee);
    uint8_t c10[10] = {0x25};
    ScsiResult r = cmd(c10, 10, SCSI_IN, buf, 8);
    EXPECT_GOOD(r);
    EXPECT_EQ(be32(buf + 4), (uint32_t)BLOCK);
    EXPECT_EQ(be32(buf), (uint32_t)(sectors - 1));
    if (sUnit.init->MaxCdb() >= 16) {
        uint8_t c16[16] = {0x9e, 0x10};
        put_be32(c16 + 10, 32);
        r = cmd(c16, 16, SCSI_IN, buf, 32);
        EXPECT_GOOD(r);
        EXPECT_EQ(((uint64_t)be32(buf) << 32) | be32(buf + 4),
                  sectors - 1);
        EXPECT_EQ(be32(buf + 8), (uint32_t)BLOCK);
        SNAP("cap16_rest", "%02x %02x %02x %02x", buf[12], buf[13],
             buf[14], buf[15]);
    }
    put_be32(c10 + 2, 5);
    EXPECT_SENSE(cmd(c10, 10, SCSI_IN, buf, 8), SCSI_KEY_ILLEGAL_REQUEST,
                 0x2400);
}

/* The READ commands of every length give the blocks asked for (SBC-3);
   READ (6) takes a length of 0 as 256 blocks, the others as
   none. */
TEST(scsi_disk, read)
{
    need_disk();
    uint8_t *buf = (uint8_t *)alloc(256 * BLOCK, 0xee);
    uint8_t r6[6] = {0x08, 0, 0, 7, 4, 0};
    EXPECT_GOOD(cmd(r6, 6, SCSI_IN, buf, 4 * BLOCK));
    EXPECT_EQ(pattern_errors(buf, 7, 4), 0u);
    r6[4] = 0;
    EXPECT_GOOD(cmd(r6, 6, SCSI_IN, buf, 256 * BLOCK));
    EXPECT_EQ(pattern_errors(buf, 7, 256), 0u);

    uint8_t r10[10];
    rw10(r10, 0x28, 300, 3);
    EXPECT_GOOD(cmd(r10, 10, SCSI_IN, buf, 3 * BLOCK));
    EXPECT_EQ(pattern_errors(buf, 300, 3), 0u);
    rw10(r10, 0x28, 300, 0);
    ScsiResult r = cmd(r10, 10);
    EXPECT_GOOD(r);

    uint8_t r12[12] = {0xa8};
    put_be32(r12 + 2, 500);
    put_be32(r12 + 6, 2);
    EXPECT_GOOD(cmd(r12, 12, SCSI_IN, buf, 2 * BLOCK));
    EXPECT_EQ(pattern_errors(buf, 500, 2), 0u);

    if (sUnit.init->MaxCdb() < 16)
        return;
    uint8_t r16[16] = {0x88};
    put_be32(r16 + 6, (uint32_t)(sUnit.blocks - 1));
    put_be32(r16 + 10, 1);
    EXPECT_GOOD(cmd(r16, 16, SCSI_IN, buf, BLOCK));
    EXPECT_EQ(pattern_errors(buf, sUnit.blocks - 1, 1), 0u);
}

/* The WRITE commands store what reads back (SBC-3). */
TEST(scsi_disk, write)
{
    need_disk();
    uint8_t *w = (uint8_t *)alloc(4 * BLOCK);
    uint8_t *r = (uint8_t *)alloc(4 * BLOCK);
    for (int i = 0; i < 4 * BLOCK; i++)
        w[i] = i * 3 + 5;
    uint8_t c[16] = {};
    rw10(c, 0x2a, 40, 2);
    EXPECT_GOOD(cmd(c, 10, SCSI_OUT, w, 2 * BLOCK));
    uint8_t w6[6] = {0x0a, 0, 0, 42, 1, 0};
    EXPECT_GOOD(cmd(w6, 6, SCSI_OUT, w + 2 * BLOCK, BLOCK));
    uint8_t w12[12] = {0xaa};
    put_be32(w12 + 2, 43);
    put_be32(w12 + 6, 1);
    EXPECT_GOOD(cmd(w12, 12, SCSI_OUT, w + 3 * BLOCK, BLOCK));
    rw10(c, 0x28, 40, 4);
    EXPECT_GOOD(cmd(c, 10, SCSI_IN, r, 4 * BLOCK));
    EXPECT_EQ(memcmp(r, w, 4 * BLOCK), 0);
}

/* A transfer that reaches past the last block fails with 5/21/00 (SBC-3);
   the sense is kept for REQUEST SENSE, which then clears it. */
TEST(scsi_disk, out_of_range)
{
    need_disk();
    uint8_t *buf = (uint8_t *)alloc(2 * BLOCK);
    uint8_t c[10];
    rw10(c, 0x28, sUnit.blocks - 1, 2);
    EXPECT_SENSE(cmd(c, 10, SCSI_IN, buf, 2 * BLOCK),
                 SCSI_KEY_ILLEGAL_REQUEST, 0x2100);
    uint8_t rs[6] = {0x03, 0, 0, 0, 18, 0};
    uint8_t *sense = (uint8_t *)alloc(18);
    ScsiResult r = cmd(rs, 6, SCSI_IN, sense, 18);
    EXPECT_GOOD(r);
    EXPECT_EQ(sense[0] & 0x7f, 0x70);
    EXPECT_EQ(sense[2] & 0x0f, SCSI_KEY_ILLEGAL_REQUEST);
    EXPECT_EQ(sense[12], 0x21);
    r = cmd(rs, 6, SCSI_IN, sense, 18);
    EXPECT_EQ(sense[2] & 0x0f, SCSI_KEY_NO_SENSE);
    EXPECT_EQ(sense[12], 0);
    rw10(c, 0x2a, sUnit.blocks, 1);
    EXPECT_SENSE(cmd(c, 10, SCSI_OUT, buf, BLOCK), SCSI_KEY_ILLEGAL_REQUEST,
                 0x2100);
    if (sUnit.init->MaxCdb() < 16)
        return;
    uint8_t r16[16] = {0x88};
    put_be32(r16 + 2, 1);
    put_be32(r16 + 10, 1);
    EXPECT_SENSE(cmd(r16, 16, SCSI_IN, buf, BLOCK), SCSI_KEY_ILLEGAL_REQUEST,
                 0x2100);
}

/* A write protected unit fails writes with 7/27/00 (SBC-3) and
   reports WP in the mode parameter header. */
TEST(scsi_disk, write_protected)
{
    need_disk(true);
    uint8_t *buf = (uint8_t *)alloc(BLOCK);
    uint8_t c[10];
    rw10(c, 0x2a, 1, 1);
    EXPECT_SENSE(cmd(c, 10, SCSI_OUT, buf, BLOCK), SCSI_KEY_DATA_PROTECT,
                 0x2700);
    rw10(c, 0x28, 1, 1);
    EXPECT_GOOD(cmd(c, 10, SCSI_IN, buf, BLOCK));
    EXPECT_EQ(pattern_errors(buf, 1, 1), 0u);
}

/* The disk failing a read or a write is a medium error, 3/11/00 or
   3/0C/00 (SPC-3). */
TEST(scsi_disk, medium_error)
{
    need_disk();
    uint8_t *buf = (uint8_t *)alloc(BLOCK);
    uint8_t c[10];
    tc_block_write(TEST_CONTROL_BLOCK_FAIL, 1);
    rw10(c, 0x28, 2, 1);
    EXPECT_SENSE(cmd(c, 10, SCSI_IN, buf, BLOCK), SCSI_KEY_MEDIUM_ERROR,
                 0x1100);
    tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_ASYNC);
    tc_block_write(TEST_CONTROL_BLOCK_FAIL, 1);
    rw10(c, 0x2a, 2, 1);
    EXPECT_SENSE(cmd(c, 10, SCSI_OUT, buf, BLOCK), SCSI_KEY_MEDIUM_ERROR,
                 0x0c00);
    rw10(c, 0x28, 2, 1);
    EXPECT_GOOD(cmd(c, 10, SCSI_IN, buf, BLOCK));
    EXPECT_EQ(pattern_errors(buf, 2, 1), 0u);
}

/* MODE SENSE (6) and (10) (SPC-3): the header lengths, the
   pages offered, a page that is not 5/24/00, and saved values, which
   nothing here keeps, 5/39/00. */
TEST(scsi_disk, mode_sense)
{
    need_disk();
    uint8_t *buf = (uint8_t *)alloc(255, 0xee);
    uint8_t m6[6] = {0x1a, 0, 0x3f, 0, 255, 0};
    ScsiResult r = cmd(m6, 6, SCSI_IN, buf, 255);
    EXPECT_GOOD(r);
    EXPECT_EQ(buf[0] + 1u, 255u - r.residual);
    EXPECT_EQ(buf[3] % 8, 0);
    char pages[64];
    size_t p = 0;
    for (uint32_t off = 4 + buf[3]; off < buf[0] + 1u && p + 4 < sizeof(pages);
         off += 2 + buf[off + 1])
        p += snprintf(pages + p, sizeof(pages) - p, "%02x/%u ",
                      buf[off] & 0x3f, buf[off + 1]);
    SNAP("pages", "%s", pages);

    uint8_t m10[10] = {0x5a, 0, 0x08, 0, 0, 0, 0, 0, 255, 0};
    r = cmd(m10, 10, SCSI_IN, buf, 255);
    EXPECT_GOOD(r);
    EXPECT_EQ(be16(buf) + 2u, 255u - r.residual);
    EXPECT_EQ(buf[8 + be16(buf + 6)] & 0x3f, 0x08);

    m6[2] = 0x3f | 0x40; /* changeable values */
    EXPECT_GOOD(cmd(m6, 6, SCSI_IN, buf, 255));
    m6[2] = 0x3f | 0xc0; /* saved values */
    EXPECT_SENSE(cmd(m6, 6, SCSI_IN, buf, 255), SCSI_KEY_ILLEGAL_REQUEST,
                 0x3900);
    m6[2] = 0x2e;
    EXPECT_SENSE(cmd(m6, 6, SCSI_IN, buf, 255), SCSI_KEY_ILLEGAL_REQUEST,
                 0x2400);
    m6[2] = 0x08;
    m6[3] = 0x05; /* a subpage */
    EXPECT_SENSE(cmd(m6, 6, SCSI_IN, buf, 255), SCSI_KEY_ILLEGAL_REQUEST,
                 0x2400);
}

/* REQUEST SENSE with DESC set asks for descriptor format sense, which a
   unit without it refuses with 5/24/00 (SPC-3). */
TEST(scsi_disk, request_sense_desc)
{
    need_disk();
    uint8_t *buf = (uint8_t *)alloc(252);
    uint8_t rs[6] = {0x03, 1, 0, 0, 252, 0};
    ScsiResult r = cmd(rs, 6, SCSI_IN, buf, 252);
    if (r.status == SCSI_GOOD) {
        /* descriptor format then */
        EXPECT_EQ(buf[0] & 0x7f, 0x72);
    } else {
        EXPECT_SENSE(r, SCSI_KEY_ILLEGAL_REQUEST, 0x2400);
    }
}

/* Commands that move no data: TEST UNIT READY, START STOP UNIT, MODE
   SELECT of nothing, SYNCHRONIZE CACHE and VERIFY within the disk, which
   past it fail with 5/21/00 (SBC-3). An operation code the
   unit does not have fails with 5/20/00. */
TEST(scsi_disk, other_commands)
{
    need_disk();
    uint8_t tur[6] = {0x00};
    EXPECT_GOOD(cmd(tur, 6));
    uint8_t ssu[6] = {0x1b, 0, 0, 0, 1, 0};
    EXPECT_GOOD(cmd(ssu, 6));
    uint8_t sync[10] = {0x35};
    EXPECT_GOOD(cmd(sync, 10));
    put_be32(sync + 2, sUnit.blocks - 2);
    sync[8] = 4;
    EXPECT_SENSE(cmd(sync, 10), SCSI_KEY_ILLEGAL_REQUEST, 0x2100);
    uint8_t verify[10] = {0x2f};
    put_be32(verify + 2, 10);
    verify[8] = 4;
    EXPECT_GOOD(cmd(verify, 10));
    put_be32(verify + 2, sUnit.blocks);
    EXPECT_SENSE(cmd(verify, 10), SCSI_KEY_ILLEGAL_REQUEST, 0x2100);
    uint8_t bad[6] = {0x04};  /* FORMAT UNIT */
    SNAP("format_unit", "%#x/%#x", cmd(bad, 6).Key(), cmd(bad, 6).Asc());
    uint8_t none[10] = {0xe7};
    EXPECT_SENSE(cmd(none, 10), SCSI_KEY_ILLEGAL_REQUEST, 0x2000);
}
