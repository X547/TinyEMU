/*
 * SD memory card tests
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

/* An SD memory card as the Physical Layer Simplified Specification 3.01
   describes it: identification (4.2), the state table (4.8, Table 4-29),
   the card status (4.10.1), the CSD (5.3) and SCR (5.6), CMD6 (4.3.10),
   on every host with an SD card. */

#define SDSC_MAX_SECTORS 4194304

namespace {

/* Run 'fn' on every SD card, or skip the test without one. */
template <typename F>
void each_card(F fn)
{
    SdCard card;
    int i = 0;
    for (; sd_find_card(false, i, &card); i++) {
        t_log("%s, %llu sectors", card.host->Name(),
              (unsigned long long)card.sectors);
        fn(card);
    }
    if (i == 0)
        t_skip("no SD card");
}

/* The card powered up afresh and left in idle. */
void power_up(const SdCard &c)
{
    ASSERT_TRUE(c.host->Init());
    sd_cmd(c.host, 0, 0, SD_RSP_NONE);
}

SdErr acmd(const SdCard &c, uint8_t index, uint32_t arg, SdRsp rsp,
           SdResult *res = nullptr)
{
    SdErr err = sd_cmd(c.host, 55, (uint32_t)c.rca << 16, SD_RSP_R1);
    if (err != SD_OK)
        return err;
    return sd_cmd(c.host, index, arg, rsp, res);
}

/* A register read over the data lines: ACMD13, ACMD22, ACMD51, CMD6. */
SdErr reg_read(const SdCard &c, bool app, uint8_t index, uint32_t arg,
               uint8_t *buf, uint32_t len, SdResult *res = nullptr)
{
    SdResult dummy;
    if (app && sd_cmd(c.host, 55, (uint32_t)c.rca << 16, SD_RSP_R1) != SD_OK)
        return SD_TIMEOUT;
    SdCmd cmd;
    cmd.index = index;
    cmd.arg = arg;
    cmd.buf = buf;
    cmd.blksz = len;
    cmd.blocks = 1;
    return c.host->Command(cmd, res != nullptr ? res : &dummy);
}

void select_disk(const SdCard &c)
{
    if (c.disk < 0)
        t_skip("no test disk behind the card");
    tc_block_find(c.sectors);
}

bool read_only(const SdCard &c)
{
    select_disk(c);
    /* a write protected test disk refuses through the card */
    return c.host->Kind() == SD_HOST_DW_MMC
               ? (static_cast<DwMmcHost *>(c.host)->R(DW_WRTPRT) & 1) != 0
               : !(static_cast<SdhciHost *>(c.host)->R32(SDHCI_PRESENT) &
                   SDHCI_PS_WRITE_ENABLED);
}

}


