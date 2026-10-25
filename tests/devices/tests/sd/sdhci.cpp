/*
 * SD host controller tests
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
#include "console.h"
#include "intc.h"
#include "sd.h"
#include "test.h"
#include "test_control.h"
#include "testctl.h"

/* The SD host controller by the SD Host Controller Simplified
   Specification 3.00: the register file (2.2), the DMA engines (1.13), the
   automatic commands, card detection and its test bits (1.14). */

namespace {

/* Run 'fn' on every SD host controller, or skip the test. */
template <typename F>
void each_host(F fn)
{
    t_on_cleanup(sd_cleanup);
    int n = 0;
    for (int i = 0; i < sd_host_count(); i++) {
        SdHost *h = sd_host(i);
        if (h->Kind() != SD_HOST_SDHCI)
            continue;
        t_log("%s", h->Name());
        fn(*static_cast<SdhciHost *>(h));
        n++;
    }
    if (n == 0)
        t_skip("no SD host controller");
}

/* The same, for the ones with a card in the slot, which is brought to the
   transfer state. */
template <typename F>
void each_card(F fn)
{
    each_host([&](SdhciHost &h) {
        SdCard c;
        if (!h.CardPresent() || !sd_card_init(&h, &c))
            return;
        fn(h, c);
    });
}

/* An address where there is no RAM. */
uint64_t not_ram()
{
    const HwDevice *tc = test_control_device();
    const HwRegion *r = tc ? hw_region(tc, HW_MMIO) : nullptr;
    if (r == nullptr)
        t_skip("needs the test-control device");
    return r->base;
}

void set_disk(const SdCard &c)
{
    if (c.disk < 0)
        t_skip("no test disk behind the card");
    tc_block_find(c.sectors);
}

/* Start CMD17/18 on the registers as given, without waiting. */
void start_read(SdhciHost &h, const SdCard &c, uint64_t sector,
                uint32_t blocks, uint16_t mode)
{
    h.W16(SDHCI_INT_STATUS, 0xffff);
    h.W16(SDHCI_ERR_STATUS, 0xffff);
    h.W16(SDHCI_BLOCK_COUNT, blocks);
    h.W16(SDHCI_TRANSFER_MODE, mode);
    h.W32(SDHCI_ARGUMENT, c.Addr(sector));
    h.W16(SDHCI_COMMAND, (blocks > 1 ? 18 : 17) << 8 | 0x3a);
}

/* An ADMA2 line */
void adma_line(uint8_t *d, uint16_t attr, uint16_t len, uint64_t addr,
               bool wide)
{
    put_le16(d, attr);
    put_le16(d + 2, len);
    put_le32(d + 4, (uint32_t)addr);
    if (wide)
        put_le32(d + 8, (uint32_t)(addr >> 32));
}

#define ADMA_VALID 0x01
#define ADMA_END   0x02
#define ADMA_INT   0x04
#define ADMA_NOP   0x00
#define ADMA_RSVD  0x10
#define ADMA_TRAN  0x20
#define ADMA_LINK  0x30

/* An ADMA2 read of 'blocks' with the table at 'table'; the normal status
   it ended with. */
uint16_t adma_read(SdhciHost &h, const SdCard &c, uint64_t table,
                   uint32_t blocks, bool wide)
{
    uint8_t hc = h.R8(SDHCI_HOST_CONTROL) & ~SDHCI_CTRL_DMA_MASK;
    h.W8(SDHCI_HOST_CONTROL, hc | (wide ? SDHCI_CTRL_ADMA64
                                        : SDHCI_CTRL_ADMA32));
    h.W32(SDHCI_ADMA_ADDRESS, (uint32_t)table);
    h.W32(SDHCI_ADMA_ADDRESS + 4, (uint32_t)(table >> 32));
    h.W16(SDHCI_BLOCK_SIZE, 512);
    uint16_t mode = SDHCI_TRNS_DMA | SDHCI_TRNS_READ;
    if (blocks > 1)
        mode |= SDHCI_TRNS_MULTI | SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_ACMD12;
    start_read(h, c, 0, blocks, mode);
    uint16_t st = 0;
    t_wait_until([&] {
        st = h.R16(SDHCI_INT_STATUS);
        return (st & (SDHCI_INT_XFER_COMPLETE | SDHCI_INT_ERROR)) != 0;
    }, 1000000);
    return st;
}

}


