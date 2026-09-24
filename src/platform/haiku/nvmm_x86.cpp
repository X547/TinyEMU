/*
 * Host hypervisor running the x86 processor: NVMM
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
#include <errno.h>
#include <inttypes.h>

#include <vector>

extern "C" {
#include <nvmm.h>
}

#include "bits.h"
#include "device_lock.h"
#include "host_memory.h"
#include "host_x86_hypervisor.h"


#define CPUID_APIC bit_at(9)
#define CPUID_ACPI bit_at(22)
#define RFLAGS_IF bit_at(9)


class NvmmX86Hypervisor;

/* What the assist callbacks are handed, from which they find their way back
   to the hypervisor. */
struct NvmmVcpu {
    struct nvmm_vcpu vcpu {};
    NvmmX86Hypervisor *owner = nullptr;
};


/* The machine has no interrupt controller in the kernel: the machine's 8259s
   raise INTR, and the vector goes in as an event before a run. Nothing can
   stop a run from another thread, so one lasts until an exit or the end of
   the host scheduler's quantum, and InterruptRun() does nothing. */
class NvmmX86Hypervisor final: public HostX86Hypervisor {
private:
    X86HypervisorTarget &fTarget;
    DeviceLock &fLock;
    struct nvmm_machine fMach {};
    bool fMachCreated = false;
    NvmmVcpu fVcpu;
    bool fVcpuCreated = false;
    /* where each slot is mapped; the platform knows ranges, not slots */
    struct Slot {
        uint64_t addr;
        uint64_t size;
        uint8_t *host_mem;
    };
    std::vector<Slot> fSlots;

    /* the interrupt state as of the last exit */
    uint64_t fRflags = 0x2;
    bool fInterruptShadow = false;
    bool fEventPending = false;
    bool fWindowRequested = false;
    /* stopped at HLT until an interrupt is taken */
    bool fHalted = false;

    void Fail(const char *what);
    void SetResetFpuState();
    bool InjectInterrupt();
    void InjectException(int vector);
    void ExitMsr(const struct nvmm_vcpu_exit *ctx);

    static void IoCallback(struct nvmm_io *io);
    static void MemCallback(struct nvmm_mem *mem);

public:
    NvmmX86Hypervisor(X86HypervisorTarget &target, DeviceLock &lock):
        fTarget(target), fLock(lock) {}
    ~NvmmX86Hypervisor() override;

    bool Init();

    bool HasInterruptControllers() override {return false;}
    uint8_t *AllocRam(size_t size) override;
    void FreeRam(uint8_t *ptr, size_t size) override;
    void MapRam(int slot, uint64_t addr, uint64_t size, uint8_t *host_mem,
                bool read_only, bool log_dirty) override;
    void GetDirtyLog(int slot, uint32_t *bitmap) override;
    void SetIRQ(int irq, int level) override;
    void SendMsi(uint64_t addr, uint32_t data) override;
    void GetRegs(HostX86Regs *regs) override;
    void SetRegs(const HostX86Regs &regs) override;
    void SetFlatProtectedMode(uint32_t gdt_base, uint16_t gdt_limit,
                              uint16_t code_sel, uint16_t data_sel) override;
    void ProcessorThreadStarted() override {}
    void Run(int64_t timeout_us) override;
    bool Idle(bool intr) override;
    void InterruptRun() override {}
};


void NvmmX86Hypervisor::Fail(const char *what)
{
    fprintf(stderr, "NVMM: %s failed: %s\n", what, strerror(errno));
    exit(1);
}


NvmmX86Hypervisor::~NvmmX86Hypervisor()
{
    if (fVcpuCreated)
        nvmm_vcpu_destroy(&fMach, &fVcpu.vcpu);
    if (fMachCreated)
        nvmm_machine_destroy(&fMach);
}


