/*
 * DesignWare mobile storage host tests
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

/* The DesignWare mobile storage host. Its databook is not public, so this
   holds it to what drivers rely on -- the command and interrupt
   handshakes, the FIFO, the internal DMA controller's descriptors -- and
   snapshots the rest. */

namespace {

template <typename F>
void each_host(F fn)
{
    t_on_cleanup(sd_cleanup);
    int n = 0;
    for (int i = 0; i < sd_host_count(); i++) {
        SdHost *h = sd_host(i);
        if (h->Kind() != SD_HOST_DW_MMC)
            continue;
        t_log("dma %d", static_cast<DwMmcHost *>(h)->dma);
        fn(*static_cast<DwMmcHost *>(h));
        n++;
    }
    if (n == 0)
        t_skip("no DesignWare host");
}

template <typename F>
void each_card(F fn)
{
    each_host([&](DwMmcHost &h) {
        SdCard c;
        if (!h.CardPresent() || !sd_card_init(&h, &c))
            return;
        fn(h, c);
    });
}

uint64_t not_ram()
{
    const HwDevice *tc = test_control_device();
    const HwRegion *r = tc ? hw_region(tc, HW_MMIO) : nullptr;
    if (r == nullptr)
        t_skip("needs the test-control device");
    return r->base;
}

/* Start a DMA read of 'blocks' from sector 0 with the descriptors at
   'table'; the raw status once DTO or an error came. */
uint32_t dma_read(DwMmcHost &h, const SdCard &c, uint64_t table,
                  uint32_t blocks, uint32_t bmod = 0)
{
    h.W(DW_RINTSTS, 0xffffffff);
    h.W(h.IdstsReg(), 0xffffffff);
    h.W(DW_BLKSIZ, 512);
    h.W(DW_BYTCNT, 512 * blocks);
    h.W(DW_CTRL, h.R(DW_CTRL) | DW_CTRL_USE_IDMAC);
    h.W(DW_BMOD, DW_BMOD_DE | bmod);
    h.W(DW_DBADDRL, (uint32_t)table);
    if (h.dma == 64)
        h.W(DW_DBADDRU, (uint32_t)(table >> 32));
    h.W(DW_CMDARG, c.Addr(0));
    h.W(DW_CMD, (blocks > 1 ? 18 : 17) | DW_CMD_START | DW_CMD_USE_HOLD |
                DW_CMD_RESP_EXP | DW_CMD_CHECK_CRC | DW_CMD_DAT_EXP |
                (blocks > 1 ? DW_CMD_SEND_STOP : 0));
    uint32_t st = 0;
    t_wait_until([&] {
        st = h.R(DW_RINTSTS);
        return (st & (DW_INT_DTO | DW_INT_DATA_ERRORS)) != 0 ||
               (h.R(h.IdstsReg()) & (DW_IDMAC_DU | DW_IDMAC_FBE)) != 0;
    }, 1000000);
    return st;
}

/* one descriptor of either width */
void desc(DwMmcHost &h, uint8_t *d, uint32_t des0, uint32_t size1,
          uint32_t size2, uint64_t buf1, uint64_t buf2)
{
    put_le32(d, des0);
    if (h.dma == 64) {
        put_le32(d + 4, 0);
        put_le32(d + 8, size1 | size2 << 13);
        put_le32(d + 12, 0);
        put_le32(d + 16, (uint32_t)buf1);
        put_le32(d + 20, (uint32_t)(buf1 >> 32));
        put_le32(d + 24, (uint32_t)buf2);
        put_le32(d + 28, (uint32_t)(buf2 >> 32));
    } else {
        put_le32(d + 4, size1 | size2 << 13);
        put_le32(d + 8, (uint32_t)buf1);
        put_le32(d + 12, (uint32_t)buf2);
    }
}

}


