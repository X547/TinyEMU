/*
 * Synopsys DesignWare I2C controller
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
#include "dw_i2c.h"

#include <string.h>

#include <algorithm>

#include "bits.h"
#include "device_class.h"
#include "fdt.h"
#include "i2c.h"
#include "machine.h"

#define DW_IC_CON               0x00
#define DW_IC_TAR               0x04
#define DW_IC_SAR               0x08
#define DW_IC_DATA_CMD          0x10
#define DW_IC_SS_SCL_HCNT       0x14
#define DW_IC_SS_SCL_LCNT       0x18
#define DW_IC_FS_SCL_HCNT       0x1c
#define DW_IC_FS_SCL_LCNT       0x20
#define DW_IC_HS_SCL_HCNT       0x24
#define DW_IC_HS_SCL_LCNT       0x28
#define DW_IC_INTR_STAT         0x2c
#define DW_IC_INTR_MASK         0x30
#define DW_IC_RAW_INTR_STAT     0x34
#define DW_IC_RX_TL             0x38
#define DW_IC_TX_TL             0x3c
#define DW_IC_CLR_INTR          0x40
#define DW_IC_CLR_RX_UNDER      0x44
#define DW_IC_CLR_RX_OVER       0x48
#define DW_IC_CLR_TX_OVER       0x4c
#define DW_IC_CLR_RD_REQ        0x50
#define DW_IC_CLR_TX_ABRT       0x54
#define DW_IC_CLR_RX_DONE       0x58
#define DW_IC_CLR_ACTIVITY      0x5c
#define DW_IC_CLR_STOP_DET      0x60
#define DW_IC_CLR_START_DET     0x64
#define DW_IC_CLR_GEN_CALL      0x68
#define DW_IC_ENABLE            0x6c
#define DW_IC_STATUS            0x70
#define DW_IC_TXFLR             0x74
#define DW_IC_RXFLR             0x78
#define DW_IC_SDA_HOLD          0x7c
#define DW_IC_TX_ABRT_SOURCE    0x80
#define DW_IC_ENABLE_STATUS     0x9c
#define DW_IC_CLR_RESTART_DET   0xa8
#define DW_IC_COMP_PARAM_1      0xf4
#define DW_IC_COMP_VERSION      0xf8
#define DW_IC_COMP_TYPE         0xfc
#define DW_IC_REG_FILE_SIZE     0x100

#define DW_IC_CON_MASTER        bit_at(0)
#define DW_IC_CON_SPEED_FAST    (2u << 1)
#define DW_IC_CON_10BITADDR_MASTER bit_at(4)
#define DW_IC_CON_RESTART_EN    bit_at(5)
#define DW_IC_CON_SLAVE_DISABLE bit_at(6)

#define DW_IC_TAR_10BITADDR_MASTER bit_at(12)

#define DW_IC_DATA_CMD_READ     bit_at(8)
#define DW_IC_DATA_CMD_STOP     bit_at(9)
#define DW_IC_DATA_CMD_RESTART  bit_at(10)

#define DW_IC_INTR_RX_UNDER     bit_at(0)
#define DW_IC_INTR_RX_OVER      bit_at(1)
#define DW_IC_INTR_RX_FULL      bit_at(2)
#define DW_IC_INTR_TX_OVER      bit_at(3)
#define DW_IC_INTR_TX_EMPTY     bit_at(4)
#define DW_IC_INTR_TX_ABRT      bit_at(6)
#define DW_IC_INTR_ACTIVITY     bit_at(8)
#define DW_IC_INTR_STOP_DET     bit_at(9)
#define DW_IC_INTR_START_DET    bit_at(10)
#define DW_IC_INTR_MST_ON_HOLD  bit_at(13)

#define DW_IC_ENABLE_ENABLE     bit_at(0)
#define DW_IC_ENABLE_ABORT      bit_at(1)

#define DW_IC_STATUS_ACTIVITY   bit_at(0)
#define DW_IC_STATUS_TFNF       bit_at(1)
#define DW_IC_STATUS_TFE        bit_at(2)
#define DW_IC_STATUS_RFNE       bit_at(3)
#define DW_IC_STATUS_RFF        bit_at(4)
#define DW_IC_STATUS_MST_ACTIVITY bit_at(5)
#define DW_IC_STATUS_MST_HOLD_TX_FIFO_EMPTY bit_at(7)

#define DW_IC_ABRT_7B_ADDR_NOACK  bit_at(0)
#define DW_IC_ABRT_10ADDR1_NOACK  bit_at(1)
#define DW_IC_ABRT_TXDATA_NOACK   bit_at(3)
#define DW_IC_ABRT_USER_ABRT      bit_at(16)

/* Commands and received bytes the FIFOs hold. */
#define DW_I2C_FIFO_DEPTH 16