bool NvmmX86Hypervisor::Init()
{
    struct nvmm_capability cap;

    if (nvmm_capability(&cap) == -1) {
        fprintf(stderr, "NVMM: cannot read the capabilities: %s\n",
                strerror(errno));
        return false;
    }
    if (!(cap.arch.vcpu_conf_support & NVMM_CAP_ARCH_VCPU_CONF_CPUID)) {
        fprintf(stderr, "NVMM: CPUID cannot be configured\n");
        return false;
    }

    if (nvmm_machine_create(&fMach) == -1) {
        fprintf(stderr, "NVMM: cannot create a machine: %s\n",
                strerror(errno));
        return false;
    }
    fMachCreated = true;

    fVcpu.owner = this;
    if (nvmm_vcpu_create(&fMach, 0, &fVcpu.vcpu) == -1)
        Fail("nvmm_vcpu_create");
    fVcpuCreated = true;

    struct nvmm_assist_callbacks callbacks = {IoCallback, MemCallback};
    if (nvmm_vcpu_configure(&fMach, &fVcpu.vcpu, NVMM_VCPU_CONF_CALLBACKS,
                            &callbacks) == -1) {
        Fail("setting the assist callbacks");
    }

    SetResetFpuState();

    /* remove the APIC & ACPI to be in sync with the emulator */
    static const uint32_t leaves[] = {1, 0x80000001};
    for (uint32_t leaf: leaves) {
        struct nvmm_vcpu_conf_cpuid cpuid;
        memset(&cpuid, 0, sizeof(cpuid));
        cpuid.mask = 1;
        cpuid.leaf = leaf;
        cpuid.u.mask.del.edx = CPUID_APIC | CPUID_ACPI;
        if (nvmm_vcpu_configure(&fMach, &fVcpu.vcpu, NVMM_VCPU_CONF_CPUID,
                                &cpuid) == -1) {
            Fail("masking CPUID");
        }
    }
    return true;
}


/* The x87 and SSE state after a processor reset. The vcpu starts with it
   already, but the driver masks the MXCSR it is given with the mask beside it,
   and the mask it resets to is zero, so the guest would start with every SSE
   exception unmasked. A full mask asks for the host's. */
void NvmmX86Hypervisor::SetResetFpuState()
{
    struct nvmm_x64_state *state = fVcpu.vcpu.state;
    struct nvmm_x64_state_fpu &fpu = state->fpu;

    memset(&fpu, 0, sizeof(fpu));
    fpu.fx_cw = 0x0040;
    fpu.fx_tw = 0x55;
    fpu.fx_zero = 0x55;
    fpu.fx_mxcsr = 0x1f80;
    fpu.fx_mxcsr_mask = ~(uint32_t)0;

    if (nvmm_vcpu_setstate(&fMach, &fVcpu.vcpu, NVMM_X64_STATE_FPU) == -1)
        Fail("nvmm_vcpu_setstate");
}


/* Mapping host memory into the machine puts a new object over the range, so
   that has to happen before anything is written to it. */
uint8_t *NvmmX86Hypervisor::AllocRam(size_t size)
{
    uint8_t *ptr = host_ram_alloc(size);
    if (ptr == nullptr)
        return nullptr;
    if (nvmm_hva_map(&fMach, (uintptr_t)ptr, size) == -1)
        Fail("nvmm_hva_map");
    return ptr;
}


void NvmmX86Hypervisor::FreeRam(uint8_t *ptr, size_t size)
{
    nvmm_hva_unmap(&fMach, (uintptr_t)ptr, size);
}


void NvmmX86Hypervisor::MapRam(int slot, uint64_t addr, uint64_t size,
                               uint8_t *host_mem, bool read_only,
                               bool log_dirty)
{
    (void)log_dirty;

    if (slot >= (int)fSlots.size())
        fSlots.resize(slot + 1, Slot {0, 0, nullptr});
    Slot &s = fSlots[slot];
    if (s.size != 0) {
        if (nvmm_gpa_unmap(&fMach, (uintptr_t)s.host_mem, s.addr,
                           s.size) == -1) {
            Fail("nvmm_gpa_unmap");
        }
        s.size = 0;
    }
    if (size == 0)
        return;

    int prot = NVMM_PROT_READ | NVMM_PROT_EXEC;
    if (!read_only)
        prot |= NVMM_PROT_WRITE;
    if (nvmm_gpa_map(&fMach, (uintptr_t)host_mem, addr, size, prot) == -1)
        Fail("nvmm_gpa_map");
    s.addr = addr;
    s.size = size;
    s.host_mem = host_mem;
}


