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

#include <memory>

#include "acpi.h"
#include "bits.h"
#include "cutils.h"
#include "device_class.h"
#include "devices.h"
#include "fdt.h"
#include "hid.h"
#include "host_timer.h"
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

/* GET_PROTOCOL and SET_PROTOCOL's values. */
#define I2C_HID_PROTOCOL_BOOT   0
#define I2C_HID_PROTOCOL_REPORT 1

/* A report ID field of 15 says the ID follows in a byte of its own. */
#define I2C_HID_REPORT_ID_ESCAPE 0x0f

/* The longest write taken: a SET_REPORT with its header and a full report. */
#define I2C_HID_WRITE_SIZE (16 + HID_MAX_REPORT_SIZE)

/* A response and its length prefix. */
#define I2C_HID_READ_SIZE (I2C_HID_DESC_SIZE + HID_MAX_REPORT_SIZE)

/* The _DSM function that returns the HID descriptor's register. */
#define I2C_HID_DSM_UUID "3cdff6f7-4267-4555-ad05-b30a3d8938de"
#define I2C_HID_DSM_DESC 1

#define I2C_HID_VENDOR_ID  0x46f4 /* as the USB transport reports */
#define I2C_HID_PRODUCT_ID 0x0005


//#pragma mark - I2CHID

/* One HID device behind an I2C target, which the protocol gives one report
   descriptor. The interrupt line is held while there is something to
   read. */
class I2CHID final: public I2CDevice, public HIDTransport,
                    public TimerTarget {
private:
    std::unique_ptr<HIDBus> fChildBus;
    HIDDevice fDev;
    /* Wakes when a report is due again on its idle rate. */
    std::unique_ptr<HostTimer> fTimer;

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
    void ArmIdleTimer();

    int DecodeCommand(int *type, int *id) const;
    int CommandValue(int pos) const;
    void ReadInput();
    void ReadRegister();
    void RespondToCommand();
    void FinishWrite();
    void RunCommand();
    void WriteOutput();

public:
    I2CHID(int address, std::unique_ptr<HostTimer> timer);

    bool Prepare() override;
    bool Realize() override;
    void BuildFDT(FDTContext &ctx) override;
    void BuildAML(AcpiContext &ctx) override;
    Bus *ChildBus() override {return fChildBus.get();}

    bool Start(bool read) override;
    bool Write(uint8_t byte) override;
    uint8_t Read() override;
    void Stop() override;

    void TimerExpired() override;

    /* HIDTransport */
    void HandleInputReport(HIDDevice *dev) override;
};


I2CHID::I2CHID(int address, std::unique_ptr<HostTimer> timer):
    I2CDevice("i2c-hid", address), fDev("i2c-hid", this),
    fTimer(std::move(timer))
{
    fTimer->target = this;
}


