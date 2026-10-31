/*
 * Goldfish real-time clock
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
#include <memory>

#include "devices.h"
#include "fdt.h"
#include "host_time.h"
#include "host_timer.h"
#include "platform.h"

/* The Android emulator's RTC: nanoseconds since the Unix epoch in two 32
   bit halves, and one alarm. Reading the low half latches the high one. */

#define RTC_TIME_LOW        0x00
#define RTC_TIME_HIGH       0x04
#define RTC_ALARM_LOW       0x08
#define RTC_ALARM_HIGH      0x0c
#define RTC_IRQ_ENABLED     0x10
#define RTC_CLEAR_ALARM     0x14
#define RTC_ALARM_STATUS    0x18
#define RTC_CLEAR_INTERRUPT 0x1c

#define RTC_SIZE 0x1000

#define NS_PER_US 1000


class GoldfishRtcDevice final: public Device, public TimerTarget {
private:
    DeviceContext *fCtx;
    Resource *fMmio = nullptr;
    Resource *fIrqRes = nullptr;
    IRQSignal *fIrq = nullptr;
    std::unique_ptr<HostTimer> fTimer;

    /* the time shown less the host's */
    int64_t fOffsetNs = 0;
    uint32_t fTimeHigh = 0;    /* latched by a read of the low half */
    uint32_t fSetHigh = 0;     /* taken by the next write of the low half */
    uint64_t fAlarm = 0;
    bool fAlarmArmed = false;
    bool fIrqEnabled = false;
    bool fIrqPending = false;

    uint64_t Now() const;
    void UpdateIrq();
    void CheckAlarm();

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t val, int size_log2);

    DeviceIOAdapter<GoldfishRtcDevice, &GoldfishRtcDevice::Read,
                    &GoldfishRtcDevice::Write> fIo {*this};

public:
    GoldfishRtcDevice(DeviceContext *ctx):
        Device("goldfish-rtc"), fCtx(ctx) {}

    bool Prepare() override
    {
        SystemBus *sys = dynamic_cast<SystemBus *>(ParentBus());
        if (sys == nullptr || sys->IsPortBased()) {
            vm_error("%s: must be attached to an FDT bus\n", Name());
            return false;
        }
        fMmio = AddResource(RES_MMIO, RTC_SIZE, RTC_SIZE);
        fIrqRes = AddResource(RES_IRQ, 1);
        return fMmio != nullptr && fIrqRes != nullptr;
    }

    bool Realize() override
    {
        SystemBus *sys = static_cast<SystemBus *>(ParentBus());

        fIrq = sys->IrqSignalFor(fIrqRes->base);
        if (fIrq == nullptr) {
            vm_error("%s: bad interrupt line %d\n", Name(),
                     (int)fIrqRes->base);
            return false;
        }
        fTimer = fCtx->platform->OpenTimer();
        fTimer->target = this;
        sys->MemMap()->RegisterDevice(fMmio->base, fMmio->size, &fIo,
                                      DEVIO_SIZE32);
        return true;
    }

    void BuildFDT(FDTContext &ctx) override
    {
        ctx.fdt->BeginNodeNum("rtc", fMmio->base);
        ctx.fdt->PropStr("compatible", "google,goldfish-rtc");
        ctx.fdt->PropU64Range("reg", fMmio->base, fMmio->size);
        fdt_prop_irq(ctx, fIrqRes->base);
        ctx.fdt->EndNode();
    }

    void TimerExpired() override
    {
        CheckAlarm();
    }
};


/* The host's wall clock moved by what the guest set; times before the
   epoch read as 0. */
uint64_t GoldfishRtcDevice::Now() const
{
    int64_t ns = host_real_time_us() * NS_PER_US + fOffsetNs;
    return ns > 0 ? (uint64_t)ns : 0;
}


void GoldfishRtcDevice::UpdateIrq()
{
    fIrq->Set(fIrqPending && fIrqEnabled ? 1 : 0);
}


/* A due alarm disarms itself and leaves the interrupt pending; one still
   ahead is timed on the host's clock, and looked at again when the timer
   fires, as either clock may have moved. */
void GoldfishRtcDevice::CheckAlarm()
{
    if (!fAlarmArmed) {
        fTimer->Disarm();
        return;
    }
    uint64_t now = Now();
    if (now >= fAlarm) {
        fAlarmArmed = false;
        fIrqPending = true;
        fTimer->Disarm();
        UpdateIrq();
        return;
    }
    uint64_t us = (fAlarm - now + NS_PER_US - 1) / NS_PER_US;
    uint64_t host_now = host_monotonic_us();
    fTimer->Arm(us <= UINT64_MAX - host_now ? host_now + us : UINT64_MAX);
}


uint32_t GoldfishRtcDevice::Read(uint32_t offset, int size_log2)
{
    (void)size_log2;
    switch (offset) {
    case RTC_TIME_LOW: {
        uint64_t now = Now();
        fTimeHigh = (uint32_t)(now >> 32);
        return (uint32_t)now;
    }
    case RTC_TIME_HIGH:
        return fTimeHigh;
    case RTC_ALARM_LOW:
        return (uint32_t)fAlarm;
    case RTC_ALARM_HIGH:
        return (uint32_t)(fAlarm >> 32);
    case RTC_IRQ_ENABLED:
        return fIrqEnabled;
    case RTC_ALARM_STATUS:
        return fAlarmArmed;
    default:
        return 0;
    }
}


void GoldfishRtcDevice::Write(uint32_t offset, uint32_t val, int size_log2)
{
    (void)size_log2;
    switch (offset) {
    case RTC_TIME_LOW: {
        uint64_t t = (uint64_t)fSetHigh << 32 | val;
        fOffsetNs = (int64_t)(t - (uint64_t)host_real_time_us() * NS_PER_US);
        /* the alarm is against the time shown */
        CheckAlarm();
        break;
    }
    case RTC_TIME_HIGH:
        fSetHigh = val;
        break;
    case RTC_ALARM_LOW:
        fAlarm = (fAlarm & ~(uint64_t)UINT32_MAX) | val;
        fAlarmArmed = true;
        CheckAlarm();
        break;
    case RTC_ALARM_HIGH:
        fAlarm = (uint64_t)val << 32 | (uint32_t)fAlarm;
        break;
    case RTC_IRQ_ENABLED:
        fIrqEnabled = (val & 1) != 0;
        UpdateIrq();
        break;
    case RTC_CLEAR_ALARM:
        fAlarmArmed = false;
        CheckAlarm();
        break;
    case RTC_CLEAR_INTERRUPT:
        fIrqPending = false;
        UpdateIrq();
        break;
    default:
        break;
    }
}


//#pragma mark - class

/* Its registers and interrupt are allocated on the FDT bus. */
class GoldfishRtcClass final: public DeviceClass {
public:
    GoldfishRtcClass(): DeviceClass("goldfish-rtc") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        (void)cfg;
        return new GoldfishRtcDevice(ctx);
    }
};

static const GoldfishRtcClass sGoldfishRtcClass;