FRESH_TEST(sdhci, reset_values)
{
    each_host([](SdhciHost &h) {
        /* specification version 3.00 */
        EXPECT_EQ(h.R16(SDHCI_VERSION) & 0xff, 0x02);
        uint32_t caps = h.R32(SDHCI_CAPS);
        EXPECT_TRUE(caps & (1u << 19));  /* ADMA2 */
        EXPECT_TRUE(caps & (1u << 22));  /* SDMA */
        EXPECT_TRUE(caps & (1u << 24));  /* 3.3 V */
        EXPECT_EQ(get_bits(caps, 16, 2), 0u); /* 512 byte blocks */
        SNAP("caps", "%08x %08x %08x", caps, h.R32(SDHCI_CAPS_HI),
             h.R32(SDHCI_MAX_CURRENT));
        uint32_t ps = h.R32(SDHCI_PRESENT);
        EXPECT_TRUE(ps & SDHCI_PS_CARD_STABLE);
        EXPECT_EQ((ps & SDHCI_PS_CARD_INSERTED) != 0,
                  (ps & SDHCI_PS_CARD_DETECT) != 0);
        EXPECT_EQ(ps & 0x0f07, 0u);
        SNAP("present", "%08x", ps);
        /* everything else starts at zero (2.1.3) */
        static const uint8_t kZero16[] = {
            SDHCI_BLOCK_SIZE, SDHCI_BLOCK_COUNT, SDHCI_TRANSFER_MODE,
            SDHCI_COMMAND, SDHCI_CLOCK, SDHCI_INT_STATUS, SDHCI_ERR_STATUS,
            SDHCI_INT_ENABLE, SDHCI_ERR_ENABLE, SDHCI_SIGNAL_ENABLE,
            SDHCI_ERR_SIGNAL_ENABLE, SDHCI_AUTO_CMD_STATUS,
            SDHCI_HOST_CONTROL2, SDHCI_SLOT_INT};
        for (uint8_t off : kZero16)
            EXPECT_EQ(h.R16(off), 0);
        static const uint8_t kZero8[] = {
            SDHCI_HOST_CONTROL, SDHCI_POWER, SDHCI_BLOCK_GAP, SDHCI_WAKEUP,
            SDHCI_TIMEOUT, SDHCI_SWRST, SDHCI_ADMA_ERROR};
        for (uint8_t off : kZero8)
            EXPECT_EQ(h.R8(off), 0);
        EXPECT_EQ(h.R32(SDHCI_ARGUMENT), 0u);
        EXPECT_EQ(h.R32(SDHCI_DMA_ADDRESS), 0u);
    });
}


TEST(sdhci, register_bits)
{
    /* reserved and read only bits ignore writes (2.1.4, 2.2) */
    each_host([](SdhciHost &h) {
        h.W16(SDHCI_BLOCK_SIZE, 0xffff);
        EXPECT_EQ(h.R16(SDHCI_BLOCK_SIZE), 0x7fff);
        h.W16(SDHCI_TRANSFER_MODE, 0xffff);
        EXPECT_EQ(h.R16(SDHCI_TRANSFER_MODE), 0x003f);
        h.W8(SDHCI_BLOCK_GAP, 0xff);
        EXPECT_EQ(h.R8(SDHCI_BLOCK_GAP), 0x0d); /* Continue clears itself */
        h.W8(SDHCI_WAKEUP, 0xff);
        EXPECT_EQ(h.R8(SDHCI_WAKEUP), 0x07);
        h.W8(SDHCI_TIMEOUT, 0xff);
        EXPECT_EQ(h.R8(SDHCI_TIMEOUT), 0x0f);
        h.W16(SDHCI_INT_ENABLE, 0xffff);
        EXPECT_EQ(h.R16(SDHCI_INT_ENABLE), 0x1fff);
        h.W16(SDHCI_ERR_ENABLE, 0xffff);
        EXPECT_EQ(h.R16(SDHCI_ERR_ENABLE), 0xf7ff);
        h.W16(SDHCI_SIGNAL_ENABLE, 0xffff);
        EXPECT_EQ(h.R16(SDHCI_SIGNAL_ENABLE), 0x1fff);
        /* Execute Tuning ends at once, with Sampling Clock Select clear */
        h.W16(SDHCI_HOST_CONTROL2, 0xffff);
        EXPECT_EQ(h.R16(SDHCI_HOST_CONTROL2), 0xc03f);
        h.W16(SDHCI_HOST_CONTROL2, 0);
        h.W8(SDHCI_ADMA_ERROR, 0xff);
        EXPECT_EQ(h.R8(SDHCI_ADMA_ERROR), 0);
        /* the clock: stable follows enable; no clock generator select */
        h.W16(SDHCI_CLOCK, 0xffff);
        uint16_t clk = h.R16(SDHCI_CLOCK);
        EXPECT_EQ(clk & ~SDHCI_CLOCK_CARD_EN, 0xffc3);
        h.W16(SDHCI_CLOCK, 0);
        EXPECT_EQ(h.R16(SDHCI_CLOCK), 0);
        /* bus power only at the 3.3 V offered, and only with a card */
        h.W8(SDHCI_POWER, 0xff);
        EXPECT_EQ(h.R8(SDHCI_POWER), h.CardPresent() ? 0x0f : 0x0e);
        h.W8(SDHCI_POWER, 0x0b);
        EXPECT_EQ(h.R8(SDHCI_POWER), 0x0a);
        h.W8(SDHCI_POWER, 0);
        /* read only registers */
        uint32_t caps = h.R32(SDHCI_CAPS);
        h.W32(SDHCI_CAPS, ~caps);
        EXPECT_EQ(h.R32(SDHCI_CAPS), caps);
        h.W16(SDHCI_VERSION, 0);
        EXPECT_EQ(h.R16(SDHCI_VERSION) & 0xff, 0x02);
        h.Reset(SDHCI_RESET_ALL);
    });
}