/* NVMM keeps no dirty log, so every page counts as written. */
void NvmmX86Hypervisor::GetDirtyLog(int slot, uint32_t *bitmap)
{
    uint64_t pages = fSlots[slot].size >> 12;
    memset(bitmap, 0xff, ((pages + 63) / 64) * 8);
}


void NvmmX86Hypervisor::SetIRQ(int irq, int level)
{
    /* the machine keeps the 8259s, so no line comes here */
    abort();
}


void NvmmX86Hypervisor::SendMsi(uint64_t addr, uint32_t data)
{
    /* there is no local APIC to send to */
    abort();
}


void NvmmX86Hypervisor::GetRegs(HostX86Regs *regs)
{
    struct nvmm_x64_state *state = fVcpu.vcpu.state;

    if (nvmm_vcpu_getstate(&fMach, &fVcpu.vcpu, NVMM_X64_STATE_GPRS) == -1)
        Fail("nvmm_vcpu_getstate");
    for (int i = 0; i < 8; i++)
        regs->gpr[i] = state->gprs[NVMM_X64_GPR_RAX + i];
    regs->rip = state->gprs[NVMM_X64_GPR_RIP];
    regs->rflags = state->gprs[NVMM_X64_GPR_RFLAGS];
}


void NvmmX86Hypervisor::SetRegs(const HostX86Regs &regs)
{
    struct nvmm_x64_state *state = fVcpu.vcpu.state;

    if (nvmm_vcpu_getstate(&fMach, &fVcpu.vcpu, NVMM_X64_STATE_GPRS) == -1)
        Fail("nvmm_vcpu_getstate");
    for (int i = 0; i < 8; i++)
        state->gprs[NVMM_X64_GPR_RAX + i] = regs.gpr[i];
    state->gprs[NVMM_X64_GPR_RIP] = regs.rip;
    state->gprs[NVMM_X64_GPR_RFLAGS] = regs.rflags;
    if (nvmm_vcpu_setstate(&fMach, &fVcpu.vcpu, NVMM_X64_STATE_GPRS) == -1)
        Fail("nvmm_vcpu_setstate");
    fRflags = regs.rflags;
}


void NvmmX86Hypervisor::SetFlatProtectedMode(uint32_t gdt_base,
                                             uint16_t gdt_limit,
                                             uint16_t code_sel,
                                             uint16_t data_sel)
{
    struct nvmm_x64_state *state = fVcpu.vcpu.state;
    static const int data_segs[] = {
        NVMM_X64_SEG_DS, NVMM_X64_SEG_ES, NVMM_X64_SEG_SS,
        NVMM_X64_SEG_FS, NVMM_X64_SEG_GS,
    };

    if (nvmm_vcpu_getstate(&fMach, &fVcpu.vcpu,
                           NVMM_X64_STATE_SEGS | NVMM_X64_STATE_CRS) == -1) {
        Fail("nvmm_vcpu_getstate");
    }
    state->crs[NVMM_X64_CR_CR0] |= 1; /* CR0_PE */
    state->segs[NVMM_X64_SEG_GDT].base = gdt_base;
    state->segs[NVMM_X64_SEG_GDT].limit = gdt_limit;

    struct nvmm_x64_state_seg seg;
    memset(&seg, 0, sizeof(seg));
    seg.base = 0;
    seg.limit = 0xffffffff;
    seg.attrib.s = 1; /* code/data */
    seg.attrib.p = 1;
    seg.attrib.def = 1;
    seg.attrib.g = 1; /* 4KB granularity */

    seg.attrib.type = 0xb; /* code */
    seg.selector = code_sel;
    state->segs[NVMM_X64_SEG_CS] = seg;

    seg.attrib.type = 0x3; /* data */
    seg.selector = data_sel;
    for (int i: data_segs)
        state->segs[i] = seg;

    if (nvmm_vcpu_setstate(&fMach, &fVcpu.vcpu,
                           NVMM_X64_STATE_SEGS | NVMM_X64_STATE_CRS) == -1) {
        Fail("nvmm_vcpu_setstate");
    }
}


