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
#include "sd.h"

#include "heap.h"
#include "mmio.h"
#include "test.h"
#include "test_control.h"
#include "testctl.h"

#define PCI_CLASS_SDHCI 0x0805
#define LIMIT_4G 0x100000000ull
#define MAX_HOSTS 8

uint32_t sd_bits(const uint8_t *reg, int size, int hi, int lo)
{
    uint32_t v = 0;
    for (int bit = hi; bit >= lo; bit--) {
        int byte = size - 1 - bit / 8;
        v = v << 1 | ((reg[byte] >> (bit % 8)) & 1);
    }
    return v;
}


//#pragma mark - SdhciHost

SdhciHost::SdhciHost(const HwDevice *dev_, PciFunc *fn_): dev(dev_), fn(fn_)
{
    if (dev != nullptr)
        base = hw_region(dev, HW_MMIO)->base;
}

const char *SdhciHost::Name() const
{
    return fn != nullptr ? "sdhci-pci" : "sdhci";
}

uint32_t SdhciHost::Rd(uint32_t off, int size_log2)
{
    if (fn != nullptr)
        return pci_bar_read(fn, 0, off, size_log2);
    switch (size_log2) {
    case 0: return mmio_read8(base + off);
    case 1: return mmio_read16(base + off);
    default: return mmio_read32(base + off);
    }
}

void SdhciHost::Wr(uint32_t off, uint32_t v, int size_log2)
{
    if (fn != nullptr) {
        pci_bar_write(fn, 0, off, v, size_log2);
        return;
    }
    switch (size_log2) {
    case 0: mmio_write8(base + off, v); break;
    case 1: mmio_write16(base + off, v); break;
    default: mmio_write32(base + off, v); break;
    }
}

bool SdhciHost::CardPresent()
{
    return (R32(SDHCI_PRESENT) & SDHCI_PS_CARD_INSERTED) != 0;
}

bool SdhciHost::Reset(uint8_t what)
{
    W8(SDHCI_SWRST, what);
    return t_wait_until([&] {return R8(SDHCI_SWRST) == 0;}, 100000);
}

bool SdhciHost::Init()
{
    if (!Reset(SDHCI_RESET_ALL))
        return false;
    W8(SDHCI_POWER, SDHCI_POWER_330);
    W8(SDHCI_POWER, SDHCI_POWER_330 | SDHCI_POWER_ON);
    /* the slowest clock the 10 bit divider gives is plenty */
    W16(SDHCI_CLOCK, 0x8000 | SDHCI_CLOCK_INT_EN);
    if (!t_wait_until([&] {
            return (R16(SDHCI_CLOCK) & SDHCI_CLOCK_INT_STABLE) != 0;
        }, 100000))
        return false;
    W16(SDHCI_CLOCK, R16(SDHCI_CLOCK) | SDHCI_CLOCK_CARD_EN);
    W8(SDHCI_TIMEOUT, 0x0e);
    W16(SDHCI_INT_ENABLE, 0x00ff);
    W16(SDHCI_ERR_ENABLE, 0x03ff);
    W16(SDHCI_SIGNAL_ENABLE, 0);
    W16(SDHCI_ERR_SIGNAL_ENABLE, 0);
    W16(SDHCI_INT_STATUS, 0xffff);
    W16(SDHCI_ERR_STATUS, 0xffff);
    return true;
}

void SdhciHost::PowerOff()
{
    W8(SDHCI_POWER, 0);
}

void SdhciHost::Cleanup()
{
    Reset(SDHCI_RESET_ALL);
}

bool SdhciHost::WaitInt(uint16_t bits, uint16_t *got)
{
    uint16_t st = 0;
    bool ok = t_wait_until([&] {
        st = R16(SDHCI_INT_STATUS);
        return (st & (bits | SDHCI_INT_ERROR)) != 0;
    }, 1000000);
    if (got != nullptr)
        *got = st;
    return ok;
}

SdErr SdhciHost::Fail(SdErr err)
{
    Reset(SDHCI_RESET_CMD | SDHCI_RESET_DATA);
    W16(SDHCI_INT_STATUS, 0xffff);
    W16(SDHCI_ERR_STATUS, 0xffff);
    return err;
}

