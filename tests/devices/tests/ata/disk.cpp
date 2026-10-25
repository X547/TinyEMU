/*
 * ATA disk tests
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
#include "ata.h"
#include "format.h"
#include "heap.h"
#include "test.h"
#include "test_control.h"
#include "testctl.h"

/* An ATA disk as ATA/ATAPI-6 (T13/1410D) describes it, the PIO and DMA
   protocols, with the bus master of SFF-8038i, on a test disk. */

#define CMD_READ        0x20
#define CMD_WRITE       0x30
#define CMD_VERIFY      0x40
#define CMD_DIAGNOSE    0x90
#define CMD_READ_MULT   0xc4
#define CMD_WRITE_MULT  0xc5
#define CMD_SET_MULT    0xc6
#define CMD_READ_DMA    0xc8
#define CMD_WRITE_DMA   0xca
#define CMD_CHECK_POWER 0xe5
#define CMD_FLUSH       0xe7
#define CMD_IDENTIFY    0xec
#define CMD_SET_FEATURES 0xef
#define CMD_NATIVE_MAX  0xf8

#define SECTOR 512
#define LIMIT_4G 0x100000000ull

namespace {

AtaChannel sCh;
int sUnit;
uint32_t sSectors;

void *alloc(size_t size, uint8_t fill = 0, size_t align = 16)
{
    void *p = heap_alloc(size ? size : 1, align, LIMIT_4G);
    if (p == nullptr)
        t_skip("no memory");
    memset(p, fill, size);
    return p;
}

uint16_t word(const uint8_t *id, int i) {return le16(id + 2 * i);}

/* A command with the LBA in the task file, waited for: the status. */
uint8_t issue(uint8_t cmd, uint32_t lba = 0, uint8_t count = 0,
              uint8_t feature = 0, bool lba_mode = true)
{
    sCh.Write(ATA_FEATURE, feature);
    sCh.Write(ATA_NSECT, count);
    sCh.Write(ATA_LBAL, lba);
    sCh.Write(ATA_LBAM, lba >> 8);
    sCh.Write(ATA_LBAH, lba >> 16);
    sCh.Write(ATA_DEVICE, (lba_mode ? 0xe0 : 0xa0) | sUnit << 4 |
                          ((lba >> 24) & 0x0f));
    sCh.Write(ATA_COMMAND, cmd);
    return sCh.WaitNotBusy();
}

bool identify(uint8_t *id)
{
    uint8_t st = issue(CMD_IDENTIFY);
    if (st == 0xff || !(st & ATA_ST_DRQ))
        return false;
    for (int i = 0; i < 256; i++)
        put_le16(id + 2 * i, sCh.ReadData());
    return true;
}

/* One DRQ block of 'n' sectors in or out. */
void pio_in(uint8_t *buf, int n)
{
    for (int i = 0; i < n * SECTOR / 2; i++)
        put_le16(buf + 2 * i, sCh.ReadData());
}

void pio_out(const uint8_t *buf, int n)
{
    for (int i = 0; i < n * SECTOR / 2; i++)
        sCh.WriteData(le16(buf + 2 * i));
}

/* A PIO read of 'count' sectors (0 for 256), 'per_block' to a DRQ block:
   the final status. */
uint8_t pio_read(uint8_t cmd, uint32_t lba, int count, uint8_t *buf,
                 int per_block = 1)
{
    uint8_t st = issue(cmd, lba, count);
    int left = count ? count : 256;
    while (left > 0) {
        if (st == 0xff || !(st & ATA_ST_DRQ) || (st & ATA_ST_ERR))
            return st;
        int n = left < per_block ? left : per_block;
        pio_in(buf, n);
        buf += n * SECTOR;
        left -= n;
        st = sCh.WaitNotBusy();
    }
    return st;
}

uint8_t pio_write(uint8_t cmd, uint32_t lba, int count, const uint8_t *buf,
                  int per_block = 1)
{
    uint8_t st = issue(cmd, lba, count);
    int left = count ? count : 256;
    while (left > 0) {
        if (st == 0xff || !(st & ATA_ST_DRQ) || (st & ATA_ST_ERR))
            return st;
        int n = left < per_block ? left : per_block;
        pio_out(buf, n);
        buf += n * SECTOR;
        left -= n;
        st = sCh.WaitNotBusy();
    }
    return st;
}

uint32_t pattern_errors(const uint8_t *buf, uint32_t lba, uint32_t n)
{
    uint32_t bad = 0;
    for (uint32_t s = 0; s < n; s++) {
        for (uint32_t i = 0; i < SECTOR; i++)
            bad += buf[s * SECTOR + i] != TEST_BLOCK_PATTERN(lba + s, i);
    }
    return bad;
}

/* Whether a write of a sector as it is, which changes nothing, is
   refused. */
bool refuses_writes()
{
    uint8_t *buf = (uint8_t *)alloc(SECTOR);
    pio_read(CMD_READ, 0, 1, buf);
    uint8_t st = pio_write(CMD_WRITE, 0, 1, buf);
    return (st & ATA_ST_ERR) != 0;
}

/* The first disk on a test disk that takes writes, or refuses them. */
void need_disk(bool read_only = false)
{
    t_on_cleanup(ata_cleanup);
    AtaChannel ch;
    for (int i = 0; ata_channel(i, &ch); i++) {
        for (int unit = 0; unit < 2; unit++) {
            if (ata_probe(ch, unit) != ATA_DISK)
                continue;
            sCh = ch;
            sUnit = unit;
            uint8_t *id = (uint8_t *)alloc(SECTOR);
            sCh.Select(unit);
            if (!identify(id))
                continue;
            sSectors = word(id, 60) | (uint32_t)word(id, 61) << 16;
            if (tc_block_find(sSectors) < 0)
                continue;
            if (refuses_writes() != read_only)
                continue;
            tc_block_find(sSectors);
            /* the drive's interrupt on, which the bus master latches */
            sCh.Control(0);
            return;
        }
    }
    t_skip("no %s ATA disk on a test disk",
           read_only ? "read only" : "writable");
}

/* A PRD table of the given pieces; the last ends the table. */
uint32_t make_prd(void **bufs, const uint32_t *lens, int n)
{
    uint8_t *prd = (uint8_t *)alloc(8 * n, 0, 64);
    for (int i = 0; i < n; i++) {
        put_le32(prd + 8 * i, (uint32_t)(uintptr_t)bufs[i]);
        put_le16(prd + 8 * i + 4, (uint16_t)lens[i]);
        put_le16(prd + 8 * i + 6, i == n - 1 ? 0x8000 : 0);
    }
    return (uint32_t)(uintptr_t)prd;
}

/* A DMA command through the bus master, the engine started after the
   command: the device's status, and the bus master's in 'bm'. */
uint8_t dma(uint8_t cmd, uint32_t lba, uint8_t count, uint32_t prd,
            uint8_t *bm)
{
    bool to_memory = cmd == CMD_READ_DMA;
    sCh.BmWrite(BM_COMMAND, 0);
    sCh.BmWrite(BM_STATUS, BM_ERROR | BM_IRQ);
    sCh.BmWritePrd(prd);
    sCh.BmWrite(BM_COMMAND, to_memory ? BM_READ : 0);
    sCh.Write(ATA_NSECT, count);
    sCh.Write(ATA_LBAL, lba);
    sCh.Write(ATA_LBAM, lba >> 8);
    sCh.Write(ATA_LBAH, lba >> 16);
    sCh.Write(ATA_DEVICE, 0xe0 | sUnit << 4 | ((lba >> 24) & 0x0f));
    sCh.Write(ATA_COMMAND, cmd);
    sCh.BmWrite(BM_COMMAND, (to_memory ? BM_READ : 0) | BM_START);
    t_wait_until([] {return (sCh.BmRead(BM_STATUS) & BM_IRQ) != 0;});
    *bm = sCh.BmRead(BM_STATUS);
    sCh.BmWrite(BM_COMMAND, to_memory ? BM_READ : 0);
    return sCh.WaitNotBusy();
}

}


