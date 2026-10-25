/*
 * SCSI CD-ROM drive tests
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

/* A CD-ROM drive as MMC (SCSI Multi-Media Commands) and SPC-3 describe it,
   with a test disk as its disc, read in 2048 byte blocks of four of the
   disk's sectors, or with no disc at all. */

#define TYPE_CDROM 0x05
#define BLOCK 2048

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

/* Tray in, unlocked, and no unit attention left over. */
void tidy()
{
    uint8_t allow[6] = {0x1e};
    uint8_t load[6] = {0x1b, 0, 0, 0, 0x03, 0};
    uint8_t tur[6] = {0x00};
    ScsiResult r;
    sUnit.Command(allow, 6, SCSI_NONE, nullptr, 0, &r);
    sUnit.Command(load, 6, SCSI_NONE, nullptr, 0, &r);
    sUnit.Command(tur, 6, SCSI_NONE, nullptr, 0, &r);
}

/* The first drive with a disc, its disk selected, or the first without. */
void need_cd(bool disc = true)
{
    for (int i = 0; scsi_find_unit(TYPE_CDROM, i, &sUnit); i++) {
        if ((sUnit.block_size != 0) != disc)
            continue;
        if (disc && tc_block_find(sUnit.blocks * 4) < 0)
            t_skip("the disc is not a test disk");
        t_on_cleanup(tidy);
        return;
    }
    t_skip("no CD-ROM drive %s a disc", disc ? "with" : "without");
}

uint32_t pattern_errors(const uint8_t *buf, uint32_t lba, uint32_t blocks)
{
    uint32_t bad = 0;
    for (uint32_t b = 0; b < blocks * 4; b++) {
        for (uint32_t i = 0; i < 512; i++)
            bad += buf[b * 512 + i] != TEST_BLOCK_PATTERN(lba * 4 + b, i);
    }
    return bad;
}

/* Minute, second and frame of a block, as the TOC gives them: 75 frames a
   second, after two seconds of lead-in. */
uint32_t msf(uint32_t lba)
{
    lba += 150;
    return (lba / (60 * 75)) << 16 | (lba / 75 % 60) << 8 | lba % 75;
}

}


/* A removable CD-ROM device, standard INQUIRY data of 36 bytes or more. */
TEST(scsi_cd, inquiry)
{
    need_cd();
    uint8_t *buf = (uint8_t *)alloc(96, 0xee);
    uint8_t cdb[6] = {0x12, 0, 0, 0, 96, 0};
    ScsiResult r = cmd(cdb, 6, SCSI_IN, buf, 96);
    EXPECT_GOOD(r);
    EXPECT_EQ(buf[0], TYPE_CDROM);
    EXPECT_EQ(buf[1] & 0x80, 0x80);
    EXPECT_GE(buf[4], 31);
    char id[29] = {};
    memcpy(id, buf + 8, 28);
    SNAP("id", "%s", id);
    uint8_t no_evpd[6] = {0x12, 0, 0x80, 0, 96, 0};
    EXPECT_SENSE(cmd(no_evpd, 6, SCSI_IN, buf, 96), SCSI_KEY_ILLEGAL_REQUEST,
                 0x2400);
}

/* 2048 byte blocks, as many as the disc holds. */
TEST(scsi_cd, read_capacity)
{
    need_cd();
    uint8_t *buf = (uint8_t *)alloc(8);
    uint8_t c[10] = {0x25};
    EXPECT_GOOD(cmd(c, 10, SCSI_IN, buf, 8));
    EXPECT_EQ(be32(buf + 4), (uint32_t)BLOCK);
    EXPECT_EQ(be32(buf) + 1, tc_block_read(TEST_CONTROL_BLOCK_SECTORS) / 4);
    put_be32(c + 2, 1);
    EXPECT_SENSE(cmd(c, 10, SCSI_IN, buf, 8), SCSI_KEY_ILLEGAL_REQUEST,
                 0x2400);
}

