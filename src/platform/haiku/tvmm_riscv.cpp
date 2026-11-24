/*
 * The RISC-V harts on tvmm, TinyEMU's own Haiku kernel add-on
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>

#include <iterator>
#include <memory>
#include <vector>

#include <TvmmClient.h>

#include "bits.h"
#include "device_lock.h"
#include "host_riscv_hypervisor.h"
#include "hypervisor_memory_map.h"
#include "iomem.h"
#include "riscv_sbi_defs.h"
#include "tvmm_common.h"


class TvmmRISCVHypervisor;

/* One hart. The kernel answers most of the SBI, keeps the timer, and waits
   at WFI and while the hart is stopped, so a run lasts until an access for
   the machine or InterruptRun(). */
class TvmmRISCVVcpu final: public RISCVVcpu {
private:
    TvmmRISCVHypervisor &fOwner;
    int fIndex;
    TvmmRun *fRun = nullptr;
    std::unique_ptr<TvmmRiscVState> fState;
    /* the guest asked for a reset or a shutdown, or took a trap nothing
       handles, after which it runs no more */
    bool fEnded = false;

    void ExitMmio();
    void ExitSbi();
    void ExitSystemEvent();
    void ExitUnhandled();

public:
    TvmmRISCVVcpu(TvmmRISCVHypervisor &owner, int index);

    /* As created, stopped, for a reset of the machine. */
    void Reset();

    /* RISCVVcpu */
    void ThreadStarted() override {}
    void Run() override;
    bool Idle() override {return fEnded;}
    void InterruptRun() override;
    void SetIrqLine(uint32_t mask, bool level) override;
    /* there is no IMSIC */
    void ImsicSetPending(bool supervisor, uint32_t id) override
    {
        (void)supervisor;
        (void)id;
        abort();
    }
    uint64_t TimerDeadline() override {return UINT64_MAX;}
    void Start(uint64_t entry, uint64_t opaque) override;
};


/* The VM. The kernel has no interrupt controller for it, so the machine
   drives each hart's supervisor external interrupt. */
class TvmmRISCVHypervisor final: public RISCVCpuBackend,
                                 public HostHypervisorRam {
private:
    friend class TvmmRISCVVcpu;

    RISCVCpuTarget &fTarget;
    DeviceLock &fLock;
    int fHartCount;
    TvmmDevice fDevice;
    std::vector<std::unique_ptr<TvmmRISCVVcpu>> fVcpus;
    TvmmRamMap fRam {fDevice};
    std::unique_ptr<HypervisorPhysMemoryMap> fMemoryMap;
    RISCVHartIsa fIsa;
    uint64_t fTimebaseFrequency = 0;
    /* the guest's time counter less the host's */
    uint64_t fTimeDelta = 0;
    /* with TVMM_STATS in the environment: the statistics are printed at the
       end */
    bool fPrintStats = false;

    void ReadIsa();
    void PrintStats();

public:
    TvmmRISCVHypervisor(RISCVCpuTarget &target, DeviceLock &lock,
                        const RISCVCpuOptions &options):
        fTarget(target), fLock(lock), fHartCount(options.hart_count) {}
    ~TvmmRISCVHypervisor() override;

    bool Init();

    /* RISCVCpuBackend */
    PhysMemoryMap &MemoryMap() override {return *fMemoryMap;}
    RISCVVcpu &Vcpu(int index) override {return *fVcpus[index];}
    const RISCVHartIsa &Isa() override {return fIsa;}
    uint64_t Time() override;
    void SetTime(uint64_t time) override;
    uint64_t TimebaseFrequency() override {return fTimebaseFrequency;}
    bool RealTimeClock() override {return true;}
    void Reset() override
    {
        for (auto &vcpu : fVcpus)
            vcpu->Reset();
    }

    /* HostHypervisorRam */
    uint8_t *AllocRam(size_t size) override {return fRam.Alloc(size);}
    void FreeRam(uint8_t *ptr, size_t size) override
    {
        (void)size;
        fRam.Free(ptr);
    }
    void MapRam(int slot, uint64_t addr, uint64_t size, uint8_t *host_mem,
                bool read_only, bool log_dirty) override
    {
        fRam.Map(slot, addr, size, host_mem, read_only, log_dirty);
    }
    void GetDirtyLog(int slot, uint32_t *bitmap) override
    {
        fRam.GetDirtyLog(slot, bitmap);
    }
};