/* The signature a software reset leaves a disk (ATA/ATAPI-6, 9.12): a
   sector count and LBA low of 1, LBA mid and high of 0, and diagnostics
   passed in the error register. */
TEST(ata_disk, signature)
{
    need_disk();
    ata_soft_reset(sCh);
    sCh.Write(ATA_DEVICE, 0xa0 | sUnit << 4);
    uint8_t st = sCh.WaitNotBusy();
    EXPECT_EQ(st & (ATA_ST_BSY | ATA_ST_DRDY | ATA_ST_ERR | ATA_ST_DRQ),
              ATA_ST_DRDY);
    EXPECT_EQ(sCh.Read(ATA_NSECT), 1);
    EXPECT_EQ(sCh.Read(ATA_LBAL), 1);
    EXPECT_EQ(sCh.Read(ATA_LBAM), 0);
    EXPECT_EQ(sCh.Read(ATA_LBAH), 0);
    EXPECT_EQ(sCh.Read(ATA_ERROR) & 0x7f, 0x01);
    SNAP("status", "%#x", st);
}

/* IDENTIFY DEVICE (ATA/ATAPI-6, 8.15): a fixed ATA device, the strings
   in ASCII, the size in words 60 and 61, the words that say they are
   valid, and the reserved bits of words 83, 84 and 87. */
