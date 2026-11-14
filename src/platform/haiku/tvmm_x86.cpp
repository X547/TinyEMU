/*
 * Host hypervisor running the x86 processor: tvmm
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

#include <algorithm>
#include <memory>
#include <vector>

#include <cpuid.h>

#include <TvmmClient.h>

#include "bits.h"
#include "device_lock.h"
#include "host_x86_hypervisor.h"
#include "hypervisor_memory_map.h"


#define CPUID_APIC bit_at(9)
#define CPUID_ACPI bit_at(22)
#define CPUID_X2APIC bit_at(21) /* 1 ECX */
#define CPUID_TOPOEXT bit_at(22) /* 0x80000001 ECX */
#define RFLAGS_IF bit_at(9)
#define MSR_IA32_APIC_BASE 0x1b

#define TVMM_PAGE_SIZE 4096


static void tvmm_fail(const char *what, status_t err)
{
    fprintf(stderr, "tvmm: %s failed: %s\n", what, strerror(err));
    exit(1);
}

static void tvmm_check(const char *what, status_t err)
{
    if (err < B_OK)
        tvmm_fail(what, err);
}

/* The host processor's physical address width, which the machine's
   processor reports unless told otherwise. */
static int host_phys_address_bits()
{
    unsigned int eax, ebx, ecx, edx;

    if (!__get_cpuid(0x80000008, &eax, &ebx, &ecx, &edx))
        return 36;
    return eax & 0xff;
}

static bool is_x2apic_msr(uint32_t msr)
{
    return msr >= X86_MSR_X2APIC_FIRST && msr <= X86_MSR_X2APIC_LAST;
}

static uint64_t round_to_page(uint64_t size)
{
    return (size + TVMM_PAGE_SIZE - 1) & ~(uint64_t)(TVMM_PAGE_SIZE - 1);
}


class TvmmX86Hypervisor;

/* One vcpu. The interrupt the machine's 8259s or local APIC has for it goes
   in through the run page before a run, and its exits come back there. A
   kick from another thread ends a run, or the next one if none is under
   way. */
class TvmmX86Vcpu final: public X86Vcpu {
private:
    TvmmX86Hypervisor &fOwner;
    int fIndex;
    TvmmRun *fRun = nullptr;
    /* the state after vcpu creation, which an INIT goes back to */
    std::unique_ptr<TvmmX86State> fResetState;
    /* where the registers are staged */
    std::unique_ptr<TvmmX86State> fState;

    /* the interrupt state as of the last exit */
    uint64_t fRflags = 0x2;
    bool fInterruptShadow = false;
    bool fEventPending = false;
    /* CR8 as the vcpu and the machine's local APIC last agreed on it */
    int fTaskPriority = 0;
    /* stopped at HLT until an interrupt is taken */
    bool fHalted = false;
    /* shut down, until the machine ends */
    bool fShutdown = false;

    void GetState(uint32 classes);
    void SetState(uint32 classes);
    void SyncTaskPriority();
    bool InjectInterrupt();
    void ExitIo();
    void ExitMmio();
    void ExitMsr();
    void DumpState();

public:
    TvmmX86Vcpu(TvmmX86Hypervisor &owner, int index);

    void GetRegs(X86Regs *regs) override;
    void SetRegs(const X86Regs &regs) override;
    void SetFlatProtectedMode(uint32_t gdt_base, uint16_t gdt_limit,
                              uint16_t code_sel, uint16_t data_sel) override;
    void ThreadStarted() override {}
    void Run() override;
    bool Idle(bool intr) override;
    void InterruptRun() override;
    /* the target is asked before each run */
    void SetInterruptRequest(bool level) override {(void)level;}
    void Init() override;
    void Startup(int vector) override;

    /* As created, for a reset of the machine, which also ends a shutdown. */
    void Reset();
};


/* With the interrupt controllers the hypervisor may have, the kernel has the
   local APICs: it waits at HLT and starts the processors beyond the first,
   the 8259s inject around the APIC while LINT0 takes ExtINT, and an EOI of a
   level triggered vector comes back for the machine's IOAPIC. Otherwise the
   machine keeps every interrupt controller. */