uint16_t SdhciHost::CommandReg(const SdCmd &cmd)
{
    uint16_t v = cmd.index << 8;
    switch (cmd.rsp) {
    case SD_RSP_NONE: break;
    case SD_RSP_R1: v |= 0x02 | 0x18; break;
    case SD_RSP_R1B: v |= 0x03 | 0x18; break;
    case SD_RSP_R2: v |= 0x01 | 0x08; break;
    case SD_RSP_R3: v |= 0x02; break;
    }
    if (cmd.blocks != 0)
        v |= SDHCI_CMD_DATA;
    if (cmd.index == 12)
        v |= SDHCI_CMD_ABORT;
    return v;
}

uint16_t SdhciHost::TransferMode(const SdCmd &cmd)
{
    uint16_t v = 0;
    if (cmd.mode != SD_PIO)
        v |= SDHCI_TRNS_DMA;
    if (cmd.multi)
        v |= SDHCI_TRNS_MULTI | SDHCI_TRNS_BLK_CNT_EN;
    if (cmd.auto_stop)
        v |= SDHCI_TRNS_ACMD12;
    if (cmd.auto_cmd23)
        v |= SDHCI_TRNS_ACMD23;
    if (cmd.read)
        v |= SDHCI_TRNS_READ;
    return v;
}

uint64_t SdhciHost::BuildAdma(void *buf, uint32_t len, bool wide)
{
    /* lines of at most 4 KB, the last one ending the table */
    uint32_t lines = (len + 4095) / 4096;
    uint32_t dsize = wide ? 12 : 8;
    uint8_t *table = (uint8_t *)heap_alloc(lines * dsize, 8, LIMIT_4G);
    if (table == nullptr)
        t_skip("no memory");
    uintptr_t addr = (uintptr_t)buf;
    for (uint32_t i = 0; i < lines; i++) {
        uint32_t n = len > 4096 ? 4096 : len;
        uint8_t *d = table + i * dsize;
        uint16_t attr = 0x21 | (i + 1 == lines ? 0x02 : 0);
        put_le16(d, attr);
        put_le16(d + 2, n);
        put_le32(d + 4, (uint32_t)addr);
        if (wide)
            put_le32(d + 8, (uint32_t)((uint64_t)addr >> 32));
        addr += n;
        len -= n;
    }
    return (uintptr_t)table;
}

void SdhciHost::ReadResponse(const SdCmd &cmd, SdResult *res)
{
    if (cmd.rsp == SD_RSP_R2) {
        /* REP[119:0] holds the register's bits 127 to 8 */
        for (int i = 0; i < 15; i++) {
            uint32_t w = R32(SDHCI_RESPONSE + (i / 4) * 4);
            res->reg[14 - i] = w >> ((i % 4) * 8);
        }
        res->reg[15] = 0;
    } else if (cmd.rsp != SD_RSP_NONE) {
        res->resp = R32(SDHCI_RESPONSE);
    }
}