TEST(sd_card, identification)
{
    each_card([](SdCard &c) {
        SdResult r;
        power_up(c);

        /* CMD8: the voltage accepted and the check pattern echoed (4.3.13) */
        ASSERT_EQ(sd_cmd(c.host, 8, 0x1aa, SD_RSP_R1, &r), SD_OK);
        EXPECT_EQ(r.resp & 0xfff, 0x1aau);
        /* CMD8 with a voltage the card cannot take goes unanswered */
        EXPECT_EQ(sd_cmd(c.host, 8, 0x2aa, SD_RSP_R1), SD_TIMEOUT);

        /* CMD55 in idle, then ACMD41 as a query: busy, no state change */
        ASSERT_EQ(sd_cmd(c.host, 55, 0, SD_RSP_R1, &r), SD_OK);
        EXPECT_TRUE(r.resp & R1_APP_CMD);
        EXPECT_EQ(R1_STATE(r.resp), STATE_IDLE);
        ASSERT_EQ(sd_cmd(c.host, 41, 0, SD_RSP_R3, &r), SD_OK);
        EXPECT_EQ(r.resp & OCR_WINDOW, OCR_WINDOW);
        EXPECT_FALSE(r.resp & OCR_READY);

        /* ACMD41 with HCS: ready, CCS saying how the card is addressed */
        sd_cmd(c.host, 55, 0, SD_RSP_R1);
        ASSERT_EQ(sd_cmd(c.host, 41, OCR_WINDOW | OCR_HCS, SD_RSP_R3, &r),
                  SD_OK);
        EXPECT_TRUE(r.resp & OCR_READY);
        EXPECT_EQ((r.resp & OCR_HCS) != 0, c.sectors > SDSC_MAX_SECTORS);

        /* CMD2: the CID; CMD3: a new RCA, R6 with the state it arrived in */
        ASSERT_EQ(sd_cmd(c.host, 2, 0, SD_RSP_R2, &r), SD_OK);
        EXPECT_EQ(memcmp(r.reg, c.cid, 15), 0);
        ASSERT_EQ(sd_cmd(c.host, 3, 0, SD_RSP_R1, &r), SD_OK);
        uint16_t rca = r.resp >> 16;
        EXPECT_NE(rca, 0);
        EXPECT_EQ(get_bits(r.resp, 9, 4), (uint32_t)STATE_IDENT);
        c.rca = rca;
        EXPECT_EQ(R1_STATE(c.Status()), STATE_STBY);

        /* CMD9: CSD version 2.0 exactly for a high capacity card, and the
           capacity it gives is the disk's */
        ASSERT_EQ(sd_cmd(c.host, 9, (uint32_t)rca << 16, SD_RSP_R2, &r),
                  SD_OK);
        uint32_t structure = sd_bits(r.reg, 16, 127, 126);
        EXPECT_EQ(structure, c.sectors > SDSC_MAX_SECTORS ? 1u : 0u);
        EXPECT_GE(c.disk, 0);
        /* READ_BL_LEN: 512 bytes, or 1024 for an SDSC card over 1 GB */
        uint32_t bl_len = sd_bits(r.reg, 16, 83, 80);
        EXPECT_TRUE(bl_len == 9 || (!structure && bl_len == 10));
        /* the command classes it claims include 0, 2, 4, 8 and 10 */
        uint32_t ccc = sd_bits(r.reg, 16, 95, 84);
        EXPECT_EQ(ccc & 0x515, 0x515u);
        SNAP("csd", "%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x%02x"
             "%02x%02x", r.reg[0], r.reg[1], r.reg[2], r.reg[3], r.reg[4],
             r.reg[5], r.reg[6], r.reg[7], r.reg[8], r.reg[9], r.reg[10],
             r.reg[11], r.reg[12], r.reg[13], r.reg[14]);
        SNAP("cid", "%02x %c%c %c%c%c%c%c %02x %02x%02x%02x%02x %02x%02x",
             c.cid[0], c.cid[1], c.cid[2], c.cid[3], c.cid[4], c.cid[5],
             c.cid[6], c.cid[7], c.cid[8], c.cid[9], c.cid[10], c.cid[11],
             c.cid[12], c.cid[13], c.cid[14]);

        /* CMD7: R1b with the standby state, then the transfer state */
        ASSERT_EQ(sd_cmd(c.host, 7, (uint32_t)rca << 16, SD_RSP_R1B, &r),
                  SD_OK);
        EXPECT_EQ(R1_STATE(r.resp), STATE_STBY);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
    });
}