/* READ (10) and (12), and READ CD for the user data alone; past the end
   5/21/00. Other fields of a raw sector are not kept, and asked for fail
   with 5/24/00. A CD-ROM has no WRITE. */
TEST(scsi_cd, read)
{
    need_cd();
    uint8_t *buf = (uint8_t *)alloc(3 * BLOCK);
    uint8_t r10[10] = {0x28, 0, 0, 0, 0, 9, 0, 0, 2, 0};
    EXPECT_GOOD(cmd(r10, 10, SCSI_IN, buf, 2 * BLOCK));
    EXPECT_EQ(pattern_errors(buf, 9, 2), 0u);
    uint8_t r12[12] = {0xa8};
    put_be32(r12 + 2, sUnit.blocks - 1);
    put_be32(r12 + 6, 1);
    EXPECT_GOOD(cmd(r12, 12, SCSI_IN, buf, BLOCK));
    EXPECT_EQ(pattern_errors(buf, sUnit.blocks - 1, 1), 0u);
    put_be32(r12 + 6, 2);
    EXPECT_SENSE(cmd(r12, 12, SCSI_IN, buf, 2 * BLOCK),
                 SCSI_KEY_ILLEGAL_REQUEST, 0x2100);

    uint8_t rcd[12] = {0xbe, 0, 0, 0, 0, 20, 0, 0, 3, 0x10, 0, 0};
    EXPECT_GOOD(cmd(rcd, 12, SCSI_IN, buf, 3 * BLOCK));
    EXPECT_EQ(pattern_errors(buf, 20, 3), 0u);
    rcd[9] = 0xf8;
    rcd[8] = 1;
    uint8_t *raw = (uint8_t *)alloc(2352);
    EXPECT_SENSE(cmd(rcd, 12, SCSI_IN, raw, 2352), SCSI_KEY_ILLEGAL_REQUEST,
                 0x2400);

    uint8_t w10[10] = {0x2a, 0, 0, 0, 0, 1, 0, 0, 1, 0};
    EXPECT_SENSE(cmd(w10, 10, SCSI_OUT, buf, BLOCK), SCSI_KEY_ILLEGAL_REQUEST,
                 0x2000);
}

/* The TOC of the one data track (MMC, READ TOC/PMA/ATIP format 0): track 1
   at block 0, or 00:02:00, and the lead-out after the last block. */
TEST(scsi_cd, toc)
{
    need_cd();
    uint8_t *buf = (uint8_t *)alloc(64);
    uint8_t c[10] = {0x43, 0, 0, 0, 0, 0, 0, 0, 64, 0};
    ScsiResult r = cmd(c, 10, SCSI_IN, buf, 64);
    EXPECT_GOOD(r);
    EXPECT_EQ(be16(buf), 18);
    EXPECT_EQ(buf[2], 1);
    EXPECT_EQ(buf[3], 1);
    EXPECT_EQ(buf[6], 1);
    EXPECT_EQ(buf[5] & 0x04, 0x04);
    EXPECT_EQ(be32(buf + 8), 0u);
    EXPECT_EQ(buf[14], 0xaa);
    EXPECT_EQ(be32(buf + 16), (uint32_t)sUnit.blocks);
    c[1] = 0x02;
    EXPECT_GOOD(cmd(c, 10, SCSI_IN, buf, 64));
    EXPECT_EQ(be32(buf + 8), msf(0));
    EXPECT_EQ(be32(buf + 16), msf(sUnit.blocks));
    c[1] = 0;
    c[2] = 1;
    EXPECT_GOOD(cmd(c, 10, SCSI_IN, buf, 64));
    SNAP("session", "%02x %02x %02x %02x", buf[2], buf[3], buf[6], buf[8]);
    c[2] = 2;
    r = cmd(c, 10, SCSI_IN, buf, 64);
    EXPECT_GOOD(r);
    SNAP("raw_len", "%u", be16(buf));
    c[2] = 0;
    c[6] = 2;
    EXPECT_SENSE(cmd(c, 10, SCSI_IN, buf, 64), SCSI_KEY_ILLEGAL_REQUEST,
                 0x2400);
}