SdErr SdhciHost::Command(const SdCmd &cmd, SdResult *res)
{
    uint32_t len = cmd.blksz * cmd.blocks;
    uint16_t st;

    W16(SDHCI_INT_STATUS, 0xffff);
    W16(SDHCI_ERR_STATUS, 0xffff);
    if (cmd.blocks != 0) {
        uint8_t hc = R8(SDHCI_HOST_CONTROL) & ~SDHCI_CTRL_DMA_MASK;
        if (cmd.mode == SD_ADMA2) {
            W8(SDHCI_HOST_CONTROL, hc | SDHCI_CTRL_ADMA32);
            uint64_t table = BuildAdma(cmd.buf, len, false);
            W32(SDHCI_ADMA_ADDRESS, (uint32_t)table);
            W32(SDHCI_ADMA_ADDRESS + 4, (uint32_t)(table >> 32));
        } else {
            W8(SDHCI_HOST_CONTROL, hc);
            if (cmd.mode == SD_DMA)
                W32(SDHCI_DMA_ADDRESS, (uint32_t)(uintptr_t)cmd.buf);
        }
        if (cmd.auto_cmd23)
            W32(SDHCI_DMA_ADDRESS, cmd.blocks);
        /* SDMA breaks at 512 KB boundaries */
        W16(SDHCI_BLOCK_SIZE, cmd.blksz | 7 << 12);
        W16(SDHCI_BLOCK_COUNT, cmd.blocks);
        W16(SDHCI_TRANSFER_MODE, TransferMode(cmd));
    }
    W32(SDHCI_ARGUMENT, cmd.arg);
    W16(SDHCI_COMMAND, CommandReg(cmd));

    if (!WaitInt(SDHCI_INT_CMD_COMPLETE, &st))
        return Fail(SD_HOST_ERROR);
    if (st & SDHCI_INT_CMD_COMPLETE)
        ReadResponse(cmd, res);
    if (st & SDHCI_INT_ERROR) {
        /* a data error right after the response is the data phase's */
        uint16_t err = R16(SDHCI_ERR_STATUS);
        if (err & SDHCI_ERR_CMD_MASK)
            return Fail(SD_TIMEOUT);
        if (!(st & SDHCI_INT_CMD_COMPLETE))
            return Fail(err & SDHCI_ERR_AUTO_CMD ? SD_AUTO_ERROR
                                                 : SD_DATA_ERROR);
        if (cmd.blocks == 0)
            return Fail(SD_DATA_ERROR);
    }
    W16(SDHCI_INT_STATUS, SDHCI_INT_CMD_COMPLETE);

    if (cmd.blocks != 0) {
        uint8_t *p = (uint8_t *)cmd.buf;
        for (uint32_t b = 0; cmd.mode == SD_PIO && b < cmd.blocks; b++) {
            uint16_t want = cmd.read ? SDHCI_INT_BUF_RD_READY
                                     : SDHCI_INT_BUF_WR_READY;
            if (!WaitInt(want, &st))
                return Fail(SD_HOST_ERROR);
            if (!(st & want))
                break;
            W16(SDHCI_INT_STATUS, want);
            for (uint32_t i = 0; i < cmd.blksz; i += 4, p += 4) {
                if (cmd.read)
                    put_le32(p, R32(SDHCI_BUFFER));
                else
                    W32(SDHCI_BUFFER, le32(p));
            }
        }
        for (;;) {
            if (!WaitInt(SDHCI_INT_XFER_COMPLETE | SDHCI_INT_DMA_END, &st))
                return Fail(SD_HOST_ERROR);
            if ((st & SDHCI_INT_DMA_END) && !(st & SDHCI_INT_ERROR)) {
                /* an SDMA boundary: carry on from where it stopped */
                W16(SDHCI_INT_STATUS, SDHCI_INT_DMA_END);
                W32(SDHCI_DMA_ADDRESS, R32(SDHCI_DMA_ADDRESS));
                if (!(st & SDHCI_INT_XFER_COMPLETE))
                    continue;
            }
            break;
        }
        res->auto_resp = R32(SDHCI_RESPONSE + 12);
        if (st & SDHCI_INT_ERROR) {
            uint16_t err = R16(SDHCI_ERR_STATUS);
            return Fail(err & SDHCI_ERR_AUTO_CMD ? SD_AUTO_ERROR
                                                 : SD_DATA_ERROR);
        }
        W16(SDHCI_INT_STATUS, 0xffff);
    } else if (cmd.rsp == SD_RSP_R1B) {
        if (!WaitInt(SDHCI_INT_XFER_COMPLETE, &st))
            return Fail(SD_HOST_ERROR);
        W16(SDHCI_INT_STATUS, 0xffff);
    }
    if (cmd.auto_cmd23)
        res->auto_resp = R32(SDHCI_RESPONSE + 12);
    return SD_OK;
}


//#pragma mark - DwMmcHost

DwMmcHost::DwMmcHost(const HwDevice *dev_): dev(dev_)
{
    base = hw_region(dev, HW_MMIO)->base;
    uint32_t hcon = R(DW_HCON);
    dma = get_bits(hcon, 16, 2) == 3 ? 0 : (hcon & (1u << 27)) ? 64 : 32;
}

uint32_t DwMmcHost::IdstsReg() const {return dma == 64 ? 0x90 : 0x8c;}
uint32_t DwMmcHost::IdintenReg() const {return dma == 64 ? 0x94 : 0x90;}

bool DwMmcHost::CardPresent()
{
    return (R(DW_CDETECT) & 1) == 0;
}

bool DwMmcHost::ClockUpdate()
{
    W(DW_CMD, DW_CMD_START | DW_CMD_UPD_CLK | DW_CMD_WAIT_PRVDATA);
    return t_wait_until([&] {return !(R(DW_CMD) & DW_CMD_START);}, 100000);
}

