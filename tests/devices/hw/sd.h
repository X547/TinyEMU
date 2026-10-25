/*
 * SD host controllers and the cards behind them
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
#pragma once

#include "mmio.h"
#include "pci.h"
#include "registry.h"

/* Commands to SD and eMMC cards over whichever host controller the machine
   has, so that the tests of the cards' command sets run on each: an SD
   host controller by the SD Host Controller Simplified Specification 3.00,
   on PCI or in the device tree, or a DesignWare mobile storage host. */

/* 'n' bits of 'v' from bit 'shift' up */
inline uint32_t get_bits(uint32_t v, int shift, int n)
{
    return (v >> shift) & ((1u << n) - 1);
}

/* card status bits (Physical Layer Simplified Specification 3.01, 4.10.1) */
#define R1_OUT_OF_RANGE     (1u << 31)
#define R1_ADDRESS_ERROR    (1u << 30)
#define R1_BLOCK_LEN_ERROR  (1u << 29)
#define R1_WP_VIOLATION     (1u << 26)
#define R1_ILLEGAL_COMMAND  (1u << 22)
#define R1_CARD_ECC_FAILED  (1u << 21)
#define R1_CC_ERROR         (1u << 20)
#define R1_ERROR            (1u << 19)
#define R1_READY_FOR_DATA   (1u << 8)
#define R1_SWITCH_ERROR     (1u << 7)
#define R1_APP_CMD          (1u << 5)
#define R1_STATE(r)         ((int)(((r) >> 9) & 0xf))

#define STATE_IDLE  0
#define STATE_READY 1
#define STATE_IDENT 2
#define STATE_STBY  3
#define STATE_TRAN  4
#define STATE_DATA  5
#define STATE_RCV   6
#define STATE_SLP   10

#define OCR_WINDOW   0x00ff8000u
#define OCR_HCS      (1u << 30)
#define OCR_READY    (1u << 31)
#define OCR_SECTOR   (2u << 29)

enum SdRsp {
    SD_RSP_NONE,
    SD_RSP_R1,   /* 48 bits, index and CRC checked */
    SD_RSP_R1B,  /* and busy on DAT0 */
    SD_RSP_R2,   /* 136 bits */
    SD_RSP_R3,   /* 48 bits, neither checked */
};

/* How a host moves the data of a command. */
enum SdMode {
    SD_PIO,
    SD_DMA,    /* SDMA, or the DesignWare internal DMA controller */
    SD_ADMA2,  /* SD host controller only */
};

struct SdCmd {
    uint8_t index = 0;
    uint32_t arg = 0;
    SdRsp rsp = SD_RSP_R1;
    /* data */
    void *buf = nullptr;
    uint32_t blksz = 0;
    uint32_t blocks = 0;  /* 0 for none */
    bool read = true;
    bool multi = false;   /* a multiple block command */
    bool auto_stop = false;
    bool auto_cmd23 = false;
    SdMode mode = SD_PIO;
};

enum SdErr {
    SD_OK,
    SD_TIMEOUT,     /* no response */
    SD_DATA_ERROR,  /* the data phase failed */
    SD_AUTO_ERROR,  /* the automatic command failed */
    SD_HOST_ERROR,  /* the host stopped answering */
};

struct SdResult {
    SdErr err = SD_OK;
    /* R1/R3: the 32 bit payload. R2: the register's bits 127 to 8 in
       reg[0..14], most significant first; the CRC byte is the host's. */
    uint32_t resp = 0;
    uint8_t reg[16] = {};
    /* the automatic CMD12 or CMD23 response */
    uint32_t auto_resp = 0;
};

enum SdHostKind {
    SD_HOST_SDHCI,
    SD_HOST_DW_MMC,
};

class SdHost {
public:
    virtual ~SdHost() = default;

    virtual SdHostKind Kind() const = 0;
    virtual const char *Name() const = 0;
    virtual bool CardPresent() = 0;
    virtual bool CanDo(SdMode mode) const = 0;
    /* Reset the controller, power the bus and start the clock: the card
       is powered up afresh. */
    virtual bool Init() = 0;
    virtual void PowerOff() = 0;
    virtual SdErr Command(const SdCmd &cmd, SdResult *res) = 0;
    /* Put the controller back as the test found it. */
    virtual void Cleanup() = 0;
};

/* Every SD host controller in the tables or on PCI, the device tree ones
   first, for the test that is running; sd_cleanup() runs after it. */
int sd_host_count();
SdHost *sd_host(int index);

/* A command without data; the result's error. */
SdErr sd_cmd(SdHost *host, uint8_t index, uint32_t arg, SdRsp rsp,
             SdResult *res = nullptr);

