/*
 * Host hypervisor running the x86 processor: Windows Hypervisor Platform
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
#include <windows.h>
#include <winhvplatform.h>
#include <winhvemulation.h>

#include "bits.h"
#include "cutils.h"
#include "device_lock.h"
#include "host_memory.h"
#include "host_x86_hypervisor.h"


#define CPUID_APIC bit_at(9)
#define CPUID_ACPI bit_at(22)
#define CPUID_X2APIC bit_at(21)
#define CPUID_TSC_DEADLINE bit_at(24)
#define RFLAGS_IF bit_at(9)
#define MSR_IA32_APIC_BASE 0x1b

/* The longest a run lasts, in case the request to end it came just before
   it started. */
#define WHP_MAX_RUN_US (10 * 1000)

static const WHV_REGISTER_NAME kGprNames[8] = {
    WHvX64RegisterRax, WHvX64RegisterRcx, WHvX64RegisterRdx,
    WHvX64RegisterRbx, WHvX64RegisterRsp, WHvX64RegisterRbp,
    WHvX64RegisterRsi, WHvX64RegisterRdi,
};

/* What an INIT puts back as the processor was created: all but the time
   stamp counter and IA32_APIC_BASE. */
static const WHV_REGISTER_NAME kResetNames[] = {
    WHvX64RegisterRax, WHvX64RegisterRcx, WHvX64RegisterRdx,
    WHvX64RegisterRbx, WHvX64RegisterRsp, WHvX64RegisterRbp,
    WHvX64RegisterRsi, WHvX64RegisterRdi, WHvX64RegisterR8,
    WHvX64RegisterR9, WHvX64RegisterR10, WHvX64RegisterR11,
    WHvX64RegisterR12, WHvX64RegisterR13, WHvX64RegisterR14,
    WHvX64RegisterR15, WHvX64RegisterRip, WHvX64RegisterRflags,
    WHvX64RegisterEs, WHvX64RegisterCs, WHvX64RegisterSs,
    WHvX64RegisterDs, WHvX64RegisterFs, WHvX64RegisterGs,
    WHvX64RegisterLdtr, WHvX64RegisterTr, WHvX64RegisterIdtr,
    WHvX64RegisterGdtr, WHvX64RegisterCr0, WHvX64RegisterCr2,
    WHvX64RegisterCr3, WHvX64RegisterCr4, WHvX64RegisterCr8,
    WHvX64RegisterDr0, WHvX64RegisterDr1, WHvX64RegisterDr2,
    WHvX64RegisterDr3, WHvX64RegisterDr6, WHvX64RegisterDr7,
    WHvX64RegisterXCr0, WHvX64RegisterXmm0, WHvX64RegisterXmm1,
    WHvX64RegisterXmm2, WHvX64RegisterXmm3, WHvX64RegisterXmm4,
    WHvX64RegisterXmm5, WHvX64RegisterXmm6, WHvX64RegisterXmm7,
    WHvX64RegisterXmm8, WHvX64RegisterXmm9, WHvX64RegisterXmm10,
    WHvX64RegisterXmm11, WHvX64RegisterXmm12, WHvX64RegisterXmm13,
    WHvX64RegisterXmm14, WHvX64RegisterXmm15, WHvX64RegisterFpMmx0,
    WHvX64RegisterFpMmx1, WHvX64RegisterFpMmx2, WHvX64RegisterFpMmx3,
    WHvX64RegisterFpMmx4, WHvX64RegisterFpMmx5, WHvX64RegisterFpMmx6,
    WHvX64RegisterFpMmx7, WHvX64RegisterFpControlStatus,
    WHvX64RegisterXmmControlStatus, WHvX64RegisterEfer,
    WHvX64RegisterKernelGsBase, WHvX64RegisterPat,
    WHvX64RegisterSysenterCs, WHvX64RegisterSysenterEip,
    WHvX64RegisterSysenterEsp, WHvX64RegisterStar, WHvX64RegisterLstar,
    WHvX64RegisterCstar, WHvX64RegisterSfmask,
    WHvRegisterPendingInterruption, WHvRegisterInterruptState,
    WHvX64RegisterDeliverabilityNotifications,
};
#define RESET_REGISTER_COUNT (sizeof(kResetNames) / sizeof(kResetNames[0]))


static void whp_fail(const char *what, HRESULT hr)
{
    fprintf(stderr, "WHP: %s failed: 0x%08lx\n", what, (unsigned long)hr);
    exit(1);
}

/* The host processor's physical address width, which the partition's
   processors report unless told otherwise. */
static int host_phys_address_bits()
{
    unsigned int eax, ebx, ecx, edx;

    if (!__get_cpuid(0x80000008, &eax, &ebx, &ecx, &edx))
        return 36;
    return eax & 0xff;
}


class WhpX86Hypervisor;

/* One virtual processor. The 8259s raise INTR for processor 0 only, or the
   machine's local APIC for its own, and the vector goes in as a pending
   interruption before a run. */
class WhpX86Vcpu final: public HostX86Vcpu {
private:
    WhpX86Hypervisor &fOwner;
    WHV_PARTITION_HANDLE fPartition;
    UINT32 fIndex;
    WHV_EMULATOR_HANDLE fEmulator = nullptr;
    /* ends a run that has gone on too long */
    PTP_TIMER fTimer = nullptr;

