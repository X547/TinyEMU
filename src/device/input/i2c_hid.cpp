/*
 * HID over I2C transport
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
#include <string.h>

#include "bits.h"
#include "cutils.h"
#include "device_class.h"
#include "fdt.h"
#include "hid.h"
#include "i2c.h"
#include "machine.h"

/* The registers the device answers at, as 16 bit little endian numbers the
   host writes before reading or writing one. The HID descriptor's is the one
   the device tree names; the descriptor gives the others. */
#define I2C_HID_REG_DESC        0x0001
#define I2C_HID_REG_REPORT_DESC 0x0002
#define I2C_HID_REG_INPUT       0x0003
#define I2C_HID_REG_OUTPUT      0x0004
#define I2C_HID_REG_COMMAND     0x0005
#define I2C_HID_REG_DATA        0x0006

#define I2C_HID_DESC_SIZE 30
#define I2C_HID_VERSION   0x0100

#define I2C_HID_OPCODE_RESET        0x01
#define I2C_HID_OPCODE_GET_REPORT   0x02
#define I2C_HID_OPCODE_SET_REPORT   0x03
#define I2C_HID_OPCODE_GET_IDLE     0x04
#define I2C_HID_OPCODE_SET_IDLE     0x05
#define I2C_HID_OPCODE_GET_PROTOCOL 0x06
#define I2C_HID_OPCODE_SET_PROTOCOL 0x07
#define I2C_HID_OPCODE_SET_POWER    0x08

/* A report ID field of 15 says the ID follows in a byte of its own. */
#define I2C_HID_REPORT_ID_ESCAPE 0x0f

/* The longest write taken: a SET_REPORT with its header and a full report. */
#define I2C_HID_WRITE_SIZE (16 + HID_MAX_REPORT_SIZE)

/* A response and its length prefix. */
#define I2C_HID_READ_SIZE (I2C_HID_DESC_SIZE + HID_MAX_REPORT_SIZE)

#define I2C_HID_VENDOR_ID  0x46f4 /* as the USB transport reports */
#define I2C_HID_PRODUCT_ID 0x0005


//#pragma mark - I2CHID

/* One HID function behind an I2C target. The protocol has one report
   descriptor per target, so each function needs a target of its own. The
   interrupt line is held while there is something to read. */
class I2CHID final: public I2CDevice, public HIDBusTarget,
                    public HIDTransport {
private:
    std::unique_ptr<HIDBus> fChildBus;
    HIDDevice *fDev = nullptr;

    Resource *fIrqRes = nullptr;
    IRQSignal *fIrq = nullptr;
    bool fIrqLevel = false;

    /* The reset the host asked for is acknowledged by an empty input
       report, which the device raises the line to have read. */
    bool fResetPending = false;

    /* What the host wrote since it addressed the device for writing. */
    uint8_t fWriteBuf[I2C_HID_WRITE_SIZE] {};
    int fWriteLen = 0;
    bool fWriting = false;

    /* What the device sends when read. */
    uint8_t fReadBuf[I2C_HID_READ_SIZE] {};
    const uint8_t *fReadData = nullptr;
    int fReadLen = 0;
    int fReadPos = 0;

    void UpdateIrq();

    int DecodeCommand(int *type, int *id) const;
    void ReadInput();
    void ReadRegister();
    void RespondToCommand();
    void FinishWrite();
    void RunCommand();
    void WriteOutput();

public:
    I2CHID(int address): I2CDevice("i2c-hid", address) {}

    bool Prepare() override;
    bool Realize() override;
    void BuildFDT(FDTContext &ctx) override;
    Bus *ChildBus() override {return fChildBus.get();}

    bool Start(bool read) override;
    bool Write(uint8_t byte) override;
    uint8_t Read() override;
    void Stop() override;

    /* HIDBusTarget */
    int FindFreeIndex() override {return fDev == nullptr ? 0 : -1;}
    bool AttachDevice(HIDDevice *dev, int index) override;

    /* HIDTransport */
    void HandleInputReport(HIDDevice *dev) override;
};


void I2CHID::UpdateIrq()
{
    bool level = fResetPending || (fDev != nullptr && fDev->HasInputReport());

    if (level != fIrqLevel) {
        fIrqLevel = level;
        fIrq->Set(level ? 1 : 0);
    }
}


void I2CHID::HandleInputReport(HIDDevice *dev)
{
    (void)dev;
    UpdateIrq();
}