FRESH_TEST(dw_mmc, reset_values)
{
    each_host([](DwMmcHost &h) {
        SNAP("ids", "%08x %08x", h.R(DW_VERID), h.R(DW_HCON));
        /* the receive watermark resets to one below the FIFO depth, which
           is how a driver learns the depth */
        EXPECT_EQ(get_bits(h.R(DW_FIFOTH), 16, 12), 31u);
        EXPECT_EQ(h.R(DW_TMOUT), 0xffffff40u);
        EXPECT_EQ(h.R(DW_BLKSIZ), 512u);
        EXPECT_EQ(h.R(DW_BYTCNT), 512u);
        EXPECT_EQ(h.R(DW_RST_N), 1u);
        EXPECT_EQ(h.R(DW_CTRL), 0u);
        EXPECT_EQ(h.R(DW_PWREN), 0u);
        EXPECT_EQ(h.R(DW_INTMASK), 0u);
        EXPECT_EQ(h.R(DW_RINTSTS), 0u);
        /* card detect is active low */
        EXPECT_EQ(h.R(DW_CDETECT) & 1, 0u);
        SNAP("status", "%08x %08x", h.R(DW_STATUS), h.R(DW_WRTPRT));
    });
}


TEST(dw_mmc, command_done)
{
    /* Command Done for every command, with Response Timeout when one that
       expects a response gets none; without power the card answers nothing */
    each_card([](DwMmcHost &h, SdCard &c) {
        uint32_t st;
        h.W(DW_RINTSTS, 0xffffffff);
        h.W(DW_CMDARG, (uint32_t)c.rca << 16);
        h.W(DW_CMD, 13 | DW_CMD_START | DW_CMD_RESP_EXP | DW_CMD_CHECK_CRC);
        ASSERT_TRUE(h.WaitInt(DW_INT_CMD_DONE, &st));
        EXPECT_EQ(st, DW_INT_CMD_DONE);
        EXPECT_FALSE(h.R(DW_CMD) & DW_CMD_START);
        EXPECT_EQ(R1_STATE(h.R(DW_RESP0)), STATE_TRAN);
        SNAP("status", "%08x", h.R(DW_STATUS) & ~(0x1fffu << 17));

        h.W(DW_RINTSTS, 0xffffffff);
        h.W(DW_CMDARG, (uint32_t)(c.rca + 1) << 16);
        h.W(DW_CMD, 13 | DW_CMD_START | DW_CMD_RESP_EXP | DW_CMD_CHECK_CRC);
        ASSERT_TRUE(h.WaitInt(DW_INT_CMD_DONE, &st));
        EXPECT_EQ(st, DW_INT_CMD_DONE | DW_INT_RTO);

        /* a deselect expects no response, and gets none */
        h.W(DW_RINTSTS, 0xffffffff);
        h.W(DW_CMDARG, 0);
        h.W(DW_CMD, 7 | DW_CMD_START);
        ASSERT_TRUE(h.WaitInt(DW_INT_CMD_DONE, &st));
        EXPECT_EQ(st, DW_INT_CMD_DONE);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_STBY);

        /* a clock update loads the clock and raises nothing */
        h.W(DW_RINTSTS, 0xffffffff);
        ASSERT_TRUE(h.ClockUpdate());
        EXPECT_EQ(h.R(DW_RINTSTS), 0u);

        /* power off resets the card */
        h.W(DW_PWREN, 0);
        EXPECT_EQ(c.Status(), 0xffffffffu);
        h.W(DW_PWREN, 1);
        sd_cmd(&h, 0, 0, SD_RSP_NONE);
        EXPECT_EQ(sd_cmd(&h, 8, 0x1aa, SD_RSP_R1), c.mmc ? SD_TIMEOUT
                                                        : SD_OK);
    });
}


TEST(dw_mmc, long_response)
{
    /* all 128 bits, the CRC byte included, RESP3 most significant */
    each_card([](DwMmcHost &h, SdCard &c) {
        sd_cmd(&h, 7, 0, SD_RSP_NONE);
        h.W(DW_RINTSTS, 0xffffffff);
        h.W(DW_CMDARG, (uint32_t)c.rca << 16);
        h.W(DW_CMD, 9 | DW_CMD_START | DW_CMD_RESP_EXP | DW_CMD_RESP_LONG |
                    DW_CMD_CHECK_CRC);
        ASSERT_TRUE(h.WaitInt(DW_INT_CMD_DONE, nullptr));
        EXPECT_EQ(h.R(DW_RESP0 + 12) >> 24, (uint32_t)c.csd[0]);
        EXPECT_EQ((h.R(DW_RESP0) >> 8) & 0xff, (uint32_t)c.csd[14]);
        /* the CRC7 of the 120 bits before it, then the end bit */
        uint8_t crc = 0;
        for (int i = 0; i < 15; i++) {
            crc ^= c.csd[i];
            for (int b = 0; b < 8; b++)
                crc = crc & 0x80 ? (uint8_t)(crc << 1 ^ 0x12)
                                 : (uint8_t)(crc << 1);
        }
        EXPECT_EQ(h.R(DW_RESP0) & 0xff, (uint32_t)(crc | 1));
    });
}