    /* the interrupt state as of the last exit */
    uint64_t fRflags = 0x2;
    bool fInterruptShadow = false;
    bool fInterruptionPending = false;
    bool fWindowRequested = false;
    /* stopped at HLT until an interrupt is taken */
    bool fHalted = false;
    /* shut down, until the machine ends */
    bool fShutdown = false;
    /* With the machine's local APIC: CR8 as the processor and the APIC last
       agreed on it, and whether IA32_APIC_BASE holds the APIC's value. */
    int fTaskPriority = 0;
    bool fApicBaseSet = false;
    /* the registers as created, for an INIT */
    WHV_REGISTER_VALUE fResetValues[RESET_REGISTER_COUNT];

    void GetVpRegisters(const WHV_REGISTER_NAME *names, UINT32 count,
                        WHV_REGISTER_VALUE *values);
    void SetVpRegisters(const WHV_REGISTER_NAME *names, UINT32 count,
                        const WHV_REGISTER_VALUE *values);
    bool AcceptsPicInterrupt();
    bool InjectInterrupt();
    void SyncApic();
    void InjectGeneralProtection();
    void ExitCpuid(const WHV_RUN_VP_EXIT_CONTEXT &ctx);
    void ExitMsr(const WHV_RUN_VP_EXIT_CONTEXT &ctx);

    static HRESULT CALLBACK IoPortCallback(void *context,
                                           WHV_EMULATOR_IO_ACCESS_INFO *io);
    static HRESULT CALLBACK MemoryCallback(
        void *context, WHV_EMULATOR_MEMORY_ACCESS_INFO *mem);
    static HRESULT CALLBACK GetRegistersCallback(
        void *context, const WHV_REGISTER_NAME *names, UINT32 count,
        WHV_REGISTER_VALUE *values);
    static HRESULT CALLBACK SetRegistersCallback(
        void *context, const WHV_REGISTER_NAME *names, UINT32 count,
        const WHV_REGISTER_VALUE *values);
    static HRESULT CALLBACK TranslateGvaCallback(
        void *context, WHV_GUEST_VIRTUAL_ADDRESS gva,
        WHV_TRANSLATE_GVA_FLAGS flags, WHV_TRANSLATE_GVA_RESULT_CODE *result,
        WHV_GUEST_PHYSICAL_ADDRESS *gpa);
    static void CALLBACK TimerCallback(PTP_CALLBACK_INSTANCE instance,
                                       void *context, PTP_TIMER timer);

public:
    WhpX86Vcpu(WhpX86Hypervisor &owner, WHV_PARTITION_HANDLE partition,
               int index);
    ~WhpX86Vcpu() override;

    void GetRegs(HostX86Regs *regs) override;
    void SetRegs(const HostX86Regs &regs) override;
    void SetFlatProtectedMode(uint32_t gdt_base, uint16_t gdt_limit,
                              uint16_t code_sel, uint16_t data_sel) override;
    void ThreadStarted() override {}
    void Run() override;
    bool Idle(bool intr) override;
    void InterruptRun() override;
    /* only with the machine's local APICs; the hypervisor's take INIT and
       STARTUP themselves */
    void Init() override;
    void Startup(int vector) override;
};


/* The partition has no interrupt controller but, when asked and allowed, the
   local APICs: the machine's IOAPIC and MSIs reach them through
   WHvRequestInterrupt(). Otherwise the machine has them, and a processor
   exits for their page and for a write to IA32_APIC_BASE. */
class WhpX86Hypervisor final: public HostX86Hypervisor {
private:
    friend class WhpX86Vcpu;

    X86HypervisorTarget &fTarget;
    DeviceLock &fLock;
    /* the processors have local APICs */
    bool fLocalApic;
    /* which the hypervisor emulates */
    bool fApicEmulation;
    int fCpuCount;
    int fPhysAddressBits;
    WHV_PARTITION_HANDLE fPartition = nullptr;
    std::vector<std::unique_ptr<WhpX86Vcpu>> fVcpus;
    /* where each slot is mapped; the platform knows ranges, not slots */
    struct Slot {
        uint64_t addr;
        uint64_t size;
    };
    std::vector<Slot> fSlots;

public:
    WhpX86Hypervisor(X86HypervisorTarget &target, DeviceLock &lock,
                     const HostX86Options &options):
        fTarget(target), fLock(lock), fLocalApic(options.local_apic),
        fApicEmulation(options.local_apic &&
                       options.hypervisor_interrupt_controllers),
        fCpuCount(options.cpu_count),
        fPhysAddressBits(std::min(host_phys_address_bits(),
                                  options.max_phys_address_bits)) {}
    ~WhpX86Hypervisor() override;

    bool Init();