/* A card as initialisation found it. */
struct SdCard {
    SdHost *host;
    bool mmc;
    bool high_capacity;  /* addressed in sectors */
    uint16_t rca;
    uint64_t sectors;
    uint8_t cid[16];
    uint8_t csd[16];
    int disk;            /* its test disk, -1 without one */

    /* The address a block command takes for 'sector'. */
    uint32_t Addr(uint64_t sector) const
        {return high_capacity ? (uint32_t)sector : (uint32_t)sector * 512;}
    /* SEND_STATUS: the R1, or 0xffffffff on no response. */
    uint32_t Status() const;
};

/* Power the card up and take it through identification to the transfer
   state. */
bool sd_card_init(SdHost *host, SdCard *card);

/* The 'index'th host whose card is an eMMC device ('mmc') or an SD card,
   initialised, and set up to be put back after the test; false when there
   is none. */
bool sd_find_card(bool mmc, int index, SdCard *card);

/* A block read or write of 'count' sectors from 'sector': CMD17/18 or
   CMD24/25, a multiple block one stopped by an automatic CMD12, or bounded
   by CMD23 when 'cmd23' (the host's automatic CMD23 where it has one). */
SdErr sd_rw(const SdCard &card, bool read, uint64_t sector, uint32_t count,
            void *buf, SdMode mode = SD_PIO, SdResult *res = nullptr,
            bool cmd23 = false);

/* A buffer DMA can reach, filled with 'fill'; the test is skipped without
   memory. */
uint8_t *sd_alloc(size_t size, uint8_t fill = 0, size_t align = 16);

/* Whether 'buf' holds the test disk pattern of 'count' sectors from
   'sector'. */
bool sd_is_pattern(const uint8_t *buf, uint64_t sector, uint32_t count);

/* Bits 'hi' down to 'lo' of a register held most significant byte first,
   as the specifications number the CID, CSD and SCR. */
uint32_t sd_bits(const uint8_t *reg, int size, int hi, int lo);

/* For t_on_cleanup(): every host reset and powered down, and PCI as
   enumeration left it. sd_find_card() registers it. */
void sd_cleanup();


/* SD Host Controller Simplified Specification 3.00, chapter 2 */
#define SDHCI_DMA_ADDRESS       0x00
#define SDHCI_BLOCK_SIZE        0x04
#define SDHCI_BLOCK_COUNT       0x06
#define SDHCI_ARGUMENT          0x08
#define SDHCI_TRANSFER_MODE     0x0c
#define SDHCI_COMMAND           0x0e
#define SDHCI_RESPONSE          0x10
#define SDHCI_BUFFER            0x20
#define SDHCI_PRESENT           0x24
#define SDHCI_HOST_CONTROL      0x28
#define SDHCI_POWER             0x29
#define SDHCI_BLOCK_GAP         0x2a
#define SDHCI_WAKEUP            0x2b
#define SDHCI_CLOCK             0x2c
#define SDHCI_TIMEOUT           0x2e
#define SDHCI_SWRST             0x2f
#define SDHCI_INT_STATUS        0x30
#define SDHCI_ERR_STATUS        0x32
#define SDHCI_INT_ENABLE        0x34
#define SDHCI_ERR_ENABLE        0x36
#define SDHCI_SIGNAL_ENABLE     0x38
#define SDHCI_ERR_SIGNAL_ENABLE 0x3a
#define SDHCI_AUTO_CMD_STATUS   0x3c
#define SDHCI_HOST_CONTROL2     0x3e
#define SDHCI_CAPS              0x40
#define SDHCI_CAPS_HI           0x44
#define SDHCI_MAX_CURRENT       0x48
#define SDHCI_FORCE_AUTO_CMD    0x50
#define SDHCI_FORCE_ERR         0x52
#define SDHCI_ADMA_ERROR        0x54
#define SDHCI_ADMA_ADDRESS      0x58
#define SDHCI_SLOT_INT          0xfc
#define SDHCI_VERSION           0xfe

#define SDHCI_TRNS_DMA          0x01
#define SDHCI_TRNS_BLK_CNT_EN   0x02
#define SDHCI_TRNS_ACMD12       0x04
#define SDHCI_TRNS_ACMD23       0x08
#define SDHCI_TRNS_READ         0x10
#define SDHCI_TRNS_MULTI        0x20
#define SDHCI_CMD_DATA          0x20
#define SDHCI_CMD_ABORT         0xc0

