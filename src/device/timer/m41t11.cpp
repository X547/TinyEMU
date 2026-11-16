/*
 * ST M41T11 serial real-time clock
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
#include "acpi.h"
#include "bits.h"
#include "calendar.h"
#include "device_class.h"
#include "devices.h"
#include "fdt.h"
#include "host_time.h"
#include "i2c.h"

/* 64 bytes behind one address pointer: the clock in BCD, a control
   register, and 56 bytes of battery backed RAM. A read copies the clock to
   the registers when it starts, so the bytes of one read agree; what a write
   puts in the clock registers becomes the time when the write ends. */

#define M41T11_REG_SECONDS 0x00
#define M41T11_REG_MINUTES 0x01
#define M41T11_REG_HOURS   0x02
#define M41T11_REG_DAY     0x03
#define M41T11_REG_DATE    0x04
#define M41T11_REG_MONTH   0x05
#define M41T11_REG_YEAR    0x06
#define M41T11_REG_CONTROL 0x07
#define M41T11_CLOCK_REGS  7
#define M41T11_SIZE        0x40

#define M41T11_SECONDS_ST 0x80  /* the oscillator is stopped */
#define M41T11_HOURS_CEB  0x80  /* the century bit toggles */
#define M41T11_HOURS_CB   0x40  /* the century bit */

/* The years the two digits and the century bit count, as Linux reads
   them: 2000-2099 with the bit clear. */
#define M41T11_BASE_YEAR 2000

/* The ACPI _DSD properties of a device whose _HID is PRP0001. */
#define ACPI_DSD_PROPERTIES_UUID "daffd814-6eba-4d8c-8a91-bc9bbf4aa301"

#define US_PER_SEC 1000000LL


static uint8_t to_bcd(int v)
{
    return (uint8_t)set_bits(v % 10, 4, 4, v / 10);
}

static int from_bcd(uint8_t v)
{
    return get_bits(v, 4, 4) * 10 + get_bits(v, 0, 4);
}


class M41T11 final: public I2CDevice {
private:
    uint8_t fRegs[M41T11_SIZE] {};
    uint8_t fPointer = 0;

    /* the time shown less the host's, in UTC */
    int64_t fOffsetUs = 0;
    /* While the oscillator is stopped, the time it stopped at. */
    bool fStopped = false;
    int64_t fStoppedUs = 0;
    /* what the day register is ahead of the date's weekday */
    int fDayAdjust = 0;
    /* The century bit as written; it follows the year only with CEB. */
    bool fCenturyEnable = false;
    bool fCenturyBit = false;

    bool fWriting = false;
    bool fHavePointer = false;
    bool fClockWritten = false;

    int64_t TimeUs() const;
    void LatchClock();
    void CommitClock();
    void FinishWrite();

public:
    /* 'address' of -1 lets the bus place the device. */
    M41T11(int address): I2CDevice("m41t11", address) {}

    bool Realize() override {return true;}
    void Reset() override;
    void BuildFDT(FDTContext &ctx) override;
    void BuildAML(AcpiContext &ctx) override;

    bool Start(bool read) override;
    bool Write(uint8_t byte) override;
    uint8_t Read() override;
    void Stop() override;
};


/* The clock keeps running as it was set, and the RAM keeps what it held,
   as on the battery; the interface starts over. */
void M41T11::Reset()
{
    fPointer = 0;
    fWriting = false;
    fHavePointer = false;
    fClockWritten = false;
}


int64_t M41T11::TimeUs() const
{
    return fStopped ? fStoppedUs : host_real_time_us() + fOffsetUs;
}


void M41T11::LatchClock()
{
    int64_t secs = floor_div(TimeUs(), US_PER_SEC);
    int64_t days = floor_div(secs, 86400);
    int sod = (int)(secs - days * 86400);
    int year, month, day;

    civil_from_days(days, &year, &month, &day);
    int century = floor_div(year - M41T11_BASE_YEAR, 100);
    bool cb = fCenturyEnable ? (century & 1) != 0 : fCenturyBit;
    int yy = year - M41T11_BASE_YEAR - century * 100;

    fRegs[M41T11_REG_SECONDS] = to_bcd(sod % 60) |
        (fStopped ? M41T11_SECONDS_ST : 0);
    fRegs[M41T11_REG_MINUTES] = to_bcd(sod / 60 % 60);
    fRegs[M41T11_REG_HOURS] = to_bcd(sod / 3600) |
        (fCenturyEnable ? M41T11_HOURS_CEB : 0) | (cb ? M41T11_HOURS_CB : 0);
    /* 1 is Sunday, as Linux counts; 1970-01-01 was a Thursday */
    fRegs[M41T11_REG_DAY] = (uint8_t)(((days + 4) % 7 + 7 + fDayAdjust) % 7 + 1);
    fRegs[M41T11_REG_DATE] = to_bcd(day);
    fRegs[M41T11_REG_MONTH] = to_bcd(month);
    fRegs[M41T11_REG_YEAR] = to_bcd(yy);
}


