/*
 * Synopsys DesignWare Mobile Storage Host controller
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
#include "dw_mmc.h"

#include <string.h>

#include <algorithm>

#include "bits.h"
#include "cutils.h"
#include "fdt.h"
#include "machine.h"
#include "sd.h"

/* Registers, with the internal DMA controller in its 64 bit address layout. */
#define DW_MMC_CTRL      0x000
#define DW_MMC_PWREN     0x004
#define DW_MMC_CLKDIV    0x008
#define DW_MMC_CLKSRC    0x00c
#define DW_MMC_CLKENA    0x010
#define DW_MMC_TMOUT     0x014
#define DW_MMC_CTYPE     0x018
#define DW_MMC_BLKSIZ    0x01c
#define DW_MMC_BYTCNT    0x020
#define DW_MMC_INTMASK   0x024
#define DW_MMC_CMDARG    0x028
#define DW_MMC_CMD       0x02c
#define DW_MMC_RESP0     0x030 /* four registers */
#define DW_MMC_MINTSTS   0x040
#define DW_MMC_RINTSTS   0x044
#define DW_MMC_STATUS    0x048
#define DW_MMC_FIFOTH    0x04c
#define DW_MMC_CDETECT   0x050
#define DW_MMC_WRTPRT    0x054
#define DW_MMC_TCBCNT    0x05c
#define DW_MMC_TBBCNT    0x060
#define DW_MMC_VERID     0x06c
#define DW_MMC_HCON      0x070
#define DW_MMC_RST_N     0x078
#define DW_MMC_BMOD      0x080
#define DW_MMC_PLDMND    0x084
#define DW_MMC_DBADDRL   0x088
#define DW_MMC_DBADDRU   0x08c
#define DW_MMC_IDSTS     0x090
#define DW_MMC_IDINTEN   0x094
#define DW_MMC_DSCADDRL  0x098
#define DW_MMC_DSCADDRU  0x09c
#define DW_MMC_BUFADDRL  0x0a0
#define DW_MMC_BUFADDRU  0x0a4

/* The 32 bit address layout packs the same registers without the high
   halves. */
#define DW_MMC_IDSTS32   0x08c
#define DW_MMC_IDINTEN32 0x090
#define DW_MMC_DSCADDR32 0x094
#define DW_MMC_BUFADDR32 0x098

/* An offset that names no register in the layout in use. */
#define DW_MMC_NO_REG    UINT32_MAX

/* Where the FIFO is from version 2.40a on; every address above it reaches
   the FIFO too. */
#define DW_MMC_DATA      0x200

#define DW_MMC_CTRL_RESET      bit_at(0)
#define DW_MMC_CTRL_FIFO_RESET bit_at(1)
#define DW_MMC_CTRL_DMA_RESET  bit_at(2)
#define DW_MMC_CTRL_INT_ENABLE bit_at(4)
#define DW_MMC_CTRL_USE_IDMAC  bit_at(25)
#define DW_MMC_CTRL_RESETS \
    (DW_MMC_CTRL_RESET | DW_MMC_CTRL_FIFO_RESET | DW_MMC_CTRL_DMA_RESET)

/* Raw and masked interrupt status. */
#define DW_MMC_INT_CMD_DONE  bit_at(2)
#define DW_MMC_INT_DTO       bit_at(3)  /* data transfer over */
#define DW_MMC_INT_TXDR      bit_at(4)
#define DW_MMC_INT_RXDR      bit_at(5)
#define DW_MMC_INT_DCRC      bit_at(7)
#define DW_MMC_INT_RTO       bit_at(8)  /* response timeout */
#define DW_MMC_INT_DRTO      bit_at(9)  /* data read timeout */
#define DW_MMC_INT_FRUN      bit_at(11) /* FIFO under or overrun */
#define DW_MMC_INT_ACD       bit_at(14) /* auto command done */
#define DW_MMC_INT_EBE       bit_at(15)
#define DW_MMC_INT_SDIO      bit_at(16)

#define DW_MMC_CMD_INDEX_BITS 6
#define DW_MMC_CMD_RESP_EXP   bit_at(6)
#define DW_MMC_CMD_RESP_LONG  bit_at(7)
#define DW_MMC_CMD_DAT_EXP    bit_at(9)
#define DW_MMC_CMD_SEND_STOP  bit_at(12)
#define DW_MMC_CMD_STOP_ABORT bit_at(14)
#define DW_MMC_CMD_UPD_CLK    bit_at(21)
#define DW_MMC_CMD_START      bit_at(31)

#define DW_MMC_STATUS_RX_WMARK  bit_at(0)
#define DW_MMC_STATUS_TX_WMARK  bit_at(1)
#define DW_MMC_STATUS_EMPTY     bit_at(2)
#define DW_MMC_STATUS_FULL      bit_at(3)
#define DW_MMC_STATUS_DAT3      bit_at(8)
#define DW_MMC_STATUS_MC_BUSY   bit_at(10)
#define DW_MMC_STATUS_RESP_SHIFT 11
#define DW_MMC_STATUS_FCNT_SHIFT 17

#define DW_MMC_BMOD_SWR bit_at(0)
#define DW_MMC_BMOD_DSL_SHIFT 2
#define DW_MMC_BMOD_DE  bit_at(7)