class TvmmX86Hypervisor final: public X86CpuBackend, public HostHypervisorRam {
private:
    friend class TvmmX86Vcpu;

    X86CpuTarget &fTarget;
    DeviceLock &fLock;
    /* the machine has local APICs */
    bool fLocalApic;
    /* and they are the kernel's */
    bool fApicEmulation;
    int fCpuCount;
    int fPhysAddressBits;
    std::vector<X86CpuidLeaf> fCpuid;
    /* the IA32_PAT processors start with, 0 for the hypervisor's */
    uint64_t fPat;
    TvmmDevice fDevice;
    std::vector<std::unique_ptr<TvmmX86Vcpu>> fVcpus;
    /* the RAM the kernel holds, which the platform knows by address */
    struct Ram {
        uint8_t *ptr;
        uint64_t size;
        uint32 id;
    };
    std::vector<Ram> fRam;
    /* what each slot maps, for the dirty log and for freeing the RAM */
    struct Slot {
        uint64_t size;
        uint32 ram_id;
    };
    std::vector<Slot> fSlots;
    std::unique_ptr<HypervisorPhysMemoryMap> fMemoryMap;

    void SetCpuid();

public:
    TvmmX86Hypervisor(X86CpuTarget &target, DeviceLock &lock,
                      const X86CpuOptions &options):
        fTarget(target), fLock(lock), fLocalApic(options.local_apic),
        fApicEmulation(options.local_apic &&
                       options.hypervisor_interrupt_controllers),
        fCpuCount(options.cpu_count),
        fPhysAddressBits(std::min(host_phys_address_bits(),
                                  options.max_phys_address_bits)),
        fCpuid(options.cpuid), fPat(options.pat) {}
    ~TvmmX86Hypervisor() override;

    bool Init();

    bool HasInterruptControllers() override {return false;}
    bool HasLocalApics() override {return fApicEmulation;}
    int PhysAddressBits() override {return fPhysAddressBits;}
    PhysMemoryMap &MemoryMap() override {return *fMemoryMap;}
    uint8_t *AllocRam(size_t size) override;
    void FreeRam(uint8_t *ptr, size_t size) override;
    void MapRam(int slot, uint64_t addr, uint64_t size, uint8_t *host_mem,
                bool read_only, bool log_dirty) override;
    void GetDirtyLog(int slot, uint32_t *bitmap) override;
    void SetIRQ(int irq, int level) override;
    void SendMsi(uint64_t addr, uint32_t data) override;
    X86Vcpu &Vcpu(int index) override {return *fVcpus[index];}
    void Reset() override
    {
        for (auto &vcpu : fVcpus)
            vcpu->Reset();
    }
};


TvmmX86Hypervisor::~TvmmX86Hypervisor()
{
    fMemoryMap.reset();
    fVcpus.clear();
    /* closing the device ends the VM and gives the RAM back */
}


bool TvmmX86Hypervisor::Init()
{
    status_t err = fDevice.Open();
    if (err < B_OK) {
        fprintf(stderr, "tvmm not available: %s\n", strerror(err));
        return false;
    }
    TvmmInfo info;
    tvmm_check("reading the capabilities", fDevice.GetInfo(info));
    if (info.abiVersion != kTvmmAbiVersion) {
        fprintf(stderr, "tvmm: the kernel add-on has interface version %u, "
                "this needs %u\n", info.abiVersion, kTvmmAbiVersion);
        return false;
    }
    if (info.hostStatus < B_OK) {
        fprintf(stderr, "tvmm: this host cannot run guests: %s\n",
                strerror(info.hostStatus));
        return false;
    }
    if (fCpuCount > (int)info.maxVcpus) {
        fprintf(stderr, "tvmm runs no more than %u processors\n",
                info.maxVcpus);
        return false;
    }
    err = fDevice.CreateVm(fApicEmulation ? TvmmVmLocalApic : 0);
    if (err < B_OK) {
        fprintf(stderr, "tvmm: cannot create a machine: %s\n", strerror(err));
        return false;
    }

    SetCpuid();
    for (int i = 0; i < fCpuCount; i++)
        fVcpus.push_back(std::make_unique<TvmmX86Vcpu>(*this, i));
    fMemoryMap = std::make_unique<HypervisorPhysMemoryMap>(*this);
    return true;
}