/* What the clock registers hold becomes the time shown, from the start of
   the second, as writing the seconds resets the divider. */
void M41T11::CommitClock()
{
    uint8_t hours = fRegs[M41T11_REG_HOURS];
    int month = from_bcd(fRegs[M41T11_REG_MONTH] & 0x1f);
    int day = from_bcd(fRegs[M41T11_REG_DATE] & 0x3f);

    fCenturyEnable = (hours & M41T11_HOURS_CEB) != 0;
    fCenturyBit = (hours & M41T11_HOURS_CB) != 0;
    int year = M41T11_BASE_YEAR + (fCenturyBit ? 100 : 0) +
        from_bcd(fRegs[M41T11_REG_YEAR]);
    month = month < 1 ? 1 : month > 12 ? 12 : month;
    day = day < 1 ? 1 : day;

    int64_t days = days_from_civil(year, month, day);
    int64_t secs = days * 86400 +
        from_bcd(hours & 0x3f) * 3600 +
        from_bcd(fRegs[M41T11_REG_MINUTES] & 0x7f) * 60 +
        from_bcd(fRegs[M41T11_REG_SECONDS] & 0x7f);
    int dow = (fRegs[M41T11_REG_DAY] & 0x07) - 1;
    fDayAdjust = (int)(((dow - (days + 4) % 7) % 7 + 7) % 7);

    int64_t us = secs * US_PER_SEC;
    fStopped = (fRegs[M41T11_REG_SECONDS] & M41T11_SECONDS_ST) != 0;
    if (fStopped) {
        fStoppedUs = us;
    } else {
        fOffsetUs = us - host_real_time_us();
    }
}


void M41T11::FinishWrite()
{
    if (fWriting && fClockWritten) {
        CommitClock();
    }
    fWriting = false;
    fHavePointer = false;
    fClockWritten = false;
}


//#pragma mark - the bus

bool M41T11::Start(bool read)
{
    FinishWrite();
    /* A write to some of the clock registers leaves the others at the
       time it started. */
    LatchClock();
    fWriting = !read;
    return true;
}


/* The first byte of a write is the address pointer, which then advances
   with each byte and wraps around after the last. */
bool M41T11::Write(uint8_t byte)
{
    if (!fHavePointer) {
        fPointer = byte % M41T11_SIZE;
        fHavePointer = true;
        return true;
    }
    fRegs[fPointer] = byte;
    if (fPointer < M41T11_CLOCK_REGS) {
        fClockWritten = true;
    }
    fPointer = (fPointer + 1) % M41T11_SIZE;
    return true;
}


uint8_t M41T11::Read()
{
    uint8_t byte = fRegs[fPointer];
    fPointer = (fPointer + 1) % M41T11_SIZE;
    return byte;
}


void M41T11::Stop()
{
    FinishWrite();
}


//#pragma mark - description

void M41T11::BuildFDT(FDTContext &ctx)
{
    FDTBuilder *fdt = ctx.fdt;

    fdt->BeginNodeNum("rtc", Address());
    fdt->PropStr("compatible", "st,m41t11");
    fdt->PropU32("reg", Address());
    fdt->EndNode();
}


/* The part has no ACPI ID of its own, so it is named by its device tree
   compatible string. */
void M41T11::BuildAML(AcpiContext &ctx)
{
    AmlBytes r;

    aml_res_i2c_serial_bus(r, Address(), I2C_SPEED_HZ, ctx.scope.c_str());

    AmlBytes dev = aml_name("_HID", aml_string("PRP0001"));
    aml_append(dev, aml_name("_UID", aml_int(ctx.NewUid("PRP0001"))));
    aml_append(dev, aml_name("_CRS", aml_resource_template(r)));
    aml_append(dev, aml_name("_DSD", aml_package({
        aml_uuid(ACPI_DSD_PROPERTIES_UUID),
        aml_package({
            aml_package({aml_string("compatible"), aml_string("st,m41t11")}),
        }),
    })));
    aml_append(*ctx.aml, aml_device(ctx.NewName("RTC").c_str(), dev));
}


//#pragma mark - class

class M41T11Class final: public DeviceClass {
public:
    M41T11Class(): DeviceClass("m41t11") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        int address;

        (void)ctx;
        /* The part answers at 0x68, but as for an I2C HID device, without
           an address the bus places the target. */
        if (!cfg.GetInt("reg", &address, -1))
            return nullptr;
        return new M41T11(address);
    }
};

static const M41T11Class sM41T11Class;