/* Internal DMA status and interrupt enable. */
#define DW_MMC_IDMAC_TI  bit_at(0)
#define DW_MMC_IDMAC_RI  bit_at(1)
#define DW_MMC_IDMAC_FBE bit_at(2)
#define DW_MMC_IDMAC_DU  bit_at(4)
#define DW_MMC_IDMAC_NI  bit_at(8)
#define DW_MMC_IDMAC_AI  bit_at(9)
#define DW_MMC_IDMAC_NORMAL   (DW_MMC_IDMAC_TI | DW_MMC_IDMAC_RI)
#define DW_MMC_IDMAC_ABNORMAL (DW_MMC_IDMAC_FBE | DW_MMC_IDMAC_DU)
#define DW_MMC_IDMAC_W1C \
    (DW_MMC_IDMAC_NORMAL | DW_MMC_IDMAC_ABNORMAL | DW_MMC_IDMAC_NI | \
     DW_MMC_IDMAC_AI)

/* A descriptor: control, sizes, buffer 1, and buffer 2 or the next
   descriptor, in words or, with 64 bit addresses, in pairs of words. */
#define DW_MMC_DESC32_SIZE 16
#define DW_MMC_DESC64_SIZE 32
#define DW_MMC_DES0_DIC bit_at(1)
#define DW_MMC_DES0_CH  bit_at(4)
#define DW_MMC_DES0_ER  bit_at(5)
#define DW_MMC_DES0_OWN bit_at(31)

/* A list of nothing but empty descriptors is a guest mistake. */
#define DW_MMC_MAX_FETCHES 1024

/* 32 words of 32 bits. */
#define DW_MMC_FIFO_DEPTH 32
#define DW_MMC_FIFO_BYTES (DW_MMC_FIFO_DEPTH * 4)

/* 2.70a: the FIFO at 0x200 and the registers above 0x100. */
#define DW_MMC_VERID_VALUE 0x5342270a

/* One SD/MMC card, AHB host bus 32 bits wide, a 32 bit address bus, the
   internal DMA controller or none, FIFO RAM inside, hold register, and 32 or
   64 bit descriptor addresses. */
#define DW_MMC_HCON_CARD_TYPE      bit_at(0)
#define DW_MMC_HCON_AHB            bit_at(6)
#define DW_MMC_HCON_DATA_32        (1u << 7)
#define DW_MMC_HCON_ADDR_32        (31u << 10)
#define DW_MMC_HCON_DMA_NONE       (3u << 16)
#define DW_MMC_HCON_DMA_WIDTH_32   (1u << 18)
#define DW_MMC_HCON_FIFO_RAM       bit_at(21)
#define DW_MMC_HCON_HOLD_REG       bit_at(22)
#define DW_MMC_HCON_ADDR_CONFIG_64 bit_at(27)


//#pragma mark - DWMMCDevice

class DWMMCDevice final: public Device, public DeviceIO, public SDBusTarget,
                         public SDHost, public SDDataCompletion {
private:
    const char *fCompatible;
    uint32_t fClockHz;
    int fDmaBits; /* DMA address width, 0 for no DMA controller */

    PhysMemoryMap *fMemMap = nullptr;
    IRQSignal *fIrq = nullptr;
    bool fIrqLevel = false;
    Resource *fMmioRes = nullptr;
    Resource *fIrqRes = nullptr;

    std::unique_ptr<SDBus> fChildBus;
    SDDevice *fCard = nullptr;

    /* Registers without side effects are kept here as written. */
    uint32_t fRegs[DW_MMC_DATA / 4] {};
    uint32_t fResp[4] {};
    uint32_t fRintsts = 0;
    uint32_t fIdsts = 0;
    uint64_t fDescBase = 0;
    uint32_t fRespIndex = 0;
    bool fCardIrq = false;

    /* The data FIFO, as bytes from fFifoHead on. */
    uint8_t fFifo[DW_MMC_FIFO_BYTES] {};
    uint32_t fFifoHead = 0;
    uint32_t fFifoCount = 0;

    /* The block on its way to or from the card. */
    uint8_t fBuffer[SD_BLOCK_SIZE] {};
    uint32_t fBufferPos = 0;
    uint32_t fBufferLen = 0;
    uint32_t fBlockLen = SD_BLOCK_SIZE;

    /* A transfer has a card side, over until DTO, and a host side, which the
       guest or the DMA controller drains or fills through the FIFO. */
    bool fTransferActive = false;
    bool fTransferIsRead = false;
    bool fWaitingCard = false;
    uint32_t fDataCmd = 0;
    uint64_t fCardBytesLeft = 0;
    uint64_t fHostBytesLeft = 0;
    uint32_t fTcbcnt = 0;
    uint32_t fTbbcnt = 0;

    /* The internal DMA controller and the descriptor it holds. */
    uint64_t fDescAddr = 0;
    bool fDescOpen = false;
    bool fDmaSuspended = false;
    uint32_t fDescCtrl = 0;
    uint64_t fSegAddr[2] {};
    uint32_t fSegLen[2] {};
    int fSeg = 0;
    uint64_t fNextDesc = 0;
    uint64_t fBufAddr = 0;

    uint32_t &Reg(uint32_t offset) {return fRegs[offset / 4];}
    uint32_t Reg(uint32_t offset) const {return fRegs[offset / 4];}

    bool DmaCopy(uint64_t addr, uint8_t *buf, uint32_t len, bool to_guest);

    /* registers */
    void ResetRegs();
    uint32_t RegOffset(uint32_t offset) const;
    uint32_t ReadReg(uint32_t offset);
    void WriteReg(uint32_t offset, uint32_t val);
    uint32_t Rintsts() const;
    uint32_t Status() const;
    uint32_t Hcon() const;

    /* interrupts */
    void RaiseInt(uint32_t bits);
    void RaiseIdmac(uint32_t bits);
    void UpdateIrq();

    /* commands */
    void StartCommand();
    void StoreResponse(const uint8_t *resp, int len);
    void SendAutoStop();

    /* the FIFO */
    uint32_t FifoWords() const {return (fFifoCount + 3) / 4;}
    uint32_t RxWatermark() const {return get_bits(Reg(DW_MMC_FIFOTH), 16, 12);}
    uint32_t TxWatermark() const {return get_bits(Reg(DW_MMC_FIFOTH), 0, 12);}
    void FifoCompact();
    void FifoPush(const uint8_t *buf, uint32_t len);
    uint32_t FifoPop(uint8_t *buf, uint32_t len);
    uint32_t FifoRead();
    void FifoWrite(uint32_t val);

    /* the data engine */
    void TransferStart();
    void TransferStop();
    void TransferFail(uint32_t bits);
    void DataOver();
    void CardToFifo();
    void FifoToCard();
    bool ReadNextBlock();
    void ReadBlockArrived();
    bool WriteCurrentBlock();
    void WriteBlockAccepted();
    void UpdateTxRequest();

    /* the internal DMA controller */
    bool DmaEnabled() const;
    void DmaReset();
    void DmaRun();
    uint32_t DmaMove(uint8_t *buf, uint32_t len, bool to_guest);
    bool DescFetch();
    void DescClose();
    void DmaFatal();

public:
    DWMMCDevice(const char *name, const char *compatible, uint32_t clock_hz,
                int dma_bits):
        Device(name), fCompatible(compatible), fClockHz(clock_hz),
        fDmaBits(dma_bits) {}

    bool Prepare() override;
    bool Realize() override;
    void BuildFDT(FDTContext &ctx) override;
    Bus *ChildBus() override {return fChildBus.get();}

    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;

    bool AttachDevice(SDDevice *dev) override;
    void SetCardInterrupt(bool level) override;
    void DataComplete(bool ok) override;
};