static uint64_t host_rdtime()
{
    uint64_t val;

    asm volatile("rdtime %0" : "=r"(val));
    return val;
}


TvmmRISCVHypervisor::~TvmmRISCVHypervisor()
{
    if (fPrintStats)
        PrintStats();
    fMemoryMap.reset();
    fVcpus.clear();
    /* closing the device ends the VM and gives the RAM back */
}


/* What the guest is shown is what the kernel can give it of the host's
   harts. */
void TvmmRISCVHypervisor::ReadIsa()
{
    auto isa = std::make_unique<TvmmHostIsa>();

    tvmm_check("reading the harts' extensions", fDevice.GetHostIsa(*isa));
    fIsa.misa = isa->misa;
    for (const char *ext = isa->extensions; *ext != '\0';
         ext += strlen(ext) + 1)
        fIsa.extensions.push_back(ext);
    fIsa.satp_mode = isa->satpMode;
    fIsa.cbom_block_size = isa->cbomBlockSize;
    fIsa.cboz_block_size = isa->cbozBlockSize;
    fTimebaseFrequency = isa->timebaseFrequency;
}


bool TvmmRISCVHypervisor::Init()
{
    TvmmInfo info;
    if (!tvmm_open(fDevice, TvmmArch::RiscVH, 0, info))
        return false;
    if (fHartCount > (int)info.maxVcpus) {
        fprintf(stderr, "tvmm runs no more than %u harts\n", info.maxVcpus);
        return false;
    }
    ReadIsa();

    for (int i = 0; i < fHartCount; i++)
        fVcpus.push_back(std::make_unique<TvmmRISCVVcpu>(*this, i));
    fMemoryMap = std::make_unique<HypervisorPhysMemoryMap>(*this);

    auto state = std::make_unique<TvmmRiscVState>();
    tvmm_check("reading the time counter",
               fDevice.GetState(0, TvmmRiscVStateTimer, *state));
    fTimeDelta = state->timeDelta;

    fPrintStats = getenv("TVMM_STATS") != nullptr;
    return true;
}


uint64_t TvmmRISCVHypervisor::Time()
{
    return host_rdtime() + fTimeDelta;
}


/* The guest's counter is the host's, offset as the kernel set it for the
   VM; a write to mtime is left undone. */
void TvmmRISCVHypervisor::SetTime(uint64_t time)
{
    static bool warned;

    (void)time;
    if (!warned) {
        warned = true;
        fprintf(stderr, "riscv tvmm: writes to mtime are not supported\n");
    }
}


static void print_stat(const char *name, const TvmmStat &stat,
                       double ns_per_tick)
{
    if (stat.count == 0)
        return;
    double ns = stat.cycles * ns_per_tick;
    fprintf(stderr, "  %-18s %12" PRIu64 " %10.3f s %9.2f us\n", name,
            (uint64_t)stat.count, ns / 1e9, ns / stat.count / 1e3);
}