#define SDHCI_PS_CMD_INHIBIT    (1u << 0)
#define SDHCI_PS_DAT_INHIBIT    (1u << 1)
#define SDHCI_PS_DAT_ACTIVE     (1u << 2)
#define SDHCI_PS_WRITE_ACTIVE   (1u << 8)
#define SDHCI_PS_READ_ACTIVE    (1u << 9)
#define SDHCI_PS_BUF_WR         (1u << 10)
#define SDHCI_PS_BUF_RD         (1u << 11)
#define SDHCI_PS_CARD_INSERTED  (1u << 16)
#define SDHCI_PS_CARD_STABLE    (1u << 17)
#define SDHCI_PS_CARD_DETECT    (1u << 18)
#define SDHCI_PS_WRITE_ENABLED  (1u << 19)

#define SDHCI_CTRL_DMA_MASK     0x18
#define SDHCI_CTRL_ADMA32       0x10
#define SDHCI_CTRL_ADMA64       0x18
#define SDHCI_CTRL_CD_TEST_LEVEL 0x40
#define SDHCI_CTRL_CD_TEST      0x80
#define SDHCI_POWER_ON          0x01
#define SDHCI_POWER_330         0x0e
#define SDHCI_CLOCK_INT_EN      0x0001
#define SDHCI_CLOCK_INT_STABLE  0x0002
#define SDHCI_CLOCK_CARD_EN     0x0004
#define SDHCI_RESET_ALL         0x01
#define SDHCI_RESET_CMD         0x02
#define SDHCI_RESET_DATA        0x04

#define SDHCI_INT_CMD_COMPLETE  0x0001
#define SDHCI_INT_XFER_COMPLETE 0x0002
#define SDHCI_INT_DMA_END       0x0008
#define SDHCI_INT_BUF_WR_READY  0x0010
#define SDHCI_INT_BUF_RD_READY  0x0020
#define SDHCI_INT_CARD_INSERT   0x0040
#define SDHCI_INT_CARD_REMOVE   0x0080
#define SDHCI_INT_CARD_INT      0x0100
#define SDHCI_INT_ERROR         0x8000
#define SDHCI_ERR_CMD_TIMEOUT   0x0001
#define SDHCI_ERR_CMD_MASK      0x000f
#define SDHCI_ERR_DATA_TIMEOUT  0x0010
#define SDHCI_ERR_DATA_CRC      0x0020
#define SDHCI_ERR_AUTO_CMD      0x0100
#define SDHCI_ERR_ADMA          0x0200

class SdhciHost final : public SdHost {
public:
    const HwDevice *dev;  /* in the tables, or */
    PciFunc *fn;          /* on PCI, with the registers in BAR 0 */
    uint64_t base = 0;

    SdhciHost(const HwDevice *dev, PciFunc *fn);

    uint32_t Rd(uint32_t off, int size_log2);
    void Wr(uint32_t off, uint32_t v, int size_log2);
    uint8_t R8(uint32_t off) {return Rd(off, 0);}
    uint16_t R16(uint32_t off) {return Rd(off, 1);}
    uint32_t R32(uint32_t off) {return Rd(off, 2);}
    void W8(uint32_t off, uint8_t v) {Wr(off, v, 0);}
    void W16(uint32_t off, uint16_t v) {Wr(off, v, 1);}
    void W32(uint32_t off, uint32_t v) {Wr(off, v, 2);}

    /* a software reset, waited for */
    bool Reset(uint8_t what);
    /* the normal status with any of 'bits' or the error bit, waited for */
    bool WaitInt(uint16_t bits, uint16_t *got);
    uint16_t CommandReg(const SdCmd &cmd);
    uint16_t TransferMode(const SdCmd &cmd);
    /* an ADMA2 table of 4 KB lines over 'buf', the last one ending it */
    uint64_t BuildAdma(void *buf, uint32_t len, bool wide);
    void ReadResponse(const SdCmd &cmd, SdResult *res);
    SdErr Fail(SdErr err);

    SdHostKind Kind() const override {return SD_HOST_SDHCI;}
    const char *Name() const override;
    bool CardPresent() override;
    bool CanDo(SdMode) const override {return true;}
    bool Init() override;
    void PowerOff() override;
    SdErr Command(const SdCmd &cmd, SdResult *res) override;
    void Cleanup() override;
};