//#pragma mark - guest memory

bool DWMMCDevice::DmaCopy(uint64_t addr, uint8_t *buf, uint32_t len,
                          bool to_guest)
{
    while (len > 0) {
        uint32_t page_left = DEVRAM_PAGE_SIZE - (addr & (DEVRAM_PAGE_SIZE - 1));
        uint32_t l = std::min(len, page_left);
        uint8_t *ptr = fMemMap->GetRamPtr(addr, to_guest);
        if (ptr == nullptr) {
            return false;
        }
        if (to_guest) {
            memcpy(ptr, buf, l);
        } else {
            memcpy(buf, ptr, l);
        }
        addr += l;
        buf += l;
        len -= l;
    }
    return true;
}


//#pragma mark - interrupts

uint32_t DWMMCDevice::Rintsts() const
{
    /* The SDIO interrupt follows the level the card drives. */
    return fRintsts | (fCardIrq ? DW_MMC_INT_SDIO : 0);
}


void DWMMCDevice::RaiseInt(uint32_t bits)
{
    /* Raw status is recorded whatever the mask says. */
    fRintsts |= bits;
    UpdateIrq();
}


void DWMMCDevice::RaiseIdmac(uint32_t bits)
{
    uint32_t enabled = bits & Reg(DW_MMC_IDINTEN);

    fIdsts |= bits;
    if ((enabled & DW_MMC_IDMAC_NORMAL) != 0) {
        fIdsts |= DW_MMC_IDMAC_NI;
    }
    if ((enabled & DW_MMC_IDMAC_ABNORMAL) != 0) {
        fIdsts |= DW_MMC_IDMAC_AI;
    }
    UpdateIrq();
}


void DWMMCDevice::UpdateIrq()
{
    bool level = (Reg(DW_MMC_CTRL) & DW_MMC_CTRL_INT_ENABLE) != 0 &&
                 (Rintsts() & Reg(DW_MMC_INTMASK)) != 0;

    /* The DMA controller's summary bits reach the same line. */
    if ((fIdsts & Reg(DW_MMC_IDINTEN) &
         (DW_MMC_IDMAC_NI | DW_MMC_IDMAC_AI)) != 0) {
        level = true;
    }
    if (level != fIrqLevel) {
        fIrqLevel = level;
        fIrq->Set(level ? 1 : 0);
    }
}


void DWMMCDevice::SetCardInterrupt(bool level)
{
    if (fCardIrq == level) {
        return;
    }
    fCardIrq = level;
    UpdateIrq();
}


//#pragma mark - register file

uint32_t DWMMCDevice::Hcon() const
{
    uint32_t hcon = DW_MMC_HCON_CARD_TYPE | DW_MMC_HCON_AHB |
                    DW_MMC_HCON_DATA_32 | DW_MMC_HCON_ADDR_32 |
                    DW_MMC_HCON_DMA_WIDTH_32 | DW_MMC_HCON_FIFO_RAM |
                    DW_MMC_HCON_HOLD_REG;

    if (fDmaBits == 0) {
        hcon |= DW_MMC_HCON_DMA_NONE;
    } else if (fDmaBits == 64) {
        hcon |= DW_MMC_HCON_ADDR_CONFIG_64;
    }
    return hcon;
}


uint32_t DWMMCDevice::RegOffset(uint32_t offset) const
{
    if (fDmaBits == 64) {
        return offset;
    }
    switch (offset) {
    case DW_MMC_IDSTS32:
        return DW_MMC_IDSTS;
    case DW_MMC_IDINTEN32:
        return DW_MMC_IDINTEN;
    case DW_MMC_DSCADDR32:
        return DW_MMC_DSCADDRL;
    case DW_MMC_BUFADDR32:
        return DW_MMC_BUFADDRL;
    case DW_MMC_DSCADDRU:
    case DW_MMC_BUFADDRL:
    case DW_MMC_BUFADDRU:
        return DW_MMC_NO_REG;
    default:
        return offset;
    }
}


