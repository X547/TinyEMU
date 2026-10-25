/*
 * ATAPI tests
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

/* A packet device as ATA/ATAPI-6 has it: its signature, IDENTIFY PACKET
   DEVICE, and the PACKET command's protocol, by PIO in blocks the byte
   count limit bounds and by DMA, with the interrupt reason in the sector
   count register: CoD in bit 0, IO in bit 1. */

#define CMD_DEVICE_RESET    0x08
#define CMD_PACKET          0xa0
#define CMD_IDENTIFY_PACKET 0xa1
#define CMD_IDENTIFY        0xec

#define IR_COD 0x01
#define IR_IO  0x02

#define LIMIT_4G 0x100000000ull

namespace {

AtaChannel sCh;
int sUnit;

void *alloc(size_t size, uint8_t fill = 0, size_t align = 16)
{
    void *p = heap_alloc(size ? size : 1, align, LIMIT_4G);
    if (p == nullptr)
        t_skip("no memory");
    memset(p, fill, size);
    return p;
}

uint16_t word(const uint8_t *id, int i) {return le16(id + 2 * i);}

/* The first packet device, or the first without a disc in it. */
void need_packet(bool disc = true)
{
    t_on_cleanup(ata_cleanup);
    AtaChannel ch;
    for (int i = 0; ata_channel(i, &ch); i++) {
        for (int unit = 0; unit < 2; unit++) {
            if (ata_probe(ch, unit) != ATA_PACKET)
                continue;
            sCh = ch;
            sUnit = unit;
            sCh.Control(ATA_CTL_NIEN);
            /* READ CAPACITY says whether there is a disc */
            uint8_t cap[12] = {0x25};
            uint8_t *buf = (uint8_t *)alloc(8);
            sCh.Write(ATA_DEVICE, 0xa0 | unit << 4);
            sCh.WaitNotBusy();
            sCh.Write(ATA_FEATURE, 0);
            sCh.Write(ATA_LBAM, 8);
            sCh.Write(ATA_LBAH, 0);
            sCh.Write(ATA_COMMAND, CMD_PACKET);
            sCh.WaitNotBusy();
            for (int k = 0; k < 6; k++)
                sCh.WriteData(cap[2 * k] | cap[2 * k + 1] << 8);
            uint8_t st = sCh.WaitNotBusy();
            bool has = false;
            if (st & ATA_ST_DRQ) {
                for (int k = 0; k < 4; k++)
                    put_le16(buf + 2 * k, sCh.ReadData());
                st = sCh.WaitNotBusy();
                has = !(st & ATA_ST_ERR) &&
                      tc_block_find((be32(buf) + 1) * 4) >= 0;
            }
            if (has == disc) {
                if (disc)
                    tc_block_find((be32(buf) + 1) * 4);
                return;
            }
        }
    }
    t_skip("no packet device %s a disc on a test disk",
           disc ? "with" : "without");
}

/* PACKET up to the packet being taken: the status, with the interrupt
   reason of the phase in 'ir'. */
uint8_t send_packet(const uint8_t *cdb, uint16_t limit, uint8_t features,
                    uint8_t *ir)
{
    sCh.Write(ATA_DEVICE, 0xa0 | sUnit << 4);
    sCh.WaitNotBusy();
    sCh.Write(ATA_FEATURE, features);
    sCh.Write(ATA_LBAM, limit);
    sCh.Write(ATA_LBAH, limit >> 8);
    sCh.Write(ATA_COMMAND, CMD_PACKET);
    uint8_t st = sCh.WaitNotBusy();
    *ir = sCh.Read(ATA_NSECT) & 3;
    if (!(st & ATA_ST_DRQ))
        return st;
    for (int k = 0; k < 6; k++)
        sCh.WriteData(cdb[2 * k] | cdb[2 * k + 1] << 8);
    return sCh.WaitNotBusy();
}

}


/* The signature of a packet device: LBA mid 14h and high EBh (ATA/ATAPI-6,
   9.12). IDENTIFY DEVICE is aborted, and leaves it there. */
TEST(atapi, signature)
{
    need_packet();
    ata_soft_reset(sCh);
    sCh.Write(ATA_DEVICE, 0xa0 | sUnit << 4);
    uint8_t st = sCh.WaitNotBusy();
    EXPECT_EQ(sCh.Read(ATA_NSECT), 1);
    EXPECT_EQ(sCh.Read(ATA_LBAL), 1);
    EXPECT_EQ(sCh.Read(ATA_LBAM), 0x14);
    EXPECT_EQ(sCh.Read(ATA_LBAH), 0xeb);
    SNAP("status_after_reset", "%#x", st);
    sCh.Write(ATA_COMMAND, CMD_IDENTIFY);
    st = sCh.WaitNotBusy();
    EXPECT_EQ(st & ATA_ST_ERR, ATA_ST_ERR);
    EXPECT_EQ(sCh.Read(ATA_ERROR) & 0x04, 0x04);
    EXPECT_EQ(sCh.Read(ATA_LBAM), 0x14);
    EXPECT_EQ(sCh.Read(ATA_LBAH), 0xeb);
}