/* The reference clock, which the driver divides down for the bus. */
#define DW_I2C_CLOCK_HZ 100000000

/* A 32 bit APB interface, fast mode at most, a single combined interrupt,
   and the FIFO depths encoded as depth minus one. */
#define DW_IC_COMP_PARAM_1_VALUE \
    (2u | (2u << 2) | bit_at(5) | bit_at(7) | \
     ((DW_I2C_FIFO_DEPTH - 1u) << 8) | ((DW_I2C_FIFO_DEPTH - 1u) << 16))

/* "201*", recent enough to have the SDA hold register. */
#define DW_IC_COMP_VERSION_VALUE 0x3230312a
#define DW_IC_COMP_TYPE_VALUE    0x44570140


//#pragma mark - DWI2CDevice

/* The controller in master mode. Commands run as they are queued, each
   taking the bus no time, so a transfer is over as soon as its last command
   is written. Like the parts the drivers assume, it holds the bus when the
   command FIFO runs dry without a STOP, rather than ending the transfer. */
class DWI2CDevice final: public Device, public DeviceIO {
private:
    const char *fCompatible;

    IRQSignal *fIrq = nullptr;
    bool fIrqLevel = false;
    Resource *fMmioRes = nullptr;
    Resource *fIrqRes = nullptr;
    std::unique_ptr<I2CBus> fChildBus;

    /* Registers without side effects are kept here as written. */
    uint32_t fRegs[DW_IC_REG_FILE_SIZE / 4] {};
    /* Interrupts that stay raised until cleared; the others follow the
       FIFO levels. */
    uint32_t fRawIntr = 0;
    uint32_t fAbortSource = 0;

    uint16_t fTxFifo[DW_I2C_FIFO_DEPTH] {};
    int fTxHead = 0;
    int fTxCount = 0;
    uint8_t fRxFifo[DW_I2C_FIFO_DEPTH] {};
    int fRxHead = 0;
    int fRxCount = 0;
    /* After an abort the command FIFO takes nothing until the abort is
       acknowledged. */
    bool fTxFlushed = false;

    /* The transfer on the bus. */
    bool fActive = false;
    bool fReading = false;
    I2CDevice *fTarget = nullptr;

    uint32_t &Reg(uint32_t offset) {return fRegs[offset / 4];}
    uint32_t Reg(uint32_t offset) const {return fRegs[offset / 4];}
    bool Enabled() const {return (Reg(DW_IC_ENABLE) & DW_IC_ENABLE_ENABLE) != 0;}

    void ResetRegs();
    uint32_t ReadReg(uint32_t offset);
    void WriteReg(uint32_t offset, uint32_t val);
    uint32_t RawIntr() const;
    uint32_t Status() const;
    void UpdateIrq();
    void SetEnable(uint32_t val);
    void ClearAbort();

    /* the transfer engine */
    void QueueCommand(uint16_t cmd);
    uint8_t ReadData();
    void Pump();
    void Execute(uint16_t cmd);
    bool BeginPhase(bool read);
    void EndTransfer();
    void Abort(uint32_t source);

public:
    DWI2CDevice(const char *name, const char *compatible):
        Device(name), fCompatible(compatible) {}