    bool HasInterruptControllers() override {return false;}
    bool HasLocalApics() override {return fApicEmulation;}
    int PhysAddressBits() override {return fPhysAddressBits;}
    uint8_t *AllocRam(size_t size) override {return host_ram_alloc(size);}
    void FreeRam(uint8_t *ptr, size_t size) override
    {
        host_ram_free(ptr, size);
    }
    void MapRam(int slot, uint64_t addr, uint64_t size, uint8_t *host_mem,
                bool read_only, bool log_dirty) override;
    void GetDirtyLog(int slot, uint32_t *bitmap) override;
    void SetIRQ(int irq, int level) override;
    void SendMsi(uint64_t addr, uint32_t data) override;
    HostX86Vcpu &Vcpu(int index) override {return *fVcpus[index];}
};


WhpX86Hypervisor::~WhpX86Hypervisor()
{
    fVcpus.clear();
    if (fPartition != nullptr)
        WHvDeletePartition(fPartition);
}


bool WhpX86Hypervisor::Init()
{
    HRESULT hr;

    hr = WHvCreatePartition(&fPartition);
    if (FAILED(hr)) {
        fprintf(stderr, "WHP: cannot create a partition: 0x%08lx\n",
                (unsigned long)hr);
        return false;
    }

    UINT32 processor_count = fCpuCount;
    hr = WHvSetPartitionProperty(fPartition,
                                 WHvPartitionPropertyCodeProcessorCount,
                                 &processor_count, sizeof(processor_count));
    if (FAILED(hr))
        whp_fail("setting the processor count", hr);

    if (fApicEmulation) {
        WHV_X64_LOCAL_APIC_EMULATION_MODE mode =
            WHvX64LocalApicEmulationModeXApic;
        hr = WHvSetPartitionProperty(
            fPartition, WHvPartitionPropertyCodeLocalApicEmulationMode,
            &mode, sizeof(mode));
        if (FAILED(hr))
            whp_fail("enabling the local APIC", hr);
    }

    /* CPUID comes to us for the leaves that report the APIC and ACPI, which
       the machine may not have, and for the physical address width. */
    WHV_EXTENDED_VM_EXITS exits {};
    exits.X64CpuidExit = 1;
    /* the machine's local APIC hears of a write to IA32_APIC_BASE */
    if (fLocalApic && !fApicEmulation)
        exits.X64MsrExit = 1;
    hr = WHvSetPartitionProperty(fPartition,
                                 WHvPartitionPropertyCodeExtendedVmExits,
                                 &exits, sizeof(exits));
    if (FAILED(hr))
        whp_fail("enabling CPUID exits", hr);
    if (exits.X64MsrExit) {
        WHV_X64_MSR_EXIT_BITMAP msr_exits {};
        msr_exits.ApicBaseMsrWrite = 1;
        hr = WHvSetPartitionProperty(fPartition,
                                     WHvPartitionPropertyCodeX64MsrExitBitmap,
                                     &msr_exits, sizeof(msr_exits));
        if (FAILED(hr))
            whp_fail("setting the MSR exit bitmap", hr);
    }
    static const UINT32 cpuid_leaves[] = {1, 0x80000001, 0x80000008};
    hr = WHvSetPartitionProperty(fPartition,
                                 WHvPartitionPropertyCodeCpuidExitList,
                                 cpuid_leaves, sizeof(cpuid_leaves));
    if (FAILED(hr))
        whp_fail("setting the CPUID exit list", hr);

    /* An MSR the hypervisor does not handle reads as 0 and ignores writes,
       rather than faulting a guest that probes it. */
    WHV_MSR_ACTION msr_action = WHvMsrActionIgnoreWriteReadZero;
    hr = WHvSetPartitionProperty(fPartition,
                                 WHvPartitionPropertyCodeUnimplementedMsrAction,
                                 &msr_action, sizeof(msr_action));
    if (FAILED(hr))
        whp_fail("setting the unimplemented MSR action", hr);

    hr = WHvSetupPartition(fPartition);
    if (FAILED(hr))
        whp_fail("WHvSetupPartition", hr);

    for (int i = 0; i < fCpuCount; i++)
        fVcpus.push_back(std::make_unique<WhpX86Vcpu>(*this, fPartition, i));
    return true;
}


void WhpX86Hypervisor::MapRam(int slot, uint64_t addr, uint64_t size,
                              uint8_t *host_mem, bool read_only,
                              bool log_dirty)
{
    HRESULT hr;

    if (slot >= (int)fSlots.size())
        fSlots.resize(slot + 1, Slot {0, 0});
    Slot &s = fSlots[slot];
    if (s.size != 0) {
        hr = WHvUnmapGpaRange(fPartition, s.addr, s.size);
        if (FAILED(hr))
            whp_fail("WHvUnmapGpaRange", hr);
        s.size = 0;
    }
    if (size == 0)
        return;

    int flags = WHvMapGpaRangeFlagRead | WHvMapGpaRangeFlagExecute;
    if (!read_only)
        flags |= WHvMapGpaRangeFlagWrite;
    if (log_dirty)
        flags |= WHvMapGpaRangeFlagTrackDirtyPages;
    hr = WHvMapGpaRange(fPartition, host_mem, addr, size,
                        (WHV_MAP_GPA_RANGE_FLAGS)flags);
    if (FAILED(hr))
        whp_fail("WHvMapGpaRange", hr);
    s.addr = addr;
    s.size = size;
}