uint32_t DWMMCDevice::Status() const
{
    uint32_t words = FifoWords();
    uint32_t status = 0;

    if (words > RxWatermark()) {
        status |= DW_MMC_STATUS_RX_WMARK;
    }
    if (words <= TxWatermark()) {
        status |= DW_MMC_STATUS_TX_WMARK;
    }
    if (words == 0) {
        status |= DW_MMC_STATUS_EMPTY;
    }
    if (words >= DW_MMC_FIFO_DEPTH) {
        status |= DW_MMC_STATUS_FULL;
    }
    /* DAT3 is pulled up by a card in the slot. Nothing holds DAT0 low, so a
       card is never busy. */
    if (fCard != nullptr) {
        status |= DW_MMC_STATUS_DAT3;
    }
    if (fTransferActive) {
        status |= DW_MMC_STATUS_MC_BUSY;
    }
    status |= fRespIndex << DW_MMC_STATUS_RESP_SHIFT;
    status |= words << DW_MMC_STATUS_FCNT_SHIFT;
    return status;
}


void DWMMCDevice::ResetRegs()
{
    memset(fRegs, 0, sizeof(fRegs));
    Reg(DW_MMC_TMOUT) = 0xffffff40;
    Reg(DW_MMC_BLKSIZ) = SD_BLOCK_SIZE;
    Reg(DW_MMC_BYTCNT) = SD_BLOCK_SIZE;
    /* The receive watermark comes out of reset one below the FIFO depth,
       which is how a driver can learn the depth. */
    Reg(DW_MMC_FIFOTH) = (DW_MMC_FIFO_DEPTH - 1) << 16;
    Reg(DW_MMC_RST_N) = 1;
}


uint32_t DWMMCDevice::ReadReg(uint32_t offset)
{
    offset = RegOffset(offset);
    switch (offset) {
    case DW_MMC_NO_REG:
        return 0;
    case DW_MMC_CMD:
        /* A command is taken as soon as it is written. */
        return Reg(DW_MMC_CMD) & ~DW_MMC_CMD_START;
    case DW_MMC_RESP0:
    case DW_MMC_RESP0 + 4:
    case DW_MMC_RESP0 + 8:
    case DW_MMC_RESP0 + 12:
        return fResp[(offset - DW_MMC_RESP0) / 4];
    case DW_MMC_MINTSTS:
        return Rintsts() & Reg(DW_MMC_INTMASK);
    case DW_MMC_RINTSTS:
        return Rintsts();
    case DW_MMC_STATUS:
        return Status();
    case DW_MMC_CDETECT:
        /* Active low. */
        return fCard != nullptr ? 0 : 1;
    case DW_MMC_WRTPRT:
        return fCard != nullptr && fCard->ReadOnly() ? 1 : 0;
    case DW_MMC_TCBCNT:
        return fTcbcnt;
    case DW_MMC_TBBCNT:
        return fTbbcnt;
    case DW_MMC_VERID:
        return DW_MMC_VERID_VALUE;
    case DW_MMC_HCON:
        return Hcon();
    case DW_MMC_PLDMND:
        return 0;
    case DW_MMC_DBADDRL:
        return (uint32_t)fDescBase;
    case DW_MMC_DBADDRU:
        return (uint32_t)(fDescBase >> 32);
    case DW_MMC_IDSTS:
        return fIdsts;
    case DW_MMC_DSCADDRL:
        return (uint32_t)fDescAddr;
    case DW_MMC_DSCADDRU:
        return (uint32_t)(fDescAddr >> 32);
    case DW_MMC_BUFADDRL:
        return (uint32_t)fBufAddr;
    case DW_MMC_BUFADDRU:
        return (uint32_t)(fBufAddr >> 32);
    default:
        return Reg(offset);
    }
}