/* GET CONFIGURATION gives the current profile, CD-ROM for a disc this
   small (MMC, profile 0008h), and the feature headers in order. */
TEST(scsi_cd, configuration)
{
    need_cd();
    uint8_t *buf = (uint8_t *)alloc(128);
    uint8_t c[10] = {0x46, 0, 0, 0, 0, 0, 0, 0, 128, 0};
    EXPECT_GOOD(cmd(c, 10, SCSI_IN, buf, 128));
    EXPECT_EQ(be16(buf + 6), 0x0008);
    uint32_t len = be32(buf) + 4, off = 8;
    char features[96];
    size_t p = 0;
    uint16_t last = 0;
    while (off + 4 <= len && off + 4 <= 128) {
        uint16_t code = be16(buf + off);
        EXPECT_TRUE(off == 8 || code > last);
        last = code;
        p += snprintf(features + p, sizeof(features) - p, "%04x%s ", code,
                      buf[off + 2] & 1 ? "*" : "");
        off += 4 + buf[off + 3];
    }
    SNAP("features", "%s", features);
    c[1] = 3;
    EXPECT_SENSE(cmd(c, 10, SCSI_IN, buf, 128), SCSI_KEY_ILLEGAL_REQUEST,
                 0x2400);
}

/* The tray: ejected, the drive is not ready with the tray open (2/3A/02);
   loaded, the next command hears of the new disc once (6/28/00), and the
   media event class reports it. Prevented, the tray stays in (5/53/02). */
TEST(scsi_cd, tray)
{
    need_cd();
    uint8_t tur[6] = {0x00};
    uint8_t eject[6] = {0x1b, 0, 0, 0, 0x02, 0};
    uint8_t load[6] = {0x1b, 0, 0, 0, 0x03, 0};
    uint8_t prevent[6] = {0x1e, 0, 0, 0, 1, 0};
    uint8_t allow[6] = {0x1e};
    uint8_t ev[10] = {0x4a, 1, 0, 0, 0x10, 0, 0, 0, 8, 0};
    uint8_t *buf = (uint8_t *)alloc(8);

    EXPECT_GOOD(cmd(tur, 6));
    EXPECT_GOOD(cmd(ev, 10, SCSI_IN, buf, 8));
    EXPECT_EQ(buf[4] & 0x0f, 0);
    EXPECT_GOOD(cmd(eject, 6));
    EXPECT_SENSE(cmd(tur, 6), SCSI_KEY_NOT_READY, 0x3a02);
    EXPECT_GOOD(cmd(ev, 10, SCSI_IN, buf, 8));
    EXPECT_EQ(buf[4] & 0x0f, 3); /* media removal */
    EXPECT_EQ(buf[5] & 0x03, 0x01);
    EXPECT_GOOD(cmd(load, 6));
    EXPECT_SENSE(cmd(tur, 6), SCSI_KEY_UNIT_ATTENTION, 0x2800);
    EXPECT_GOOD(cmd(tur, 6));
    EXPECT_GOOD(cmd(ev, 10, SCSI_IN, buf, 8));
    EXPECT_EQ(buf[4] & 0x0f, 2); /* new media */
    EXPECT_EQ(buf[5] & 0x03, 0x02);

    EXPECT_GOOD(cmd(prevent, 6));
    EXPECT_SENSE(cmd(eject, 6), SCSI_KEY_ILLEGAL_REQUEST, 0x5302);
    EXPECT_GOOD(cmd(tur, 6));
    EXPECT_GOOD(cmd(allow, 6));
    EXPECT_GOOD(cmd(eject, 6));
}

