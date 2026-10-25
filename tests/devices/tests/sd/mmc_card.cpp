/*
 * eMMC device tests
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
#include "sd.h"
#include "test.h"
#include "test_control.h"
#include "testctl.h"

/* An eMMC device as JESD84-B451 (eMMC 4.51) describes it: the device
   identification mode, the state transitions, the extended CSD and SWITCH,
   SLEEP_AWAKE, on every host with one. */

#define EXT_CSD_ERASE_GROUP_DEF 175
#define EXT_CSD_PART_CONFIG     179
#define EXT_CSD_BUS_WIDTH       183
#define EXT_CSD_HS_TIMING       185
#define EXT_CSD_REV             192
#define EXT_CSD_STRUCTURE       194
#define EXT_CSD_CARD_TYPE       196
#define EXT_CSD_SEC_COUNT       212
#define EXT_CSD_S_CMD_SET       504

#define SWITCH_SET_BITS   1
#define SWITCH_CLEAR_BITS 2
#define SWITCH_WRITE      3

namespace {

template <typename F>
void each_device(F fn)
{
    SdCard card;
    int i = 0;
    for (; sd_find_card(true, i, &card); i++) {
        t_log("%s, %llu sectors", card.host->Name(),
              (unsigned long long)card.sectors);
        fn(card);
    }
    if (i == 0)
        t_skip("no eMMC device");
}

uint8_t *ext_csd(const SdCard &c)
{
    SdResult r;
    uint8_t *buf = sd_alloc(512);
    SdCmd cmd;
    cmd.index = 8;
    cmd.buf = buf;
    cmd.blksz = 512;
    cmd.blocks = 1;
    if (c.host->Command(cmd, &r) != SD_OK)
        t_fail(__FILE__, __LINE__, "SEND_EXT_CSD failed");
    return buf;
}

/* SWITCH, R1b: its response, or 0xffffffff on no answer */
uint32_t switch_cmd(const SdCard &c, int access, int index, uint8_t value)
{
    SdResult r;
    uint32_t arg = access << 24 | index << 16 | value << 8;
    if (sd_cmd(c.host, 6, arg, SD_RSP_R1B, &r) != SD_OK)
        return 0xffffffff;
    return r.resp;
}

bool read_only(const SdCard &c)
{
    return c.host->Kind() == SD_HOST_DW_MMC
               ? (static_cast<DwMmcHost *>(c.host)->R(DW_WRTPRT) & 1) != 0
               : !(static_cast<SdhciHost *>(c.host)->R32(SDHCI_PRESENT) &
                   SDHCI_PS_WRITE_ENABLED);
}

}