/* Each leaf is configured once, as the bits it deletes from the host's and
   those it sets: first the processor shown in place of the host's, then the
   changes below. The kernel fills in the APIC IDs. */
void TvmmX86Hypervisor::SetCpuid()
{
    std::vector<TvmmCpuidEntry> leaves;
    auto leaf_conf = [&](uint32_t leaf) -> TvmmCpuidEntry & {
        for (TvmmCpuidEntry &c: leaves) {
            if (c.leaf == leaf)
                return c;
        }
        TvmmCpuidEntry c;
        memset(&c, 0, sizeof(c));
        c.leaf = leaf;
        leaves.push_back(c);
        return leaves.back();
    };
    for (const X86CpuidLeaf &ent: fCpuid) {
        TvmmCpuidEntry &c = leaf_conf(ent.leaf);
        for (int i = 0; i < 4; i++) {
            c.del[i] = ~ent.host_mask[i];
            c.set[i] = ent.value[i];
        }
    }

    /* remove the APIC, unless the machine has one, & ACPI to be in sync with
       the emulator */
    static const uint32_t apic_leaves[] = {1, 0x80000001};
    for (uint32_t leaf: apic_leaves) {
        TvmmCpuidEntry &c = leaf_conf(leaf);
        uint32_t del = CPUID_ACPI;
        if (!fLocalApic)
            del |= CPUID_APIC;
        c.del[3] |= del;
        c.set[3] &= ~del;
        /* The host's topology leaves would describe the host processor the
           vcpu happens to run on, so the guest is not told they exist. */
        if (leaf == 0x80000001) {
            c.del[2] |= CPUID_TOPOEXT;
            c.set[2] &= ~CPUID_TOPOEXT;
        }
        /* the machine's local APIC has x2APIC mode */
        if (leaf == 1) {
            if (fLocalApic) {
                c.del[2] &= ~CPUID_X2APIC;
                c.set[2] |= CPUID_X2APIC;
            } else {
                c.del[2] |= CPUID_X2APIC;
                c.set[2] &= ~CPUID_X2APIC;
            }
        }
    }

    /* the physical address width, with no separate guest width */
    TvmmCpuidEntry &width = leaf_conf(0x80000008);
    width.del[0] |= 0x00ff00ff;
    width.set[0] = (width.set[0] & ~0x00ff00ffu) | fPhysAddressBits;

    if (leaves.size() > kTvmmMaxCpuidEntries) {
        fprintf(stderr, "tvmm: more CPUID leaves than the kernel takes\n");
        exit(1);
    }
    tvmm_check("configuring CPUID",
               fDevice.SetCpuid(leaves.data(), leaves.size()));
}


/* The RAM comes zeroed from the kernel, which keeps it mapped where the
   guest has it. */
uint8_t *TvmmX86Hypervisor::AllocRam(size_t size)
{
    uint64_t rounded = round_to_page(size);
    uint32 id;
    uint8 *ptr;
    if (fDevice.AllocRam(rounded, id, ptr) < B_OK)
        return nullptr;
    fRam.push_back(Ram {ptr, rounded, id});
    return ptr;
}


/* The kernel keeps RAM a slot maps, so the slots go first. */
void TvmmX86Hypervisor::FreeRam(uint8_t *ptr, size_t size)
{
    (void)size;
    for (auto it = fRam.begin(); it != fRam.end(); ++it) {
        if (it->ptr == ptr) {
            for (size_t slot = 0; slot < fSlots.size(); slot++) {
                if (fSlots[slot].size != 0 && fSlots[slot].ram_id == it->id)
                    MapRam(slot, 0, 0, nullptr, false, false);
            }
            tvmm_check("freeing RAM", fDevice.FreeRam(it->id, ptr));
            fRam.erase(it);
            return;
        }
    }
}