TEST(dw_mmc, fifo)
{
    /* PIO through the FIFO: Receive and Transmit Data Request by the
       watermarks, the FIFO count in STATUS, the byte counters */
    each_card([](DwMmcHost &h, SdCard &c) {
        uint8_t *buf = sd_alloc(4 * 512);
        h.W(DW_FIFOTH, (15u << 16) | 16);
        h.W(DW_RINTSTS, 0xffffffff);
        h.W(DW_CTRL, h.R(DW_CTRL) & ~DW_CTRL_USE_IDMAC);
        h.W(DW_BLKSIZ, 512);
        h.W(DW_BYTCNT, 4 * 512);
        h.W(DW_CMDARG, c.Addr(8));
        h.W(DW_CMD, 18 | DW_CMD_START | DW_CMD_USE_HOLD | DW_CMD_RESP_EXP |
                    DW_CMD_CHECK_CRC | DW_CMD_DAT_EXP | DW_CMD_SEND_STOP);
        uint32_t st;
        ASSERT_TRUE(h.WaitInt(DW_INT_RXDR, &st));
        uint32_t status = h.R(DW_STATUS);
        EXPECT_EQ(get_bits(status, 17, 13), 32u);
        EXPECT_TRUE(status & (1u << 3));      /* full */
        EXPECT_TRUE(status & (1u << 0));      /* past the RX watermark */
        EXPECT_TRUE(status & (1u << 10));     /* data state machine busy */
        uint32_t got = 0;
        while (got < 4 * 512) {
            h.W(DW_RINTSTS, DW_INT_RXDR);
            uint32_t words = get_bits(h.R(DW_STATUS), 17, 13);
            if (words == 0) {
                ASSERT_TRUE(h.WaitInt(DW_INT_RXDR | DW_INT_DTO, nullptr));
                continue;
            }
            for (; words > 0; words--, got += 4)
                put_le32(buf + got, h.R(DW_DATA));
        }
        ASSERT_TRUE(h.WaitInt(DW_INT_DTO, &st));
        EXPECT_TRUE(st & DW_INT_ACD);
        EXPECT_FALSE(st & DW_INT_DATA_ERRORS);
        EXPECT_TRUE(sd_is_pattern(buf, 8, 4));
        EXPECT_EQ(h.R(DW_TCBCNT), 4u * 512);
        EXPECT_EQ(h.R(DW_TBBCNT), 4u * 512);
        EXPECT_EQ(R1_STATE(h.R(DW_RESP0 + 4)), STATE_DATA);
        EXPECT_TRUE(h.R(DW_STATUS) & (1u << 2)); /* empty */
        /* reading the empty FIFO is an underrun */
        h.R(DW_DATA);
        EXPECT_TRUE(h.R(DW_RINTSTS) & DW_INT_FRUN);
        /* a byte count that is no multiple of the block size: the short
           last block still reads what was asked */
        h.W(DW_RINTSTS, 0xffffffff);
        h.W(DW_BYTCNT, 512 + 100);
        h.W(DW_CMDARG, c.Addr(8));
        h.W(DW_CMD, 18 | DW_CMD_START | DW_CMD_USE_HOLD | DW_CMD_RESP_EXP |
                    DW_CMD_CHECK_CRC | DW_CMD_DAT_EXP | DW_CMD_SEND_STOP);
        ASSERT_TRUE(h.WaitInt(DW_INT_CMD_DONE, nullptr));
        memset(buf, 0, 1024);
        got = 0;
        while (!(h.R(DW_RINTSTS) & DW_INT_DTO) ||
               get_bits(h.R(DW_STATUS), 17, 13) != 0) {
            h.W(DW_RINTSTS, DW_INT_RXDR);
            uint32_t words = get_bits(h.R(DW_STATUS), 17, 13);
            for (; words > 0; words--, got += 4)
                put_le32(buf + got, h.R(DW_DATA));
        }
        EXPECT_EQ(got, 612u);
        EXPECT_EQ(h.R(DW_TCBCNT), 612u);
        EXPECT_TRUE(sd_is_pattern(buf, 8, 1));
        bool same = true;
        for (int i = 0; i < 100; i++)
            same = same && buf[512 + i] == TEST_BLOCK_PATTERN(9, i);
        EXPECT_TRUE(same);
    });
}