void WhpX86Hypervisor::GetDirtyLog(int slot, uint32_t *bitmap)
{
    const Slot &s = fSlots[slot];
    uint64_t pages = s.size >> 12;
    UINT32 bitmap_size = ((pages + 63) / 64) * 8;
    HRESULT hr;

    hr = WHvQueryGpaRangeDirtyBitmap(fPartition, s.addr, s.size,
                                     reinterpret_cast<UINT64 *>(bitmap),
                                     bitmap_size);
    if (FAILED(hr))
        whp_fail("WHvQueryGpaRangeDirtyBitmap", hr);
}


void WhpX86Hypervisor::SetIRQ(int irq, int level)
{
    /* the machine keeps the 8259s, so no line comes here */
    abort();
}


/* The MSI address holds the destination and its mode, the data the vector,
   the delivery mode and the trigger. */
void WhpX86Hypervisor::SendMsi(uint64_t addr, uint32_t data)
{
    WHV_INTERRUPT_CONTROL ic {};
    HRESULT hr;

    switch (get_bits(data, 8, 3)) {
    case 0: ic.Type = WHvX64InterruptTypeFixed; break;
    case 1: ic.Type = WHvX64InterruptTypeLowestPriority; break;
    case 4: ic.Type = WHvX64InterruptTypeNmi; break;
    case 5: ic.Type = WHvX64InterruptTypeInit; break;
    default:
        /* SMI and ExtINT have nowhere to go */
        return;
    }
    ic.DestinationMode = get_bit(addr, 2) ?
        WHvX64InterruptDestinationModeLogical :
        WHvX64InterruptDestinationModePhysical;
    if (get_bit(data, 15)) {
        /* a level triggered message only counts while asserted */
        if (!get_bit(data, 14))
            return;
        ic.TriggerMode = WHvX64InterruptTriggerModeLevel;
    } else {
        ic.TriggerMode = WHvX64InterruptTriggerModeEdge;
    }
    ic.Destination = get_bits(addr, 12, 8);
    ic.Vector = get_bits(data, 0, 8);
    hr = WHvRequestInterrupt(fPartition, &ic, sizeof(ic));
    if (FAILED(hr))
        whp_fail("WHvRequestInterrupt", hr);
}


WhpX86Vcpu::WhpX86Vcpu(WhpX86Hypervisor &owner,
                       WHV_PARTITION_HANDLE partition, int index):
    fOwner(owner), fPartition(partition), fIndex(index)
{
    HRESULT hr;

    hr = WHvCreateVirtualProcessor(fPartition, fIndex, 0);
    if (FAILED(hr))
        whp_fail("WHvCreateVirtualProcessor", hr);

    WHV_EMULATOR_CALLBACKS callbacks {};
    callbacks.Size = sizeof(callbacks);
    callbacks.WHvEmulatorIoPortCallback = IoPortCallback;
    callbacks.WHvEmulatorMemoryCallback = MemoryCallback;
    callbacks.WHvEmulatorGetVirtualProcessorRegisters = GetRegistersCallback;
    callbacks.WHvEmulatorSetVirtualProcessorRegisters = SetRegistersCallback;
    callbacks.WHvEmulatorTranslateGvaPage = TranslateGvaCallback;
    hr = WHvEmulatorCreateEmulator(&callbacks, &fEmulator);
    if (FAILED(hr))
        whp_fail("WHvEmulatorCreateEmulator", hr);

    fTimer = CreateThreadpoolTimer(TimerCallback, this, nullptr);
    if (fTimer == nullptr)
        whp_fail("CreateThreadpoolTimer", HRESULT_FROM_WIN32(GetLastError()));

    GetVpRegisters(kResetNames, RESET_REGISTER_COUNT, fResetValues);
}


WhpX86Vcpu::~WhpX86Vcpu()
{
    SetThreadpoolTimer(fTimer, nullptr, 0, 0);
    WaitForThreadpoolTimerCallbacks(fTimer, TRUE);
    CloseThreadpoolTimer(fTimer);
    WHvEmulatorDestroyEmulator(fEmulator);
    WHvDeleteVirtualProcessor(fPartition, fIndex);
}


void WhpX86Vcpu::GetVpRegisters(const WHV_REGISTER_NAME *names, UINT32 count,
                                WHV_REGISTER_VALUE *values)
{
    HRESULT hr = WHvGetVirtualProcessorRegisters(fPartition, fIndex, names,
                                                 count, values);
    if (FAILED(hr))
        whp_fail("WHvGetVirtualProcessorRegisters", hr);
}


void WhpX86Vcpu::SetVpRegisters(const WHV_REGISTER_NAME *names, UINT32 count,
                                const WHV_REGISTER_VALUE *values)
{
    HRESULT hr = WHvSetVirtualProcessorRegisters(fPartition, fIndex, names,
                                                 count, values);
    if (FAILED(hr))
        whp_fail("WHvSetVirtualProcessorRegisters", hr);
}