void DWMMCDevice::WriteReg(uint32_t offset, uint32_t val)
{
    offset = RegOffset(offset);
    switch (offset) {
    case DW_MMC_CTRL:
        /* The reset bits clear themselves: every reset is over at once. */
        Reg(DW_MMC_CTRL) = val & ~DW_MMC_CTRL_RESETS;
        if ((val & DW_MMC_CTRL_RESET) != 0) {
            TransferStop();
        }
        if ((val & DW_MMC_CTRL_FIFO_RESET) != 0) {
            fFifoHead = 0;
            fFifoCount = 0;
        }
        if ((val & DW_MMC_CTRL_DMA_RESET) != 0) {
            fDescOpen = false;
            fDmaSuspended = false;
        }
        UpdateIrq();
        break;
    case DW_MMC_CMD:
        Reg(DW_MMC_CMD) = val;
        if ((val & DW_MMC_CMD_START) != 0) {
            StartCommand();
        }
        break;
    case DW_MMC_INTMASK:
        Reg(DW_MMC_INTMASK) = val;
        UpdateIrq();
        break;
    case DW_MMC_RINTSTS:
        fRintsts &= ~val;
        UpdateIrq();
        break;
    case DW_MMC_BMOD:
        Reg(DW_MMC_BMOD) = val & ~DW_MMC_BMOD_SWR;
        if ((val & DW_MMC_BMOD_SWR) != 0) {
            DmaReset();
        }
        DmaRun();
        break;
    case DW_MMC_PLDMND:
        /* Resumes a controller that found a descriptor it did not own. */
        fDmaSuspended = false;
        DmaRun();
        break;
    case DW_MMC_DBADDRL:
        fDescBase = set_bits(fDescBase, 0, 32, val);
        fDescAddr = fDescBase;
        break;
    case DW_MMC_DBADDRU:
        fDescBase = set_bits(fDescBase, 32, 32, val);
        fDescAddr = fDescBase;
        break;
    case DW_MMC_IDSTS:
        fIdsts &= ~(val & DW_MMC_IDMAC_W1C);
        UpdateIrq();
        break;
    case DW_MMC_IDINTEN:
        Reg(DW_MMC_IDINTEN) = val;
        UpdateIrq();
        break;
    case DW_MMC_RESP0:
    case DW_MMC_RESP0 + 4:
    case DW_MMC_RESP0 + 8:
    case DW_MMC_RESP0 + 12:
    case DW_MMC_MINTSTS:
    case DW_MMC_STATUS:
    case DW_MMC_CDETECT:
    case DW_MMC_WRTPRT:
    case DW_MMC_TCBCNT:
    case DW_MMC_TBBCNT:
    case DW_MMC_VERID:
    case DW_MMC_HCON:
    case DW_MMC_DSCADDRL:
    case DW_MMC_DSCADDRU:
    case DW_MMC_BUFADDRL:
    case DW_MMC_BUFADDRU:
    case DW_MMC_NO_REG:
        break;
    default:
        Reg(offset) = val;
        break;
    }
}


//#pragma mark - commands

void DWMMCDevice::StoreResponse(const uint8_t *resp, int len)
{
    if (len == SD_RESPONSE_LONG) {
        /* All 128 bits, CRC included, least significant word first. */
        fResp[3] = get_be32(resp);
        fResp[2] = get_be32(resp + 4);
        fResp[1] = get_be32(resp + 8);
        fResp[0] = get_be32(resp + 12);
    } else if (len == SD_RESPONSE_SHORT) {
        fResp[0] = get_be32(resp);
    }
}


void DWMMCDevice::SendAutoStop()
{
    SDCommand cmd;
    uint8_t resp[SD_RESPONSE_LONG];

    cmd.index = SD_CMD_STOP_TRANSMISSION;
    cmd.arg = 0;
    int len = fCard->Command(cmd, resp);
    if (len < 0) {
        RaiseInt(DW_MMC_INT_RTO);
        return;
    }
    /* The auto stop response goes in the second response register. */
    if (len == SD_RESPONSE_SHORT) {
        fResp[1] = get_be32(resp);
    }
    RaiseInt(DW_MMC_INT_ACD);
}


void DWMMCDevice::StartCommand()
{
    SDCommand cmd;
    uint8_t resp[SD_RESPONSE_LONG];
    uint32_t cmdr = Reg(DW_MMC_CMD);

    /* A clock update only loads the clock registers into the card side, and
       raises no interrupt. */
    if ((cmdr & DW_MMC_CMD_UPD_CLK) != 0) {
        return;
    }

    cmd.index = get_bits(cmdr, 0, DW_MMC_CMD_INDEX_BITS);
    cmd.arg = Reg(DW_MMC_CMDARG);

    int len = fCard != nullptr ? fCard->Command(cmd, resp) : -1;
    bool want_resp = (cmdr & DW_MMC_CMD_RESP_EXP) != 0;
    if (len < 0 || (want_resp && len == 0)) {
        RaiseInt(DW_MMC_INT_RTO | DW_MMC_INT_CMD_DONE);
        return;
    }
    if (want_resp) {
        StoreResponse(resp, len);
        fRespIndex = cmd.index;
    }

    if ((cmdr & DW_MMC_CMD_STOP_ABORT) != 0 && fTransferActive) {
        /* A stop ends the data transfer in progress, which is over from
           the controller's point of view too. */
        TransferStop();
        RaiseInt(DW_MMC_INT_DTO);
    }
    RaiseInt(DW_MMC_INT_CMD_DONE);

    if ((cmdr & DW_MMC_CMD_DAT_EXP) != 0) {
        fDataCmd = cmdr;
        TransferStart();
    }
}


//#pragma mark - the FIFO

void DWMMCDevice::FifoCompact()
{
    if (fFifoHead != 0) {
        memmove(fFifo, fFifo + fFifoHead, fFifoCount);
        fFifoHead = 0;
    }
}


void DWMMCDevice::FifoPush(const uint8_t *buf, uint32_t len)
{
    FifoCompact();
    memcpy(fFifo + fFifoCount, buf, len);
    fFifoCount += len;
}


uint32_t DWMMCDevice::FifoPop(uint8_t *buf, uint32_t len)
{
    len = std::min(len, fFifoCount);
    if (buf != nullptr) {
        memcpy(buf, fFifo + fFifoHead, len);
    }
    fFifoHead += len;
    fFifoCount -= len;
    if (fFifoCount == 0) {
        fFifoHead = 0;
    }
    return len;
}


uint32_t DWMMCDevice::FifoRead()
{
    uint8_t word[4] = {};

    uint32_t n = FifoPop(word, 4);
    if (n == 0) {
        RaiseInt(DW_MMC_INT_FRUN);
        return 0;
    }
    if (fTransferIsRead) {
        fHostBytesLeft -= std::min<uint64_t>(n, fHostBytesLeft);
    }
    fTbbcnt += n;
    CardToFifo();
    return get_le32(word);
}