TEST(sdhci, partial_writes)
{
    each_card([](SdhciHost &h, SdCard &c) {
        /* a byte written to a status register clears bits in that byte
           only (RW1C, 2.2.18) */
        h.W16(SDHCI_FORCE_ERR, 0x0101);
        EXPECT_EQ(h.R16(SDHCI_ERR_STATUS), 0x0101);
        EXPECT_TRUE(h.R16(SDHCI_INT_STATUS) & SDHCI_INT_ERROR);
        h.W8(SDHCI_ERR_STATUS, 0x01);
        EXPECT_EQ(h.R16(SDHCI_ERR_STATUS), 0x0100);
        h.W8(SDHCI_ERR_STATUS + 1, 0x01);
        EXPECT_EQ(h.R16(SDHCI_ERR_STATUS), 0);
        EXPECT_FALSE(h.R16(SDHCI_INT_STATUS) & SDHCI_INT_ERROR);

        /* writing the command's low byte issues nothing; the upper byte
           does (2.2.6) */
        h.W16(SDHCI_INT_STATUS, 0xffff);
        h.W32(SDHCI_ARGUMENT, (uint32_t)c.rca << 16);
        h.W8(SDHCI_COMMAND, 0x1a);
        t_delay_us(100);
        EXPECT_EQ(h.R16(SDHCI_INT_STATUS), 0);
        h.W8(SDHCI_COMMAND + 1, 13);
        EXPECT_TRUE(h.WaitInt(SDHCI_INT_CMD_COMPLETE, nullptr));
        EXPECT_EQ(h.R16(SDHCI_INT_STATUS), SDHCI_INT_CMD_COMPLETE);
        EXPECT_EQ(R1_STATE(h.R32(SDHCI_RESPONSE)), STATE_TRAN);

        /* a 32 bit write of transfer mode and command is both, the command
           last */
        uint8_t *buf = sd_alloc(512);
        h.W16(SDHCI_INT_STATUS, 0xffff);
        h.W16(SDHCI_BLOCK_SIZE, 512);
        h.W16(SDHCI_BLOCK_COUNT, 1);
        h.W32(SDHCI_ARGUMENT, c.Addr(2));
        h.W32(SDHCI_TRANSFER_MODE, (uint32_t)(17 << 8 | 0x3a) << 16 |
                                   SDHCI_TRNS_READ);
        ASSERT_TRUE(h.WaitInt(SDHCI_INT_BUF_RD_READY, nullptr));
        for (int i = 0; i < 512; i += 4)
            put_le32(buf + i, h.R32(SDHCI_BUFFER));
        EXPECT_TRUE(sd_is_pattern(buf, 2, 1));
        ASSERT_TRUE(h.WaitInt(SDHCI_INT_XFER_COMPLETE, nullptr));
    });
}


TEST(sdhci, software_reset)
{
    each_card([](SdhciHost &h, SdCard &c) {
        /* CMD line: Command Complete only (2.2.16) */
        h.W16(SDHCI_INT_STATUS, 0xffff);
        h.W32(SDHCI_ARGUMENT, (uint32_t)c.rca << 16);
        h.W16(SDHCI_COMMAND, 13 << 8 | 0x1a);
        ASSERT_TRUE(h.WaitInt(SDHCI_INT_CMD_COMPLETE, nullptr));
        h.W16(SDHCI_FORCE_ERR, SDHCI_ERR_DATA_CRC);
        h.W8(SDHCI_BLOCK_GAP, 0x01);
        ASSERT_TRUE(h.Reset(SDHCI_RESET_CMD));
        EXPECT_EQ(h.R16(SDHCI_INT_STATUS), SDHCI_INT_ERROR);
        EXPECT_EQ(h.R8(SDHCI_BLOCK_GAP), 0x01);
        /* DAT line: the block gap requests too */
        ASSERT_TRUE(h.Reset(SDHCI_RESET_DATA));
        EXPECT_EQ(h.R8(SDHCI_BLOCK_GAP), 0);
        EXPECT_NE(h.R8(SDHCI_POWER), 0);
        /* all: power goes, and the card is reset with it */
        uint32_t caps = h.R32(SDHCI_CAPS);
        ASSERT_TRUE(h.Reset(SDHCI_RESET_ALL));
        EXPECT_EQ(h.R8(SDHCI_POWER), 0);
        EXPECT_EQ(h.R16(SDHCI_INT_ENABLE), 0);
        EXPECT_EQ(h.R32(SDHCI_CAPS), caps);
        EXPECT_TRUE(h.R32(SDHCI_PRESENT) & SDHCI_PS_CARD_INSERTED);
        /* without power the card answers nothing */
        h.W16(SDHCI_ERR_ENABLE, 0xffff);
        h.W16(SDHCI_INT_ENABLE, 0xffff);
        EXPECT_EQ(c.Status(), 0xffffffffu);
        h.W8(SDHCI_POWER, 0x0f);
        EXPECT_EQ(c.Status(), 0xffffffffu); /* reset: in idle, no RCA */
        sd_cmd(&h, 0, 0, SD_RSP_NONE);
        EXPECT_EQ(sd_cmd(&h, 8, 0x1aa, SD_RSP_R1), c.mmc ? SD_TIMEOUT
                                                        : SD_OK);
    });
}


