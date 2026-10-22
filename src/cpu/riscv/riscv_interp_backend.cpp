/*
 * The RISC-V interpreter as the machine's harts
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
#include <vector>

#include "host_time.h"
#include "iomem.h"
#include "riscv_cpu.h"
#include "riscv_interp_sbi.h"
#include "riscv_vcpu.h"


RISCVCPU *riscv_cpu_create(PhysMemoryMap *mem_map, int max_xlen,
                           uint32_t hart_id)
{
    switch (max_xlen) {
        case 32:
            return riscv_cpu_create32(mem_map, hart_id);
        case 64:
            return riscv_cpu_create64(mem_map, hart_id);
#if CONFIG_RISCV_MAX_XLEN == 128
        case 128:
            return riscv_cpu_create128(mem_map, hart_id);
#endif
        default:
            return nullptr;
    }
}


#define RTC_FREQ 1000000
#define RTC_FREQ_DIV 16 /* arbitrary, relative to CPU freq to have a
                           10 MHz frequency */


/* One interpreted hart. */
class RISCVInterpVcpu final: public RISCVVcpu {
private:
    std::unique_ptr<RISCVCPU> fCpu;

public:
    RISCVInterpVcpu(RISCVCPU *cpu): fCpu(cpu) {}

    RISCVCPU *Cpu() {return fCpu.get();}

    /* RISCVVcpu */
    void ThreadStarted() override {}
    void Run() override {fCpu->Interp();}
    bool Idle() override;
    void InterruptRun() override {fCpu->RequestExit();}
    void SetIrqLine(uint32_t mask, bool level) override
        {fCpu->SetIrqLine(mask, level);}
    void ImsicSetPending(bool supervisor, uint32_t id) override
        {fCpu->ImsicSetPending(supervisor, id);}
    uint64_t TimerDeadline() override {return fCpu->STimerDeadline();}
    void Start(uint64_t entry, uint64_t opaque) override
        {fCpu->HsmStart(entry, opaque);}
};


/* The interpreter keeps the time counter and the Sstc comparators, and
   answers the SBI when no firmware runs in M-mode. */
class RISCVInterpBackend final:
    public RISCVCpuBackend,
    public RtcTimeSource,
    public RISCVSbiHarts,
    public TlbFlushTarget {
private:
    RISCVCpuTarget &fTarget;
    RISCVHartIsa fIsa;
    bool fRealTime;
    uint64_t fStartTime = 0;
    /* before the harts, which use it until they go */
    PhysMemoryMap fMemoryMap;
    std::vector<std::unique_ptr<RISCVInterpVcpu>> fVcpus;
    std::unique_ptr<RISCVSbi> fSbi;

public:
    RISCVInterpBackend(RISCVCpuTarget &target,
                       const RISCVCpuOptions &options);

    /* False if the XLEN is not one this build has. */
    bool Init(DeviceLock &lock, const RISCVCpuOptions &options);

    /* RISCVCpuBackend */
    PhysMemoryMap &MemoryMap() override {return fMemoryMap;}
    RISCVVcpu &Vcpu(int index) override {return *fVcpus[index];}
    const RISCVHartIsa &Isa() override {return fIsa;}
    uint64_t Time() override;
    uint64_t TimebaseFrequency() override {return RTC_FREQ;}
    bool RealTimeClock() override {return fRealTime;}

    /* RtcTimeSource */
    uint64_t RtcTime() override {return Time();}
    void STimerChanged(uint64_t deadline) override
        {fTarget.TimerChanged(deadline);}

    /* RISCVSbiHarts */
    int HartCount() override {return fVcpus.size();}
    RISCVCPU *Hart(int hart) override {return fVcpus[hart]->Cpu();}

    /* TlbFlushTarget, for the RAM dirty bits */
    void FlushTlbWriteRange(uint8_t *ram_addr, size_t ram_size) override;
};


bool RISCVInterpVcpu::Idle()
{
    fCpu->UpdateSTimer();
    return fCpu->PowerDown();
}


static uint64_t rtc_get_real_time()
{
    return host_monotonic_us() / (1000000 / RTC_FREQ);
}


RISCVInterpBackend::RISCVInterpBackend(RISCVCpuTarget &target,
                                       const RISCVCpuOptions &options):
    fTarget(target),
    fRealTime(options.rtc_real_time)
{
    fMemoryMap.SetTlbFlushTarget(this);
}


bool RISCVInterpBackend::Init(DeviceLock &lock,
                              const RISCVCpuOptions &options)
{
    for (int hart = 0; hart < options.hart_count; hart++) {
        RISCVCPU *cpu = riscv_cpu_create(&fMemoryMap, options.xlen, hart);
        if (cpu == nullptr)
            return false;
        fVcpus.push_back(std::make_unique<RISCVInterpVcpu>(cpu));
        cpu->SetDeviceLock(&lock);
        cpu->SetSmp(options.hart_count > 1);
        if (options.interrupt_arch != RISCV_INTR_BASE)
            cpu->SetInterruptArch(options.interrupt_arch);
    }
    fIsa.misa = fVcpus[0]->Cpu()->Misa();
    fIsa.extensions = {"zicsr", "zifencei", "zicntr"};
    /* the interpreter has the bit manipulation instructions up to XLEN 64 */
    if (options.xlen <= 64)
        fIsa.extensions.insert(fIsa.extensions.end(), {"zba", "zbb", "zbs"});
    if (options.interrupt_arch != RISCV_INTR_BASE)
        fIsa.extensions.insert(fIsa.extensions.end(), {"smaia", "ssaia"});
    fIsa.extensions.insert(fIsa.extensions.end(),
                           {"sstc", "svadu", "svinval"});
    fIsa.satp_mode = options.xlen == 32 ? RISCV_SATP_SV32 : RISCV_SATP_SV48;
    if (fRealTime)
        fStartTime = rtc_get_real_time();
    /* the 'time' CSR must read the same counter as the machine's timers */
    for (std::unique_ptr<RISCVInterpVcpu> &vcpu : fVcpus)
        vcpu->Cpu()->SetRtcTimeSource(this);
    if (!options.machine_mode) {
        fSbi = std::make_unique<RISCVSbi>(*this, fTarget, lock);
        for (std::unique_ptr<RISCVInterpVcpu> &vcpu : fVcpus)
            vcpu->Cpu()->SetSbiHandler(fSbi.get());
    }
    return true;
}


uint64_t RISCVInterpBackend::Time()
{
    uint64_t val;

    if (fRealTime) {
        val = rtc_get_real_time() - fStartTime;
    } else {
        /* the sum keeps advancing while some harts wait for interrupts */
        val = 0;
        for (std::unique_ptr<RISCVInterpVcpu> &vcpu : fVcpus)
            val += vcpu->Cpu()->Cycles();
        val /= RTC_FREQ_DIV;
    }
    return val;
}


void RISCVInterpBackend::FlushTlbWriteRange(uint8_t *ram_addr,
                                            size_t ram_size)
{
    for (std::unique_ptr<RISCVInterpVcpu> &vcpu : fVcpus)
        vcpu->Cpu()->FlushTlbWriteRangeRam(ram_addr, ram_size);
}


std::unique_ptr<RISCVCpuBackend> riscv_interp_open(
    RISCVCpuTarget &target, DeviceLock &lock, const RISCVCpuOptions &options)
{
    auto backend = std::make_unique<RISCVInterpBackend>(target, options);

    if (!backend->Init(lock, options))
        return nullptr;
    return backend;
}