void DWMMCDevice::FifoWrite(uint32_t val)
{
    uint8_t word[4];
    uint32_t n = 4;

    /* The last word of a transfer may carry fewer bytes than it holds. */
    if (fTransferActive && !fTransferIsRead) {
        n = (uint32_t)std::min<uint64_t>(n, fHostBytesLeft);
        if (n == 0) {
            return;
        }
    }
    if (fFifoCount + n > DW_MMC_FIFO_BYTES) {
        RaiseInt(DW_MMC_INT_FRUN);
        return;
    }
    put_le32(word, val);
    FifoPush(word, n);
    if (fTransferActive && !fTransferIsRead) {
        fHostBytesLeft -= n;
    }
    fTbbcnt += n;
    FifoToCard();
    UpdateTxRequest();
}


//#pragma mark - the data engine

void DWMMCDevice::TransferStart()
{
    uint32_t block_len = get_bits(Reg(DW_MMC_BLKSIZ), 0, 16);
    uint32_t byte_count = Reg(DW_MMC_BYTCNT);

    fTcbcnt = 0;
    fTbbcnt = 0;

    /* The card's view of the command decides the direction. */
    SDDataDirEnum dir = fCard->DataDir();
    if (dir == SD_DATA_NONE) {
        TransferFail(DW_MMC_INT_DRTO);
        return;
    }
    if (block_len == 0 || block_len > SD_BLOCK_SIZE) {
        TransferFail(DW_MMC_INT_EBE);
        return;
    }

    fBlockLen = block_len;
    fTransferIsRead = dir == SD_DATA_READ;
    fTransferActive = true;
    fWaitingCard = false;
    fBufferPos = 0;
    fBufferLen = 0;
    /* A byte count of zero runs until a stop command ends it. */
    fCardBytesLeft = byte_count != 0 ? byte_count : UINT64_MAX;
    fHostBytesLeft = fCardBytesLeft;

    if (fTransferIsRead) {
        CardToFifo();
    } else {
        FifoToCard();
        UpdateTxRequest();
    }
    DmaRun();
}


void DWMMCDevice::TransferStop()
{
    if (fTransferActive && fCard != nullptr) {
        fCard->StopTransfer();
    }
    fTransferActive = false;
    fWaitingCard = false;
    fCardBytesLeft = 0;
    fHostBytesLeft = 0;
    fBufferPos = 0;
    fBufferLen = 0;
}


void DWMMCDevice::TransferFail(uint32_t bits)
{
    TransferStop();
    fDescOpen = false;
    /* Data transfer over follows an error too, so a driver waiting for it
       is not left waiting. */
    RaiseInt(bits | DW_MMC_INT_DTO);
}


void DWMMCDevice::DataOver()
{
    fTransferActive = false;
    if ((fDataCmd & DW_MMC_CMD_SEND_STOP) != 0) {
        SendAutoStop();
    }
    RaiseInt(DW_MMC_INT_DTO);
}


bool DWMMCDevice::ReadNextBlock()
{
    SDXferStatusEnum status = fCard->ReadBlock(fBuffer, fBlockLen, this);
    if (status == SD_XFER_PENDING) {
        fWaitingCard = true;
        return false;
    }
    if (status == SD_XFER_ERROR) {
        TransferFail(DW_MMC_INT_DCRC);
        return false;
    }
    ReadBlockArrived();
    return true;
}


void DWMMCDevice::ReadBlockArrived()
{
    fBufferPos = 0;
    fBufferLen = (uint32_t)std::min<uint64_t>(fBlockLen, fCardBytesLeft);
    fCardBytesLeft -= fBufferLen;
    fTcbcnt += fBufferLen;
}


void DWMMCDevice::CardToFifo()
{
    bool added = false;

    while (fTransferActive && fFifoCount < DW_MMC_FIFO_BYTES) {
        if (fBufferPos < fBufferLen) {
            uint32_t n = std::min(DW_MMC_FIFO_BYTES - fFifoCount,
                                  fBufferLen - fBufferPos);
            FifoPush(fBuffer + fBufferPos, n);
            fBufferPos += n;
            added = true;
            continue;
        }
        if (fCardBytesLeft == 0) {
            /* Everything the card sent is in the FIFO. */
            DataOver();
            break;
        }
        if (fWaitingCard || !ReadNextBlock()) {
            break;
        }
    }
    /* Each refill past the watermark asks for the FIFO to be drained. */
    if (added && !DmaEnabled() && FifoWords() > RxWatermark()) {
        RaiseInt(DW_MMC_INT_RXDR);
    }
}


bool DWMMCDevice::WriteCurrentBlock()
{
    /* A short last block is padded out to the length the card takes. */
    memset(fBuffer + fBufferPos, 0, fBlockLen - fBufferPos);
    SDXferStatusEnum status = fCard->WriteBlock(fBuffer, fBlockLen, this);
    if (status == SD_XFER_PENDING) {
        fWaitingCard = true;
        return false;
    }
    if (status == SD_XFER_ERROR) {
        TransferFail(DW_MMC_INT_DCRC);
        return false;
    }
    WriteBlockAccepted();
    return true;
}


void DWMMCDevice::WriteBlockAccepted()
{
    uint32_t n = (uint32_t)std::min<uint64_t>(fBlockLen, fCardBytesLeft);
    fCardBytesLeft -= n;
    fTcbcnt += n;
    fBufferPos = 0;
}