TEST(sdhci, no_response)
{
    /* Command Timeout only for a command that expects a response; one
       that does not completes (2.2.17, 2.2.18) */
    each_host([](SdhciHost &h) {
        ASSERT_TRUE(h.Init());
        EXPECT_EQ(sd_cmd(&h, 13, 0x10000, SD_RSP_R1), SD_TIMEOUT);
        h.W16(SDHCI_INT_STATUS, 0xffff);
        h.W16(SDHCI_ERR_STATUS, 0xffff);
        h.W32(SDHCI_ARGUMENT, 0);
        h.W16(SDHCI_COMMAND, 13 << 8 | 0x1a);
        ASSERT_TRUE(h.WaitInt(SDHCI_INT_CMD_COMPLETE, nullptr));
        EXPECT_EQ(h.R16(SDHCI_ERR_STATUS), SDHCI_ERR_CMD_TIMEOUT);
        EXPECT_EQ(h.R16(SDHCI_INT_STATUS), SDHCI_INT_ERROR);
        h.W16(SDHCI_ERR_STATUS, 0xffff);
        h.W16(SDHCI_COMMAND, 0);
        ASSERT_TRUE(h.WaitInt(SDHCI_INT_CMD_COMPLETE, nullptr));
        EXPECT_EQ(h.R16(SDHCI_INT_STATUS), SDHCI_INT_CMD_COMPLETE);
        /* a card in idle answers CMD0 by silence: with R1 expected that
           is a timeout too */
        if (h.CardPresent())
            EXPECT_EQ(sd_cmd(&h, 0, 0, SD_RSP_R1), SD_TIMEOUT);
    });
}


TEST(sdhci, card_detect_test)
{
    /* The test bits stand in for the card detect pin; a change raises the
       insertion or removal status, and removal takes the bus power and the
       SD clock away (1.14, 2.2.9, 2.2.10) */
    each_host([](SdhciHost &h) {
        ASSERT_TRUE(h.Init());
        bool card = h.CardPresent();
        h.W16(SDHCI_INT_ENABLE, 0x00ff);
        h.W16(SDHCI_INT_STATUS, 0xffff);
        h.W8(SDHCI_HOST_CONTROL, SDHCI_CTRL_CD_TEST);
        EXPECT_FALSE(h.R32(SDHCI_PRESENT) & SDHCI_PS_CARD_INSERTED);
        EXPECT_EQ((h.R32(SDHCI_PRESENT) & SDHCI_PS_CARD_DETECT) != 0, card);
        EXPECT_EQ(h.R16(SDHCI_INT_STATUS), card ? SDHCI_INT_CARD_REMOVE : 0);
        if (card) {
            EXPECT_EQ(h.R8(SDHCI_POWER) & SDHCI_POWER_ON, 0);
            EXPECT_EQ(h.R16(SDHCI_CLOCK) & SDHCI_CLOCK_CARD_EN, 0);
        }
        /* no card, no power, no answer */
        h.W8(SDHCI_POWER, 0x0f);
        EXPECT_EQ(h.R8(SDHCI_POWER), 0x0e);
        EXPECT_EQ(sd_cmd(&h, 13, 0, SD_RSP_R1), SD_TIMEOUT);
        h.W16(SDHCI_INT_STATUS, 0xffff);
        h.W8(SDHCI_HOST_CONTROL, SDHCI_CTRL_CD_TEST | SDHCI_CTRL_CD_TEST_LEVEL);
        EXPECT_TRUE(h.R32(SDHCI_PRESENT) & SDHCI_PS_CARD_INSERTED);
        EXPECT_EQ(h.R16(SDHCI_INT_STATUS), SDHCI_INT_CARD_INSERT);
        h.W16(SDHCI_INT_STATUS, 0xffff);
        /* back to the pin */
        h.W8(SDHCI_HOST_CONTROL, 0);
        EXPECT_EQ((h.R32(SDHCI_PRESENT) & SDHCI_PS_CARD_INSERTED) != 0, card);
        EXPECT_EQ(h.R16(SDHCI_INT_STATUS), card ? 0 : SDHCI_INT_CARD_REMOVE);
        /* an empty slot powered by the test level still has nobody to
           answer */
        if (!card) {
            h.W8(SDHCI_HOST_CONTROL,
                 SDHCI_CTRL_CD_TEST | SDHCI_CTRL_CD_TEST_LEVEL);
            h.W8(SDHCI_POWER, 0x0f);
            EXPECT_EQ(h.R8(SDHCI_POWER), 0x0f);
            EXPECT_EQ(sd_cmd(&h, 0, 0, SD_RSP_NONE), SD_OK);
            EXPECT_EQ(sd_cmd(&h, 8, 0x1aa, SD_RSP_R1), SD_TIMEOUT);
        }
    });
}