TEST(sd_card, illegal_commands)
{
    /* A command outside its states is not answered, and the next response
       reports ILLEGAL_COMMAND once (Table 4-29, 4.10.1 clear condition B). */
    each_card([](SdCard &c) {
        static const uint8_t kInTran[] = {2, 3, 8, 9, 10, 12, 1, 5};
        for (uint8_t index : kInTran) {
            uint32_t arg = index == 8 ? 0x1aa : (uint32_t)c.rca << 16;
            EXPECT_EQ(sd_cmd(c.host, index, arg, SD_RSP_R1), SD_TIMEOUT);
            uint32_t st = c.Status();
            EXPECT_TRUE(st & R1_ILLEGAL_COMMAND);
            EXPECT_TRUE(st & R1_ERROR);
            EXPECT_EQ(R1_STATE(st), STATE_TRAN);
            EXPECT_FALSE(c.Status() & R1_ILLEGAL_COMMAND);
        }
        /* ACMD41 outside idle */
        EXPECT_EQ(acmd(c, 41, OCR_WINDOW, SD_RSP_R3), SD_TIMEOUT);
        EXPECT_TRUE(c.Status() & R1_ILLEGAL_COMMAND);

        /* in standby: block commands and ACMD6 are not taken */
        ASSERT_EQ(sd_cmd(c.host, 7, 0, SD_RSP_NONE), SD_OK);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_STBY);
        EXPECT_EQ(sd_cmd(c.host, 16, 512, SD_RSP_R1), SD_TIMEOUT);
        EXPECT_TRUE(c.Status() & R1_ILLEGAL_COMMAND);
        EXPECT_EQ(sd_cmd(c.host, 23, 1, SD_RSP_R1), SD_TIMEOUT);
        EXPECT_TRUE(c.Status() & R1_ILLEGAL_COMMAND);
        EXPECT_EQ(acmd(c, 6, 2, SD_RSP_R1), SD_TIMEOUT);
        EXPECT_TRUE(c.Status() & R1_ILLEGAL_COMMAND);

        /* a command for another card's address is not an error */
        uint32_t other = (uint32_t)(c.rca + 1) << 16;
        EXPECT_EQ(sd_cmd(c.host, 13, other, SD_RSP_R1), SD_TIMEOUT);
        EXPECT_EQ(sd_cmd(c.host, 9, other, SD_RSP_R2), SD_TIMEOUT);
        EXPECT_EQ(sd_cmd(c.host, 55, other, SD_RSP_R1), SD_TIMEOUT);
        EXPECT_FALSE(c.Status() & R1_ILLEGAL_COMMAND);
    });
}


TEST(sd_card, response_state)
{
    /* CURRENT_STATE is the state the command arrived in (4.10.1) */
    each_card([](SdCard &c) {
        SdResult r;
        uint8_t *buf = sd_alloc(2 * 512);
        ASSERT_EQ(sd_rw(c, true, 0, 1, buf, SD_PIO, &r), SD_OK);
        EXPECT_EQ(R1_STATE(r.resp), STATE_TRAN);
        EXPECT_FALSE(r.resp & R1_ERROR);
        EXPECT_TRUE(r.resp & R1_READY_FOR_DATA);
        /* the automatic CMD12 finds the card still sending */
        ASSERT_EQ(sd_rw(c, true, 0, 2, buf, SD_PIO, &r), SD_OK);
        EXPECT_EQ(R1_STATE(r.auto_resp), STATE_DATA);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
        /* CMD7 deselecting: no answer from the card that was selected */
        EXPECT_EQ(sd_cmd(c.host, 7, 0, SD_RSP_R1B), SD_TIMEOUT);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_STBY);
        /* CMD7 for its own address in transfer is illegal */
        ASSERT_EQ(sd_cmd(c.host, 7, (uint32_t)c.rca << 16, SD_RSP_R1B, &r),
                  SD_OK);
        EXPECT_EQ(sd_cmd(c.host, 7, (uint32_t)c.rca << 16, SD_RSP_R1B),
                  SD_TIMEOUT);
        EXPECT_TRUE(c.Status() & R1_ILLEGAL_COMMAND);
    });
}