void NvmmX86Hypervisor::IoCallback(struct nvmm_io *io)
{
    NvmmVcpu *v = reinterpret_cast<NvmmVcpu *>(io->vcpu);
    X86HypervisorTarget &target = v->owner->fTarget;
    int size_log2;

    switch (io->size) {
    case 1: size_log2 = 0; break;
    case 2: size_log2 = 1; break;
    case 4: size_log2 = 2; break;
    default: abort();
    }
    if (io->in) {
        uint32_t val = target.PortRead(io->port, size_log2);
        memcpy(io->data, &val, io->size);
    } else {
        uint32_t val = 0;
        memcpy(&val, io->data, io->size);
        target.PortWrite(io->port, val, size_log2);
    }
}


void NvmmX86Hypervisor::MemCallback(struct nvmm_mem *mem)
{
    NvmmVcpu *v = reinterpret_cast<NvmmVcpu *>(mem->vcpu);
    X86HypervisorTarget &target = v->owner->fTarget;

    switch (mem->size) {
    case 1:
    case 2:
    case 4:
    case 8:
        if (mem->write)
            target.MmioWrite(mem->gpa, mem->data, mem->size);
        else
            target.MmioRead(mem->gpa, mem->data, mem->size);
        break;
    default:
        /* an access the target has no width for goes a byte at a time */
        for (size_t i = 0; i < mem->size; i++) {
            if (mem->write)
                target.MmioWrite(mem->gpa + i, mem->data + i, 1);
            else
                target.MmioRead(mem->gpa + i, mem->data + i, 1);
        }
        break;
    }
}


/* With the lock held, before a run: hands the processor the interrupt the
   8259s raise when it can take one, and otherwise asks to exit once it can.
   False while it stays halted. */
bool NvmmX86Hypervisor::InjectInterrupt()
{
    struct nvmm_vcpu *vcpu = &fVcpu.vcpu;
    bool intr = fTarget.InterruptRequested();

    if (intr && !fEventPending && !fInterruptShadow &&
        (fRflags & RFLAGS_IF)) {
        vcpu->event->type = NVMM_VCPU_EVENT_INTR;
        vcpu->event->vector = fTarget.AcknowledgeInterrupt();
        if (nvmm_vcpu_inject(&fMach, vcpu) == -1)
            Fail("nvmm_vcpu_inject");
        fEventPending = true;
        fHalted = false;
        intr = fTarget.InterruptRequested();
    }
    if (fHalted)
        return false;
    if (intr && !fWindowRequested) {
        if (nvmm_vcpu_getstate(&fMach, vcpu, NVMM_X64_STATE_INTR) == -1)
            Fail("nvmm_vcpu_getstate");
        vcpu->state->intr.int_window_exiting = 1;
        if (nvmm_vcpu_setstate(&fMach, vcpu, NVMM_X64_STATE_INTR) == -1)
            Fail("nvmm_vcpu_setstate");
        fWindowRequested = true;
    }
    return true;
}


void NvmmX86Hypervisor::InjectException(int vector)
{
    struct nvmm_vcpu *vcpu = &fVcpu.vcpu;

    vcpu->event->type = NVMM_VCPU_EVENT_EXCP;
    vcpu->event->vector = vector;
    vcpu->event->u.excp.error = 0;
    if (nvmm_vcpu_inject(&fMach, vcpu) == -1)
        Fail("nvmm_vcpu_inject");
}


/* An MSR the kernel does not handle reads as 0 and ignores writes, rather
   than faulting a guest that probes it. */