TEST(mmc_card, identification)
{
    each_device([](SdCard &c) {
        SdResult r;
        ASSERT_TRUE(c.host->Init());
        sd_cmd(c.host, 0, 0, SD_RSP_NONE);

        /* SD's commands go unanswered */
        EXPECT_EQ(sd_cmd(c.host, 8, 0x1aa, SD_RSP_R1), SD_TIMEOUT);
        EXPECT_EQ(sd_cmd(c.host, 55, 0, SD_RSP_R1), SD_TIMEOUT);

        /* CMD1 with no voltage is a query: busy, the access mode given */
        ASSERT_EQ(sd_cmd(c.host, 1, 0, SD_RSP_R3, &r), SD_OK);
        EXPECT_FALSE(r.resp & OCR_READY);
        EXPECT_EQ(r.resp & OCR_WINDOW, OCR_WINDOW);
        EXPECT_EQ(r.resp & (3u << 29), c.high_capacity ? OCR_SECTOR : 0u);
        ASSERT_EQ(sd_cmd(c.host, 1, OCR_WINDOW | OCR_SECTOR, SD_RSP_R3, &r),
                  SD_OK);
        EXPECT_TRUE(r.resp & OCR_READY);
        /* CMD1 once ready is illegal */
        EXPECT_EQ(sd_cmd(c.host, 1, OCR_WINDOW, SD_RSP_R3), SD_TIMEOUT);

        ASSERT_EQ(sd_cmd(c.host, 2, 0, SD_RSP_R2, &r), SD_OK);
        EXPECT_EQ(memcmp(r.reg, c.cid, 15), 0);
        SNAP("cid", "%02x %02x %02x %c%c%c%c%c%c %02x %02x%02x%02x%02x "
             "%02x", c.cid[0], c.cid[1], c.cid[2], c.cid[3], c.cid[4],
             c.cid[5], c.cid[6], c.cid[7], c.cid[8], c.cid[9], c.cid[10],
             c.cid[11], c.cid[12], c.cid[13], c.cid[14]);

        /* CMD3: the host gives the address; R1 with the state it arrived in */
        ASSERT_EQ(sd_cmd(c.host, 3, 0x1234u << 16, SD_RSP_R1, &r), SD_OK);
        EXPECT_EQ(R1_STATE(r.resp), STATE_IDENT);
        c.rca = 0x1234;
        EXPECT_EQ(R1_STATE(c.Status()), STATE_STBY);

        /* the CSD: structure 1.2, a 4.x device */
        ASSERT_EQ(sd_cmd(c.host, 9, 0x1234u << 16, SD_RSP_R2, &r), SD_OK);
        EXPECT_EQ(sd_bits(r.reg, 16, 127, 126), 3u);
        EXPECT_EQ(sd_bits(r.reg, 16, 125, 122), 4u);
        /* SEND_EXT_CSD is a transfer state command */
        EXPECT_EQ(sd_cmd(c.host, 8, 0, SD_RSP_R1), SD_TIMEOUT);
        EXPECT_TRUE(c.Status() & R1_ILLEGAL_COMMAND);

        ASSERT_EQ(sd_cmd(c.host, 7, 0x1234u << 16, SD_RSP_R1B, &r), SD_OK);
        EXPECT_EQ(R1_STATE(r.resp), STATE_STBY);
        uint8_t *ext = ext_csd(c);
        EXPECT_EQ(ext[EXT_CSD_REV], 6);
        EXPECT_EQ(ext[EXT_CSD_STRUCTURE], 2);
        EXPECT_EQ(ext[EXT_CSD_CARD_TYPE], 3);
        EXPECT_EQ(ext[EXT_CSD_S_CMD_SET], 1);
        if (c.high_capacity) {
            EXPECT_EQ(le32(ext + EXT_CSD_SEC_COUNT), (uint32_t)c.sectors);
            /* the CSD's capacity fields say "look in the extended CSD" */
            EXPECT_EQ(sd_bits(r.reg, 16, 73, 62), 0xfffu);
        }
        EXPECT_GE(c.disk, 0);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
    });
}


TEST(mmc_card, illegal_commands)
{
    each_device([](SdCard &c) {
        static const uint8_t kInTran[] = {1, 2, 3, 9, 10, 12, 55};
        for (uint8_t index : kInTran) {
            uint32_t arg = index == 1 ? OCR_WINDOW : (uint32_t)c.rca << 16;
            EXPECT_EQ(sd_cmd(c.host, index, arg, SD_RSP_R1), SD_TIMEOUT);
            uint32_t st = c.Status();
            EXPECT_TRUE(st & R1_ILLEGAL_COMMAND);
            EXPECT_EQ(R1_STATE(st), STATE_TRAN);
        }
        /* another device's address is not an error */
        EXPECT_EQ(sd_cmd(c.host, 13, (uint32_t)(c.rca + 1) << 16, SD_RSP_R1),
                  SD_TIMEOUT);
        EXPECT_FALSE(c.Status() & R1_ILLEGAL_COMMAND);
    });
}