void WhpX86Vcpu::GetRegs(HostX86Regs *regs)
{
    WHV_REGISTER_NAME names[10];
    WHV_REGISTER_VALUE values[10];

    memcpy(names, kGprNames, sizeof(kGprNames));
    names[8] = WHvX64RegisterRip;
    names[9] = WHvX64RegisterRflags;
    GetVpRegisters(names, 10, values);
    for (int i = 0; i < 8; i++)
        regs->gpr[i] = values[i].Reg64;
    regs->rip = values[8].Reg64;
    regs->rflags = values[9].Reg64;
}


void WhpX86Vcpu::SetRegs(const HostX86Regs &regs)
{
    WHV_REGISTER_NAME names[10];
    WHV_REGISTER_VALUE values[10] {};

    memcpy(names, kGprNames, sizeof(kGprNames));
    for (int i = 0; i < 8; i++)
        values[i].Reg64 = regs.gpr[i];
    names[8] = WHvX64RegisterRip;
    values[8].Reg64 = regs.rip;
    names[9] = WHvX64RegisterRflags;
    values[9].Reg64 = regs.rflags;
    SetVpRegisters(names, 10, values);
    fRflags = regs.rflags;
}


void WhpX86Vcpu::SetFlatProtectedMode(uint32_t gdt_base, uint16_t gdt_limit,
                                      uint16_t code_sel, uint16_t data_sel)
{
    static const WHV_REGISTER_NAME names[] = {
        WHvX64RegisterCr0, WHvX64RegisterGdtr, WHvX64RegisterCs,
        WHvX64RegisterDs, WHvX64RegisterEs, WHvX64RegisterSs,
        WHvX64RegisterFs, WHvX64RegisterGs,
    };
    WHV_REGISTER_VALUE values[8] {};

    GetVpRegisters(names, 1, values);
    values[0].Reg64 |= 1; /* CR0_PE */

    values[1].Table.Base = gdt_base;
    values[1].Table.Limit = gdt_limit;

    WHV_X64_SEGMENT_REGISTER seg {};
    seg.Base = 0;
    seg.Limit = 0xffffffff;
    seg.NonSystemSegment = 1;
    seg.Present = 1;
    seg.Default = 1;
    seg.Granularity = 1;

    seg.SegmentType = 0xb; /* code */
    seg.Selector = code_sel;
    values[2].Segment = seg;

    seg.SegmentType = 0x3; /* data */
    seg.Selector = data_sel;
    for (int i = 3; i < 8; i++)
        values[i].Segment = seg;

    SetVpRegisters(names, 8, values);
}


HRESULT CALLBACK WhpX86Vcpu::IoPortCallback(
    void *context, WHV_EMULATOR_IO_ACCESS_INFO *io)
{
    WhpX86Vcpu *v = static_cast<WhpX86Vcpu *>(context);
    X86HypervisorTarget &target = v->fOwner.fTarget;
    int size_log2;

    switch (io->AccessSize) {
    case 1: size_log2 = 0; break;
    case 2: size_log2 = 1; break;
    case 4: size_log2 = 2; break;
    default: return E_INVALIDARG;
    }
    if (io->Direction == 0) {
        io->Data = target.PortRead(io->Port, size_log2);
    } else {
        target.PortWrite(io->Port, io->Data, size_log2);
    }
    return S_OK;
}


HRESULT CALLBACK WhpX86Vcpu::MemoryCallback(
    void *context, WHV_EMULATOR_MEMORY_ACCESS_INFO *mem)
{
    WhpX86Vcpu *v = static_cast<WhpX86Vcpu *>(context);
    X86HypervisorTarget &target = v->fOwner.fTarget;

    switch (mem->AccessSize) {
    case 1:
    case 2:
    case 4:
    case 8:
        break;
    default:
        return E_INVALIDARG;
    }
    if (mem->Direction == 0) {
        target.MmioRead(mem->GpaAddress, mem->Data, mem->AccessSize);
    } else {
        target.MmioWrite(mem->GpaAddress, mem->Data, mem->AccessSize);
    }
    return S_OK;
}


HRESULT CALLBACK WhpX86Vcpu::GetRegistersCallback(
    void *context, const WHV_REGISTER_NAME *names, UINT32 count,
    WHV_REGISTER_VALUE *values)
{
    WhpX86Vcpu *v = static_cast<WhpX86Vcpu *>(context);

    return WHvGetVirtualProcessorRegisters(v->fPartition, v->fIndex, names,
                                           count, values);
}


HRESULT CALLBACK WhpX86Vcpu::SetRegistersCallback(
    void *context, const WHV_REGISTER_NAME *names, UINT32 count,
    const WHV_REGISTER_VALUE *values)
{
    WhpX86Vcpu *v = static_cast<WhpX86Vcpu *>(context);

    return WHvSetVirtualProcessorRegisters(v->fPartition, v->fIndex, names,
                                           count, values);
}


HRESULT CALLBACK WhpX86Vcpu::TranslateGvaCallback(
    void *context, WHV_GUEST_VIRTUAL_ADDRESS gva,
    WHV_TRANSLATE_GVA_FLAGS flags, WHV_TRANSLATE_GVA_RESULT_CODE *result,
    WHV_GUEST_PHYSICAL_ADDRESS *gpa)
{
    WhpX86Vcpu *v = static_cast<WhpX86Vcpu *>(context);
    WHV_TRANSLATE_GVA_RESULT res;
    HRESULT hr;

    hr = WHvTranslateGva(v->fPartition, v->fIndex, gva, flags, &res, gpa);
    *result = res.ResultCode;
    return hr;
}