/* Each hart's exits, timed on the time counter. */
void TvmmRISCVHypervisor::PrintStats()
{
    static const char *const arch_names[] = {
        "interrupt", "wfi", "sbi", "sbi client", "gpf ram", "mmio",
        "virtual insn", "other",
    };
    static const char *const exit_names[] = {
        "none", "canceled", "port", "mmio", "msr", "halted",
        "interrupt window", "tpr changed", "eoi", "extint accepted",
        "shutdown", "failed entry", "unhandled", "sbi", "system event",
    };
    static_assert(std::size(arch_names) == TvmmRiscVStatCount);
    static_assert(std::size(exit_names) == kTvmmExitCount);
    double ns_per_tick = 1e9 / fTimebaseFrequency;

    for (int i = 0; i < fHartCount; i++) {
        TvmmVcpuStats stats;
        if (fDevice.GetStats(i, stats) < B_OK)
            continue;
        fprintf(stderr, "tvmm hart %d:\n", i);
        print_stat("runs", stats.runs, ns_per_tick);
        print_stat("in the guest", stats.entries, ns_per_tick);
        print_stat("waits", stats.waits, ns_per_tick);
        for (uint32 e = 0; e < TvmmRiscVStatCount; e++)
            print_stat(arch_names[e], stats.archExits[e], ns_per_tick);
        for (uint32 e = 0; e < kTvmmExitCount; e++) {
            char name[32];
            snprintf(name, sizeof(name), "client %s", exit_names[e]);
            print_stat(name, stats.clientExits[e], ns_per_tick);
        }
    }
}


TvmmRISCVVcpu::TvmmRISCVVcpu(TvmmRISCVHypervisor &owner, int index):
    fOwner(owner), fIndex(index), fState(std::make_unique<TvmmRiscVState>())
{
    /* every hart waits stopped, the first for Start(), the others for an
       SBI start */
    tvmm_check("creating a vcpu", fOwner.fDevice.CreateVcpu(index, fRun));
}


void TvmmRISCVVcpu::Reset()
{
    tvmm_check("resetting a vcpu", fOwner.fDevice.ResetVcpu(fIndex));
    fEnded = false;
}


void TvmmRISCVVcpu::Start(uint64_t entry, uint64_t opaque)
{
    TvmmDevice &device = fOwner.fDevice;

    tvmm_check("reading the vcpu state",
               device.GetState(fIndex, TvmmRiscVStateCore, *fState));
    fState->pc = entry;
    fState->x[10] = fIndex;
    fState->x[11] = opaque;
    fState->mode = TvmmRiscVModeSupervisor;
    fState->mpState = TvmmRiscVHartStarted;
    tvmm_check("starting a vcpu",
               device.SetState(fIndex, TvmmRiscVStateCore | TvmmRiscVStateMp,
                               *fState));
}


/* With the lock held. The kernel decodes the access; a write to a ROM
   range goes to the device behind it, as a flash takes its commands. */
void TvmmRISCVVcpu::ExitMmio()
{
    PhysMemoryMap &map = *fOwner.fMemoryMap;
    TvmmExitMmio &mmio = fRun->exit.mmio;
    uint64_t paddr = mmio.gpa;
    int len = mmio.size;
    PhysMemoryRange *pr = map.FindRange(paddr);
    int size_log2;
    uint64_t val = 0;

    switch (len) {
    case 1: size_log2 = 0; break;
    case 2: size_log2 = 1; break;
    case 4: size_log2 = 2; break;
    case 8: size_log2 = 3; break;
    default:
        abort();
    }

    if (mmio.write) {
        memcpy(&val, mmio.data, len);
        if (pr == nullptr)
            return;
        if (pr->is_ram && !((pr->devram_flags & DEVRAM_FLAG_ROM) &&
                            pr->io != nullptr))
            return;
        pr->DeviceWrite(paddr - pr->addr, val, size_log2);
    } else {
        if (pr == nullptr) {
            val = 0;
        } else if (pr->is_ram) {
            uint64_t offset = paddr - pr->addr;
            if (offset + len <= pr->size)
                memcpy(&val, pr->phys_mem + offset, len);
        } else {
            pr->DeviceRead(paddr - pr->addr, size_log2, &val);
        }
        memcpy(mmio.data, &val, len);
    }
}


/* With the lock held: an SBI call the kernel passed on, which the kernel
   then returns in a0 and a1. A legacy call keeps a1. */
void TvmmRISCVVcpu::ExitSbi()
{
    uint64_t a[8];

    memcpy(a, fRun->exit.sbi.a, sizeof(a));
    if (!fOwner.fTarget.SbiCall(fIndex, 64, a)) {
        a[0] = (uint64_t)(int64_t)SBI_ERR_NOT_SUPPORTED;
        a[1] = 0;
    }
    fRun->exit.sbi.a[0] = a[0];
    fRun->exit.sbi.a[1] = a[1];
}