TEST(dw_mmc, idmac)
{
    /* chained and ring descriptors: OWN handed back, Receive Interrupt
       unless DIC, the end of ring going back to the base */
    each_card([](DwMmcHost &h, SdCard &c) {
        if (h.dma == 0)
            return;
        uint32_t ds = h.dma == 64 ? 32 : 16;
        uint8_t *data = sd_alloc(4 * 512);
        uint8_t *t = sd_alloc(4 * ds, 0, 8);
        /* chained: two lines, the second with DIC */
        desc(h, t, DW_DES0_OWN | DW_DES0_CH | DW_DES0_FS, 1024, 0,
             (uintptr_t)data + 1024, (uintptr_t)(t + ds));
        desc(h, t + ds, DW_DES0_OWN | DW_DES0_CH | DW_DES0_LD | DW_DES0_DIC,
             1024, 0, (uintptr_t)data, 0);
        uint32_t st = dma_read(h, c, (uintptr_t)t, 4);
        EXPECT_EQ(st & (DW_INT_DTO | DW_INT_DATA_ERRORS), DW_INT_DTO);
        EXPECT_TRUE(sd_is_pattern(data + 1024, 0, 2));
        EXPECT_TRUE(sd_is_pattern(data, 2, 2));
        EXPECT_FALSE(le32(t) & DW_DES0_OWN);
        EXPECT_FALSE(le32(t + ds) & DW_DES0_OWN);
        EXPECT_EQ(h.R(h.IdstsReg()) & (DW_IDMAC_RI | DW_IDMAC_TI),
                  DW_IDMAC_RI);
        SNAP("idsts", "%08x", h.R(h.IdstsReg()));

        /* ring: two buffers a descriptor, the second descriptor a skip
           length away, End of Ring on it */
        memset(data, 0, 4 * 512);
        memset(t, 0, 4 * ds);
        desc(h, t, DW_DES0_OWN | DW_DES0_FS, 512, 512,
             (uintptr_t)data + 3 * 512, (uintptr_t)data + 2 * 512);
        desc(h, t + ds + 8, DW_DES0_OWN | DW_DES0_LD | DW_DES0_ER, 512, 512,
             (uintptr_t)data + 512, (uintptr_t)data);
        st = dma_read(h, c, (uintptr_t)t, 4, 2 << 2);
        EXPECT_EQ(st & (DW_INT_DTO | DW_INT_DATA_ERRORS), DW_INT_DTO);
        EXPECT_TRUE(sd_is_pattern(data + 3 * 512, 0, 1));
        EXPECT_TRUE(sd_is_pattern(data + 2 * 512, 1, 1));
        EXPECT_TRUE(sd_is_pattern(data + 512, 2, 1));
        EXPECT_TRUE(sd_is_pattern(data, 3, 1));
        /* the next descriptor is the base again */
        SNAP("dscaddr", "%d", (int)(h.R(h.dma == 64 ? 0x98 : 0x94) -
                                    (uint32_t)(uintptr_t)t));
    });
}