void CALLBACK WhpX86Vcpu::TimerCallback(PTP_CALLBACK_INSTANCE instance,
                                        void *context, PTP_TIMER timer)
{
    WhpX86Vcpu *v = static_cast<WhpX86Vcpu *>(context);

    WHvCancelRunVirtualProcessor(v->fPartition, v->fIndex, 0);
}


/* The 8259s reach the processor through LINT0 of its local APIC, so only
   while that is in ExtINT mode and unmasked, or the APIC is off, as KVM's
   APIC has it. The injection itself goes around the APIC. */
bool WhpX86Vcpu::AcceptsPicInterrupt()
{
    WHV_REGISTER_NAME name = WHvX64RegisterApicBase;
    WHV_REGISTER_VALUE base;
    /* the xAPIC register page */
    alignas(16) uint8_t page[4096];
    UINT32 size;
    HRESULT hr;

    if (!fOwner.fApicEmulation)
        return true;
    GetVpRegisters(&name, 1, &base);
    if (!get_bit(base.Reg64, 11)) /* the APIC is disabled */
        return true;
    hr = WHvGetVirtualProcessorInterruptControllerState2(
        fPartition, fIndex, page, sizeof(page), &size);
    if (FAILED(hr))
        whp_fail("WHvGetVirtualProcessorInterruptControllerState2", hr);
    uint32_t lvt0 = get_le32(page + 0x350);
    return !get_bit(lvt0, 16) && /* not masked */
        get_bits(lvt0, 8, 3) == 7; /* ExtINT */
}


/* With the lock held, before a run: hands the processor the interrupt the
   8259s or the machine's local APIC raise when it can take one, and
   otherwise asks to exit once it can. False while it stays halted. */
bool WhpX86Vcpu::InjectInterrupt()
{
    X86HypervisorTarget &target = fOwner.fTarget;
    WHV_REGISTER_NAME names[3];
    WHV_REGISTER_VALUE values[3] {};
    UINT32 count = 0;
    bool intr = (fIndex == 0 || !fOwner.fApicEmulation) &&
        target.InterruptRequested() && AcceptsPicInterrupt();

    if (intr && !fInterruptionPending && !fInterruptShadow &&
        (fRflags & RFLAGS_IF)) {
        /* With the local APIC, HLT waits inside the hypervisor, and an
           injected interrupt does not end the wait by itself. */
        if (fOwner.fApicEmulation) {
            WHV_REGISTER_NAME name = WHvRegisterInternalActivityState;
            GetVpRegisters(&name, 1, &values[count]);
            if (values[count].InternalActivity.HaltSuspend) {
                values[count].InternalActivity.HaltSuspend = 0;
                names[count] = name;
                count++;
            }
        }
        names[count] = WHvRegisterPendingInterruption;
        values[count].PendingInterruption.InterruptionPending = 1;
        values[count].PendingInterruption.InterruptionType =
            WHvX64PendingInterrupt;
        values[count].PendingInterruption.InterruptionVector =
            target.AcknowledgeInterrupt();
        count++;
        fInterruptionPending = true;
        fHalted = false;
        intr = target.InterruptRequested();
    }
    if (intr && !fWindowRequested) {
        names[count] = WHvX64RegisterDeliverabilityNotifications;
        values[count].DeliverabilityNotifications.InterruptNotification = 1;
        count++;
        fWindowRequested = true;
    }
    if (count != 0)
        SetVpRegisters(names, count, values);
    return !fHalted;
}


void WhpX86Vcpu::ExitCpuid(const WHV_RUN_VP_EXIT_CONTEXT &ctx)
{
    const WHV_X64_CPUID_ACCESS_CONTEXT &c = ctx.CpuidAccess;
    static const WHV_REGISTER_NAME names[] = {
        WHvX64RegisterRip, WHvX64RegisterRax, WHvX64RegisterRbx,
        WHvX64RegisterRcx, WHvX64RegisterRdx,
    };
    WHV_REGISTER_VALUE values[5] {};

    values[0].Reg64 = ctx.VpContext.Rip + ctx.VpContext.InstructionLength;
    values[1].Reg64 = c.DefaultResultRax;
    values[2].Reg64 = c.DefaultResultRbx;
    values[3].Reg64 = c.DefaultResultRcx;
    values[4].Reg64 = c.DefaultResultRdx;
    /* remove the APIC (unless the machine has one) & ACPI to be in sync
       with the emulator */
    if (c.Rax == 1 || c.Rax == 0x80000001) {
        values[4].Reg64 &= ~(uint64_t)CPUID_ACPI;
        if (!fOwner.fLocalApic)
            values[4].Reg64 &= ~(uint64_t)CPUID_APIC;
    }
    /* the machine's local APIC: xAPIC only, with the processor's index as
       its ID */
    if (c.Rax == 1 && fOwner.fLocalApic && !fOwner.fApicEmulation) {
        values[2].Reg64 = set_bits((uint32_t)values[2].Reg64, 24, 8, fIndex);
        values[3].Reg64 &= ~(uint64_t)(CPUID_X2APIC | CPUID_TSC_DEADLINE);
        values[4].Reg64 |= CPUID_APIC;
    }
    /* the physical address width, with no separate guest width */
    if (c.Rax == 0x80000008) {
        values[1].Reg64 = (values[1].Reg64 & ~(uint64_t)0x00ff00ff) |
            fOwner.fPhysAddressBits;
    }
    SetVpRegisters(names, 5, values);
}