TEST(ata_disk, identify)
{
    need_disk();
    uint8_t *id = (uint8_t *)alloc(SECTOR);
    ASSERT_TRUE(identify(id));
    EXPECT_EQ(word(id, 0) & 0x8000, 0);
    /* the serial number, firmware revision and model */
    for (int i = 20; i < 94; i++) {
        if (i >= 40 && i < 46)
            continue;
        EXPECT_TRUE(id[i] >= 0x20 && id[i] < 0x7f);
    }
    EXPECT_EQ(word(id, 47) & 0xff00, 0x8000);
    EXPECT_EQ(word(id, 49) & 0x0300, 0x0300);
    EXPECT_EQ(word(id, 53) & 0x0006, 0x0006);
    EXPECT_EQ(sSectors, tc_block_read(TEST_CONTROL_BLOCK_SECTORS));
    EXPECT_EQ(word(id, 83) & 0xc000, 0x4000);
    EXPECT_EQ(word(id, 84) & 0xc000, 0x4000);
    EXPECT_EQ(word(id, 87) & 0xc000, 0x4000);
    EXPECT_EQ(word(id, 93) & 0xc000, 0x4000);
    char model[41] = {};
    for (int i = 0; i < 40; i++)
        model[i] = id[54 + (i ^ 1)];
    SNAP("model", "%s", model);
    SNAP("geometry", "%u/%u/%u", word(id, 1), word(id, 3), word(id, 6));
    SNAP("versions", "%#x %#x", word(id, 80), word(id, 81));
    SNAP("features", "%#x %#x %#x %#x %#x %#x", word(id, 82), word(id, 83),
         word(id, 84), word(id, 85), word(id, 86), word(id, 87));
    SNAP("modes", "pio %#x mwdma %#x udma %#x", word(id, 64), word(id, 63),
         word(id, 88));
    SNAP("hw_reset", "%#x", word(id, 93));
    /* the data port gives nothing more once the block is read */
    SNAP("past_end", "%#x", sCh.ReadData());
}

/* READ SECTOR(S): a DRQ block a sector, a count of 0 meaning 256, and the
   LBA registers left at the sector after the last (ATA/ATAPI-6, 8.27). */
TEST(ata_disk, pio_read)
{
    need_disk();
    uint8_t *buf = (uint8_t *)alloc(256 * SECTOR, 0xee);
    uint8_t st = pio_read(CMD_READ, 5, 3, buf);
    EXPECT_EQ(st & (ATA_ST_ERR | ATA_ST_DRQ), 0);
    EXPECT_EQ(pattern_errors(buf, 5, 3), 0u);
    SNAP("lba_after", "%u", sCh.Read(ATA_LBAL));
    st = pio_read(CMD_READ, 300, 0, buf);
    EXPECT_EQ(st & (ATA_ST_ERR | ATA_ST_DRQ), 0);
    EXPECT_EQ(pattern_errors(buf, 300, 256), 0u);
    /* and with 32 bit accesses to the data port */
    issue(CMD_READ, 7, 1);
    for (int i = 0; i < SECTOR / 4; i++)
        put_le32(buf + 4 * i, sCh.ReadData32());
    EXPECT_EQ(pattern_errors(buf, 7, 1), 0u);
    EXPECT_EQ(sCh.WaitNotBusy() & ATA_ST_DRQ, 0);
}