TEST(mmc_card, switch)
{
    /* SWITCH answers before it acts, so whether it worked shows in the
       next response, which is SEND_STATUS's */
    each_device([](SdCard &c) {
        uint32_t r1 = switch_cmd(c, SWITCH_WRITE, EXT_CSD_HS_TIMING, 1);
        EXPECT_EQ(R1_STATE(r1), STATE_TRAN);
        EXPECT_FALSE(r1 & R1_ERROR);
        EXPECT_FALSE(c.Status() & R1_SWITCH_ERROR);
        uint8_t *ext = ext_csd(c);
        EXPECT_EQ(ext[EXT_CSD_HS_TIMING], 1);

        /* the CSD reports 52 MHz now */
        SdResult r;
        sd_cmd(c.host, 7, 0, SD_RSP_NONE);
        ASSERT_EQ(sd_cmd(c.host, 9, (uint32_t)c.rca << 16, SD_RSP_R2, &r),
                  SD_OK);
        EXPECT_EQ(sd_bits(c.csd, 16, 103, 96), 0x32u);
        EXPECT_EQ(sd_bits(r.reg, 16, 103, 96), 0x5au);
        ASSERT_EQ(sd_cmd(c.host, 7, (uint32_t)c.rca << 16, SD_RSP_R1B),
                  SD_OK);

        /* bus widths of one, four and eight bits; not DDR */
        EXPECT_FALSE(switch_cmd(c, SWITCH_WRITE, EXT_CSD_BUS_WIDTH, 2) &
                     R1_ERROR);
        EXPECT_FALSE(c.Status() & R1_SWITCH_ERROR);
        r1 = switch_cmd(c, SWITCH_WRITE, EXT_CSD_BUS_WIDTH, 6);
        EXPECT_FALSE(r1 & R1_SWITCH_ERROR);
        EXPECT_TRUE(c.Status() & R1_SWITCH_ERROR);
        EXPECT_FALSE(c.Status() & R1_SWITCH_ERROR);
        /* HS200 is not in CARD_TYPE */
        switch_cmd(c, SWITCH_WRITE, EXT_CSD_HS_TIMING, 2);
        EXPECT_TRUE(c.Status() & R1_SWITCH_ERROR);
        /* the properties segment is read only */
        switch_cmd(c, SWITCH_WRITE, EXT_CSD_REV, 7);
        EXPECT_TRUE(c.Status() & R1_SWITCH_ERROR);
        /* only the user area exists */
        switch_cmd(c, SWITCH_WRITE, EXT_CSD_PART_CONFIG, 1);
        EXPECT_TRUE(c.Status() & R1_SWITCH_ERROR);
        /* set and clear bits */
        switch_cmd(c, SWITCH_SET_BITS, EXT_CSD_ERASE_GROUP_DEF, 1);
        EXPECT_FALSE(c.Status() & R1_SWITCH_ERROR);
        switch_cmd(c, SWITCH_SET_BITS, EXT_CSD_ERASE_GROUP_DEF, 2);
        EXPECT_TRUE(c.Status() & R1_SWITCH_ERROR);
        ext = ext_csd(c);
        EXPECT_EQ(ext[EXT_CSD_ERASE_GROUP_DEF], 1);
        EXPECT_EQ(ext[EXT_CSD_BUS_WIDTH], 2);
        EXPECT_EQ(ext[EXT_CSD_REV], 6);
        switch_cmd(c, SWITCH_CLEAR_BITS, EXT_CSD_ERASE_GROUP_DEF, 1);
        EXPECT_FALSE(c.Status() & R1_SWITCH_ERROR);
        EXPECT_EQ(ext_csd(c)[EXT_CSD_ERASE_GROUP_DEF], 0);
        /* SWITCH outside the transfer state is illegal */
        sd_cmd(c.host, 7, 0, SD_RSP_NONE);
        EXPECT_EQ(switch_cmd(c, SWITCH_WRITE, EXT_CSD_HS_TIMING, 0),
                  0xffffffffu);
        EXPECT_TRUE(c.Status() & R1_ILLEGAL_COMMAND);
    });
}