/* With the lock held, before a run: IA32_APIC_BASE, which the processor
   reads without an exit, and CR8 take what the machine's local APIC has.
   CR8 comes back at every exit; a write that lowers it goes unnoticed until
   then, since the hypervisor has no exit for it. */
void WhpX86Vcpu::SyncApic()
{
    X86HypervisorTarget &target = fOwner.fTarget;
    WHV_REGISTER_NAME names[2];
    WHV_REGISTER_VALUE values[2] {};
    UINT32 count = 0;

    if (!fApicBaseSet) {
        names[count] = WHvX64RegisterApicBase;
        values[count].Reg64 = target.ApicBase();
        count++;
        fApicBaseSet = true;
    }
    int tpr = target.TaskPriority();
    if (tpr != fTaskPriority) {
        names[count] = WHvX64RegisterCr8;
        values[count].Reg64 = tpr;
        count++;
        fTaskPriority = tpr;
    }
    if (count != 0)
        SetVpRegisters(names, count, values);
}


void WhpX86Vcpu::InjectGeneralProtection()
{
    WHV_REGISTER_NAME name = WHvRegisterPendingInterruption;
    WHV_REGISTER_VALUE value {};

    value.PendingInterruption.InterruptionPending = 1;
    value.PendingInterruption.InterruptionType = WHvX64PendingException;
    value.PendingInterruption.DeliverErrorCode = 1;
    value.PendingInterruption.InterruptionVector = 13;
    SetVpRegisters(&name, 1, &value);
}


/* Only a write to IA32_APIC_BASE exits, for the machine's local APIC. */
void WhpX86Vcpu::ExitMsr(const WHV_RUN_VP_EXIT_CONTEXT &ctx)
{
    const WHV_X64_MSR_ACCESS_CONTEXT &m = ctx.MsrAccess;
    X86HypervisorTarget &target = fOwner.fTarget;
    WHV_REGISTER_NAME names[2] = {WHvX64RegisterRip, WHvX64RegisterApicBase};
    WHV_REGISTER_VALUE values[2] {};

    if (!m.AccessInfo.IsWrite || m.MsrNumber != MSR_IA32_APIC_BASE) {
        fprintf(stderr, "WHP: unexpected exit for MSR 0x%x at rip=0x%" PRIx64
                "\n", m.MsrNumber, (uint64_t)ctx.VpContext.Rip);
        exit(1);
    }
    DeviceLocker locker(fOwner.fLock);
    if (!target.SetApicBase(((uint64_t)m.Rdx << 32) | (uint32_t)m.Rax)) {
        InjectGeneralProtection();
        return;
    }
    values[0].Reg64 = ctx.VpContext.Rip + ctx.VpContext.InstructionLength;
    values[1].Reg64 = target.ApicBase();
    SetVpRegisters(names, 2, values);
}


/* The registers as created; the machine's local APIC keeps its base. */
void WhpX86Vcpu::Init()
{
    SetVpRegisters(kResetNames, RESET_REGISTER_COUNT, fResetValues);
    fRflags = 0x2;
    fInterruptShadow = false;
    fInterruptionPending = false;
    fWindowRequested = false;
    fHalted = false;
    fTaskPriority = 0;
}


void WhpX86Vcpu::Startup(int vector)
{
    WHV_REGISTER_NAME names[2] = {WHvX64RegisterCs, WHvX64RegisterRip};
    WHV_REGISTER_VALUE values[2];

    GetVpRegisters(names, 2, values);
    values[0].Segment.Selector = vector << 8;
    values[0].Segment.Base = (uint64_t)vector << 12;
    values[1].Reg64 = 0;
    SetVpRegisters(names, 2, values);
}