    bool Prepare() override;
    bool Realize() override;
    void BuildFDT(FDTContext &ctx) override;
    Bus *ChildBus() override {return fChildBus.get();}

    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;
};


//#pragma mark - interrupts and status

uint32_t DWI2CDevice::RawIntr() const
{
    uint32_t raw = fRawIntr;

    if (fRxCount > (int)Reg(DW_IC_RX_TL)) {
        raw |= DW_IC_INTR_RX_FULL;
    }
    /* A disabled controller with nothing on the bus asks for no data. */
    if (Enabled() && fTxCount <= (int)Reg(DW_IC_TX_TL)) {
        raw |= DW_IC_INTR_TX_EMPTY;
    }
    if (fActive && fTxCount == 0) {
        raw |= DW_IC_INTR_MST_ON_HOLD;
    }
    return raw;
}


uint32_t DWI2CDevice::Status() const
{
    uint32_t status = 0;

    if (fActive || fTxCount > 0) {
        status |= DW_IC_STATUS_ACTIVITY;
    }
    if (fTxCount < DW_I2C_FIFO_DEPTH) {
        status |= DW_IC_STATUS_TFNF;
    }
    if (fTxCount == 0) {
        status |= DW_IC_STATUS_TFE;
    }
    if (fRxCount > 0) {
        status |= DW_IC_STATUS_RFNE;
    }
    if (fRxCount == DW_I2C_FIFO_DEPTH) {
        status |= DW_IC_STATUS_RFF;
    }
    if (fActive) {
        status |= DW_IC_STATUS_MST_ACTIVITY;
        if (fTxCount == 0) {
            status |= DW_IC_STATUS_MST_HOLD_TX_FIFO_EMPTY;
        }
    }
    return status;
}


void DWI2CDevice::UpdateIrq()
{
    bool level = (RawIntr() & Reg(DW_IC_INTR_MASK)) != 0;

    if (level != fIrqLevel) {
        fIrqLevel = level;
        fIrq->Set(level ? 1 : 0);
    }
}


//#pragma mark - the transfer engine

void DWI2CDevice::Abort(uint32_t source)
{
    /* The controller ends the transfer with a STOP and drops the commands
       still queued. */
    if (fTarget != nullptr) {
        fTarget->Stop();
    }
    fTarget = nullptr;
    fActive = false;
    fAbortSource |= source;
    fRawIntr |= DW_IC_INTR_TX_ABRT | DW_IC_INTR_STOP_DET;
    fTxCount = 0;
    fTxFlushed = true;
}


void DWI2CDevice::ClearAbort()
{
    fRawIntr &= ~DW_IC_INTR_TX_ABRT;
    fAbortSource = 0;
    fTxFlushed = false;
}


bool DWI2CDevice::BeginPhase(bool read)
{
    bool ten_bit = (Reg(DW_IC_TAR) & DW_IC_TAR_10BITADDR_MASTER) != 0 ||
                   (Reg(DW_IC_CON) & DW_IC_CON_10BITADDR_MASTER) != 0;

    if (!fActive) {
        /* Only 7 bit targets exist, so a 10 bit address finds nobody. */
        fTarget = ten_bit ? nullptr : fChildBus->DeviceAtAddress(
            get_bits(Reg(DW_IC_TAR), 0, 7));
        fActive = true;
        fRawIntr |= DW_IC_INTR_ACTIVITY;
    } else if ((Reg(DW_IC_CON) & DW_IC_CON_RESTART_EN) == 0) {
        /* Without repeated starts the direction changes over a STOP. */
        if (fTarget != nullptr) {
            fTarget->Stop();
        }
        fRawIntr |= DW_IC_INTR_STOP_DET;
    }
    fRawIntr |= DW_IC_INTR_START_DET;
    fReading = read;

    if (fTarget == nullptr || !fTarget->Start(read)) {
        Abort(ten_bit ? DW_IC_ABRT_10ADDR1_NOACK : DW_IC_ABRT_7B_ADDR_NOACK);
        return false;
    }
    return true;
}