/* A drive without a disc is there, and not ready with the tray closed
   (2/3A/01). */
TEST(scsi_cd, no_disc)
{
    need_cd(false);
    uint8_t tur[6] = {0x00};
    EXPECT_SENSE(cmd(tur, 6), SCSI_KEY_NOT_READY, 0x3a01);
    uint8_t *buf = (uint8_t *)alloc(128);
    uint8_t cap[10] = {0x25};
    EXPECT_SENSE(cmd(cap, 10, SCSI_IN, buf, 8), SCSI_KEY_NOT_READY, 0x3a01);
    uint8_t conf[10] = {0x46, 0, 0, 0, 0, 0, 0, 0, 128, 0};
    EXPECT_GOOD(cmd(conf, 10, SCSI_IN, buf, 128));
    EXPECT_EQ(be16(buf + 6), 0);
    uint8_t rs[6] = {0x03, 0, 0, 0, 18, 0};
    cmd(tur, 6);
    EXPECT_GOOD(cmd(rs, 6, SCSI_IN, buf, 18));
    EXPECT_EQ(buf[2] & 0x0f, SCSI_KEY_NOT_READY);
    EXPECT_EQ(buf[12], 0x3a);
}

/* MODE SENSE: the read error recovery and capabilities pages; saved
   values, which there are none of, 5/39/00. The other reports, as they
   are. */
TEST(scsi_cd, other_commands)
{
    need_cd();
    uint8_t *buf = (uint8_t *)alloc(255);
    uint8_t m10[10] = {0x5a, 0, 0x3f, 0, 0, 0, 0, 0, 255, 0};
    ScsiResult r = cmd(m10, 10, SCSI_IN, buf, 255);
    EXPECT_GOOD(r);
    EXPECT_EQ(be16(buf) + 2u, 255u - r.residual);
    char pages[32];
    size_t p = 0;
    for (uint32_t off = 8 + be16(buf + 6); off < be16(buf) + 2u &&
         p + 8 < sizeof(pages); off += 2 + buf[off + 1])
        p += snprintf(pages + p, sizeof(pages) - p, "%02x/%u ",
                      buf[off] & 0x3f, buf[off + 1]);
    SNAP("pages", "%s", pages);
    m10[2] = 0xc0 | 0x2a;
    EXPECT_SENSE(cmd(m10, 10, SCSI_IN, buf, 255), SCSI_KEY_ILLEGAL_REQUEST,
                 0x3900);

    uint8_t disc[10] = {0x51, 0, 0, 0, 0, 0, 0, 0, 34, 0};
    EXPECT_GOOD(cmd(disc, 10, SCSI_IN, buf, 34));
    SNAP("disc_info", "%02x %02x %02x %02x %02x", buf[2], buf[3], buf[4],
         buf[5], buf[6]);
    uint8_t track[10] = {0x52, 1, 0, 0, 0, 1, 0, 0, 36, 0};
    EXPECT_GOOD(cmd(track, 10, SCSI_IN, buf, 36));
    EXPECT_EQ(be32(buf + 24), (uint32_t)sUnit.blocks);
    uint8_t dvd[12] = {0xad, 0, 0, 0, 0, 0, 0, 0, 0, 255, 0, 0};
    EXPECT_SENSE(cmd(dvd, 12, SCSI_IN, buf, 255), SCSI_KEY_ILLEGAL_REQUEST,
                 0x3002);
    uint8_t sub[10] = {0x42, 0, 0x40, 1, 0, 0, 0, 0, 16, 0};
    EXPECT_GOOD(cmd(sub, 10, SCSI_IN, buf, 16));
    SNAP("subchannel", "%02x %02x", buf[1], buf[5]);
    uint8_t mech[12] = {0xbd, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 0};
    EXPECT_GOOD(cmd(mech, 12, SCSI_IN, buf, 8));
    SNAP("mechanism", "%02x %02x", buf[0], buf[1]);
}