bool I2CHID::AttachDevice(HIDDevice *dev, int index)
{
    if (fDev != nullptr || index != 0) {
        vm_error("i2c-hid: carries one HID function; declare an i2c-hid for "
                 "each\n");
        return false;
    }
    fDev = dev;
    dev->SetTransport(this, 0);
    return true;
}


//#pragma mark - reads

void I2CHID::ReadInput()
{
    if (fResetPending) {
        /* A length of zero, which is how the reset is acknowledged. */
        fResetPending = false;
        put_le16(fReadBuf, 0);
        fReadLen = 2;
    } else if (fDev != nullptr && fDev->HasInputReport()) {
        int len = fDev->TakeInputReport(fReadBuf + 2, HID_MAX_REPORT_SIZE);
        put_le16(fReadBuf, len + 2);
        fReadLen = len + 2;
    } else {
        put_le16(fReadBuf, 0);
        fReadLen = 2;
    }
    fReadData = fReadBuf;
    UpdateIrq();
}


/* The report type and ID of the command in the write buffer, and where the
   bytes after them start; -1 when there is no whole command. */
int I2CHID::DecodeCommand(int *type, int *id) const
{
    if (fWriteLen < 4) {
        return -1;
    }
    *type = get_bits(fWriteBuf[2], 4, 2);
    *id = get_bits(fWriteBuf[2], 0, 4);
    if (*id != I2C_HID_REPORT_ID_ESCAPE) {
        return 4;
    }
    if (fWriteLen < 5) {
        return -1;
    }
    *id = fWriteBuf[4];
    return 5;
}


void I2CHID::RespondToCommand()
{
    int type, id;

    if (DecodeCommand(&type, &id) < 0 || fDev == nullptr) {
        return;
    }
    switch (get_bits(fWriteBuf[3], 0, 4)) {
    case I2C_HID_OPCODE_GET_REPORT: {
        /* Reports here carry no ID, so there is one report of each type. */
        int len = id == 0 ? fDev->GetReport(type, fReadBuf + 2,
                                            HID_MAX_REPORT_SIZE) : -1;
        if (len < 0) {
            len = 0;
        }
        put_le16(fReadBuf, len + 2);
        fReadLen = len + 2;
        break;
    }
    case I2C_HID_OPCODE_GET_IDLE:
    case I2C_HID_OPCODE_GET_PROTOCOL:
        /* Neither idle rates nor the boot protocol exist over I2C, so both
           read as zero. */
        put_le16(fReadBuf, 4);
        put_le16(fReadBuf + 2, 0);
        fReadLen = 4;
        break;
    default:
        return;
    }
    fReadData = fReadBuf;
}


/* A read after the host wrote a register number. */
void I2CHID::ReadRegister()
{
    switch (get_le16(fWriteBuf)) {
    case I2C_HID_REG_DESC: {
        int desc_len = 0;
        if (fDev != nullptr) {
            fDev->ReportDescriptor(&desc_len);
        }
        uint8_t *d = fReadBuf;
        memset(d, 0, I2C_HID_DESC_SIZE);
        put_le16(d + 0, I2C_HID_DESC_SIZE);
        put_le16(d + 2, I2C_HID_VERSION);
        put_le16(d + 4, desc_len);
        put_le16(d + 6, I2C_HID_REG_REPORT_DESC);
        put_le16(d + 8, I2C_HID_REG_INPUT);
        put_le16(d + 10, 2 + (fDev != nullptr ? fDev->InputReportSize() : 0));
        put_le16(d + 12, I2C_HID_REG_OUTPUT);
        put_le16(d + 14, 2 + HID_MAX_REPORT_SIZE);
        put_le16(d + 16, I2C_HID_REG_COMMAND);
        put_le16(d + 18, I2C_HID_REG_DATA);
        put_le16(d + 20, I2C_HID_VENDOR_ID);
        put_le16(d + 22, I2C_HID_PRODUCT_ID);
        put_le16(d + 24, 0x0100);
        fReadData = d;
        fReadLen = I2C_HID_DESC_SIZE;
        break;
    }
    case I2C_HID_REG_REPORT_DESC:
        if (fDev != nullptr) {
            fReadData = fDev->ReportDescriptor(&fReadLen);
        }
        break;
    case I2C_HID_REG_INPUT:
        ReadInput();
        break;
    case I2C_HID_REG_COMMAND:
        RespondToCommand();
        break;
    default:
        break;
    }
}


//#pragma mark - writes