/* With the lock held. The kernel stopped the hart; the machine stops the
   others with its reset. */
void TvmmRISCVVcpu::ExitSystemEvent()
{
    const TvmmExitSystemEvent &event = fRun->exit.systemEvent;

    fEnded = true;
    fOwner.fTarget.SystemReset(event.type != SBI_SRST_SHUTDOWN,
                               event.reason == SBI_SRST_REASON_FAILURE);
}


/* With the lock held: a trap the guest cannot take, such as an access the
   kernel cannot emulate. */
void TvmmRISCVVcpu::ExitUnhandled()
{
    const TvmmExitUnhandled &u = fRun->exit.unhandled;

    fprintf(stderr, "tvmm: unhandled trap, scause 0x%" PRIx64 " at pc 0x%"
            PRIx64 ", stval 0x%" PRIx64 ", guest physical address 0x%" PRIx64,
            (uint64_t)u.code, (uint64_t)u.rip, (uint64_t)u.info1,
            (uint64_t)u.info2);
    if (u.insnLength != 0) {
        fprintf(stderr, ", instruction");
        for (int i = u.insnLength - 1; i >= 0; i--)
            fprintf(stderr, "%s%02x", i == u.insnLength - 1 ? " " : "",
                    u.insnBytes[i]);
    }
    fprintf(stderr, "\n");
    fEnded = true;
    fOwner.fTarget.HartFault(fIndex, u.code, u.rip, u.info1);
}


void TvmmRISCVVcpu::Run()
{
    DeviceLock &lock = fOwner.fLock;

    tvmm_check("running a vcpu", fOwner.fDevice.Run(fIndex));
    switch (fRun->exitReason) {
    case TvmmExit::None:
    case TvmmExit::Canceled:
        break;
    case TvmmExit::Mmio: {
        DeviceLocker locker(lock);
        ExitMmio();
        break;
    }
    case TvmmExit::Sbi: {
        DeviceLocker locker(lock);
        ExitSbi();
        break;
    }
    case TvmmExit::SystemEvent: {
        DeviceLocker locker(lock);
        ExitSystemEvent();
        break;
    }
    case TvmmExit::Unhandled: {
        DeviceLocker locker(lock);
        ExitUnhandled();
        break;
    }
    default:
        fprintf(stderr, "tvmm: unsupported exit reason %u\n",
                (unsigned)fRun->exitReason);
        exit(1);
    }
}


void TvmmRISCVVcpu::InterruptRun()
{
    tvmm_check("kicking a vcpu", fOwner.fDevice.Kick(fIndex));
}


/* The kernel takes the level from any thread and wakes the hart from WFI. */
void TvmmRISCVVcpu::SetIrqLine(uint32_t mask, bool level)
{
    if (!(mask & MIP_SEIP))
        return;
    tvmm_check("setting an interrupt",
               fOwner.fDevice.SetIrqLine(fIndex, level));
}


bool host_riscv_hypervisor_open(RISCVCpuTarget &target, DeviceLock &lock,
                                const RISCVCpuOptions &options,
                                std::unique_ptr<RISCVCpuBackend> *backend)
{
    const char *why = nullptr;
    if (options.machine_mode)
        why = "cannot run M-mode firmware; set sbi: \"native\"";
    else if (options.xlen != 64)
        why = "runs only riscv64 guests";
    else if (options.interrupt_arch == RISCV_INTR_AIA_IMSIC)
        why = "has no IMSIC to give the guest";
    if (why != nullptr) {
        fprintf(stderr, "tvmm %s, or accel: \"none\" for the interpreter\n",
                why);
        return false;
    }

    auto tvmm = std::make_unique<TvmmRISCVHypervisor>(target, lock, options);
    if (!tvmm->Init()) {
        /* no add-on, or one this cannot use: the interpreter runs the
           harts */
        return true;
    }
    *backend = std::move(tvmm);
    return true;
}