bool DwMmcHost::Init()
{
    W(DW_PWREN, 0);
    W(DW_CTRL, DW_CTRL_RESETS);
    if (!t_wait_until([&] {return !(R(DW_CTRL) & DW_CTRL_RESETS);}, 100000))
        return false;
    W(DW_BMOD, 1); /* software reset of the DMA controller */
    W(DW_RINTSTS, 0xffffffff);
    W(DW_INTMASK, 0);
    W(IdstsReg(), 0xffffffff);
    W(IdintenReg(), 0);
    W(DW_TMOUT, 0xffffffff);
    W(DW_FIFOTH, (2u << 28) | (15u << 16) | 16);
    W(DW_CTYPE, 0);
    W(DW_PWREN, 1);
    W(DW_CLKENA, 0);
    W(DW_CLKDIV, 0xff);
    if (!ClockUpdate())
        return false;
    W(DW_CLKENA, 1);
    return ClockUpdate();
}

void DwMmcHost::PowerOff()
{
    W(DW_PWREN, 0);
}

void DwMmcHost::Cleanup()
{
    W(DW_CTRL, DW_CTRL_RESETS);
    t_wait_until([&] {return !(R(DW_CTRL) & DW_CTRL_RESETS);}, 100000);
    W(DW_BMOD, 1);
    W(DW_RINTSTS, 0xffffffff);
    W(IdstsReg(), 0xffffffff);
    W(DW_INTMASK, 0);
    W(IdintenReg(), 0);
    W(DW_PWREN, 0);
}

bool DwMmcHost::WaitInt(uint32_t bits, uint32_t *got)
{
    uint32_t st = 0;
    bool ok = t_wait_until([&] {
        st = R(DW_RINTSTS);
        return (st & bits) != 0;
    }, 1000000);
    if (got != nullptr)
        *got = st;
    return ok;
}

SdErr DwMmcHost::Fail(SdErr err)
{
    W(DW_CTRL, R(DW_CTRL) | DW_CTRL_FIFO_RESET | DW_CTRL_DMA_RESET);
    t_wait_until([&] {return !(R(DW_CTRL) & DW_CTRL_RESETS);}, 100000);
    W(DW_RINTSTS, 0xffffffff);
    W(IdstsReg(), 0xffffffff);
    return err;
}

uint64_t DwMmcHost::BuildDescriptors(void *buf, uint32_t len)
{
    /* chained, 4 KB a descriptor */
    uint32_t n = (len + 4095) / 4096;
    uint32_t dsize = dma == 64 ? 32 : 16;
    uint8_t *table = (uint8_t *)heap_alloc(n * dsize, 8, LIMIT_4G);
    if (table == nullptr)
        t_skip("no memory");
    memset(table, 0, n * dsize);
    uintptr_t addr = (uintptr_t)buf;
    for (uint32_t i = 0; i < n; i++) {
        uint8_t *d = table + i * dsize;
        uint32_t size = len > 4096 ? 4096 : len;
        uint32_t des0 = DW_DES0_OWN | DW_DES0_CH;
        if (i == 0)
            des0 |= DW_DES0_FS;
        if (i + 1 == n)
            des0 |= DW_DES0_LD;
        uint64_t next = (uintptr_t)(d + dsize);
        put_le32(d, des0);
        if (dma == 64) {
            put_le32(d + 8, size);
            put_le32(d + 16, (uint32_t)addr);
            put_le32(d + 20, (uint32_t)((uint64_t)addr >> 32));
            put_le32(d + 24, (uint32_t)next);
            put_le32(d + 28, (uint32_t)(next >> 32));
        } else {
            put_le32(d + 4, size);
            put_le32(d + 8, (uint32_t)addr);
            put_le32(d + 12, (uint32_t)next);
        }
        addr += size;
        len -= size;
    }
    return (uintptr_t)table;
}

uint32_t DwMmcHost::CommandReg(const SdCmd &cmd)
{
    uint32_t v = cmd.index | DW_CMD_START | DW_CMD_USE_HOLD |
                 DW_CMD_WAIT_PRVDATA;
    if (cmd.rsp != SD_RSP_NONE)
        v |= DW_CMD_RESP_EXP;
    if (cmd.rsp == SD_RSP_R2)
        v |= DW_CMD_RESP_LONG;
    if (cmd.rsp != SD_RSP_NONE && cmd.rsp != SD_RSP_R3)
        v |= DW_CMD_CHECK_CRC;
    if (cmd.blocks != 0)
        v |= DW_CMD_DAT_EXP | (cmd.read ? 0 : DW_CMD_WRITE);
    if (cmd.auto_stop)
        v |= DW_CMD_SEND_STOP;
    if (cmd.index == 0)
        v |= DW_CMD_INIT;
    if (cmd.index == 12)
        v |= DW_CMD_STOP_ABORT;
    return v;
}

