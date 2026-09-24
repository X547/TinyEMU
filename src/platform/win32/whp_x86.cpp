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

#include <memory>
#include <vector>

#include <windows.h>
#include <winhvplatform.h>
#include <winhvemulation.h>

#include "bits.h"
#include "device_lock.h"
#include "host_memory.h"
#include "host_x86_hypervisor.h"


#define CPUID_APIC bit_at(9)
#define CPUID_ACPI bit_at(22)
#define RFLAGS_IF bit_at(9)

/* The longest a run lasts, in case the request to end it came just before
   it started. */
#define WHP_MAX_RUN_US (10 * 1000)

static const WHV_REGISTER_NAME kGprNames[8] = {
    WHvX64RegisterRax, WHvX64RegisterRcx, WHvX64RegisterRdx,
    WHvX64RegisterRbx, WHvX64RegisterRsp, WHvX64RegisterRbp,
    WHvX64RegisterRsi, WHvX64RegisterRdi,
};


static void whp_fail(const char *what, HRESULT hr)
{
    fprintf(stderr, "WHP: %s failed: 0x%08lx\n", what, (unsigned long)hr);
    exit(1);
}


class WhpX86Hypervisor;

/* One virtual processor. The 8259s raise INTR for processor 0 only, and the
   vector goes in as a pending interruption before a run. */
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

    void GetVpRegisters(const WHV_REGISTER_NAME *names, UINT32 count,
                        WHV_REGISTER_VALUE *values);
    void SetVpRegisters(const WHV_REGISTER_NAME *names, UINT32 count,
                        const WHV_REGISTER_VALUE *values);
    bool InjectInterrupt();
    void ExitCpuid(const WHV_RUN_VP_EXIT_CONTEXT &ctx);

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
};


/* The partition has no interrupt controller but, when asked, the local
   APICs: the machine's IOAPIC and MSIs reach them through
   WHvRequestInterrupt(). */
class WhpX86Hypervisor final: public HostX86Hypervisor {
private:
    friend class WhpX86Vcpu;

    X86HypervisorTarget &fTarget;
    DeviceLock &fLock;
    /* the hypervisor emulates the local APICs */
    bool fLocalApic;
    int fCpuCount;
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
        fCpuCount(options.cpu_count) {}
    ~WhpX86Hypervisor() override;

    bool Init();

    bool HasInterruptControllers() override {return false;}
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

    if (fLocalApic) {
        WHV_X64_LOCAL_APIC_EMULATION_MODE mode =
            WHvX64LocalApicEmulationModeXApic;
        hr = WHvSetPartitionProperty(
            fPartition, WHvPartitionPropertyCodeLocalApicEmulationMode,
            &mode, sizeof(mode));
        if (FAILED(hr))
            whp_fail("enabling the local APIC", hr);
    }

    /* CPUID comes to us for the leaves that report the APIC and ACPI, which
       the machine may not have. */
    WHV_EXTENDED_VM_EXITS exits {};
    exits.X64CpuidExit = 1;
    hr = WHvSetPartitionProperty(fPartition,
                                 WHvPartitionPropertyCodeExtendedVmExits,
                                 &exits, sizeof(exits));
    if (FAILED(hr))
        whp_fail("enabling CPUID exits", hr);
    static const UINT32 cpuid_leaves[] = {1, 0x80000001};
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


/* With the lock held, before a run: hands the processor the interrupt the
   8259s raise when it can take one, and otherwise asks to exit once it can.
   False while it stays halted. */
bool WhpX86Vcpu::InjectInterrupt()
{
    X86HypervisorTarget &target = fOwner.fTarget;
    WHV_REGISTER_NAME names[3];
    WHV_REGISTER_VALUE values[3] {};
    UINT32 count = 0;
    bool intr = fIndex == 0 && target.InterruptRequested();

    if (intr && !fInterruptionPending && !fInterruptShadow &&
        (fRflags & RFLAGS_IF)) {
        /* With the local APIC, HLT waits inside the hypervisor, and an
           injected interrupt does not end the wait by itself. */
        if (fOwner.fLocalApic) {
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
    SetVpRegisters(names, 5, values);
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
    case WHvRunVpExitReasonUnrecoverableException:
        fprintf(stderr, "WHP: unrecoverable exception at rip=0x%" PRIx64 "\n",
                (uint64_t)ctx.VpContext.Rip);
        exit(1);
    default:
        fprintf(stderr, "WHP: unsupported exit reason 0x%x at rip=0x%"
                PRIx64 "\n", ctx.ExitReason, (uint64_t)ctx.VpContext.Rip);
        exit(1);
    }
}


bool WhpX86Vcpu::Idle(bool intr)
{
    return fHalted && !(intr && (fRflags & RFLAGS_IF));
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

    if (options.local_apic) {
        hr = WHvGetCapability(WHvCapabilityCodeFeatures, &cap, sizeof(cap),
                              &size);
        if (FAILED(hr) || !cap.Features.LocalApicEmulation) {
            fprintf(stderr, "WHP has no local APIC emulation\n");
            return nullptr;
        }
    }

    auto whp = std::make_unique<WhpX86Hypervisor>(target, lock, options);
    if (!whp->Init())
        return nullptr;
    return whp;
}