void DWI2CDevice::EndTransfer()
{
    fTarget->Stop();
    fTarget = nullptr;
    fActive = false;
    fRawIntr |= DW_IC_INTR_STOP_DET;
}


void DWI2CDevice::Execute(uint16_t cmd)
{
    bool read = (cmd & DW_IC_DATA_CMD_READ) != 0;

    if (!fActive || read != fReading ||
        (cmd & DW_IC_DATA_CMD_RESTART) != 0) {
        if (!BeginPhase(read)) {
            return;
        }
    }

    if (read) {
        int slot = (fRxHead + fRxCount) % DW_I2C_FIFO_DEPTH;
        fRxFifo[slot] = fTarget->Read();
        fRxCount++;
    } else if (!fTarget->Write(get_bits(cmd, 0, 8))) {
        Abort(DW_IC_ABRT_TXDATA_NOACK);
        return;
    }

    if ((cmd & DW_IC_DATA_CMD_STOP) != 0) {
        EndTransfer();
    }
}


void DWI2CDevice::Pump()
{
    while (Enabled() && fTxCount > 0) {
        uint16_t cmd = fTxFifo[fTxHead];
        /* A read waits for room to put its byte in. */
        if ((cmd & DW_IC_DATA_CMD_READ) != 0 && fRxCount == DW_I2C_FIFO_DEPTH) {
            break;
        }
        fTxHead = (fTxHead + 1) % DW_I2C_FIFO_DEPTH;
        fTxCount--;
        Execute(cmd);
    }
    UpdateIrq();
}


void DWI2CDevice::QueueCommand(uint16_t cmd)
{
    if (!Enabled() || fTxFlushed) {
        return;
    }
    if (fTxCount == DW_I2C_FIFO_DEPTH) {
        fRawIntr |= DW_IC_INTR_TX_OVER;
        UpdateIrq();
        return;
    }
    fTxFifo[(fTxHead + fTxCount) % DW_I2C_FIFO_DEPTH] = cmd;
    fTxCount++;
    Pump();
}


uint8_t DWI2CDevice::ReadData()
{
    if (fRxCount == 0) {
        fRawIntr |= DW_IC_INTR_RX_UNDER;
        UpdateIrq();
        return 0;
    }
    uint8_t byte = fRxFifo[fRxHead];
    fRxHead = (fRxHead + 1) % DW_I2C_FIFO_DEPTH;
    fRxCount--;
    /* Room for a byte lets a waiting read go on. */
    Pump();
    return byte;
}


void DWI2CDevice::SetEnable(uint32_t val)
{
    bool was_enabled = Enabled();

    Reg(DW_IC_ENABLE) = val & DW_IC_ENABLE_ENABLE;

    if ((val & DW_IC_ENABLE_ABORT) != 0 && was_enabled) {
        /* Over at once, so the bit reads back clear. */
        if (fActive) {
            Abort(DW_IC_ABRT_USER_ABRT);
        } else {
            fTxCount = 0;
        }
    }
    if (!Enabled()) {
        /* Disabling ends any transfer and empties both FIFOs. */
        if (fActive) {
            EndTransfer();
        }
        fTxCount = 0;
        fRxCount = 0;
    }
    Pump();
}


//#pragma mark - register file

void DWI2CDevice::ResetRegs()
{
    memset(fRegs, 0, sizeof(fRegs));
    Reg(DW_IC_CON) = DW_IC_CON_MASTER | DW_IC_CON_SPEED_FAST |
                     DW_IC_CON_RESTART_EN | DW_IC_CON_SLAVE_DISABLE;
    Reg(DW_IC_TAR) = 0x055;
    Reg(DW_IC_SAR) = 0x055;
    Reg(DW_IC_SS_SCL_HCNT) = 0x190;
    Reg(DW_IC_SS_SCL_LCNT) = 0x1d6;
    Reg(DW_IC_FS_SCL_HCNT) = 0x3c;
    Reg(DW_IC_FS_SCL_LCNT) = 0x82;
    Reg(DW_IC_HS_SCL_HCNT) = 0x6;
    Reg(DW_IC_HS_SCL_LCNT) = 0x10;
    Reg(DW_IC_INTR_MASK) = 0x8ff;
    Reg(DW_IC_SDA_HOLD) = 1;
}