void I2CHID::UpdateIrq()
{
    bool level = fResetPending || fDev.HasInputReport();

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


/* An unchanged report is due again only once the host read the last one,
   as the input register holds one report at a time. */
void I2CHID::ArmIdleTimer()
{
    uint64_t due = fDev.HasInputReport() ? UINT64_MAX : fDev.NextIdleDue();
    if (due == UINT64_MAX) {
        fTimer->Disarm();
    } else {
        fTimer->Arm(due);
    }
}


void I2CHID::TimerExpired()
{
    if (!fDev.HasInputReport()) {
        fDev.QueueIdleReport();
    }
    ArmIdleTimer();
}


//#pragma mark - reads

void I2CHID::ReadInput()
{
    if (fResetPending) {
        /* A length of zero, which is how the reset is acknowledged. */
        fResetPending = false;
        put_le16(fReadBuf, 0);
        fReadLen = 2;
    } else if (fDev.HasInputReport()) {
        int len = fDev.TakeInputReport(fReadBuf + 2, HID_MAX_REPORT_SIZE);
        put_le16(fReadBuf, len + 2);
        fReadLen = len + 2;
    } else {
        put_le16(fReadBuf, 0);
        fReadLen = 2;
    }
    fReadData = fReadBuf;
    UpdateIrq();
    ArmIdleTimer();
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


/* The two byte value SET_IDLE and SET_PROTOCOL leave in the data register
   after the command at 'pos': the register, a length of 4, and the value;
   -1 when the write is short. */
int I2CHID::CommandValue(int pos) const
{
    if (fWriteLen < pos + 6) {
        return -1;
    }
    return get_le16(fWriteBuf + pos + 4);
}


void I2CHID::RespondToCommand()
{
    int type, id;

    if (DecodeCommand(&type, &id) < 0) {
        return;
    }
    switch (get_bits(fWriteBuf[3], 0, 4)) {
    case I2C_HID_OPCODE_GET_REPORT: {
        int len = fDev.GetReport(type, id, fReadBuf + 2, HID_MAX_REPORT_SIZE);
        if (len < 0) {
            len = 0;
        }
        put_le16(fReadBuf, len + 2);
        fReadLen = len + 2;
        break;
    }
    case I2C_HID_OPCODE_GET_IDLE: {
        /* In milliseconds, where USB counts in fours of them (7.2.4). */
        int idle = fDev.Idle(id);
        if (idle < 0) {
            return;
        }
        put_le16(fReadBuf, 4);
        put_le16(fReadBuf + 2, idle);
        fReadLen = 4;
        break;
    }
    case I2C_HID_OPCODE_GET_PROTOCOL:
        put_le16(fReadBuf, 4);
        put_le16(fReadBuf + 2, fDev.Protocol() ? I2C_HID_PROTOCOL_BOOT
                                               : I2C_HID_PROTOCOL_REPORT);
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
        int desc_len;
        fDev.ReportDescriptor(&desc_len);
        int output_len = fDev.MaxOutputReportSize();
        uint8_t *d = fReadBuf;
        memset(d, 0, I2C_HID_DESC_SIZE);
        put_le16(d + 0, I2C_HID_DESC_SIZE);
        put_le16(d + 2, I2C_HID_VERSION);
        put_le16(d + 4, desc_len);
        put_le16(d + 6, I2C_HID_REG_REPORT_DESC);
        put_le16(d + 8, I2C_HID_REG_INPUT);
        put_le16(d + 10, 2 + fDev.MaxInputReportSize());
        put_le16(d + 12, I2C_HID_REG_OUTPUT);
        put_le16(d + 14, output_len != 0 ? 2 + output_len : 0);
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
        fReadData = fDev.ReportDescriptor(&fReadLen);
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
        fDev.Reset();
        fResetPending = true;
        UpdateIrq();
        ArmIdleTimer();
        break;
    case I2C_HID_OPCODE_SET_REPORT: {
        /* The data register, the length, and the report, which starts with
           its ID when there is one. */
        pos += 2;
        if (fWriteLen < pos + 2) {
            break;
        }
        int len = get_le16(fWriteBuf + pos) - 2;
        pos += 2;
        if (len >= 0 && pos + len <= fWriteLen) {
            fDev.SetReport(type, id, fWriteBuf + pos, len);
        }
        break;
    }
    case I2C_HID_OPCODE_SET_IDLE: {
        int ms = CommandValue(pos);
        if (ms >= 0 && fDev.SetIdle(id, ms)) {
            ArmIdleTimer();
        }
        break;
    }
    case I2C_HID_OPCODE_SET_PROTOCOL: {
        /* The boot protocol only for a device with a boot format; the
           request is ignored otherwise, as one a device does not support
           is. */
        int value = CommandValue(pos);
        if (value == I2C_HID_PROTOCOL_REPORT ||
            (value == I2C_HID_PROTOCOL_BOOT &&
             fDev.BootProtocol() != HID_BOOT_NONE)) {
            fDev.SetProtocol(value == I2C_HID_PROTOCOL_BOOT);
            UpdateIrq();
            ArmIdleTimer();
        }
        break;
    }
    default:
        /* Power states change nothing here. */
        break;
    }
}


void I2CHID::WriteOutput()
{
    if (fWriteLen < 4) {
        return;
    }
    /* The report says its own ID, if it has one. */
    int len = get_le16(fWriteBuf + 2) - 2;
    if (len >= 0 && 4 + len <= fWriteLen) {
        fDev.SetReport(HID_REPORT_OUTPUT, -1, fWriteBuf + 4, len);
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
    fChildBus = std::make_unique<HIDBus>(this, &fDev);
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


/* The scope is the controller's. Function 0 of the _DSM reports which
   functions there are; the others return nothing. */
void I2CHID::BuildAML(AcpiContext &ctx)
{
    AmlBytes r;

    aml_res_i2c_serial_bus(r, Address(), I2C_SPEED_HZ, ctx.scope.c_str());
    ctx.AddInterrupt(r, fIrqRes->base);

    AmlBytes functions = aml_if(aml_lequal(aml_arg(2), aml_int(0)),
        aml_return(aml_buffer({bit_at(0) | bit_at(I2C_HID_DSM_DESC)})));
    aml_append(functions, aml_if(
        aml_lequal(aml_arg(2), aml_int(I2C_HID_DSM_DESC)),
        aml_return(aml_int(I2C_HID_REG_DESC))));
    AmlBytes dsm = aml_if(aml_lequal(aml_arg(0), aml_uuid(I2C_HID_DSM_UUID)),
                          functions);
    aml_append(dsm, aml_return(aml_buffer({0})));

    AmlBytes dev = aml_name("_HID", aml_eisaid("PNP0C50"));
    aml_append(dev, aml_name("_UID", aml_int(ctx.NewUid("PNP0C50"))));
    aml_append(dev, aml_name("_CRS", aml_resource_template(r)));
    aml_append(dev, aml_method("_DSM", 4, dsm));
    aml_append(*ctx.aml, aml_device(ctx.NewName("HID").c_str(), dev));
}


//#pragma mark - class

class I2CHIDClass final: public DeviceClass {
public:
    I2CHIDClass(): DeviceClass("i2c-hid") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        int address;

        /* Without an address the bus places the target, as it does an
           Ethernet PHY. */
        if (!cfg.GetInt("reg", &address, -1)) {
            return nullptr;
        }
        if (!cfg.HasChildren()) {
            vm_error("i2c-hid: needs a nested HID bus with at least one "
                     "collection on it\n");
            return nullptr;
        }
        return new I2CHID(address, ctx->platform->OpenTimer());
    }
};

static const I2CHIDClass sI2CHIDClass;