TEST(mmc_card, sleep)
{
    /* SLEEP_AWAKE between standby and sleep; asleep, the device takes
       nothing but CMD0 and CMD5 */
    each_device([](SdCard &c) {
        SdResult r;
        uint32_t rca = (uint32_t)c.rca << 16;
        EXPECT_EQ(sd_cmd(c.host, 5, rca | 0x8000, SD_RSP_R1B), SD_TIMEOUT);
        EXPECT_TRUE(c.Status() & R1_ILLEGAL_COMMAND);
        sd_cmd(c.host, 7, 0, SD_RSP_NONE);
        ASSERT_EQ(sd_cmd(c.host, 5, rca | 0x8000, SD_RSP_R1B, &r), SD_OK);
        EXPECT_EQ(R1_STATE(r.resp), STATE_STBY);
        EXPECT_EQ(c.Status(), 0xffffffffu);
        ASSERT_EQ(sd_cmd(c.host, 5, rca, SD_RSP_R1B, &r), SD_OK);
        EXPECT_EQ(R1_STATE(r.resp), STATE_SLP);
        EXPECT_TRUE(r.resp & R1_ILLEGAL_COMMAND);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_STBY);
        EXPECT_EQ(sd_cmd(c.host, 7, rca, SD_RSP_R1B), SD_OK);
    });
}


TEST(mmc_card, read_write)
{
    each_device([](SdCard &c) {
        SdResult r;
        tc_block_find(c.sectors);
        uint8_t *buf = sd_alloc(4 * 512);
        static const SdMode kModes[] = {SD_PIO, SD_DMA, SD_ADMA2};
        for (SdMode mode : kModes) {
            if (!c.host->CanDo(mode))
                continue;
            uint64_t s = c.sectors - 4;
            memset(buf, 0, 4 * 512);
            ASSERT_EQ(sd_rw(c, true, s, 4, buf, mode), SD_OK);
            EXPECT_TRUE(sd_is_pattern(buf, s, 4));
            memset(buf, 0, 4 * 512);
            ASSERT_EQ(sd_rw(c, true, 1, 2, buf, mode, nullptr, true), SD_OK);
            EXPECT_TRUE(sd_is_pattern(buf, 1, 2));
        }
        /* a byte address that is not block aligned, and past the end */
        if (!c.high_capacity) {
            ASSERT_EQ(sd_cmd(c.host, 17, 0x10, SD_RSP_R1, &r), SD_OK);
            EXPECT_TRUE(r.resp & R1_ADDRESS_ERROR);
        }
        EXPECT_NE(sd_rw(c, true, c.sectors, 1, buf, SD_PIO, &r), SD_OK);
        EXPECT_TRUE(r.resp & R1_OUT_OF_RANGE);

        if (read_only(c)) {
            EXPECT_NE(sd_rw(c, false, 2, 1, buf, SD_PIO, &r), SD_OK);
            EXPECT_TRUE(r.resp & R1_WP_VIOLATION);
            return;
        }
        uint8_t *in = sd_alloc(4 * 512);
        for (int i = 0; i < 4 * 512; i++)
            buf[i] = i ^ 0x3c;
        ASSERT_EQ(sd_rw(c, false, 20, 4, buf), SD_OK);
        /* CMD23 with reliable write asked for: the count is the low 16 */
        ASSERT_EQ(sd_cmd(c.host, 23, 0x80000002, SD_RSP_R1), SD_OK);
        SdCmd cmd;
        cmd.index = 25;
        cmd.arg = c.Addr(30);
        cmd.buf = buf;
        cmd.blksz = 512;
        cmd.blocks = 2;
        cmd.read = false;
        cmd.multi = true;
        ASSERT_EQ(c.host->Command(cmd, &r), SD_OK);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
        ASSERT_EQ(sd_rw(c, true, 20, 4, in), SD_OK);
        EXPECT_EQ(memcmp(buf, in, 4 * 512), 0);
        ASSERT_EQ(sd_rw(c, true, 30, 2, in), SD_OK);
        EXPECT_EQ(memcmp(buf, in, 2 * 512), 0);
    });
}