uint32_t DWI2CDevice::ReadReg(uint32_t offset)
{
    switch (offset) {
    case DW_IC_DATA_CMD:
        return ReadData();
    case DW_IC_INTR_STAT:
        return RawIntr() & Reg(DW_IC_INTR_MASK);
    case DW_IC_RAW_INTR_STAT:
        return RawIntr();
    case DW_IC_CLR_INTR:
        /* Every interrupt software clears, and the abort with them. */
        ClearAbort();
        fRawIntr &= DW_IC_INTR_ACTIVITY;
        break;
    case DW_IC_CLR_RX_UNDER:
        fRawIntr &= ~DW_IC_INTR_RX_UNDER;
        break;
    case DW_IC_CLR_RX_OVER:
        fRawIntr &= ~DW_IC_INTR_RX_OVER;
        break;
    case DW_IC_CLR_TX_OVER:
        fRawIntr &= ~DW_IC_INTR_TX_OVER;
        break;
    case DW_IC_CLR_TX_ABRT:
        ClearAbort();
        break;
    case DW_IC_CLR_ACTIVITY:
        /* Activity stays raised while there is any. */
        if (!fActive) {
            fRawIntr &= ~DW_IC_INTR_ACTIVITY;
        }
        break;
    case DW_IC_CLR_STOP_DET:
        fRawIntr &= ~DW_IC_INTR_STOP_DET;
        break;
    case DW_IC_CLR_START_DET:
        fRawIntr &= ~DW_IC_INTR_START_DET;
        break;
    case DW_IC_CLR_RD_REQ:
    case DW_IC_CLR_RX_DONE:
    case DW_IC_CLR_GEN_CALL:
    case DW_IC_CLR_RESTART_DET:
        /* Target mode events, which never happen here. */
        return 0;
    case DW_IC_STATUS:
        return Status();
    case DW_IC_TXFLR:
        return fTxCount;
    case DW_IC_RXFLR:
        return fRxCount;
    case DW_IC_TX_ABRT_SOURCE:
        return fAbortSource;
    case DW_IC_ENABLE_STATUS:
        return Enabled() ? 1 : 0;
    case DW_IC_COMP_PARAM_1:
        return DW_IC_COMP_PARAM_1_VALUE;
    case DW_IC_COMP_VERSION:
        return DW_IC_COMP_VERSION_VALUE;
    case DW_IC_COMP_TYPE:
        return DW_IC_COMP_TYPE_VALUE;
    default:
        return Reg(offset);
    }

    /* A clear register reads as zero, having cleared. */
    UpdateIrq();
    return 0;
}


void DWI2CDevice::WriteReg(uint32_t offset, uint32_t val)
{
    switch (offset) {
    case DW_IC_DATA_CMD:
        QueueCommand(get_bits(val, 0, 11));
        break;
    case DW_IC_ENABLE:
        SetEnable(val);
        break;
    case DW_IC_INTR_MASK:
        Reg(offset) = get_bits(val, 0, 15);
        UpdateIrq();
        break;
    case DW_IC_RX_TL:
    case DW_IC_TX_TL:
        /* A threshold beyond the FIFO is taken as its last entry. */
        Reg(offset) = std::min<uint32_t>(get_bits(val, 0, 8),
                                         DW_I2C_FIFO_DEPTH - 1);
        UpdateIrq();
        break;
    case DW_IC_INTR_STAT:
    case DW_IC_RAW_INTR_STAT:
    case DW_IC_STATUS:
    case DW_IC_TXFLR:
    case DW_IC_RXFLR:
    case DW_IC_TX_ABRT_SOURCE:
    case DW_IC_ENABLE_STATUS:
    case DW_IC_COMP_PARAM_1:
    case DW_IC_COMP_VERSION:
    case DW_IC_COMP_TYPE:
        break;
    default:
        if (offset >= DW_IC_CLR_INTR && offset <= DW_IC_CLR_GEN_CALL) {
            break;
        }
        Reg(offset) = val;
        break;
    }
}