TEST(sdhci, pio)
{
    /* Buffer Read/Write Ready per block, Buffer Read/Write Enable while it
       may be accessed, the inhibits while the transfer runs, the block
       count counting down (2.2.3, 2.2.9, 1.7) */
    each_card([](SdhciHost &h, SdCard &c) {
        uint8_t *buf = sd_alloc(3 * 512);
        h.W16(SDHCI_BLOCK_SIZE, 512);
        start_read(h, c, 4, 3, SDHCI_TRNS_READ | SDHCI_TRNS_MULTI |
                               SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_ACMD12);
        for (int b = 0; b < 3; b++) {
            ASSERT_TRUE(h.WaitInt(SDHCI_INT_BUF_RD_READY, nullptr));
            uint32_t ps = h.R32(SDHCI_PRESENT);
            EXPECT_EQ(ps & (SDHCI_PS_DAT_INHIBIT | SDHCI_PS_READ_ACTIVE |
                            SDHCI_PS_BUF_RD | SDHCI_PS_CMD_INHIBIT),
                      SDHCI_PS_DAT_INHIBIT | SDHCI_PS_READ_ACTIVE |
                      SDHCI_PS_BUF_RD);
            EXPECT_EQ(h.R16(SDHCI_BLOCK_COUNT), 2 - b);
            h.W16(SDHCI_INT_STATUS, SDHCI_INT_BUF_RD_READY);
            /* 8, 16 and 32 bit cycles all take their width */
            uint8_t *p = buf + b * 512;
            for (int i = 0; i < 128; i++, p++)
                *p = h.R8(SDHCI_BUFFER);
            for (int i = 128; i < 256; i += 2, p += 2)
                put_le16(p, h.R16(SDHCI_BUFFER));
            for (int i = 256; i < 512; i += 4, p += 4)
                put_le32(p, h.R32(SDHCI_BUFFER));
        }
        ASSERT_TRUE(h.WaitInt(SDHCI_INT_XFER_COMPLETE, nullptr));
        EXPECT_TRUE(sd_is_pattern(buf, 4, 3));
        EXPECT_EQ(h.R32(SDHCI_PRESENT) & 0x0f07, 0u);
        EXPECT_EQ(R1_STATE(h.R32(SDHCI_RESPONSE + 12)), STATE_DATA);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
        if (!(h.R32(SDHCI_PRESENT) & SDHCI_PS_WRITE_ENABLED))
            return;
        /* a write: Buffer Write Enable until the last block is in */
        for (int i = 0; i < 2 * 512; i++)
            buf[i] = i * 3;
        h.W16(SDHCI_INT_STATUS, 0xffff);
        h.W16(SDHCI_BLOCK_COUNT, 2);
        h.W16(SDHCI_TRANSFER_MODE, SDHCI_TRNS_MULTI | SDHCI_TRNS_BLK_CNT_EN |
                                   SDHCI_TRNS_ACMD12);
        h.W32(SDHCI_ARGUMENT, c.Addr(40));
        h.W16(SDHCI_COMMAND, 25 << 8 | 0x3a);
        for (int b = 0; b < 2; b++) {
            ASSERT_TRUE(h.WaitInt(SDHCI_INT_BUF_WR_READY, nullptr));
            EXPECT_TRUE(h.R32(SDHCI_PRESENT) & SDHCI_PS_BUF_WR);
            EXPECT_TRUE(h.R32(SDHCI_PRESENT) & SDHCI_PS_WRITE_ACTIVE);
            h.W16(SDHCI_INT_STATUS, SDHCI_INT_BUF_WR_READY);
            for (int i = 0; i < 512; i += 4)
                h.W32(SDHCI_BUFFER, le32(buf + b * 512 + i));
        }
        ASSERT_TRUE(h.WaitInt(SDHCI_INT_XFER_COMPLETE, nullptr));
        EXPECT_FALSE(h.R16(SDHCI_INT_STATUS) & SDHCI_INT_BUF_WR_READY);
        uint8_t *in = sd_alloc(2 * 512);
        ASSERT_EQ(sd_rw(c, true, 40, 2, in), SD_OK);
        EXPECT_EQ(memcmp(in, buf, 2 * 512), 0);
    });
}