void TvmmX86Hypervisor::MapRam(int slot, uint64_t addr, uint64_t size,
                               uint8_t *host_mem, bool read_only,
                               bool log_dirty)
{
    (void)log_dirty;

    if (slot >= (int)fSlots.size())
        fSlots.resize(slot + 1, Slot {0, 0});
    if (size == 0) {
        tvmm_check("unmapping RAM", fDevice.MapRam(slot, 0, 0, 0, 0, 0));
        fSlots[slot] = Slot {0, 0};
        return;
    }
    for (const Ram &ram: fRam) {
        if (host_mem >= ram.ptr && host_mem < ram.ptr + ram.size) {
            uint64_t offset = host_mem - ram.ptr;
            tvmm_check("mapping RAM",
                       fDevice.MapRam(slot, addr, round_to_page(size), ram.id,
                                      offset,
                                      read_only ? TvmmMapReadOnly : 0));
            fSlots[slot] = Slot {size, ram.id};
            return;
        }
    }
    fprintf(stderr, "tvmm: RAM at %p is not the kernel's\n", host_mem);
    exit(1);
}


/* tvmm keeps no dirty log yet, so every page counts as written. */
void TvmmX86Hypervisor::GetDirtyLog(int slot, uint32_t *bitmap)
{
    uint64_t pages = fSlots[slot].size >> 12;
    memset(bitmap, 0xff, ((pages + 63) / 64) * 8);
}


void TvmmX86Hypervisor::SetIRQ(int irq, int level)
{
    /* the machine keeps the 8259s, so no line comes here */
    (void)irq;
    (void)level;
    abort();
}


/* Only with the kernel's local APICs. */
void TvmmX86Hypervisor::SendMsi(uint64_t addr, uint32_t data)
{
    tvmm_check("sending an MSI", fDevice.SendMsi(addr, data));
}


/* The kernel gives CPUID the APIC ID, which is the vcpu's index. */
TvmmX86Vcpu::TvmmX86Vcpu(TvmmX86Hypervisor &owner, int index):
    fOwner(owner), fIndex(index),
    fResetState(std::make_unique<TvmmX86State>()),
    fState(std::make_unique<TvmmX86State>())
{
    TvmmDevice &device = fOwner.fDevice;

    tvmm_check("creating a vcpu", device.CreateVcpu(index, fRun));
    /* A write to CR8 exits, for the machine's local APIC to know at once of
       a priority that lets an interrupt through. */
    fRun->tprExits = fOwner.fLocalApic && !fOwner.fApicEmulation;

    GetState(TvmmX86StateAll);
    if (fOwner.fPat != 0) {
        fState->msrs[TvmmX86MsrPat] = fOwner.fPat;
        SetState(TvmmX86StateMsrs);
    }
    *fResetState = *fState;
}


void TvmmX86Vcpu::GetState(uint32 classes)
{
    tvmm_check("reading the vcpu state",
               fOwner.fDevice.GetState(fIndex, classes, *fState));
}


void TvmmX86Vcpu::SetState(uint32 classes)
{
    tvmm_check("setting the vcpu state",
               fOwner.fDevice.SetState(fIndex, classes, *fState));
}


void TvmmX86Vcpu::GetRegs(X86Regs *regs)
{
    GetState(TvmmX86StateGprs);
    for (int i = 0; i < 8; i++)
        regs->gpr[i] = fState->gprs[i];
    regs->rip = fState->rip;
    regs->rflags = fState->rflags;
}


void TvmmX86Vcpu::SetRegs(const X86Regs &regs)
{
    GetState(TvmmX86StateGprs);
    for (int i = 0; i < 8; i++)
        fState->gprs[i] = regs.gpr[i];
    fState->rip = regs.rip;
    fState->rflags = regs.rflags;
    SetState(TvmmX86StateGprs);
    fRflags = regs.rflags;
}