uint32_t DWI2CDevice::DeviceRead(uint32_t offset, int size_log2)
{
    (void)size_log2;
    if (offset >= DW_IC_REG_FILE_SIZE) {
        return 0;
    }
    return ReadReg(offset & ~3u);
}


void DWI2CDevice::DeviceWrite(uint32_t offset, uint32_t val, int size_log2)
{
    (void)size_log2;
    if (offset >= DW_IC_REG_FILE_SIZE) {
        return;
    }
    WriteReg(offset & ~3u, val);
}


//#pragma mark - lifecycle

bool DWI2CDevice::Prepare()
{
    if (ParentBus()->AsPCIBus() != nullptr) {
        vm_error("%s: must be attached to a system bus\n", Name());
        return false;
    }
    fMmioRes = AddResource(RES_MMIO, DW_I2C_REG_SIZE, DW_I2C_REG_SIZE);
    fIrqRes = AddResource(RES_IRQ, 1);
    if (fMmioRes == nullptr || fIrqRes == nullptr) {
        return false;
    }
    fChildBus = std::make_unique<I2CBus>(this);
    return true;
}


bool DWI2CDevice::Realize()
{
    SystemBus *sys = static_cast<SystemBus *>(ParentBus());

    fIrq = sys->IrqSignalFor(fIrqRes->base);
    if (fIrq == nullptr) {
        vm_error("%s: bad interrupt line %d\n", Name(), (int)fIrqRes->base);
        return false;
    }
    ResetRegs();
    sys->MemMap()->RegisterDevice(fMmioRes->base, DW_I2C_REG_SIZE, this,
                                  DEVIO_SIZE32);
    return true;
}


void DWI2CDevice::BuildFDT(FDTContext &ctx)
{
    FDTBuilder *fdt = ctx.fdt;

    /* The reference and the bus interface clock, from one fixed clock of
       their own. */
    uint32_t clock_phandle = fdt->AllocPhandle();
    fdt->BeginNodeNum("i2c-clock", fMmioRes->base);
    fdt->PropStr("compatible", "fixed-clock");
    fdt->PropU32("#clock-cells", 0);
    fdt->PropU32("clock-frequency", DW_I2C_CLOCK_HZ);
    fdt->PropU32("phandle", clock_phandle);
    fdt->EndNode();

    fdt->BeginNodeNum("i2c", fMmioRes->base);
    fdt->PropStr("compatible", fCompatible);
    fdt->PropU64Range("reg", fMmioRes->base, fMmioRes->size);
    fdt_prop_irq(ctx, fIrqRes->base);
    fdt->AddCellU32(clock_phandle);
    fdt->AddCellU32(clock_phandle);
    fdt->PropCells("clocks");
    fdt->PropStrList("clock-names", "ref", "pclk", nullptr);
    fdt->PropU32("#address-cells", 1);
    fdt->PropU32("#size-cells", 0);
    fChildBus->BuildFDTAll(ctx);
    fdt->EndNode();
}


//#pragma mark - class

class DWI2CClass final: public DeviceClass {
public:
    DWI2CClass(): DeviceClass("dw-i2c") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        const char *compatible;

        (void)ctx;
        if (!cfg.GetStrOpt("compatible", &compatible)) {
            return nullptr;
        }
        if (compatible == nullptr) {
            compatible = DW_I2C_DEFAULT_COMPATIBLE;
        }
        return new DWI2CDevice(cfg.IdOr("dw-i2c"), compatible);
    }
};

static const DWI2CClass sDWI2CClass;