/* DesignWare mobile storage host, as Linux's dw_mmc drives it */
#define DW_CTRL      0x000
#define DW_PWREN     0x004
#define DW_CLKDIV    0x008
#define DW_CLKSRC    0x00c
#define DW_CLKENA    0x010
#define DW_TMOUT     0x014
#define DW_CTYPE     0x018
#define DW_BLKSIZ    0x01c
#define DW_BYTCNT    0x020
#define DW_INTMASK   0x024
#define DW_CMDARG    0x028
#define DW_CMD       0x02c
#define DW_RESP0     0x030
#define DW_MINTSTS   0x040
#define DW_RINTSTS   0x044
#define DW_STATUS    0x048
#define DW_FIFOTH    0x04c
#define DW_CDETECT   0x050
#define DW_WRTPRT    0x054
#define DW_TCBCNT    0x05c
#define DW_TBBCNT    0x060
#define DW_VERID     0x06c
#define DW_HCON      0x070
#define DW_RST_N     0x078
#define DW_BMOD      0x080
#define DW_PLDMND    0x084
#define DW_DBADDRL   0x088
#define DW_DBADDRU   0x08c
#define DW_DATA      0x200

#define DW_CTRL_RESET      (1u << 0)
#define DW_CTRL_FIFO_RESET (1u << 1)
#define DW_CTRL_DMA_RESET  (1u << 2)
#define DW_CTRL_RESETS     0x7
#define DW_CTRL_INT_ENABLE (1u << 4)
#define DW_CTRL_USE_IDMAC  (1u << 25)

#define DW_INT_RE      (1u << 1)
#define DW_INT_CMD_DONE (1u << 2)
#define DW_INT_DTO     (1u << 3)
#define DW_INT_TXDR    (1u << 4)
#define DW_INT_RXDR    (1u << 5)
#define DW_INT_RCRC    (1u << 6)
#define DW_INT_DCRC    (1u << 7)
#define DW_INT_RTO     (1u << 8)
#define DW_INT_DRTO    (1u << 9)
#define DW_INT_FRUN    (1u << 11)
#define DW_INT_ACD     (1u << 14)
#define DW_INT_EBE     (1u << 15)
#define DW_INT_SDIO    (1u << 16)
#define DW_INT_DATA_ERRORS (DW_INT_DCRC | DW_INT_DRTO | DW_INT_FRUN | \
                            DW_INT_EBE)

#define DW_CMD_RESP_EXP     (1u << 6)
#define DW_CMD_RESP_LONG    (1u << 7)
#define DW_CMD_CHECK_CRC    (1u << 8)
#define DW_CMD_DAT_EXP      (1u << 9)
#define DW_CMD_WRITE        (1u << 10)
#define DW_CMD_SEND_STOP    (1u << 12)
#define DW_CMD_WAIT_PRVDATA (1u << 13)
#define DW_CMD_STOP_ABORT   (1u << 14)
#define DW_CMD_INIT         (1u << 15)
#define DW_CMD_UPD_CLK      (1u << 21)
#define DW_CMD_USE_HOLD     (1u << 29)
#define DW_CMD_START        (1u << 31)

#define DW_BMOD_SWR (1u << 0)
#define DW_BMOD_FB  (1u << 1)
#define DW_BMOD_DE  (1u << 7)

#define DW_IDMAC_TI  (1u << 0)
#define DW_IDMAC_RI  (1u << 1)
#define DW_IDMAC_FBE (1u << 2)
#define DW_IDMAC_DU  (1u << 4)
#define DW_IDMAC_NI  (1u << 8)
#define DW_IDMAC_AI  (1u << 9)

#define DW_DES0_DIC (1u << 1)
#define DW_DES0_LD  (1u << 2)
#define DW_DES0_FS  (1u << 3)
#define DW_DES0_CH  (1u << 4)
#define DW_DES0_ER  (1u << 5)
#define DW_DES0_OWN (1u << 31)

class DwMmcHost final : public SdHost {
public:
    const HwDevice *dev;
    uint64_t base;
    int dma;  /* descriptor address width, 0 without the DMA controller */

    explicit DwMmcHost(const HwDevice *dev);

    uint32_t R(uint32_t off) {return mmio_read32(base + off);}
    void W(uint32_t off, uint32_t v) {mmio_write32(base + off, v);}
    /* the internal DMA status and interrupt enable, which move with the
       descriptor width */
    uint32_t IdstsReg() const;
    uint32_t IdintenReg() const;
    bool ClockUpdate();
    bool WaitInt(uint32_t bits, uint32_t *got);
    uint32_t CommandReg(const SdCmd &cmd);
    /* a chain of 4 KB descriptors over 'buf' */
    uint64_t BuildDescriptors(void *buf, uint32_t len);
    SdErr Fail(SdErr err);

    SdHostKind Kind() const override {return SD_HOST_DW_MMC;}
    const char *Name() const override {return "dw-mmc";}
    bool CardPresent() override;
    bool CanDo(SdMode mode) const override
        {return mode == SD_PIO || (mode == SD_DMA && dma != 0);}
    bool Init() override;
    void PowerOff() override;
    SdErr Command(const SdCmd &cmd, SdResult *res) override;
    void Cleanup() override;
};