TEST(sdhci, sdma_boundary)
{
    /* SDMA stops at every buffer boundary with a DMA Interrupt, the address
       register pointing where it stopped, and carries on when the top byte
       of the address is written (2.2.1, 2.2.2) */
    each_card([](SdhciHost &h, SdCard &c) {
        uint8_t *buf = sd_alloc(3 * 4096, 0, 4096);
        uintptr_t start = (uintptr_t)buf + 4096 - 1024;
        h.W32(SDHCI_DMA_ADDRESS, (uint32_t)start);
        h.W16(SDHCI_BLOCK_SIZE, 512); /* 4 KB boundaries */
        start_read(h, c, 0, 8, SDHCI_TRNS_DMA | SDHCI_TRNS_READ |
                               SDHCI_TRNS_MULTI | SDHCI_TRNS_BLK_CNT_EN |
                               SDHCI_TRNS_ACMD12);
        uint16_t st;
        ASSERT_TRUE(h.WaitInt(SDHCI_INT_DMA_END | SDHCI_INT_XFER_COMPLETE,
                              &st));
        EXPECT_EQ(st, SDHCI_INT_CMD_COMPLETE | SDHCI_INT_DMA_END);
        EXPECT_EQ(h.R32(SDHCI_DMA_ADDRESS), (uint32_t)(start + 1024));
        EXPECT_TRUE(h.R32(SDHCI_PRESENT) & SDHCI_PS_DAT_INHIBIT);
        h.W16(SDHCI_INT_STATUS, SDHCI_INT_DMA_END);
        /* a write of the low half does not restart it */
        h.W16(SDHCI_DMA_ADDRESS, (uint16_t)(start + 1024));
        t_delay_us(100);
        EXPECT_FALSE(h.R16(SDHCI_INT_STATUS) & SDHCI_INT_XFER_COMPLETE);
        h.W32(SDHCI_DMA_ADDRESS, (uint32_t)(start + 1024));
        ASSERT_TRUE(h.WaitInt(SDHCI_INT_XFER_COMPLETE, &st));
        EXPECT_FALSE(st & SDHCI_INT_ERROR);
        EXPECT_TRUE(sd_is_pattern((uint8_t *)start, 0, 8));
        EXPECT_EQ(h.R32(SDHCI_DMA_ADDRESS), (uint32_t)(start + 4096));
    });
}


TEST(sdhci, adma2)
{
    /* Tran, Link, Nop and the reserved action, which is a Nop; Int once the
       line's data has moved; 32 and 64 bit descriptors (1.13) */
    each_card([](SdhciHost &h, SdCard &c) {
        for (int wide = 0; wide < 2; wide++) {
            uint32_t ds = wide ? 12 : 8;
            uint8_t *data = sd_alloc(3 * 1024);
            uint8_t *t1 = sd_alloc(4 * ds, 0, 8);
            uint8_t *t2 = sd_alloc(4 * ds, 0, 8);
            adma_line(t1, ADMA_VALID | ADMA_TRAN, 1024,
                      (uintptr_t)data + 2048, wide);
            adma_line(t1 + ds, ADMA_VALID | ADMA_NOP, 0, 0, wide);
            adma_line(t1 + 2 * ds, ADMA_VALID | ADMA_LINK, 0,
                      (uintptr_t)t2, wide);
            adma_line(t2, ADMA_VALID | ADMA_RSVD, 0, 0, wide);
            adma_line(t2 + ds, ADMA_VALID | ADMA_TRAN | ADMA_INT, 1024,
                      (uintptr_t)data, wide);
            adma_line(t2 + 2 * ds, ADMA_VALID | ADMA_NOP | ADMA_END, 0, 0,
                      wide);
            uint16_t st = adma_read(h, c, (uintptr_t)t1, 4, wide);
            EXPECT_EQ(st, SDHCI_INT_CMD_COMPLETE | SDHCI_INT_XFER_COMPLETE |
                          SDHCI_INT_DMA_END);
            EXPECT_TRUE(sd_is_pattern(data + 2048, 0, 2));
            EXPECT_TRUE(sd_is_pattern(data, 2, 2));
            EXPECT_EQ(R1_STATE(h.R32(SDHCI_RESPONSE + 12)), STATE_DATA);
            h.W16(SDHCI_INT_STATUS, 0xffff);
        }
    });
}


TEST(sdhci, adma2_errors)
{
    /* ADMA Error with the state the engine stopped in and the descriptor
       address around the failing line; no Data CRC error (2.2.29) */
    each_card([](SdhciHost &h, SdCard &c) {
        uint8_t *data = sd_alloc(2 * 512);
        uint8_t *t = sd_alloc(4 * 8, 0, 8);
        auto check = [&](uint8_t state, uint64_t addr) {
            uint16_t st = adma_read(h, c, (uintptr_t)t, 2, false);
            EXPECT_EQ(st & (SDHCI_INT_ERROR | SDHCI_INT_XFER_COMPLETE),
                      SDHCI_INT_ERROR);
            EXPECT_EQ(h.R16(SDHCI_ERR_STATUS), SDHCI_ERR_ADMA);
            EXPECT_EQ(h.R8(SDHCI_ADMA_ERROR), state);
            EXPECT_EQ(h.R32(SDHCI_ADMA_ADDRESS), (uint32_t)addr);
            h.Reset(SDHCI_RESET_CMD | SDHCI_RESET_DATA);
            h.W16(SDHCI_INT_STATUS, 0xffff);
            h.W16(SDHCI_ERR_STATUS, 0xffff);
            sd_cmd(&h, 12, 0, SD_RSP_R1B);
            EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
        };
        uintptr_t base = (uintptr_t)t;
        /* Valid clear: ST_FDS, pointing at that line */
        adma_line(t, ADMA_VALID | ADMA_TRAN, 512, (uintptr_t)data, false);
        adma_line(t + 8, ADMA_TRAN, 512, (uintptr_t)data + 512, false);
        check(1, base + 8);
        /* a line that cannot be fetched */
        adma_line(t, ADMA_VALID | ADMA_LINK, 0, not_ram(), false);
        check(1, not_ram());
        /* data where there is no RAM: ST_TFR, pointing past the line */
        adma_line(t, ADMA_VALID | ADMA_TRAN | ADMA_END, 1024, not_ram(),
                  false);
        check(3, base + 8);
        /* a table shorter than the transfer: ST_STOP, length mismatch */
        adma_line(t, ADMA_VALID | ADMA_TRAN | ADMA_END, 512,
                  (uintptr_t)data, false);
        check(0x04, base + 8);
        /* longer: ST_TFR, length mismatch */
        adma_line(t, ADMA_VALID | ADMA_TRAN, 1024, (uintptr_t)data, false);
        adma_line(t + 8, ADMA_VALID | ADMA_TRAN | ADMA_END, 512,
                  (uintptr_t)data, false);
        check(0x07, base + 16);
    });
}