TEST(sd_card, capacity_support)
{
    /* An SDHC card stays busy for a host without HCS, or one that did not
       send CMD8; an SDSC card ignores HCS (4.2.3.1). */
    each_card([](SdCard &c) {
        SdResult r;
        bool hc = c.sectors > SDSC_MAX_SECTORS;

        power_up(c);
        sd_cmd(c.host, 8, 0x1aa, SD_RSP_R1);
        for (int i = 0; i < 4; i++) {
            sd_cmd(c.host, 55, 0, SD_RSP_R1);
            ASSERT_EQ(sd_cmd(c.host, 41, OCR_WINDOW, SD_RSP_R3, &r), SD_OK);
            EXPECT_EQ((r.resp & OCR_READY) != 0, !hc);
            EXPECT_FALSE(r.resp & OCR_HCS);
            if (!hc)
                break;
        }
        EXPECT_EQ(sd_cmd(c.host, 2, 0, SD_RSP_R2) == SD_OK, !hc);

        power_up(c);
        sd_cmd(c.host, 55, 0, SD_RSP_R1);
        ASSERT_EQ(sd_cmd(c.host, 41, OCR_WINDOW | OCR_HCS, SD_RSP_R3, &r),
                  SD_OK);
        EXPECT_EQ((r.resp & OCR_READY) != 0, !hc);
        EXPECT_FALSE(r.resp & OCR_HCS);
    });
}


TEST(sd_card, inactive)
{
    /* An offer outside the voltage window, or CMD15, leaves the card
       inactive: nothing is answered, not even CMD0, until the power goes
       (4.2.3, Table 4-29). */
    each_card([](SdCard &c) {
        power_up(c);
        ASSERT_EQ(sd_cmd(c.host, 55, 0, SD_RSP_R1), SD_OK);
        EXPECT_EQ(sd_cmd(c.host, 41, 0x80, SD_RSP_R3), SD_TIMEOUT);
        sd_cmd(c.host, 0, 0, SD_RSP_NONE);
        EXPECT_EQ(sd_cmd(c.host, 8, 0x1aa, SD_RSP_R1), SD_TIMEOUT);
        EXPECT_EQ(sd_cmd(c.host, 55, 0, SD_RSP_R1), SD_TIMEOUT);

        ASSERT_TRUE(sd_card_init(c.host, &c));
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
        EXPECT_EQ(sd_cmd(c.host, 15, (uint32_t)c.rca << 16, SD_RSP_NONE),
                  SD_OK);
        EXPECT_EQ(c.Status(), 0xffffffffu);
        sd_cmd(c.host, 0, 0, SD_RSP_NONE);
        EXPECT_EQ(sd_cmd(c.host, 8, 0x1aa, SD_RSP_R1), SD_TIMEOUT);

        c.host->PowerOff();
        ASSERT_TRUE(sd_card_init(c.host, &c));
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
    });
}


TEST(sd_card, read)
{
    each_card([](SdCard &c) {
        select_disk(c);
        uint8_t *buf = sd_alloc(8 * 512);
        static const SdMode kModes[] = {SD_PIO, SD_DMA, SD_ADMA2};
        for (SdMode mode : kModes) {
            if (!c.host->CanDo(mode))
                continue;
            uint64_t s = c.sectors - 8;
            uint32_t reads = tc_block_read(TEST_CONTROL_BLOCK_READS);
            memset(buf, 0, 8 * 512);
            ASSERT_EQ(sd_rw(c, true, s, 1, buf, mode), SD_OK);
            EXPECT_TRUE(sd_is_pattern(buf, s, 1));
            memset(buf, 0, 8 * 512);
            ASSERT_EQ(sd_rw(c, true, s, 8, buf, mode), SD_OK);
            EXPECT_TRUE(sd_is_pattern(buf, s, 8));
            memset(buf, 0, 8 * 512);
            ASSERT_EQ(sd_rw(c, true, 3, 5, buf, mode, nullptr, true), SD_OK);
            EXPECT_TRUE(sd_is_pattern(buf, 3, 5));
            EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
            EXPECT_EQ(tc_block_read(TEST_CONTROL_BLOCK_READS) - reads, 14u);
        }
    });
}