SdErr DwMmcHost::Command(const SdCmd &cmd, SdResult *res)
{
    uint32_t len = cmd.blksz * cmd.blocks;
    uint32_t st;
    bool use_dma = cmd.blocks != 0 && cmd.mode != SD_PIO;

    W(DW_RINTSTS, 0xffffffff);
    W(IdstsReg(), 0xffffffff);
    if (cmd.blocks != 0) {
        W(DW_BLKSIZ, cmd.blksz);
        W(DW_BYTCNT, len);
        uint32_t ctrl = R(DW_CTRL) & ~DW_CTRL_USE_IDMAC;
        if (use_dma) {
            uint64_t table = BuildDescriptors(cmd.buf, len);
            W(DW_CTRL, ctrl | DW_CTRL_USE_IDMAC);
            W(DW_BMOD, DW_BMOD_DE | DW_BMOD_FB);
            W(DW_DBADDRL, (uint32_t)table);
            if (dma == 64)
                W(DW_DBADDRU, (uint32_t)(table >> 32));
        } else {
            W(DW_CTRL, ctrl);
            W(DW_BMOD, 0);
        }
    }
    W(DW_CMDARG, cmd.arg);
    W(DW_CMD, CommandReg(cmd));
    if (use_dma)
        W(DW_PLDMND, 1);

    if (!WaitInt(DW_INT_CMD_DONE, &st))
        return Fail(SD_HOST_ERROR);
    if (st & (DW_INT_RTO | DW_INT_RCRC | DW_INT_RE))
        return Fail(SD_TIMEOUT);
    if (cmd.rsp == SD_RSP_R2) {
        /* RESP3 to RESP0: the register's bits 127 to 0 */
        for (int k = 0; k < 15; k++) {
            uint32_t w = R(DW_RESP0 + (3 - k / 4) * 4);
            res->reg[k] = w >> (24 - (k % 4) * 8);
        }
        res->reg[15] = 0;
    } else if (cmd.rsp != SD_RSP_NONE) {
        res->resp = R(DW_RESP0);
    }

    if (cmd.blocks != 0) {
        uint8_t *p = (uint8_t *)cmd.buf;
        uint32_t left = len;
        for (;;) {
            if (!WaitInt(DW_INT_DTO | DW_INT_RXDR | DW_INT_TXDR |
                         DW_INT_DATA_ERRORS, &st))
                return Fail(SD_HOST_ERROR);
            if (!use_dma && cmd.read && (st & (DW_INT_RXDR | DW_INT_DTO))) {
                W(DW_RINTSTS, DW_INT_RXDR);
                uint32_t words = get_bits(R(DW_STATUS), 17, 13);
                for (; words > 0 && left > 0; words--) {
                    uint32_t w = R(DW_DATA);
                    for (int i = 0; i < 4 && left > 0; i++, left--)
                        *p++ = w >> (8 * i);
                }
            }
            if (!use_dma && !cmd.read && (st & DW_INT_TXDR)) {
                W(DW_RINTSTS, DW_INT_TXDR);
                uint32_t room = 32 - get_bits(R(DW_STATUS), 17, 13);
                for (; room > 0 && left > 0; room--) {
                    uint32_t w = 0;
                    for (int i = 0; i < 4 && left > 0; i++, left--)
                        w |= (uint32_t)*p++ << (8 * i);
                    W(DW_DATA, w);
                }
            }
            if (st & (DW_INT_DTO | DW_INT_DATA_ERRORS))
                break;
        }
        if (st & DW_INT_DATA_ERRORS)
            return Fail(SD_DATA_ERROR);
        if (cmd.auto_stop) {
            WaitInt(DW_INT_ACD | DW_INT_RTO, &st);
            res->auto_resp = R(DW_RESP0 + 4);
            if (!(st & DW_INT_ACD))
                return Fail(SD_AUTO_ERROR);
        }
        if (use_dma && (R(IdstsReg()) & (DW_IDMAC_FBE | DW_IDMAC_DU)))
            return Fail(SD_DATA_ERROR);
    }
    W(DW_RINTSTS, 0xffffffff);
    return SD_OK;
}