/* WRITE SECTOR(S) stores what reads back. */
TEST(ata_disk, pio_write)
{
    need_disk();
    uint8_t *w = (uint8_t *)alloc(3 * SECTOR);
    for (int i = 0; i < 3 * SECTOR; i++)
        w[i] = i * 11 + 3;
    uint8_t st = pio_write(CMD_WRITE, 50, 3, w);
    EXPECT_EQ(st & (ATA_ST_ERR | ATA_ST_DRQ), 0);
    uint8_t *r = (uint8_t *)alloc(5 * SECTOR);
    pio_read(CMD_READ, 49, 5, r);
    EXPECT_EQ(pattern_errors(r, 49, 1), 0u);
    EXPECT_EQ(memcmp(r + SECTOR, w, 3 * SECTOR), 0);
    EXPECT_EQ(pattern_errors(r + 4 * SECTOR, 53, 1), 0u);
    /* 32 bit accesses */
    issue(CMD_WRITE, 60, 1);
    for (int i = 0; i < SECTOR / 4; i++)
        sCh.WriteData32(le32(w + 4 * i));
    EXPECT_EQ(sCh.WaitNotBusy() & (ATA_ST_ERR | ATA_ST_DRQ), 0);
    pio_read(CMD_READ, 60, 1, r);
    EXPECT_EQ(memcmp(r, w, SECTOR), 0);
}

/* SET MULTIPLE MODE, READ and WRITE MULTIPLE (ATA/ATAPI-6, 8.37, 8.29,
   8.48): DRQ blocks of the count set, and the last of what is left; word
   59 reports it. Without it the commands abort, and a count the disk
   cannot do is refused. */
TEST(ata_disk, multiple)
{
    need_disk();
    uint8_t *buf = (uint8_t *)alloc(10 * SECTOR, 0xee);
    uint8_t st = pio_read(CMD_READ_MULT, 10, 10, buf, 4);
    EXPECT_EQ(st & ATA_ST_ERR, ATA_ST_ERR);
    EXPECT_EQ(sCh.Read(ATA_ERROR) & ATA_ER_ABRT, ATA_ER_ABRT);
    st = issue(CMD_SET_MULT, 0, 4);
    EXPECT_EQ(st & ATA_ST_ERR, 0);
    uint8_t *id = (uint8_t *)alloc(SECTOR);
    identify(id);
    EXPECT_EQ(word(id, 59), 0x104);
    st = pio_read(CMD_READ_MULT, 10, 10, buf, 4);
    EXPECT_EQ(st & (ATA_ST_ERR | ATA_ST_DRQ), 0);
    EXPECT_EQ(pattern_errors(buf, 10, 10), 0u);
    for (int i = 0; i < 6 * SECTOR; i++)
        buf[i] = i * 5;
    st = pio_write(CMD_WRITE_MULT, 70, 6, buf, 4);
    EXPECT_EQ(st & (ATA_ST_ERR | ATA_ST_DRQ), 0);
    uint8_t *r = (uint8_t *)alloc(6 * SECTOR);
    pio_read(CMD_READ, 70, 6, r);
    EXPECT_EQ(memcmp(r, buf, 6 * SECTOR), 0);
    st = issue(CMD_SET_MULT, 0, 3);
    EXPECT_EQ(st & ATA_ST_ERR, ATA_ST_ERR);
    st = issue(CMD_SET_MULT, 0, 0);
    SNAP("set_mult_0", "%#x", st);
}

/* CHS addressing, by the geometry IDENTIFY gives: cylinder, head and a
   sector numbered from 1. */
TEST(ata_disk, chs)
{
    need_disk();
    uint8_t *id = (uint8_t *)alloc(SECTOR);
    identify(id);
    uint32_t heads = word(id, 3), spt = word(id, 6);
    uint8_t *buf = (uint8_t *)alloc(SECTOR, 0xee);
    /* cylinder 1, head 2, sector 3 */
    sCh.Write(ATA_NSECT, 1);
    sCh.Write(ATA_LBAL, 3);
    sCh.Write(ATA_LBAM, 1);
    sCh.Write(ATA_LBAH, 0);
    sCh.Write(ATA_DEVICE, 0xa0 | sUnit << 4 | 2);
    sCh.Write(ATA_COMMAND, CMD_READ);
    uint8_t st = sCh.WaitNotBusy();
    ASSERT_EQ(st & ATA_ST_DRQ, ATA_ST_DRQ);
    pio_in(buf, 1);
    EXPECT_EQ(pattern_errors(buf, (1 * heads + 2) * spt + 2, 1), 0u);
}