TEST(sd_card, write)
{
    each_card([](SdCard &c) {
        if (read_only(c))
            return;
        uint8_t *out = sd_alloc(4 * 512);
        uint8_t *in = sd_alloc(4 * 512);
        static const SdMode kModes[] = {SD_PIO, SD_DMA, SD_ADMA2};
        int round = 0;
        for (SdMode mode : kModes) {
            if (!c.host->CanDo(mode))
                continue;
            for (int i = 0; i < 4 * 512; i++)
                out[i] = i * 7 + round;
            uint64_t s = 64 + round * 8;
            ASSERT_EQ(sd_rw(c, false, s, 1, out, mode), SD_OK);
            ASSERT_EQ(sd_rw(c, false, s + 1, 3, out + 512, mode), SD_OK);
            ASSERT_EQ(sd_rw(c, true, s, 4, in, SD_PIO), SD_OK);
            EXPECT_EQ(memcmp(in, out, 4 * 512), 0);
            /* ACMD22: the blocks the last write programmed */
            uint8_t *n = sd_alloc(4);
            ASSERT_EQ(reg_read(c, true, 22, 0, n, 4), SD_OK);
            EXPECT_EQ(be32(n), 3u);
            ASSERT_EQ(sd_rw(c, false, s, 2, out, mode, nullptr, true), SD_OK);
            EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
            round++;
        }
    });
}


TEST(sd_card, write_protect)
{
    each_card([](SdCard &c) {
        if (!read_only(c))
            return;
        SdResult r;
        uint8_t *buf = sd_alloc(512, 0x5a);
        uint32_t writes = tc_block_read(TEST_CONTROL_BLOCK_WRITES);
        EXPECT_NE(sd_rw(c, false, 1, 1, buf, SD_PIO, &r), SD_OK);
        EXPECT_TRUE(r.resp & R1_WP_VIOLATION);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
        EXPECT_EQ(tc_block_read(TEST_CONTROL_BLOCK_WRITES), writes);
        ASSERT_EQ(sd_rw(c, true, 1, 1, buf), SD_OK);
        EXPECT_TRUE(sd_is_pattern(buf, 1, 1));
    });
}


TEST(sd_card, address_errors)
{
    /* Past the end is OUT_OF_RANGE and ADDRESS_ERROR; a byte address that
       is not block aligned is ADDRESS_ERROR on SDSC; both in the response
       to the command itself (4.10.1, types E R X). */
    each_card([](SdCard &c) {
        SdResult r;
        uint8_t *buf = sd_alloc(512);
        EXPECT_NE(sd_rw(c, true, c.sectors, 1, buf, SD_PIO, &r), SD_OK);
        EXPECT_EQ(r.resp & (R1_OUT_OF_RANGE | R1_ADDRESS_ERROR),
                  R1_OUT_OF_RANGE | R1_ADDRESS_ERROR);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
        if (!c.high_capacity) {
            ASSERT_EQ(sd_cmd(c.host, 17, 0x101, SD_RSP_R1, &r), SD_OK);
            EXPECT_TRUE(r.resp & R1_ADDRESS_ERROR);
            EXPECT_FALSE(r.resp & R1_OUT_OF_RANGE);
        }
        EXPECT_FALSE(c.Status() & R1_ERROR);
        /* CMD16: 512 is the only block length the CSD offers */
        ASSERT_EQ(sd_cmd(c.host, 16, 512, SD_RSP_R1, &r), SD_OK);
        EXPECT_FALSE(r.resp & R1_ERROR);
        ASSERT_EQ(sd_cmd(c.host, 16, 1024, SD_RSP_R1, &r), SD_OK);
        EXPECT_TRUE(r.resp & R1_BLOCK_LEN_ERROR);
        EXPECT_FALSE(c.Status() & R1_ERROR);
    });
}