void NvmmX86Hypervisor::ExitMsr(const struct nvmm_vcpu_exit *ctx)
{
    struct nvmm_x64_state *state = fVcpu.vcpu.state;

    if (nvmm_vcpu_getstate(&fMach, &fVcpu.vcpu, NVMM_X64_STATE_GPRS) == -1)
        Fail("nvmm_vcpu_getstate");
    if (ctx->reason == NVMM_VCPU_EXIT_RDMSR) {
        state->gprs[NVMM_X64_GPR_RAX] = 0;
        state->gprs[NVMM_X64_GPR_RDX] = 0;
        state->gprs[NVMM_X64_GPR_RIP] = ctx->u.rdmsr.npc;
    } else {
        state->gprs[NVMM_X64_GPR_RIP] = ctx->u.wrmsr.npc;
    }
    if (nvmm_vcpu_setstate(&fMach, &fVcpu.vcpu, NVMM_X64_STATE_GPRS) == -1)
        Fail("nvmm_vcpu_setstate");
}


void NvmmX86Hypervisor::Run(int64_t timeout_us)
{
    struct nvmm_vcpu *vcpu = &fVcpu.vcpu;
    const struct nvmm_vcpu_exit *ctx = vcpu->exit;

    (void)timeout_us;
    {
        DeviceLocker locker(fLock);
        if (!InjectInterrupt())
            return;
    }

    if (nvmm_vcpu_run(&fMach, vcpu) == -1) {
        if (errno == EINTR || errno == EAGAIN)
            return;
        Fail("nvmm_vcpu_run");
    }

    fRflags = ctx->exitstate.rflags;
    fInterruptShadow = ctx->exitstate.int_shadow;
    fEventPending = ctx->exitstate.evt_pending;
    fWindowRequested = ctx->exitstate.int_window_exiting;

    switch (ctx->reason) {
    case NVMM_VCPU_EXIT_NONE:
    case NVMM_VCPU_EXIT_INT_READY:
    case NVMM_VCPU_EXIT_TPR_CHANGED:
        break;
    case NVMM_VCPU_EXIT_MEMORY: {
        DeviceLocker locker(fLock);
        if (nvmm_assist_mem(&fMach, vcpu) == -1) {
            fprintf(stderr, "NVMM: cannot emulate the access to 0x%" PRIx64
                    ": %s\n", (uint64_t)ctx->u.mem.gpa, strerror(errno));
            exit(1);
        }
        break;
    }
    case NVMM_VCPU_EXIT_IO: {
        DeviceLocker locker(fLock);
        if (nvmm_assist_io(&fMach, vcpu) == -1) {
            fprintf(stderr, "NVMM: cannot emulate the access to port 0x%x:"
                    " %s\n", ctx->u.io.port, strerror(errno));
            exit(1);
        }
        break;
    }
    case NVMM_VCPU_EXIT_HALTED:
        fHalted = true;
        break;
    case NVMM_VCPU_EXIT_RDMSR:
    case NVMM_VCPU_EXIT_WRMSR:
        ExitMsr(ctx);
        break;
    case NVMM_VCPU_EXIT_MONITOR:
    case NVMM_VCPU_EXIT_MWAIT:
        InjectException(6); /* #UD, as on a processor without them */
        break;
    case NVMM_VCPU_EXIT_SHUTDOWN:
        fprintf(stderr, "NVMM: the guest shut down (triple fault)\n");
        exit(1);
    default:
        fprintf(stderr, "NVMM: unsupported exit reason 0x%" PRIx64 "\n",
                (uint64_t)ctx->reason);
        exit(1);
    }
}


bool NvmmX86Hypervisor::Idle(bool intr)
{
    return fHalted && !(intr && (fRflags & RFLAGS_IF));
}


std::unique_ptr<HostX86Hypervisor> host_x86_hypervisor_open(
    X86HypervisorTarget &target, DeviceLock &lock,
    const HostX86Options &options)
{
    if (options.local_apic) {
        fprintf(stderr, "NVMM has no local APIC\n");
        return nullptr;
    }
    if (nvmm_init() == -1) {
        fprintf(stderr, "NVMM not available: %s\n", strerror(errno));
        return nullptr;
    }

    auto nvmm = std::make_unique<NvmmX86Hypervisor>(target, lock);
    if (!nvmm->Init())
        return nullptr;
    return nvmm;
}