void TvmmX86Vcpu::SetFlatProtectedMode(uint32_t gdt_base, uint16_t gdt_limit,
                                       uint16_t code_sel, uint16_t data_sel)
{
    static const int data_segs[] = {
        TvmmX86SegDs, TvmmX86SegEs, TvmmX86SegSs, TvmmX86SegFs, TvmmX86SegGs,
    };

    GetState(TvmmX86StateSegs | TvmmX86StateCrs);
    fState->cr0 |= 1; /* CR0_PE */
    fState->segs[TvmmX86SegGdt].base = gdt_base;
    fState->segs[TvmmX86SegGdt].limit = gdt_limit;

    TvmmX86Segment seg;
    memset(&seg, 0, sizeof(seg));
    seg.base = 0;
    seg.limit = 0xffffffff;
    /* type in bits 0-3, then S, DPL, P, and above them AVL, L, D/B, G */
    const uint16_t present_flat = (1 << 4) | (1 << 7) | (1 << 10) | (1 << 11);

    seg.attrib = 0xb | present_flat; /* code */
    seg.selector = code_sel;
    fState->segs[TvmmX86SegCs] = seg;

    seg.attrib = 0x3 | present_flat; /* data */
    seg.selector = data_sel;
    for (int i: data_segs)
        fState->segs[i] = seg;

    SetState(TvmmX86StateSegs | TvmmX86StateCrs);
}


/* With the lock held, before a run: CR8 takes a task priority the guest
   wrote to the local APIC's page. */
void TvmmX86Vcpu::SyncTaskPriority()
{
    int tpr = fOwner.fTarget.TaskPriority();

    if (tpr == fTaskPriority)
        return;
    fRun->cr8Value = tpr;
    fRun->setCr8 = 1;
    fTaskPriority = tpr;
}


/* With the lock held, before a run: hands the processor the interrupt the
   8259s or the machine's local APIC raise when it can take one, and
   otherwise asks to exit once it can. False while it stays halted. With the
   kernel's APIC the 8259s reach only the first processor, through LINT0,
   and the injection goes around the APIC. */
bool TvmmX86Vcpu::InjectInterrupt()
{
    X86CpuTarget &target = fOwner.fTarget;
    bool emulation = fOwner.fApicEmulation;
    bool intr = (!emulation || (fIndex == 0 && fRun->extIntAccepted)) &&
        target.InterruptRequested();

    if (intr && !fEventPending && !fInterruptShadow &&
        (fRflags & RFLAGS_IF)) {
        fRun->injectType = TvmmInjectInterrupt;
        fRun->injectVector = target.AcknowledgeInterrupt();
        fEventPending = true;
        fHalted = false;
        intr = target.InterruptRequested() &&
            (!emulation || fRun->extIntAccepted);
    }
    if (fHalted)
        return false;
    fRun->requestInterruptWindow = intr;
    return true;
}


void TvmmX86Vcpu::ExitIo()
{
    TvmmExitIo &io = fRun->exit.io;
    X86CpuTarget &target = fOwner.fTarget;
    int size_log2;

    switch (io.size) {
    case 1: size_log2 = 0; break;
    case 2: size_log2 = 1; break;
    case 4: size_log2 = 2; break;
    default: abort();
    }
    if (io.in)
        io.data = target.PortRead(io.port, size_log2);
    else
        target.PortWrite(io.port, io.data, size_log2);
}


/* An SSE access arrives as 16 bytes, which the target takes as two
   quadwords. */
void TvmmX86Vcpu::ExitMmio()
{
    TvmmExitMmio &mmio = fRun->exit.mmio;
    X86CpuTarget &target = fOwner.fTarget;
    int step = mmio.size > 8 ? 8 : mmio.size;

    for (int i = 0; i < mmio.size; i += step) {
        if (mmio.write)
            target.MmioWrite(mmio.gpa + i, mmio.data + i, step);
        else
            target.MmioRead(mmio.gpa + i, mmio.data + i, step);
    }
}


/* IA32_APIC_BASE and the x2APIC registers go to the machine's local APIC.
   Another MSR the kernel does not handle reads as 0 and ignores writes,
   rather than faulting a guest that probes it. */