TEST(sd_card, past_the_end)
{
    /* A multiple block read that walks off the medium: the card stops
       sending and stays in the data state, and CMD12's response says why
       (4.3.3). */
    each_card([](SdCard &c) {
        SdResult r;
        uint8_t *buf = sd_alloc(2 * 512);
        SdCmd cmd;
        cmd.index = 18;
        cmd.arg = c.Addr(c.sectors - 1);
        cmd.buf = buf;
        cmd.blksz = 512;
        cmd.blocks = 2;
        cmd.multi = true;
        EXPECT_EQ(c.host->Command(cmd, &r), SD_DATA_ERROR);
        EXPECT_TRUE(sd_is_pattern(buf, c.sectors - 1, 1));
        uint32_t st = c.Status();
        EXPECT_EQ(R1_STATE(st), STATE_DATA);
        ASSERT_EQ(sd_cmd(c.host, 12, 0, SD_RSP_R1B, &r), SD_OK);
        EXPECT_TRUE((st | r.resp) & R1_OUT_OF_RANGE);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
        /* reading up to the last block is no error */
        ASSERT_EQ(sd_rw(c, true, c.sectors - 2, 2, buf, SD_PIO, &r), SD_OK);
        EXPECT_FALSE(r.auto_resp & R1_ERROR);
    });
}


TEST(sd_card, media_errors)
{
    /* A block the disk fails is CARD_ECC_FAILED on a read and CC_ERROR on a
       write, with the card left in the data or receive state for CMD12. */
    each_card([](SdCard &c) {
        select_disk(c);
        SdResult r;
        uint8_t *buf = sd_alloc(512);
        tc_block_write(TEST_CONTROL_BLOCK_FAIL, 1);
        EXPECT_EQ(sd_rw(c, true, 5, 1, buf), SD_DATA_ERROR);
        uint32_t st = c.Status();
        EXPECT_TRUE(st & R1_CARD_ECC_FAILED);
        EXPECT_EQ(R1_STATE(st), STATE_DATA);
        ASSERT_EQ(sd_cmd(c.host, 12, 0, SD_RSP_R1B, &r), SD_OK);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
        if (read_only(c))
            return;
        tc_block_write(TEST_CONTROL_BLOCK_FAIL, 1);
        EXPECT_EQ(sd_rw(c, false, 5, 1, buf), SD_DATA_ERROR);
        st = c.Status();
        EXPECT_TRUE(st & R1_CC_ERROR);
        EXPECT_EQ(R1_STATE(st), STATE_RCV);
        ASSERT_EQ(sd_cmd(c.host, 12, 0, SD_RSP_R1B, &r), SD_OK);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
    });
}


TEST(sd_card, async_disk)
{
    /* the same transfers on a disk that finishes later */
    each_card([](SdCard &c) {
        select_disk(c);
        tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_ASYNC);
        uint8_t *buf = sd_alloc(4 * 512);
        static const SdMode kModes[] = {SD_PIO, SD_DMA, SD_ADMA2};
        for (SdMode mode : kModes) {
            if (!c.host->CanDo(mode))
                continue;
            memset(buf, 0, 4 * 512);
            ASSERT_EQ(sd_rw(c, true, 10, 4, buf, mode), SD_OK);
            EXPECT_TRUE(sd_is_pattern(buf, 10, 4));
            if (!read_only(c)) {
                tc_block_write(TEST_CONTROL_BLOCK_MODE,
                               TEST_BLOCK_MODE_ASYNC);
                ASSERT_EQ(sd_rw(c, false, 10, 4, buf, mode), SD_OK);
            }
        }
        tc_block_write(TEST_CONTROL_BLOCK_FAIL, 1);
        EXPECT_EQ(sd_rw(c, true, 5, 2, buf), SD_DATA_ERROR);
        sd_cmd(c.host, 12, 0, SD_RSP_R1B);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
    });
}