//#pragma mark - discovery

namespace {

SdHost *sHosts[MAX_HOSTS];
int sHostCount = -1;

/* The hosts live on the test's heap, so they are found afresh in every
   test that wants them, and forgotten when it ends. */
void discover()
{
    t_on_cleanup(sd_cleanup);
    sHostCount = 0;
    static const char *const kSdhci[] = {"arasan,sdhci-8.9a", "sdhci"};
    for (const char *compat : kSdhci) {
        for (int i = 0; sHostCount < MAX_HOSTS; i++) {
            const HwDevice *dev = registry_find(compat, i);
            if (dev == nullptr || hw_region(dev, HW_MMIO) == nullptr)
                break;
            sHosts[sHostCount++] = new SdhciHost(dev, nullptr);
        }
    }
    for (int i = 0; sHostCount < MAX_HOSTS; i++) {
        const HwDevice *dev = registry_find("snps,dw-mshc", i);
        if (dev == nullptr || hw_region(dev, HW_MMIO) == nullptr)
            break;
        sHosts[sHostCount++] = new DwMmcHost(dev);
    }
    pci_enumerate();
    for (int i = 0; i < pci_func_count() && sHostCount < MAX_HOSTS; i++) {
        PciFunc *f = pci_func(i);
        if ((f->class_rev >> 16) == PCI_CLASS_SDHCI && f->bars[0].size &&
            !f->bars[0].io && pci_reachable(f->host, f->bars[0].addr,
                                            f->bars[0].size, false))
            sHosts[sHostCount++] = new SdhciHost(nullptr, f);
    }
}

void cleanup()
{
    for (int i = 0; i < sHostCount; i++)
        sHosts[i]->Cleanup();
    if (sHostCount >= 0)
        pci_restore_all();
    sHostCount = -1;
}

}

int sd_host_count()
{
    if (sHostCount < 0)
        discover();
    return sHostCount;
}

SdHost *sd_host(int index)
{
    return index < sd_host_count() ? sHosts[index] : nullptr;
}

void sd_cleanup()
{
    cleanup();
}


//#pragma mark - cards

SdErr sd_cmd(SdHost *host, uint8_t index, uint32_t arg, SdRsp rsp,
             SdResult *res)
{
    SdResult dummy;
    SdCmd cmd;
    cmd.index = index;
    cmd.arg = arg;
    cmd.rsp = rsp;
    return host->Command(cmd, res != nullptr ? res : &dummy);
}

uint32_t SdCard::Status() const
{
    SdResult r;
    if (sd_cmd(host, 13, (uint32_t)rca << 16, SD_RSP_R1, &r) != SD_OK)
        return 0xffffffff;
    return r.resp;
}

static uint64_t csd_v1_sectors(const uint8_t *csd)
{
    uint32_t c_size = sd_bits(csd, 16, 73, 62);
    uint32_t mult = sd_bits(csd, 16, 49, 47);
    uint32_t bl_len = sd_bits(csd, 16, 83, 80);
    return ((uint64_t)(c_size + 1) << (mult + 2)) << (bl_len - 9);
}