void TvmmX86Vcpu::ExitMsr()
{
    TvmmExitMsr &msr = fRun->exit.msr;
    X86CpuTarget &target = fOwner.fTarget;
    bool apic = fOwner.fLocalApic;
    bool ok = true;

    if (!msr.write) {
        uint64_t val = 0;
        if (apic && msr.msr == MSR_IA32_APIC_BASE)
            val = target.ApicBase();
        else if (apic && is_x2apic_msr(msr.msr))
            ok = target.ApicMsrRead(msr.msr, &val);
        msr.value = val;
    } else {
        if (apic && msr.msr == MSR_IA32_APIC_BASE)
            ok = target.SetApicBase(msr.value);
        else if (apic && is_x2apic_msr(msr.msr))
            ok = target.ApicMsrWrite(msr.msr, msr.value);
    }
    msr.ok = ok;
}


/* The state the vcpu was created with, less the time stamp counter, which
   an INIT leaves running. */
void TvmmX86Vcpu::Init()
{
    GetState(TvmmX86StateMsrs);
    uint64_t tsc = fState->msrs[TvmmX86MsrTsc];
    *fState = *fResetState;
    fState->msrs[TvmmX86MsrTsc] = tsc;
    SetState(TvmmX86StateAll);

    fRflags = 0x2;
    fInterruptShadow = false;
    fEventPending = false;
    fTaskPriority = 0;
    fHalted = false;
}


/* Without the kernel's APIC an INIT; with it, the kernel resets the vcpu and
   its APIC, and holds the processors beyond the first until a STARTUP. */
void TvmmX86Vcpu::Reset()
{
    fShutdown = false;
    if (!fOwner.fApicEmulation) {
        Init();
        return;
    }
    tvmm_check("resetting a vcpu", fOwner.fDevice.ResetVcpu(fIndex));
    if (fOwner.fPat != 0) {
        GetState(TvmmX86StateMsrs);
        fState->msrs[TvmmX86MsrPat] = fOwner.fPat;
        SetState(TvmmX86StateMsrs);
    }
    fRflags = 0x2;
    fInterruptShadow = false;
    fEventPending = false;
    fTaskPriority = 0;
    fHalted = false;
}


void TvmmX86Vcpu::Startup(int vector)
{
    GetState(TvmmX86StateSegs | TvmmX86StateGprs);
    fState->segs[TvmmX86SegCs].selector = vector << 8;
    fState->segs[TvmmX86SegCs].base = (uint64_t)vector << 12;
    fState->rip = 0;
    SetState(TvmmX86StateSegs | TvmmX86StateGprs);
}


void TvmmX86Vcpu::InterruptRun()
{
    tvmm_check("kicking a vcpu", fOwner.fDevice.Kick(fIndex));
}