TEST(sd_card, registers)
{
    each_card([](SdCard &c) {
        SdResult r;
        uint8_t *buf = sd_alloc(64);

        /* ACMD51: the SCR (5.6) */
        ASSERT_EQ(reg_read(c, true, 51, 0, buf, 8, &r), SD_OK);
        EXPECT_TRUE(r.resp & R1_APP_CMD);
        EXPECT_EQ(sd_bits(buf, 8, 63, 60), 0u);     /* SCR_STRUCTURE */
        EXPECT_EQ(sd_bits(buf, 8, 59, 56), 2u);     /* SD_SPEC */
        EXPECT_EQ(sd_bits(buf, 8, 47, 47), 1u);     /* SD_SPEC3 */
        EXPECT_EQ(sd_bits(buf, 8, 51, 48), 0x5u);   /* 1 and 4 bits */
        EXPECT_EQ(sd_bits(buf, 8, 33, 33), 1u);     /* CMD23 */
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);

        /* ACMD6 then ACMD13: the SD status reports the bus width (4.10.2) */
        ASSERT_EQ(acmd(c, 6, 2, SD_RSP_R1), SD_OK);
        ASSERT_EQ(reg_read(c, true, 13, 0, buf, 64), SD_OK);
        EXPECT_EQ(sd_bits(buf, 64, 511, 510), 2u);
        EXPECT_EQ(sd_bits(buf, 64, 495, 480), 0u);  /* regular card */
        ASSERT_EQ(acmd(c, 6, 0, SD_RSP_R1), SD_OK);
        ASSERT_EQ(reg_read(c, true, 13, 0, buf, 64), SD_OK);
        EXPECT_EQ(sd_bits(buf, 64, 511, 510), 0u);

        /* CMD6 mode 0: high speed offered in group 1 (4.3.10) */
        ASSERT_EQ(reg_read(c, false, 6, 0x00fffff1, buf, 64), SD_OK);
        EXPECT_EQ(sd_bits(buf, 64, 415, 400) & 3, 3u);
        EXPECT_EQ(sd_bits(buf, 64, 379, 376), 1u);
        EXPECT_EQ(sd_bits(buf, 64, 375, 368), 1u);
        /* a function a group does not have is refused with 0xF */
        ASSERT_EQ(reg_read(c, false, 6, 0x00ffff1e, buf, 64), SD_OK);
        EXPECT_EQ(sd_bits(buf, 64, 379, 376), 0xfu);
        EXPECT_EQ(sd_bits(buf, 64, 383, 380), 0xfu);
        /* mode 1 switches, and the CSD reports 50 MHz */
        ASSERT_EQ(reg_read(c, false, 6, 0x80fffff1, buf, 64), SD_OK);
        EXPECT_EQ(sd_bits(buf, 64, 379, 376), 1u);
        ASSERT_EQ(sd_cmd(c.host, 7, 0, SD_RSP_NONE), SD_OK);
        ASSERT_EQ(sd_cmd(c.host, 9, (uint32_t)c.rca << 16, SD_RSP_R2, &r),
                  SD_OK);
        EXPECT_EQ(sd_bits(c.csd, 16, 103, 96), 0x32u);
        EXPECT_EQ(sd_bits(r.reg, 16, 103, 96), 0x5au);
    });
}


TEST(sd_card, app_cmd)
{
    /* APP_CMD in the response to CMD55 and to the ACMD after it; a
       command that is no ACMD after CMD55 is the ordinary one (4.3.9) */
    each_card([](SdCard &c) {
        SdResult r;
        ASSERT_EQ(sd_cmd(c.host, 55, (uint32_t)c.rca << 16, SD_RSP_R1, &r),
                  SD_OK);
        EXPECT_TRUE(r.resp & R1_APP_CMD);
        ASSERT_EQ(sd_cmd(c.host, 16, 512, SD_RSP_R1, &r), SD_OK);
        EXPECT_FALSE(r.resp & R1_APP_CMD);
        EXPECT_FALSE(r.resp & R1_ERROR);
        ASSERT_EQ(acmd(c, 6, 0, SD_RSP_R1, &r), SD_OK);
        EXPECT_TRUE(r.resp & R1_APP_CMD);
        EXPECT_FALSE(c.Status() & R1_APP_CMD);
    });
}