bool sd_card_init(SdHost *host, SdCard *card)
{
    SdResult r;

    memset(card, 0, sizeof(*card));
    card->host = host;
    card->disk = -1;
    if (!host->Init())
        return false;
    sd_cmd(host, 0, 0, SD_RSP_NONE);
    bool v2 = sd_cmd(host, 8, 0x1aa, SD_RSP_R1, &r) == SD_OK &&
              (r.resp & 0xfff) == 0x1aa;
    uint32_t ocr = 0;
    if (sd_cmd(host, 55, 0, SD_RSP_R1, &r) == SD_OK) {
        for (int i = 0; i < 100 && !(ocr & OCR_READY); i++) {
            if (i > 0)
                sd_cmd(host, 55, 0, SD_RSP_R1);
            if (sd_cmd(host, 41, OCR_WINDOW | (v2 ? OCR_HCS : 0), SD_RSP_R3,
                       &r) != SD_OK)
                return false;
            ocr = r.resp;
        }
        card->high_capacity = (ocr & OCR_HCS) != 0;
    } else {
        card->mmc = true;
        for (int i = 0; i < 100 && !(ocr & OCR_READY); i++) {
            if (sd_cmd(host, 1, OCR_WINDOW | OCR_SECTOR, SD_RSP_R3, &r) !=
                SD_OK)
                return false;
            ocr = r.resp;
        }
        card->high_capacity = (ocr & (3u << 29)) == OCR_SECTOR;
    }
    if (!(ocr & OCR_READY))
        return false;
    if (sd_cmd(host, 2, 0, SD_RSP_R2, &r) != SD_OK)
        return false;
    memcpy(card->cid, r.reg, 16);
    if (card->mmc) {
        card->rca = 2;
        if (sd_cmd(host, 3, 2u << 16, SD_RSP_R1, &r) != SD_OK)
            return false;
    } else {
        if (sd_cmd(host, 3, 0, SD_RSP_R1, &r) != SD_OK)
            return false;
        card->rca = r.resp >> 16;
    }
    if (sd_cmd(host, 9, (uint32_t)card->rca << 16, SD_RSP_R2, &r) != SD_OK)
        return false;
    memcpy(card->csd, r.reg, 16);
    if (sd_cmd(host, 7, (uint32_t)card->rca << 16, SD_RSP_R1B, &r) != SD_OK)
        return false;

    if (!card->mmc && sd_bits(card->csd, 16, 127, 126) == 1)
        card->sectors = (uint64_t)(sd_bits(card->csd, 16, 69, 48) + 1) * 1024;
    else if (card->mmc && card->high_capacity) {
        uint8_t *ext = (uint8_t *)heap_alloc(512, 16, LIMIT_4G);
        if (ext == nullptr)
            t_skip("no memory");
        SdCmd cmd;
        cmd.index = 8;
        cmd.buf = ext;
        cmd.blksz = 512;
        cmd.blocks = 1;
        if (host->Command(cmd, &r) != SD_OK)
            return false;
        card->sectors = le32(ext + 212);
    } else {
        card->sectors = csd_v1_sectors(card->csd);
    }
    card->disk = tc_block_find(card->sectors);
    return true;
}

bool sd_find_card(bool mmc, int index, SdCard *card)
{
    t_on_cleanup(sd_cleanup);
    for (int i = 0; i < sd_host_count(); i++) {
        SdHost *host = sd_host(i);
        if (!host->CardPresent())
            continue;
        SdCard c;
        if (!sd_card_init(host, &c) || c.mmc != mmc)
            continue;
        if (index-- == 0) {
            *card = c;
            return true;
        }
    }
    return false;
}


uint8_t *sd_alloc(size_t size, uint8_t fill, size_t align)
{
    uint8_t *p = (uint8_t *)heap_alloc(size ? size : 1, align, LIMIT_4G);
    if (p == nullptr)
        t_skip("no memory");
    memset(p, fill, size);
    return p;
}

bool sd_is_pattern(const uint8_t *buf, uint64_t sector, uint32_t count)
{
    for (uint32_t k = 0; k < count; k++) {
        uint64_t s = sector + k;
        for (uint32_t i = 0; i < 512; i++) {
            if (buf[k * 512 + i] != TEST_BLOCK_PATTERN(s, i))
                return false;
        }
    }
    return true;
}

SdErr sd_rw(const SdCard &card, bool read, uint64_t sector, uint32_t count,
            void *buf, SdMode mode, SdResult *res, bool cmd23)
{
    SdResult dummy;
    SdCmd cmd;
    bool multi = count > 1 || cmd23;

    if (res == nullptr)
        res = &dummy;
    cmd.index = read ? (multi ? 18 : 17) : (multi ? 25 : 24);
    cmd.arg = card.Addr(sector);
    cmd.buf = buf;
    cmd.blksz = 512;
    cmd.blocks = count;
    cmd.read = read;
    cmd.multi = multi;
    cmd.mode = mode;
    if (cmd23 && card.host->Kind() == SD_HOST_SDHCI && mode != SD_DMA) {
        cmd.auto_cmd23 = true;
    } else if (cmd23) {
        SdErr err = sd_cmd(card.host, 23, count, SD_RSP_R1);
        if (err != SD_OK)
            return err;
    } else if (multi) {
        cmd.auto_stop = true;
    }
    return card.host->Command(cmd, res);
}