void DWMMCDevice::FifoToCard()
{
    while (fTransferActive && !fWaitingCard) {
        if (fCardBytesLeft == 0) {
            DataOver();
            return;
        }
        uint32_t target = (uint32_t)std::min<uint64_t>(fBlockLen,
                                                       fCardBytesLeft);
        if (fBufferPos < target) {
            if (fFifoCount == 0) {
                return;
            }
            fBufferPos += FifoPop(fBuffer + fBufferPos, target - fBufferPos);
            continue;
        }
        if (!WriteCurrentBlock()) {
            return;
        }
    }
}


void DWMMCDevice::UpdateTxRequest()
{
    if (fTransferActive && !fTransferIsRead && fHostBytesLeft > 0 &&
        !DmaEnabled() && FifoWords() <= TxWatermark()) {
        RaiseInt(DW_MMC_INT_TXDR);
    }
}


void DWMMCDevice::DataComplete(bool ok)
{
    if (!fWaitingCard) {
        return;
    }
    fWaitingCard = false;
    if (!fTransferActive) {
        /* The transfer was stopped while the block was with the card. */
        return;
    }
    if (!ok) {
        TransferFail(DW_MMC_INT_DCRC);
        return;
    }
    if (fTransferIsRead) {
        ReadBlockArrived();
        CardToFifo();
    } else {
        WriteBlockAccepted();
        FifoToCard();
        UpdateTxRequest();
    }
    DmaRun();
}


//#pragma mark - the internal DMA controller

bool DWMMCDevice::DmaEnabled() const
{
    return fDmaBits != 0 && (Reg(DW_MMC_CTRL) & DW_MMC_CTRL_USE_IDMAC) != 0 &&
           (Reg(DW_MMC_BMOD) & DW_MMC_BMOD_DE) != 0;
}


void DWMMCDevice::DmaReset()
{
    fDescAddr = fDescBase;
    fDescOpen = false;
    fDmaSuspended = false;
}


void DWMMCDevice::DmaFatal()
{
    /* A bus error disables the controller until it is reset. */
    Reg(DW_MMC_BMOD) &= ~DW_MMC_BMOD_DE;
    RaiseIdmac(DW_MMC_IDMAC_FBE);
    TransferFail(DW_MMC_INT_DCRC);
}


bool DWMMCDevice::DescFetch()
{
    bool wide = fDmaBits == 64;
    uint32_t desc_size = wide ? DW_MMC_DESC64_SIZE : DW_MMC_DESC32_SIZE;
    uint8_t desc[DW_MMC_DESC64_SIZE];

    if (!DmaCopy(fDescAddr, desc, desc_size, false)) {
        DmaFatal();
        return false;
    }
    uint32_t des0 = get_le32(desc);
    if ((des0 & DW_MMC_DES0_OWN) == 0) {
        /* Suspended until the driver demands a poll. */
        fDmaSuspended = true;
        RaiseIdmac(DW_MMC_IDMAC_DU);
        return false;
    }

    uint32_t sizes;
    uint64_t second;
    if (wide) {
        sizes = get_le32(desc + 8);
        fSegAddr[0] = get_le64(desc + 16);
        second = get_le64(desc + 24);
    } else {
        sizes = get_le32(desc + 4);
        fSegAddr[0] = get_le32(desc + 8);
        second = get_le32(desc + 12);
    }

    fDescCtrl = des0;
    fSegLen[0] = get_bits(sizes, 0, 13);
    if ((des0 & DW_MMC_DES0_CH) != 0) {
        fSegLen[1] = 0;
        fNextDesc = second;
    } else {
        /* Ring mode: the descriptors follow one another, the skip length
           apart in words. */
        fSegAddr[1] = second;
        fSegLen[1] = get_bits(sizes, 13, 13);
        fNextDesc = fDescAddr + desc_size +
                    4 * get_bits(Reg(DW_MMC_BMOD), DW_MMC_BMOD_DSL_SHIFT, 5);
        if (!wide) {
            fNextDesc = (uint32_t)fNextDesc;
        }
    }
    if ((des0 & DW_MMC_DES0_ER) != 0) {
        fNextDesc = fDescBase;
    }
    fSeg = 0;
    fDescOpen = true;
    return true;
}


void DWMMCDevice::DescClose()
{
    uint8_t des0[4];

    put_le32(des0, fDescCtrl & ~DW_MMC_DES0_OWN);
    fDescOpen = false;
    if (!DmaCopy(fDescAddr, des0, sizeof(des0), true)) {
        DmaFatal();
        return;
    }
    fDescAddr = fNextDesc;
    if ((fDescCtrl & DW_MMC_DES0_DIC) == 0) {
        RaiseIdmac(fTransferIsRead ? DW_MMC_IDMAC_RI : DW_MMC_IDMAC_TI);
    }
}


uint32_t DWMMCDevice::DmaMove(uint8_t *buf, uint32_t len, bool to_guest)
{
    uint32_t done = 0;
    int fetches = 0;

    while (done < len) {
        if (!fDescOpen) {
            if (++fetches > DW_MMC_MAX_FETCHES) {
                DmaFatal();
                return done;
            }
            if (!DescFetch()) {
                return done;
            }
            continue;
        }
        if (fSeg >= 2) {
            DescClose();
            continue;
        }
        if (fSegLen[fSeg] == 0) {
            fSeg++;
            continue;
        }
        uint32_t n = std::min(len - done, fSegLen[fSeg]);
        if (!DmaCopy(fSegAddr[fSeg], buf + done, n, to_guest)) {
            DmaFatal();
            return done;
        }
        fSegAddr[fSeg] += n;
        fSegLen[fSeg] -= n;
        fBufAddr = fSegAddr[fSeg];
        done += n;
    }
    return done;
}