/* A sector past the end is not found: IDNF (ATA/ATAPI-6, 8.27). */
TEST(ata_disk, out_of_range)
{
    need_disk();
    uint8_t *buf = (uint8_t *)alloc(2 * SECTOR);
    uint8_t st = pio_read(CMD_READ, sSectors, 1, buf);
    EXPECT_EQ(st & ATA_ST_ERR, ATA_ST_ERR);
    EXPECT_EQ(sCh.Read(ATA_ERROR), ATA_ER_IDNF);
    st = pio_read(CMD_READ, sSectors - 1, 2, buf);
    EXPECT_EQ(sCh.Read(ATA_ERROR), ATA_ER_IDNF);
    st = issue(CMD_VERIFY, sSectors - 1, 2);
    EXPECT_EQ(sCh.Read(ATA_ERROR), ATA_ER_IDNF);
    st = issue(CMD_VERIFY, sSectors - 2, 2);
    EXPECT_EQ(st & ATA_ST_ERR, 0);
}

/* A read only disk aborts writes. */
TEST(ata_disk, read_only)
{
    need_disk(true);
    uint8_t *buf = (uint8_t *)alloc(SECTOR);
    uint8_t st = pio_write(CMD_WRITE, 3, 1, buf);
    EXPECT_EQ(st & ATA_ST_ERR, ATA_ST_ERR);
    EXPECT_EQ(sCh.Read(ATA_ERROR), ATA_ER_ABRT);
    pio_read(CMD_READ, 3, 1, buf);
    EXPECT_EQ(pattern_errors(buf, 3, 1), 0u);
}

/* The disk failing a read is uncorrectable data, UNC; failing a write is
   an aborted command, for which no other bit is defined (ATA/ATAPI-6,
   8.27, 8.62). */
TEST(ata_disk, media_errors)
{
    need_disk();
    uint8_t *buf = (uint8_t *)alloc(SECTOR);
    tc_block_write(TEST_CONTROL_BLOCK_FAIL, 1);
    uint8_t st = pio_read(CMD_READ, 4, 1, buf);
    EXPECT_EQ(st & ATA_ST_ERR, ATA_ST_ERR);
    EXPECT_EQ(sCh.Read(ATA_ERROR), ATA_ER_UNC);
    tc_block_write(TEST_CONTROL_BLOCK_FAIL, 1);
    st = pio_write(CMD_WRITE, 4, 1, buf);
    EXPECT_EQ(st & ATA_ST_ERR, ATA_ST_ERR);
    EXPECT_EQ(sCh.Read(ATA_ERROR), ATA_ER_ABRT);
    if (sCh.bm_bar < 0)
        return;
    void *bufs[1] = {buf};
    uint32_t lens[1] = {SECTOR};
    uint32_t prd = make_prd(bufs, lens, 1);
    uint8_t bm;
    tc_block_write(TEST_CONTROL_BLOCK_FAIL, 1);
    st = dma(CMD_READ_DMA, 4, 1, prd, &bm);
    EXPECT_EQ(st & ATA_ST_ERR, ATA_ST_ERR);
    EXPECT_EQ(sCh.Read(ATA_ERROR), ATA_ER_UNC);
    tc_block_write(TEST_CONTROL_BLOCK_FAIL, 1);
    st = dma(CMD_WRITE_DMA, 4, 1, prd, &bm);
    EXPECT_EQ(sCh.Read(ATA_ERROR), ATA_ER_ABRT);
}

/* READ and WRITE DMA through the bus master (SFF-8038i): the data goes
   where the PRD table says, and at the end the interrupt bit is set and
   the engine is no longer active. */