void WhpX86Vcpu::Run()
{
    DeviceLock &lock = fOwner.fLock;
    X86HypervisorTarget &target = fOwner.fTarget;
    WHV_RUN_VP_EXIT_CONTEXT ctx;
    WHV_EMULATOR_STATUS status;
    HRESULT hr;

    {
        DeviceLocker locker(lock);
        if (fOwner.fLocalApic && !fOwner.fApicEmulation)
            SyncApic();
        if (!InjectInterrupt())
            return;
    }

    ULARGE_INTEGER due;
    due.QuadPart = (ULONGLONG)(-(LONGLONG)WHP_MAX_RUN_US * 10);
    FILETIME due_time;
    due_time.dwLowDateTime = due.LowPart;
    due_time.dwHighDateTime = due.HighPart;
    SetThreadpoolTimer(fTimer, &due_time, 0, 0);

    hr = WHvRunVirtualProcessor(fPartition, fIndex, &ctx, sizeof(ctx));
    /* a callback already under way at most ends the next run early */
    SetThreadpoolTimer(fTimer, nullptr, 0, 0);
    if (FAILED(hr))
        whp_fail("WHvRunVirtualProcessor", hr);

    fRflags = ctx.VpContext.Rflags;
    fInterruptShadow = ctx.VpContext.ExecutionState.InterruptShadow;
    fInterruptionPending = ctx.VpContext.ExecutionState.InterruptionPending;
    /* the local APIC learns of a write to CR8 before anything else sees
       it */
    if (fOwner.fLocalApic && !fOwner.fApicEmulation &&
        ctx.VpContext.Cr8 != fTaskPriority) {
        fTaskPriority = ctx.VpContext.Cr8;
        DeviceLocker locker(lock);
        target.SetTaskPriority(fTaskPriority);
    }

    switch (ctx.ExitReason) {
    case WHvRunVpExitReasonMemoryAccess: {
        DeviceLocker locker(lock);
        hr = WHvEmulatorTryMmioEmulation(fEmulator, this, &ctx.VpContext,
                                         &ctx.MemoryAccess, &status);
        if (FAILED(hr) || !status.EmulationSuccessful) {
            fprintf(stderr, "WHP: cannot emulate the access to 0x%" PRIx64
                    " at rip=0x%" PRIx64 " (status 0x%x)\n",
                    (uint64_t)ctx.MemoryAccess.Gpa,
                    (uint64_t)ctx.VpContext.Rip, status.AsUINT32);
            exit(1);
        }
        break;
    }
    case WHvRunVpExitReasonX64IoPortAccess: {
        DeviceLocker locker(lock);
        hr = WHvEmulatorTryIoEmulation(fEmulator, this, &ctx.VpContext,
                                       &ctx.IoPortAccess, &status);
        if (FAILED(hr) || !status.EmulationSuccessful) {
            fprintf(stderr, "WHP: cannot emulate the access to port 0x%x"
                    " at rip=0x%" PRIx64 " (status 0x%x)\n",
                    ctx.IoPortAccess.PortNumber,
                    (uint64_t)ctx.VpContext.Rip, status.AsUINT32);
            exit(1);
        }
        break;
    }
    case WHvRunVpExitReasonX64Cpuid:
        ExitCpuid(ctx);
        break;
    case WHvRunVpExitReasonX64MsrAccess:
        ExitMsr(ctx);
        break;
    case WHvRunVpExitReasonX64Halt:
        fHalted = true;
        break;
    case WHvRunVpExitReasonX64ApicEoi: {
        DeviceLocker locker(lock);
        target.ApicEoi(ctx.ApicEoi.InterruptVector);
        break;
    }
    case WHvRunVpExitReasonX64InterruptWindow:
        fWindowRequested = false;
        break;
    case WHvRunVpExitReasonCanceled:
        break;
    case WHvRunVpExitReasonUnrecoverableException: {
        DeviceLocker locker(lock);
        fprintf(stderr, "WHP: processor %u shut down at rip=0x%" PRIx64 "\n",
                fIndex, (uint64_t)ctx.VpContext.Rip);
        fShutdown = true;
        target.ProcessorShutdown();
        break;
    }
    default:
        fprintf(stderr, "WHP: unsupported exit reason 0x%x at rip=0x%"
                PRIx64 "\n", ctx.ExitReason, (uint64_t)ctx.VpContext.Rip);
        exit(1);
    }
}


bool WhpX86Vcpu::Idle(bool intr)
{
    return fShutdown || (fHalted && !(intr && (fRflags & RFLAGS_IF)));
}


void WhpX86Vcpu::InterruptRun()
{
    WHvCancelRunVirtualProcessor(fPartition, fIndex, 0);
}


std::unique_ptr<HostX86Hypervisor> host_x86_hypervisor_open(
    X86HypervisorTarget &target, DeviceLock &lock,
    const HostX86Options &options)
{
    WHV_CAPABILITY cap;
    UINT32 size;
    HRESULT hr;

    hr = WHvGetCapability(WHvCapabilityCodeHypervisorPresent, &cap,
                          sizeof(cap), &size);
    if (FAILED(hr) || !cap.HypervisorPresent) {
        fprintf(stderr, "Windows Hypervisor Platform not available\n");
        return nullptr;
    }

    if (options.local_apic && options.hypervisor_interrupt_controllers) {
        hr = WHvGetCapability(WHvCapabilityCodeFeatures, &cap, sizeof(cap),
                              &size);
        if (FAILED(hr) || !cap.Features.LocalApicEmulation) {
            fprintf(stderr, "WHP has no local APIC emulation\n");
            return nullptr;
        }
    } else if (options.local_apic) {
        hr = WHvGetCapability(WHvCapabilityCodeX64MsrExitBitmap, &cap,
                              sizeof(cap), &size);
        if (FAILED(hr) || !cap.X64MsrExitBitmap.ApicBaseMsrWrite) {
            fprintf(stderr, "WHP cannot exit at a write to "
                    "IA32_APIC_BASE\n");
            return nullptr;
        }
    }

    auto whp = std::make_unique<WhpX86Hypervisor>(target, lock, options);
    if (!whp->Init())
        return nullptr;
    return whp;
}