TEST(sdhci, auto_commands)
{
    /* the Auto CMD12 and Auto CMD23 responses go in REP[127:96], the
       command's own in REP[31:0] (2.2.7); forcing an Auto CMD error sets
       the Auto CMD Error interrupt for bits 0 to 4 only (2.2.27) */
    each_card([](SdhciHost &h, SdCard &c) {
        SdResult r;
        uint8_t *buf = sd_alloc(2 * 512);
        ASSERT_EQ(sd_rw(c, true, 0, 2, buf, SD_ADMA2, &r, true), SD_OK);
        EXPECT_EQ(R1_STATE(r.resp), STATE_TRAN);
        EXPECT_EQ(R1_STATE(h.R32(SDHCI_RESPONSE + 12)), STATE_TRAN);
        EXPECT_FALSE(h.R32(SDHCI_RESPONSE + 12) & R1_ERROR);
        EXPECT_TRUE(sd_is_pattern(buf, 0, 2));
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
        ASSERT_EQ(sd_rw(c, true, 0, 2, buf, SD_PIO, &r), SD_OK);
        EXPECT_EQ(R1_STATE(h.R32(SDHCI_RESPONSE + 12)), STATE_DATA);
        EXPECT_EQ(R1_STATE(h.R32(SDHCI_RESPONSE)), STATE_TRAN);

        h.W16(SDHCI_ERR_STATUS, 0xffff);
        h.W16(SDHCI_FORCE_AUTO_CMD, 0x80);
        EXPECT_EQ(h.R16(SDHCI_AUTO_CMD_STATUS), 0x80);
        EXPECT_EQ(h.R16(SDHCI_ERR_STATUS), 0);
        h.W16(SDHCI_FORCE_AUTO_CMD, 0xff62);
        EXPECT_EQ(h.R16(SDHCI_AUTO_CMD_STATUS), 0x82);
        EXPECT_EQ(h.R16(SDHCI_ERR_STATUS), SDHCI_ERR_AUTO_CMD);
    });
}


TEST(sdhci, transfer_protected)
{
    /* transfer mode, block size and block count ignore writes while the
       transfer runs (2.2.2, 2.2.3, 2.2.5) */
    each_card([](SdhciHost &h, SdCard &c) {
        set_disk(c);
        tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_HOLD);
        h.W16(SDHCI_BLOCK_SIZE, 512);
        uint16_t mode = SDHCI_TRNS_READ | SDHCI_TRNS_MULTI |
                        SDHCI_TRNS_BLK_CNT_EN | SDHCI_TRNS_ACMD12;
        start_read(h, c, 0, 2, mode);
        ASSERT_TRUE(t_wait_until([] {
            return tc_block_read(TEST_CONTROL_BLOCK_HELD) == 1;
        }));
        EXPECT_TRUE(h.R32(SDHCI_PRESENT) & SDHCI_PS_DAT_INHIBIT);
        h.W16(SDHCI_TRANSFER_MODE, 0);
        h.W16(SDHCI_BLOCK_SIZE, 8);
        h.W16(SDHCI_BLOCK_COUNT, 100);
        EXPECT_EQ(h.R16(SDHCI_TRANSFER_MODE), mode);
        EXPECT_EQ(h.R16(SDHCI_BLOCK_SIZE), 512);
        EXPECT_EQ(h.R16(SDHCI_BLOCK_COUNT), 2);
        tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_SYNC);
        tc_block_write(TEST_CONTROL_BLOCK_RELEASE, 1);
        uint8_t *buf = sd_alloc(2 * 512);
        for (int b = 0; b < 2; b++) {
            ASSERT_TRUE(h.WaitInt(SDHCI_INT_BUF_RD_READY, nullptr));
            h.W16(SDHCI_INT_STATUS, SDHCI_INT_BUF_RD_READY);
            for (int i = 0; i < 512; i += 4)
                put_le32(buf + b * 512 + i, h.R32(SDHCI_BUFFER));
        }
        ASSERT_TRUE(h.WaitInt(SDHCI_INT_XFER_COMPLETE, nullptr));
        EXPECT_TRUE(sd_is_pattern(buf, 0, 2));

        /* a data reset while a block is with the disk gives it up; the card
           waits for CMD12 */
        tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_HOLD);
        uint32_t cancels = tc_block_read(TEST_CONTROL_BLOCK_CANCELS);
        start_read(h, c, 0, 2, mode);
        ASSERT_TRUE(t_wait_until([] {
            return tc_block_read(TEST_CONTROL_BLOCK_HELD) == 1;
        }));
        ASSERT_TRUE(h.Reset(SDHCI_RESET_DATA));
        EXPECT_EQ(tc_block_read(TEST_CONTROL_BLOCK_CANCELS) - cancels, 1u);
        EXPECT_FALSE(h.R32(SDHCI_PRESENT) & SDHCI_PS_DAT_INHIBIT);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_DATA);
        EXPECT_EQ(sd_cmd(&h, 12, 0, SD_RSP_R1B), SD_OK);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
        tc_block_write(TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_SYNC);
    });
}