TEST(dw_mmc, idmac_errors)
{
    /* a descriptor the host does not own suspends the controller with
       Descriptor Unavailable until a poll demand; one it cannot fetch is a
       fatal bus error */
    each_card([](DwMmcHost &h, SdCard &c) {
        if (h.dma == 0)
            return;
        uint32_t ds = h.dma == 64 ? 32 : 16;
        uint8_t *data = sd_alloc(512);
        uint8_t *t = sd_alloc(2 * ds, 0, 8);
        desc(h, t, DW_DES0_CH | DW_DES0_FS | DW_DES0_LD, 512, 0,
             (uintptr_t)data, 0);
        dma_read(h, c, (uintptr_t)t, 1);
        EXPECT_TRUE(h.R(h.IdstsReg()) & DW_IDMAC_DU);
        EXPECT_FALSE(h.R(DW_RINTSTS) & DW_INT_DTO);
        put_le32(t, le32(t) | DW_DES0_OWN);
        h.W(DW_PLDMND, 1);
        EXPECT_TRUE(t_wait_until([&] {
            return (h.R(DW_RINTSTS) & DW_INT_DTO) != 0;
        }));
        EXPECT_TRUE(sd_is_pattern(data, 0, 1));
        EXPECT_FALSE(le32(t) & DW_DES0_OWN);

        h.Init();
        sd_card_init(&h, &c);
        dma_read(h, c, not_ram(), 1);
        EXPECT_TRUE(h.R(h.IdstsReg()) & DW_IDMAC_FBE);
        EXPECT_FALSE(h.R(DW_BMOD) & DW_BMOD_DE);
        EXPECT_TRUE(h.R(DW_RINTSTS) & DW_INT_DTO);
        /* the abnormal summary needs its enable */
        EXPECT_FALSE(h.R(h.IdstsReg()) & DW_IDMAC_AI);
    });
}


TEST(dw_mmc, interrupt)
{
    /* the line: the global enable, the mask, MINTSTS the masked status */
    each_card([](DwMmcHost &h, SdCard &c) {
        static volatile int count;
        static DwMmcHost *host;
        count = 0;
        host = &h;
        if (!irq_route(h.dev, 0, [](void *) {
                count = count + 1;
                host->W(DW_RINTSTS, DW_INT_CMD_DONE);
            }, nullptr)) {
            t_log("the interrupt reaches no controller the suite drives");
            return;
        }
        h.W(DW_RINTSTS, 0xffffffff);
        h.W(DW_INTMASK, DW_INT_CMD_DONE);
        h.W(DW_CMDARG, (uint32_t)c.rca << 16);
        h.W(DW_CMD, 13 | DW_CMD_START | DW_CMD_RESP_EXP);
        EXPECT_EQ(h.R(DW_MINTSTS), DW_INT_CMD_DONE);
        arch_irq_enable();
        t_delay_us(1000);
        EXPECT_EQ(count, 0);
        h.W(DW_CTRL, h.R(DW_CTRL) | DW_CTRL_INT_ENABLE);
        EXPECT_TRUE(t_wait_until([] {return count > 0;}));
        arch_irq_disable();
        EXPECT_EQ(count, 1);
        EXPECT_EQ(h.R(DW_MINTSTS), 0u);
        h.W(DW_INTMASK, 0);
        irq_unroute(h.dev, 0);
    });
}


TEST(dw_mmc, resets)
{
    /* the reset bits clear themselves; the FIFO reset empties it */
    each_card([](DwMmcHost &h, SdCard &c) {
        h.W(DW_CTRL, h.R(DW_CTRL) & ~DW_CTRL_USE_IDMAC);
        h.W(DW_RINTSTS, 0xffffffff);
        h.W(DW_BLKSIZ, 512);
        h.W(DW_BYTCNT, 4 * 512);
        h.W(DW_CMDARG, c.Addr(0));
        h.W(DW_CMD, 18 | DW_CMD_START | DW_CMD_RESP_EXP | DW_CMD_DAT_EXP);
        ASSERT_TRUE(h.WaitInt(DW_INT_RXDR, nullptr));
        EXPECT_NE(get_bits(h.R(DW_STATUS), 17, 13), 0u);
        h.W(DW_CTRL, DW_CTRL_FIFO_RESET);
        EXPECT_EQ(h.R(DW_CTRL) & DW_CTRL_RESETS, 0u);
        EXPECT_EQ(get_bits(h.R(DW_STATUS), 17, 13), 0u);
        h.W(DW_CTRL, DW_CTRL_RESET);
        EXPECT_FALSE(h.R(DW_STATUS) & (1u << 10));
        EXPECT_EQ(R1_STATE(c.Status()), STATE_DATA);
        sd_cmd(&h, 12, 0, SD_RSP_R1B);
        EXPECT_EQ(R1_STATE(c.Status()), STATE_TRAN);
    });
}