TEST(ata_disk, dma)
{
    need_disk();
    if (sCh.bm_bar < 0)
        t_skip("no bus master");
    uint8_t *a = (uint8_t *)alloc(3 * SECTOR, 0xee);
    uint8_t *b = (uint8_t *)alloc(SECTOR, 0xee);
    void *bufs[2] = {a, b};
    uint32_t lens[2] = {3 * SECTOR, SECTOR};
    uint32_t prd = make_prd(bufs, lens, 2);
    uint8_t bm;
    uint8_t st = dma(CMD_READ_DMA, 20, 4, prd, &bm);
    EXPECT_EQ(st & (ATA_ST_ERR | ATA_ST_DRQ), 0);
    EXPECT_EQ(bm & (BM_ACTIVE | BM_ERROR | BM_IRQ), BM_IRQ);
    EXPECT_EQ(pattern_errors(a, 20, 3), 0u);
    EXPECT_EQ(pattern_errors(b, 23, 1), 0u);

    for (int i = 0; i < 3 * SECTOR; i++)
        a[i] = i * 7;
    for (int i = 0; i < SECTOR; i++)
        b[i] = i * 13;
    st = dma(CMD_WRITE_DMA, 80, 4, prd, &bm);
    EXPECT_EQ(st & (ATA_ST_ERR | ATA_ST_DRQ), 0);
    EXPECT_EQ(bm & (BM_ACTIVE | BM_ERROR | BM_IRQ), BM_IRQ);
    uint8_t *r = (uint8_t *)alloc(4 * SECTOR);
    pio_read(CMD_READ, 80, 4, r);
    EXPECT_EQ(memcmp(r, a, 3 * SECTOR), 0);
    EXPECT_EQ(memcmp(r + 3 * SECTOR, b, SECTOR), 0);
}

/* A PRD table longer than the transfer leaves the engine active with the
   interrupt set, which SFF-8038i gives as a normal end; a shorter one
   cannot finish the command. The engine may also be started before the
   command. */
TEST(ata_disk, dma_prd_length)
{
    need_disk();
    if (sCh.bm_bar < 0)
        t_skip("no bus master");
    uint8_t *big = (uint8_t *)alloc(4 * SECTOR, 0xee);
    void *bufs[1] = {big};
    uint32_t lens[1] = {4 * SECTOR};
    uint32_t prd = make_prd(bufs, lens, 1);
    uint8_t bm;
    uint8_t st = dma(CMD_READ_DMA, 30, 2, prd, &bm);
    EXPECT_EQ(st & ATA_ST_ERR, 0);
    EXPECT_EQ(bm & (BM_ACTIVE | BM_IRQ), BM_ACTIVE | BM_IRQ);
    EXPECT_EQ(pattern_errors(big, 30, 2), 0u);
    EXPECT_EQ(big[2 * SECTOR], 0xee);

    lens[0] = SECTOR;
    prd = make_prd(bufs, lens, 1);
    st = dma(CMD_READ_DMA, 30, 2, prd, &bm);
    SNAP("short_prd", "status %#x error %#x bm %#x", st, sCh.Read(ATA_ERROR),
         bm);

    /* the engine first */
    lens[0] = 2 * SECTOR;
    prd = make_prd(bufs, lens, 1);
    memset(big, 0xee, 4 * SECTOR);
    sCh.BmWrite(BM_STATUS, BM_ERROR | BM_IRQ);
    sCh.BmWritePrd(prd);
    sCh.BmWrite(BM_COMMAND, BM_READ | BM_START);
    EXPECT_EQ(sCh.BmRead(BM_STATUS) & BM_ACTIVE, BM_ACTIVE);
    issue(CMD_READ_DMA, 40, 2);
    EXPECT_TRUE(t_wait_until([] {
        return (sCh.BmRead(BM_STATUS) & BM_IRQ) != 0;
    }));
    sCh.BmWrite(BM_COMMAND, BM_READ);
    EXPECT_EQ(pattern_errors(big, 40, 2), 0u);
}

/* The interrupt: raised for each DRQ block of a PIO read and at the end
   of a command, taken back by reading the status register but not the
   alternate status, and held down by nIEN (ATA/ATAPI-6, 5.2 and 9). */