TEST(sdhci, block_size_zero)
{
    /* a block size of zero transfers no data (2.2.2) */
    each_card([](SdhciHost &h, SdCard &c) {
        h.W16(SDHCI_BLOCK_SIZE, 0);
        start_read(h, c, 0, 1, SDHCI_TRNS_READ);
        uint16_t st;
        ASSERT_TRUE(h.WaitInt(SDHCI_INT_XFER_COMPLETE, &st));
        EXPECT_EQ(st, SDHCI_INT_CMD_COMPLETE | SDHCI_INT_XFER_COMPLETE);
        EXPECT_FALSE(h.R32(SDHCI_PRESENT) & SDHCI_PS_DAT_INHIBIT);
        sd_cmd(&h, 12, 0, SD_RSP_R1B);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
    });
}


TEST(sdhci, data_errors)
{
    /* a block the disk fails, and a status enable that hides it */
    each_card([](SdhciHost &h, SdCard &c) {
        set_disk(c);
        uint8_t *buf = sd_alloc(512);
        tc_block_write(TEST_CONTROL_BLOCK_FAIL, 1);
        EXPECT_EQ(sd_rw(c, true, 0, 1, buf, SD_DMA), SD_DATA_ERROR);
        sd_cmd(&h, 12, 0, SD_RSP_R1B);
        tc_block_write(TEST_CONTROL_BLOCK_FAIL, 1);
        h.W16(SDHCI_BLOCK_SIZE, 512);
        start_read(h, c, 0, 1, SDHCI_TRNS_READ);
        ASSERT_TRUE(h.WaitInt(SDHCI_INT_CMD_COMPLETE, nullptr));
        t_delay_us(100);
        SNAP("error", "%04x %04x", h.R16(SDHCI_INT_STATUS),
             h.R16(SDHCI_ERR_STATUS));
        h.Reset(SDHCI_RESET_CMD | SDHCI_RESET_DATA);
        h.W16(SDHCI_ERR_ENABLE, 0);
        h.W16(SDHCI_FORCE_ERR, 0xffff);
        EXPECT_EQ(h.R16(SDHCI_ERR_STATUS), 0);
        h.W16(SDHCI_ERR_ENABLE, 0xffff);
        h.W16(SDHCI_FORCE_ERR, 0xffff);
        EXPECT_EQ(h.R16(SDHCI_ERR_STATUS), 0xf7ff);
        h.W16(SDHCI_ERR_ENABLE, 0x0001);
        EXPECT_EQ(h.R16(SDHCI_ERR_STATUS), 0x0001);
    });
}


TEST(sdhci, interrupt)
{
    /* the line follows status and signal enable; Slot Interrupt Status
       says the slot is signalling (2.2.21, 2.2.33) */
    each_card([](SdhciHost &h, SdCard &c) {
        static volatile int count;
        static SdhciHost *host;
        count = 0;
        host = &h;
        IrqHandler fn = [](void *) {
            count = count + 1;
            host->W16(SDHCI_INT_STATUS, SDHCI_INT_CMD_COMPLETE);
        };
        bool routed = h.fn != nullptr ? pci_intx_route(h.fn, fn, nullptr)
                                      : irq_route(h.dev, 0, fn, nullptr);
        if (!routed) {
            t_log("the interrupt reaches no controller the suite drives");
            return;
        }
        h.W16(SDHCI_SIGNAL_ENABLE, SDHCI_INT_CMD_COMPLETE);
        h.W32(SDHCI_ARGUMENT, (uint32_t)c.rca << 16);
        h.W16(SDHCI_COMMAND, 13 << 8 | 0x1a);
        EXPECT_EQ(h.R16(SDHCI_SLOT_INT), 1);
        arch_irq_enable();
        EXPECT_TRUE(t_wait_until([] {return count > 0;}));
        arch_irq_disable();
        EXPECT_EQ(count, 1);
        EXPECT_EQ(h.R16(SDHCI_SLOT_INT), 0);
        h.W16(SDHCI_SIGNAL_ENABLE, 0);
        if (h.fn != nullptr)
            pci_intx_unroute(h.fn);
        else
            irq_unroute(h.dev, 0);
    });
}