void I2CHID::RunCommand()
{
    int type, id;
    int pos = DecodeCommand(&type, &id);

    if (pos < 0) {
        return;
    }
    switch (get_bits(fWriteBuf[3], 0, 4)) {
    case I2C_HID_OPCODE_RESET:
        if (fDev != nullptr) {
            fDev->Reset();
        }
        fResetPending = true;
        UpdateIrq();
        break;
    case I2C_HID_OPCODE_SET_REPORT: {
        /* The data register, the length, the ID when there is one, and the
           report. */
        pos += 2;
        if (fWriteLen < pos + 2 || fDev == nullptr) {
            break;
        }
        int len = get_le16(fWriteBuf + pos) - 2;
        pos += 2;
        if (id != 0) {
            pos++;
            len--;
        }
        if (len >= 0 && pos + len <= fWriteLen) {
            fDev->SetReport(type, fWriteBuf + pos, len);
        }
        break;
    }
    default:
        /* Power states, idle rates and protocols change nothing here. */
        break;
    }
}


void I2CHID::WriteOutput()
{
    if (fWriteLen < 4 || fDev == nullptr) {
        return;
    }
    int len = get_le16(fWriteBuf + 2) - 2;
    if (len >= 0 && 4 + len <= fWriteLen) {
        fDev->SetReport(HID_REPORT_OUTPUT, fWriteBuf + 4, len);
    }
}


/* A write that is not followed by a read is complete in itself. */
void I2CHID::FinishWrite()
{
    if (!fWriting || fWriteLen < 2) {
        return;
    }
    switch (get_le16(fWriteBuf)) {
    case I2C_HID_REG_COMMAND:
        RunCommand();
        break;
    case I2C_HID_REG_OUTPUT:
        WriteOutput();
        break;
    default:
        break;
    }
}


//#pragma mark - the bus

bool I2CHID::Start(bool read)
{
    if (!read) {
        FinishWrite();
        fWriting = true;
        fWriteLen = 0;
        return true;
    }

    fReadData = nullptr;
    fReadLen = 0;
    fReadPos = 0;
    if (fWriting && fWriteLen >= 2) {
        ReadRegister();
    } else {
        /* A plain read collects the input report. */
        ReadInput();
    }
    fWriting = false;
    fWriteLen = 0;
    return true;
}


bool I2CHID::Write(uint8_t byte)
{
    if (fWriteLen < I2C_HID_WRITE_SIZE) {
        fWriteBuf[fWriteLen++] = byte;
    }
    return true;
}


uint8_t I2CHID::Read()
{
    /* Past the end of a response the bus reads as zeros. */
    if (fReadData == nullptr || fReadPos >= fReadLen) {
        return 0;
    }
    return fReadData[fReadPos++];
}


void I2CHID::Stop()
{
    FinishWrite();
    fWriting = false;
    fWriteLen = 0;
    fReadData = nullptr;
}


//#pragma mark - lifecycle

bool I2CHID::Prepare()
{
    if (!I2CDevice::Prepare()) {
        return false;
    }
    fIrqRes = AddResource(RES_IRQ, 1);
    if (fIrqRes == nullptr) {
        return false;
    }
    fChildBus = std::make_unique<HIDBus>(this, this);
    return true;
}


bool I2CHID::Realize()
{
    SystemBus *sys = dynamic_cast<SystemBus *>(ParentBus()->Root());

    fIrq = sys != nullptr ? sys->IrqSignalFor(fIrqRes->base) : nullptr;
    if (fIrq == nullptr) {
        vm_error("%s: bad interrupt line %d\n", Name(), (int)fIrqRes->base);
        return false;
    }
    return true;
}


void I2CHID::BuildFDT(FDTContext &ctx)
{
    FDTBuilder *fdt = ctx.fdt;

    fdt->BeginNodeNum("hid", Address());
    fdt->PropStr("compatible", "hid-over-i2c");
    fdt->PropU32("reg", Address());
    fdt->PropU32("hid-descr-addr", I2C_HID_REG_DESC);
    fdt_prop_irq(ctx, fIrqRes->base);
    fdt->EndNode();
}


//#pragma mark - class

class I2CHIDClass final: public DeviceClass {
public:
    I2CHIDClass(): DeviceClass("i2c-hid") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        int address;

        (void)ctx;
        /* Without an address the bus places the target, as it does an
           Ethernet PHY. */
        if (!cfg.GetInt("reg", &address, -1)) {
            return nullptr;
        }
        if (!cfg.HasChildren()) {
            vm_error("i2c-hid: needs a nested HID bus with a function on "
                     "it\n");
            return nullptr;
        }
        return new I2CHID(address);
    }
};

static const I2CHIDClass sI2CHIDClass;