/* IDENTIFY PACKET DEVICE: an ATAPI device (word 0 bits 15:14 of 10b), a
   CD-ROM, removable, with 12 byte packets; the strings in ASCII. */
TEST(atapi, identify_packet)
{
    need_packet();
    sCh.Write(ATA_DEVICE, 0xa0 | sUnit << 4);
    sCh.WaitNotBusy();
    sCh.Write(ATA_COMMAND, CMD_IDENTIFY_PACKET);
    uint8_t st = sCh.WaitNotBusy();
    ASSERT_EQ(st & ATA_ST_DRQ, ATA_ST_DRQ);
    uint8_t *id = (uint8_t *)alloc(512);
    for (int i = 0; i < 256; i++)
        put_le16(id + 2 * i, sCh.ReadData());
    EXPECT_EQ(word(id, 0) & 0xc000, 0x8000);
    EXPECT_EQ((word(id, 0) >> 8) & 0x1f, 0x05);
    EXPECT_EQ(word(id, 0) & 0x0080, 0x0080);
    EXPECT_EQ(word(id, 0) & 0x0003, 0);
    for (int i = 20; i < 94; i++) {
        if (i >= 40 && i < 46)
            continue;
        EXPECT_TRUE(id[i] >= 0x20 && id[i] < 0x7f);
    }
    EXPECT_EQ(word(id, 83) & 0xc000, 0x4000);
    SNAP("word0", "%#x", word(id, 0));
    SNAP("features", "%#x %#x %#x", word(id, 49), word(id, 82),
         word(id, 85));
    SNAP("modes", "%#x %#x %#x", word(id, 63), word(id, 64), word(id, 88));
}

/* A PIO data in command: the device asks for the packet with CoD set and
   no interrupt, moves the data in blocks no larger than the byte count
   limit, each with IO set and its length in the cylinder registers, and
   ends with IO and CoD both set. */
TEST(atapi, pio_data_in)
{
    need_packet();
    uint8_t ir;
    uint8_t inquiry[12] = {0x12, 0, 0, 0, 36, 0};
    sCh.Write(ATA_DEVICE, 0xa0 | sUnit << 4);
    sCh.WaitNotBusy();
    sCh.Write(ATA_FEATURE, 0);
    sCh.Write(ATA_LBAM, 16);
    sCh.Write(ATA_LBAH, 0);
    sCh.Write(ATA_COMMAND, CMD_PACKET);
    uint8_t st = sCh.WaitNotBusy();
    EXPECT_EQ(st & ATA_ST_DRQ, ATA_ST_DRQ);
    EXPECT_EQ(sCh.Read(ATA_NSECT) & 3, IR_COD);
    for (int k = 0; k < 6; k++)
        sCh.WriteData(inquiry[2 * k] | inquiry[2 * k + 1] << 8);
    uint8_t data[40] = {};
    uint32_t got = 0;
    char blocks[32];
    size_t p = 0;
    for (;;) {
        st = sCh.WaitNotBusy();
        ir = sCh.Read(ATA_NSECT) & 3;
        if (!(st & ATA_ST_DRQ))
            break;
        EXPECT_EQ(ir, IR_IO);
        uint32_t n = sCh.Read(ATA_LBAM) | sCh.Read(ATA_LBAH) << 8;
        EXPECT_LE(n, 16u);
        p += snprintf(blocks + p, sizeof(blocks) - p, "%u ", n);
        for (uint32_t i = 0; i < n; i += 2) {
            uint16_t w = sCh.ReadData();
            if (got + i + 1 < sizeof(data)) {
                data[got + i] = w;
                data[got + i + 1] = w >> 8;
            }
        }
        got += n;
    }
    EXPECT_EQ(st & ATA_ST_ERR, 0);
    EXPECT_EQ(ir, IR_IO | IR_COD);
    EXPECT_EQ(got, 36u);
    EXPECT_EQ(data[0], 0x05);
    SNAP("blocks", "%s", blocks);
}

/* Data out by PIO, here a MODE SELECT parameter list: the blocks have IO
   clear. */
TEST(atapi, pio_data_out)
{
    need_packet();
    uint8_t ir;
    uint8_t select[12] = {0x55, 0x10, 0, 0, 0, 0, 0, 0, 8, 0};
    uint8_t st = send_packet(select, 0xfffe, 0, &ir);
    ASSERT_EQ(st & ATA_ST_DRQ, ATA_ST_DRQ);
    ir = sCh.Read(ATA_NSECT) & 3;
    EXPECT_EQ(ir, 0);
    EXPECT_EQ(sCh.Read(ATA_LBAM), 8);
    for (int i = 0; i < 4; i++)
        sCh.WriteData(0);
    st = sCh.WaitNotBusy();
    EXPECT_EQ(st & (ATA_ST_DRQ | ATA_ST_ERR), 0);
    EXPECT_EQ(sCh.Read(ATA_NSECT) & 3, IR_IO | IR_COD);
}