TEST(ata_disk, interrupts)
{
    need_disk();
    static volatile int count;
    count = 0;
    if (!sCh.Route([](void *) {
            count = count + 1;
            sCh.Read(ATA_STATUS);
        }, nullptr))
        t_skip("the interrupt reaches no controller the suite drives");
    t_on_cleanup([] {sCh.Unroute();});
    sCh.Control(0);
    uint8_t *buf = (uint8_t *)alloc(2 * SECTOR);
    arch_irq_enable();
    sCh.Write(ATA_NSECT, 2);
    sCh.Write(ATA_LBAL, 1);
    sCh.Write(ATA_LBAM, 0);
    sCh.Write(ATA_LBAH, 0);
    sCh.Write(ATA_DEVICE, 0xe0 | sUnit << 4);
    sCh.Write(ATA_COMMAND, CMD_READ);
    EXPECT_TRUE(t_wait_until([] {return count >= 1;}));
    pio_in(buf, 1);
    EXPECT_TRUE(t_wait_until([] {return count >= 2;}));
    pio_in(buf + SECTOR, 1);
    t_delay_us(2000);
    EXPECT_EQ(count, 2);
    arch_irq_disable();

    count = 0;
    sCh.Control(ATA_CTL_NIEN);
    arch_irq_enable();
    issue(CMD_CHECK_POWER);
    t_delay_us(2000);
    arch_irq_disable();
    EXPECT_EQ(count, 0);
}

/* The other commands: SET FEATURES selects a transfer mode IDENTIFY then
   reports, and refuses one the disk lacks; CHECK POWER MODE says active;
   READ NATIVE MAX ADDRESS gives the last sector; FLUSH CACHE and EXECUTE
   DEVICE DIAGNOSTIC succeed; NOP and unknown commands abort. */
TEST(ata_disk, other_commands)
{
    need_disk();
    uint8_t *id = (uint8_t *)alloc(SECTOR);
    uint8_t st = issue(CMD_SET_FEATURES, 0, 0x45, 0x03);
    EXPECT_EQ(st & ATA_ST_ERR, 0);
    identify(id);
    EXPECT_EQ(word(id, 88) & 0xff00, 0x2000);
    EXPECT_EQ(word(id, 63) & 0xff00, 0);
    st = issue(CMD_SET_FEATURES, 0, 0x22, 0x03);
    identify(id);
    EXPECT_EQ(word(id, 63) & 0xff00, 0x0400);
    EXPECT_EQ(word(id, 88) & 0xff00, 0);
    st = issue(CMD_SET_FEATURES, 0, 0x47, 0x03);
    EXPECT_EQ(st & ATA_ST_ERR, ATA_ST_ERR);
    st = issue(CMD_SET_FEATURES, 0, 0, 0x02);
    SNAP("write_cache_on", "%#x", st);

    st = issue(CMD_CHECK_POWER);
    EXPECT_EQ(st & ATA_ST_ERR, 0);
    EXPECT_EQ(sCh.Read(ATA_NSECT), 0xff);
    st = issue(CMD_NATIVE_MAX);
    EXPECT_EQ(st & ATA_ST_ERR, 0);
    uint32_t max = sCh.Read(ATA_LBAL) | sCh.Read(ATA_LBAM) << 8 |
                   sCh.Read(ATA_LBAH) << 16 |
                   (sCh.Read(ATA_DEVICE) & 0x0f) << 24;
    EXPECT_EQ(max, sSectors - 1);
    EXPECT_EQ(issue(CMD_FLUSH) & ATA_ST_ERR, 0);
    st = issue(CMD_DIAGNOSE);
    EXPECT_EQ(sCh.Read(ATA_ERROR) & 0x7f, 0x01);
    st = issue(0x00);
    EXPECT_EQ(st & ATA_ST_ERR, ATA_ST_ERR);
    EXPECT_EQ(sCh.Read(ATA_ERROR), ATA_ER_ABRT);
    st = issue(0xfe);
    EXPECT_EQ(sCh.Read(ATA_ERROR), ATA_ER_ABRT);
    static const uint8_t accepted[] = {0x10, 0x70, 0x91, 0xe0, 0xe1, 0xe2,
                                       0xe3, 0x41};
    char s[64];
    size_t p = 0;
    for (uint8_t c : accepted)
        p += snprintf(s + p, sizeof(s) - p, "%02x:%02x ", c,
                      issue(c, 0, 1) & 0xff);
    SNAP("accepted", "%s", s);
}