void TvmmX86Vcpu::Run()
{
    DeviceLock &lock = fOwner.fLock;
    TvmmRun *run = fRun;

    bool machine_apic = fOwner.fLocalApic && !fOwner.fApicEmulation;
    {
        DeviceLocker locker(lock);
        if (machine_apic)
            SyncTaskPriority();
        if (!InjectInterrupt())
            return;
    }

    status_t err = fOwner.fDevice.Run(fIndex);
    /* the requests are taken */
    run->injectType = TvmmInjectNone;
    run->setCr8 = 0;
    if (err < B_OK)
        tvmm_fail("running a vcpu", err);

    fRflags = run->rflags;
    fInterruptShadow = run->interruptShadow;
    fEventPending = run->eventPending;
    /* the local APIC learns of a write to CR8 before anything else sees
       it */
    if (machine_apic && (int)run->cr8 != fTaskPriority) {
        fTaskPriority = run->cr8;
        DeviceLocker locker(lock);
        fOwner.fTarget.SetTaskPriority(fTaskPriority);
    }

    switch (run->exitReason) {
    case TvmmExit::None:
    case TvmmExit::Canceled:
    case TvmmExit::InterruptWindow:
    case TvmmExit::TprChanged:
    case TvmmExit::ExtIntAccepted:
        break;
    case TvmmExit::Io: {
        DeviceLocker locker(lock);
        ExitIo();
        break;
    }
    case TvmmExit::Mmio: {
        DeviceLocker locker(lock);
        ExitMmio();
        break;
    }
    case TvmmExit::Msr: {
        DeviceLocker locker(lock);
        ExitMsr();
        break;
    }
    case TvmmExit::Halted:
        fHalted = true;
        break;
    case TvmmExit::Eoi: {
        DeviceLocker locker(lock);
        fOwner.fTarget.ApicEoi(run->exit.eoi.vector);
        break;
    }
    case TvmmExit::Shutdown: {
        DeviceLocker locker(lock);
        fprintf(stderr, "tvmm: the guest shut down (triple fault)\n");
        DumpState();
        fShutdown = true;
        fOwner.fTarget.ProcessorShutdown();
        break;
    }
    case TvmmExit::FailEntry:
        fprintf(stderr, "tvmm: the processor refused the guest state "
                "(code 0x%" PRIx64 ")\n", (uint64_t)run->exit.failEntry.code);
        exit(1);
    case TvmmExit::Unhandled: {
        const TvmmExitUnhandled &u = run->exit.unhandled;
        fprintf(stderr, "tvmm: unhandled exit 0x%" PRIx64 " at rip 0x%"
                PRIx64 ", info 0x%" PRIx64 " 0x%" PRIx64 ", instruction",
                (uint64_t)u.code, (uint64_t)u.rip, (uint64_t)u.info1,
                (uint64_t)u.info2);
        for (int i = 0; i < u.insnLength; i++)
            fprintf(stderr, " %02x", u.insnBytes[i]);
        fprintf(stderr, "\n");
        exit(1);
    }
    default:
        fprintf(stderr, "tvmm: unsupported exit reason %u\n",
                (unsigned)run->exitReason);
        exit(1);
    }
}


/* For a guest that failed: where it was, and in what mode. */
void TvmmX86Vcpu::DumpState()
{
    static const char *const seg_names[] = {
        "es", "cs", "ss", "ds", "fs", "gs", "gdt", "ldt", "idt", "tr",
    };
    static const char *const gpr_names[] = {
        "rax", "rcx", "rdx", "rbx", "rsp", "rbp", "rsi", "rdi",
        "r8", "r9", "r10", "r11", "r12", "r13", "r14", "r15",
    };

    GetState(TvmmX86StateGprs | TvmmX86StateSegs | TvmmX86StateCrs |
             TvmmX86StateIntr);
    const TvmmX86State &s = *fState;
    fprintf(stderr, "  cpu %d rip %016" PRIx64 " rflags %08" PRIx64 "\n",
            fIndex, s.rip, s.rflags);
    for (int i = 0; i < TvmmX86GprCount; i++) {
        fprintf(stderr, "  %-3s %016" PRIx64 "%s", gpr_names[i], s.gprs[i],
                i % 4 == 3 ? "\n" : "");
    }
    fprintf(stderr, "  cr0 %016" PRIx64 " cr2 %016" PRIx64 " cr3 %016"
            PRIx64 " cr4 %016" PRIx64 "\n", s.cr0, s.cr2, s.cr3, s.cr4);
    fprintf(stderr, "  efer %" PRIx64 " xcr0 %" PRIx64 " cr8 %" PRIx64
            " pending event %" PRIx64 "\n", s.efer, s.xcr0, s.cr8,
            s.pendingEvent);
    for (int i = 0; i < TvmmX86SegCount; i++) {
        fprintf(stderr, "  %-3s sel %04x base %016" PRIx64 " limit %08x "
                "attrib %03x\n", seg_names[i], s.segs[i].selector,
                s.segs[i].base, s.segs[i].limit, s.segs[i].attrib);
    }
}


bool TvmmX86Vcpu::Idle(bool intr)
{
    return fShutdown || (fHalted && !(intr && (fRflags & RFLAGS_IF)));
}


std::unique_ptr<X86CpuBackend> host_x86_hypervisor_open(
    X86CpuTarget &target, DeviceLock &lock,
    const X86CpuOptions &options)
{
    auto tvmm = std::make_unique<TvmmX86Hypervisor>(target, lock, options);
    if (!tvmm->Init())
        return nullptr;
    return tvmm;
}