/* A command that fails ends with ERR and the sense key in the top of the
   error register; REQUEST SENSE has the rest. */
TEST(atapi, check_condition)
{
    need_packet(false);
    uint8_t ir;
    uint8_t tur[12] = {0x00};
    uint8_t st = send_packet(tur, 0xfffe, 0, &ir);
    EXPECT_EQ(st & ATA_ST_ERR, ATA_ST_ERR);
    EXPECT_EQ(sCh.Read(ATA_ERROR) >> 4, 0x2);
    EXPECT_EQ(sCh.Read(ATA_NSECT) & 3, IR_IO | IR_COD);
    uint8_t rs[12] = {0x03, 0, 0, 0, 18, 0};
    st = send_packet(rs, 0xfffe, 0, &ir);
    ASSERT_EQ(st & ATA_ST_DRQ, ATA_ST_DRQ);
    uint8_t sense[18];
    for (int i = 0; i < 9; i++)
        put_le16(sense + 2 * i, sCh.ReadData());
    EXPECT_EQ(sense[2] & 0x0f, 0x2);
    EXPECT_EQ(sense[12], 0x3a);
}

/* With the DMA bit of the features the data goes through the bus
   master. */
TEST(atapi, dma)
{
    need_packet();
    if (sCh.bm_bar < 0)
        t_skip("no bus master");
    uint8_t *buf = (uint8_t *)alloc(4096, 0xee);
    uint8_t *prd = (uint8_t *)alloc(8, 0, 64);
    put_le32(prd, (uint32_t)(uintptr_t)buf);
    put_le16(prd + 4, 4096);
    put_le16(prd + 6, 0x8000);
    sCh.Control(0);
    sCh.BmWrite(BM_COMMAND, 0);
    sCh.BmWrite(BM_STATUS, BM_ERROR | BM_IRQ);
    sCh.BmWritePrd((uint32_t)(uintptr_t)prd);
    sCh.BmWrite(BM_COMMAND, BM_READ);
    uint8_t ir;
    uint8_t rd[12] = {0x28, 0, 0, 0, 0, 3, 0, 0, 2, 0};
    send_packet(rd, 0xfffe, 0x01, &ir);
    sCh.BmWrite(BM_COMMAND, BM_READ | BM_START);
    EXPECT_TRUE(t_wait_until([] {
        return (sCh.BmRead(BM_STATUS) & BM_IRQ) != 0;
    }));
    sCh.BmWrite(BM_COMMAND, BM_READ);
    uint8_t st = sCh.WaitNotBusy();
    EXPECT_EQ(st & (ATA_ST_ERR | ATA_ST_DRQ), 0);
    uint32_t bad = 0;
    for (uint32_t s = 0; s < 8; s++) {
        for (uint32_t i = 0; i < 512; i++)
            bad += buf[s * 512 + i] != TEST_BLOCK_PATTERN(12 + s, i);
    }
    EXPECT_EQ(bad, 0u);
}

/* DEVICE RESET is taken while a command runs, gives up the command, and
   leaves the signature. Overlapped commands are not offered: the OVL bit
   aborts. */
TEST(atapi, device_reset)
{
    need_packet();
    tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_HOLD);
    uint32_t cancels = tc_block_read(TEST_CONTROL_BLOCK_CANCELS);
    uint8_t ir;
    uint8_t rd[12] = {0x28, 0, 0, 0, 0, 1, 0, 0, 1, 0};
    sCh.Write(ATA_DEVICE, 0xa0 | sUnit << 4);
    sCh.WaitNotBusy();
    sCh.Write(ATA_FEATURE, 0);
    sCh.Write(ATA_LBAM, 0xfe);
    sCh.Write(ATA_LBAH, 0xff);
    sCh.Write(ATA_COMMAND, CMD_PACKET);
    sCh.WaitNotBusy();
    for (int k = 0; k < 6; k++)
        sCh.WriteData(rd[2 * k] | rd[2 * k + 1] << 8);
    EXPECT_TRUE(t_wait_until([] {
        return tc_block_read(TEST_CONTROL_BLOCK_HELD) == 1;
    }));
    EXPECT_EQ(sCh.AltStatus() & ATA_ST_BSY, ATA_ST_BSY);
    sCh.Write(ATA_COMMAND, CMD_DEVICE_RESET);
    uint8_t st = sCh.WaitNotBusy();
    EXPECT_NE(st, 0xff);
    EXPECT_EQ(tc_block_read(TEST_CONTROL_BLOCK_CANCELS) - cancels, 1u);
    EXPECT_EQ(sCh.Read(ATA_LBAM), 0x14);
    EXPECT_EQ(sCh.Read(ATA_LBAH), 0xeb);
    SNAP("status_after_device_reset", "%#x", st);
    tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_SYNC);

    uint8_t tur[12] = {0x00};
    st = send_packet(tur, 0xfffe, 0x02, &ir);
    EXPECT_EQ(st & ATA_ST_ERR, ATA_ST_ERR);
    EXPECT_EQ(sCh.Read(ATA_ERROR) & 0x04, 0x04);
}