void DWMMCDevice::DmaRun()
{
    if (!DmaEnabled() || fDmaSuspended || fHostBytesLeft == 0) {
        return;
    }

    while (fHostBytesLeft > 0) {
        uint32_t n;
        if (fTransferIsRead) {
            if (fFifoCount == 0) {
                CardToFifo();
                if (fFifoCount == 0) {
                    return;
                }
            }
            n = (uint32_t)std::min<uint64_t>(fFifoCount, fHostBytesLeft);
            n = DmaMove(fFifo + fFifoHead, n, true);
            FifoPop(nullptr, n);
        } else {
            if (fFifoCount == DW_MMC_FIFO_BYTES) {
                FifoToCard();
                if (fFifoCount == DW_MMC_FIFO_BYTES) {
                    return;
                }
            }
            FifoCompact();
            n = (uint32_t)std::min<uint64_t>(DW_MMC_FIFO_BYTES - fFifoCount,
                                             fHostBytesLeft);
            n = DmaMove(fFifo + fFifoCount, n, false);
            fFifoCount += n;
        }
        if (n == 0) {
            return;
        }
        fHostBytesLeft -= n;
        fTbbcnt += n;
        if (!fTransferIsRead) {
            FifoToCard();
        }
    }
    /* The last descriptor is handed back once the data it describes has
       moved. */
    if (fDescOpen) {
        DescClose();
    }
    if (fTransferIsRead) {
        CardToFifo();
    }
}


//#pragma mark - memory mapped access

uint32_t DWMMCDevice::DeviceRead(uint32_t offset, int size_log2)
{
    (void)size_log2;
    if (offset >= DW_MMC_DATA) {
        return FifoRead();
    }
    return ReadReg(offset & ~3u);
}


void DWMMCDevice::DeviceWrite(uint32_t offset, uint32_t val, int size_log2)
{
    (void)size_log2;
    if (offset >= DW_MMC_DATA) {
        FifoWrite(val);
        return;
    }
    WriteReg(offset & ~3u, val);
}


//#pragma mark - the slot

bool DWMMCDevice::AttachDevice(SDDevice *dev)
{
    if (fCard != nullptr) {
        vm_error("%s: the slot already holds '%s'\n", Name(), fCard->Name());
        return false;
    }
    fCard = dev;
    dev->SetHost(this);
    dev->Reset();
    return true;
}


//#pragma mark - lifecycle

bool DWMMCDevice::Prepare()
{
    if (ParentBus()->AsPCIBus() != nullptr) {
        vm_error("%s: must be attached to a system bus\n", Name());
        return false;
    }
    fMmioRes = AddResource(RES_MMIO, DW_MMC_REG_SIZE, DW_MMC_REG_SIZE);
    fIrqRes = AddResource(RES_IRQ, 1);
    if (fMmioRes == nullptr || fIrqRes == nullptr) {
        return false;
    }
    fChildBus = std::make_unique<SDBus>(this, this);
    return true;
}


bool DWMMCDevice::Realize()
{
    SystemBus *sys = static_cast<SystemBus *>(ParentBus());

    fIrq = sys->IrqSignalFor(fIrqRes->base);
    if (fIrq == nullptr) {
        vm_error("%s: bad interrupt line %d\n", Name(), (int)fIrqRes->base);
        return false;
    }
    fMemMap = sys->MemMap();
    ResetRegs();
    fMemMap->RegisterDevice(fMmioRes->base, DW_MMC_REG_SIZE, this,
                            DEVIO_SIZE32);
    return true;
}


void DWMMCDevice::BuildFDT(FDTContext &ctx)
{
    FDTBuilder *fdt = ctx.fdt;

    /* Both the bus interface and the card interface clock, from one fixed
       clock of their own. */
    uint32_t clock_phandle = fdt->AllocPhandle();
    fdt->BeginNodeNum("mmc-clock", fMmioRes->base);
    fdt->PropStr("compatible", "fixed-clock");
    fdt->PropU32("#clock-cells", 0);
    fdt->PropU32("clock-frequency", fClockHz);
    fdt->PropU32("phandle", clock_phandle);
    fdt->EndNode();

    fdt->BeginNodeNum("mmc", fMmioRes->base);
    fdt->PropStr("compatible", fCompatible);
    fdt->PropU64Range("reg", fMmioRes->base, fMmioRes->size);
    fdt_prop_irq(ctx, fIrqRes->base);
    fdt->AddCellU32(clock_phandle);
    fdt->AddCellU32(clock_phandle);
    fdt->PropCells("clocks");
    fdt->PropStrList("clock-names", "biu", "ciu", nullptr);
    fdt->PropU32("fifo-depth", DW_MMC_FIFO_DEPTH);
    if (fDmaBits == 0) {
        /* For drivers that take the DMA controller for granted rather than
           reading it from the hardware configuration register. */
        fdt->PropEmpty("fifo-mode");
    }
    fdt->PropU32("max-frequency", fClockHz);
    if (fCard != nullptr) {
        fdt->PropU32("bus-width", fCard->BusWidth());
        if (!fCard->Removable()) {
            fdt->PropEmpty("non-removable");
        }
        if (fCard->CardType() == SD_CARD_MMC) {
            fdt->PropEmpty("cap-mmc-highspeed");
        } else {
            fdt->PropEmpty("cap-sd-highspeed");
        }
    }
    fdt->EndNode();
}


//#pragma mark - factory

Device *dw_mmc_node_create(const char *name, const char *compatible,
                           uint32_t clock_hz, int dma_bits)
{
    return new DWMMCDevice(name, compatible, clock_hz, dma_bits);
}
