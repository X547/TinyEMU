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
#include <pthread.h>
#include <signal.h>

#include <algorithm>
#include <atomic>
#include <memory>
#include <vector>

#include <cpuid.h>

extern "C" {
#include <nvmm.h>
}

#include "bits.h"
#include "device_lock.h"
#include "host_memory.h"
#include "host_x86_hypervisor.h"


#define CPUID_APIC bit_at(9)
#define CPUID_ACPI bit_at(22)
#define CPUID_TOPOEXT bit_at(22) /* 0x80000001 ECX */
#define RFLAGS_IF bit_at(9)


static void nvmm_fail(const char *what)
{
    fprintf(stderr, "NVMM: %s failed: %s\n", what, strerror(errno));
    exit(1);
}

/* Its delivery is all a kick needs. */
static void sigalrm_handler(int sig)
{
    (void)sig;
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


class NvmmX86Hypervisor;
class NvmmX86Vcpu;

/* What the assist callbacks are handed, from which they find their way back
   to the vcpu. */
struct NvmmVcpu {
    struct nvmm_vcpu vcpu {};
    NvmmX86Vcpu *owner = nullptr;
};


/* One vcpu. The interrupt the machine's 8259s or local APIC has for it
   goes in as an event before a run. A signal to its thread ends a run on
   kernels that leave a run for a pending signal and send it to a thread on
   another processor at once; elsewhere the run goes on to its next exit.
   A signal that lands between the look at fKicked and the kernel entry is
   lost, and the run goes on to its next exit too. */
class NvmmX86Vcpu final: public HostX86Vcpu {
private:
    NvmmX86Hypervisor &fOwner;
    struct nvmm_machine *fMach;
    NvmmVcpu fVcpu;
    /* the thread that runs it, for signalling it out of a run */
    pthread_t fThread {};
    /* in or about to enter nvmm_vcpu_run() */
    std::atomic<bool> fInRun {false};
    /* asked to leave the run since the interrupts were last looked at */
    std::atomic<bool> fKicked {false};
    /* the state after vcpu creation, which an INIT goes back to */
    struct nvmm_x64_state fResetState;

    /* the interrupt state as of the last exit */
    uint64_t fRflags = 0x2;
    bool fInterruptShadow = false;
    bool fEventPending = false;
    bool fWindowRequested = false;
    /* CR8 as the vcpu and the machine's local APIC last agreed on it */
    int fTaskPriority = 0;
    /* stopped at HLT until an interrupt is taken */
    bool fHalted = false;
    /* shut down, until the machine ends */
    bool fShutdown = false;

    void SetResetFpuState();
    void SyncTaskPriority();
    bool InjectInterrupt();
    void InjectException(int vector);
    void ExitMsr(const struct nvmm_vcpu_exit *ctx);

    static void IoCallback(struct nvmm_io *io);
    static void MemCallback(struct nvmm_mem *mem);

public:
    NvmmX86Vcpu(NvmmX86Hypervisor &owner, struct nvmm_machine *mach,
                int index);
    ~NvmmX86Vcpu() override;

    void GetRegs(HostX86Regs *regs) override;
    void SetRegs(const HostX86Regs &regs) override;
    void SetFlatProtectedMode(uint32_t gdt_base, uint16_t gdt_limit,
                              uint16_t code_sel, uint16_t data_sel) override;
    void ThreadStarted() override;
    void Run() override;
    bool Idle(bool intr) override;
    void InterruptRun() override;
    void Init() override;
    void Startup(int vector) override;
};


/* The kernel has no interrupt controllers, so the machine keeps them all,
   the local APICs included. */
class NvmmX86Hypervisor final: public HostX86Hypervisor {
private:
    friend class NvmmX86Vcpu;

    X86HypervisorTarget &fTarget;
    DeviceLock &fLock;
    /* the machine has local APICs */
    bool fLocalApic;
    /* and the kernel can exit at a write to CR8 */
    bool fTprExits = false;
    int fCpuCount;
    int fPhysAddressBits;
    struct nvmm_machine fMach {};
    bool fMachCreated = false;
    std::vector<std::unique_ptr<NvmmX86Vcpu>> fVcpus;
    /* where each slot is mapped; the platform knows ranges, not slots */
    struct Slot {
        uint64_t addr;
        uint64_t size;
        uint8_t *host_mem;
    };
    std::vector<Slot> fSlots;

public:
    NvmmX86Hypervisor(X86HypervisorTarget &target, DeviceLock &lock,
                      const HostX86Options &options):
        fTarget(target), fLock(lock), fLocalApic(options.local_apic),
        fCpuCount(options.cpu_count),
        fPhysAddressBits(std::min(host_phys_address_bits(),
                                  options.max_phys_address_bits)) {}
    ~NvmmX86Hypervisor() override;

    bool Init();

    bool HasInterruptControllers() override {return false;}
    bool HasLocalApics() override {return false;}
    int PhysAddressBits() override {return fPhysAddressBits;}
    uint8_t *AllocRam(size_t size) override;
    void FreeRam(uint8_t *ptr, size_t size) override;
    void MapRam(int slot, uint64_t addr, uint64_t size, uint8_t *host_mem,
                bool read_only, bool log_dirty) override;
    void GetDirtyLog(int slot, uint32_t *bitmap) override;
    void SetIRQ(int irq, int level) override;
    void SendMsi(uint64_t addr, uint32_t data) override;
    HostX86Vcpu &Vcpu(int index) override {return *fVcpus[index];}
};


NvmmX86Hypervisor::~NvmmX86Hypervisor()
{
    fVcpus.clear();
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
    if (fCpuCount > (int)cap.max_vcpus) {
        fprintf(stderr, "NVMM runs no more than %u processors\n",
                cap.max_vcpus);
        return false;
    }
    fTprExits = fLocalApic &&
        (cap.arch.vcpu_conf_support & NVMM_CAP_ARCH_VCPU_CONF_TPR);

    if (nvmm_machine_create(&fMach) == -1) {
        fprintf(stderr, "NVMM: cannot create a machine: %s\n",
                strerror(errno));
        return false;
    }
    fMachCreated = true;

    for (int i = 0; i < fCpuCount; i++)
        fVcpus.push_back(std::make_unique<NvmmX86Vcpu>(*this, &fMach, i));

    struct sigaction act;
    memset(&act, 0, sizeof(act));
    act.sa_handler = sigalrm_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = SA_RESTART;
    sigaction(SIGALRM, &act, NULL);
    /* The signal is for the vcpu threads, which unblock it. Every thread
       started after this inherits the mask. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    return true;
}


/* The kernel gives CPUID the APIC ID, which is the vcpu's index, and the
   number of processors, which is the vcpus created so far: all of them by
   the time the guest runs. */
NvmmX86Vcpu::NvmmX86Vcpu(NvmmX86Hypervisor &owner, struct nvmm_machine *mach,
                         int index):
    fOwner(owner), fMach(mach)
{
    fVcpu.owner = this;
    if (nvmm_vcpu_create(fMach, index, &fVcpu.vcpu) == -1)
        nvmm_fail("nvmm_vcpu_create");

    struct nvmm_assist_callbacks callbacks = {IoCallback, MemCallback};
    if (nvmm_vcpu_configure(fMach, &fVcpu.vcpu, NVMM_VCPU_CONF_CALLBACKS,
                            &callbacks) == -1) {
        nvmm_fail("setting the assist callbacks");
    }

    SetResetFpuState();
    if (nvmm_vcpu_getstate(fMach, &fVcpu.vcpu, NVMM_X64_STATE_ALL) == -1)
        nvmm_fail("nvmm_vcpu_getstate");
    fResetState = *fVcpu.vcpu.state;

    /* remove the APIC, unless the machine has one, & ACPI to be in sync with
       the emulator */
    static const uint32_t leaves[] = {1, 0x80000001};
    for (uint32_t leaf: leaves) {
        struct nvmm_vcpu_conf_cpuid cpuid;
        memset(&cpuid, 0, sizeof(cpuid));
        cpuid.mask = 1;
        cpuid.leaf = leaf;
        cpuid.u.mask.del.edx = CPUID_ACPI;
        if (!owner.fLocalApic)
            cpuid.u.mask.del.edx |= CPUID_APIC;
        /* The kernel passes the topology leaves through from the host
           processor the vcpu happens to run on, APIC IDs included, so the
           guest is not told they exist. */
        if (leaf == 0x80000001)
            cpuid.u.mask.del.ecx = CPUID_TOPOEXT;
        if (nvmm_vcpu_configure(fMach, &fVcpu.vcpu, NVMM_VCPU_CONF_CPUID,
                                &cpuid) == -1) {
            nvmm_fail("masking CPUID");
        }
    }

    /* A write to CR8 exits, for the local APIC to know at once of a priority
       that lets an interrupt through. */
    if (owner.fTprExits) {
        struct nvmm_vcpu_conf_tpr tpr;
        memset(&tpr, 0, sizeof(tpr));
        tpr.exit_changed = 1;
        if (nvmm_vcpu_configure(fMach, &fVcpu.vcpu, NVMM_VCPU_CONF_TPR,
                                &tpr) == -1) {
            nvmm_fail("asking for CR8 exits");
        }
    }

    /* the physical address width, with no separate guest width; the kernel
       refuses a bit both set and deleted */
    struct nvmm_vcpu_conf_cpuid cpuid;
    memset(&cpuid, 0, sizeof(cpuid));
    cpuid.mask = 1;
    cpuid.leaf = 0x80000008;
    cpuid.u.mask.set.eax = owner.fPhysAddressBits;
    cpuid.u.mask.del.eax = 0x00ff00ff & ~cpuid.u.mask.set.eax;
    if (nvmm_vcpu_configure(fMach, &fVcpu.vcpu, NVMM_VCPU_CONF_CPUID,
                            &cpuid) == -1) {
        nvmm_fail("setting the physical address width");
    }
}


NvmmX86Vcpu::~NvmmX86Vcpu()
{
    nvmm_vcpu_destroy(fMach, &fVcpu.vcpu);
}


/* The x87 and SSE state after a processor reset. The vcpu starts with it
   already, but the driver masks the MXCSR it is given with the mask beside it,
   and the mask it resets to is zero, so the guest would start with every SSE
   exception unmasked. A full mask asks for the host's. */
void NvmmX86Vcpu::SetResetFpuState()
{
    struct nvmm_x64_state *state = fVcpu.vcpu.state;
    struct nvmm_x64_state_fpu &fpu = state->fpu;

    memset(&fpu, 0, sizeof(fpu));
    fpu.fx_cw = 0x0040;
    fpu.fx_tw = 0x55;
    fpu.fx_zero = 0x55;
    fpu.fx_mxcsr = 0x1f80;
    fpu.fx_mxcsr_mask = ~(uint32_t)0;

    if (nvmm_vcpu_setstate(fMach, &fVcpu.vcpu, NVMM_X64_STATE_FPU) == -1)
        nvmm_fail("nvmm_vcpu_setstate");
}


/* Mapping host memory into the machine puts a new object over the range, so
   that has to happen before anything is written to it. */
uint8_t *NvmmX86Hypervisor::AllocRam(size_t size)
{
    uint8_t *ptr = host_ram_alloc(size);
    if (ptr == nullptr)
        return nullptr;
    if (nvmm_hva_map(&fMach, (uintptr_t)ptr, size) == -1)
        nvmm_fail("nvmm_hva_map");
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
            nvmm_fail("nvmm_gpa_unmap");
        }
        s.size = 0;
    }
    if (size == 0)
        return;

    int prot = NVMM_PROT_READ | NVMM_PROT_EXEC;
    if (!read_only)
        prot |= NVMM_PROT_WRITE;
    if (nvmm_gpa_map(&fMach, (uintptr_t)host_mem, addr, size, prot) == -1)
        nvmm_fail("nvmm_gpa_map");
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
    /* the local APICs are the machine's, so no MSI comes here */
    abort();
}


void NvmmX86Vcpu::GetRegs(HostX86Regs *regs)
{
    struct nvmm_x64_state *state = fVcpu.vcpu.state;

    if (nvmm_vcpu_getstate(fMach, &fVcpu.vcpu, NVMM_X64_STATE_GPRS) == -1)
        nvmm_fail("nvmm_vcpu_getstate");
    for (int i = 0; i < 8; i++)
        regs->gpr[i] = state->gprs[NVMM_X64_GPR_RAX + i];
    regs->rip = state->gprs[NVMM_X64_GPR_RIP];
    regs->rflags = state->gprs[NVMM_X64_GPR_RFLAGS];
}


void NvmmX86Vcpu::SetRegs(const HostX86Regs &regs)
{
    struct nvmm_x64_state *state = fVcpu.vcpu.state;

    if (nvmm_vcpu_getstate(fMach, &fVcpu.vcpu, NVMM_X64_STATE_GPRS) == -1)
        nvmm_fail("nvmm_vcpu_getstate");
    for (int i = 0; i < 8; i++)
        state->gprs[NVMM_X64_GPR_RAX + i] = regs.gpr[i];
    state->gprs[NVMM_X64_GPR_RIP] = regs.rip;
    state->gprs[NVMM_X64_GPR_RFLAGS] = regs.rflags;
    if (nvmm_vcpu_setstate(fMach, &fVcpu.vcpu, NVMM_X64_STATE_GPRS) == -1)
        nvmm_fail("nvmm_vcpu_setstate");
    fRflags = regs.rflags;
}


void NvmmX86Vcpu::SetFlatProtectedMode(uint32_t gdt_base,
                                             uint16_t gdt_limit,
                                             uint16_t code_sel,
                                             uint16_t data_sel)
{
    struct nvmm_x64_state *state = fVcpu.vcpu.state;
    static const int data_segs[] = {
        NVMM_X64_SEG_DS, NVMM_X64_SEG_ES, NVMM_X64_SEG_SS,
        NVMM_X64_SEG_FS, NVMM_X64_SEG_GS,
    };

    if (nvmm_vcpu_getstate(fMach, &fVcpu.vcpu,
                           NVMM_X64_STATE_SEGS | NVMM_X64_STATE_CRS) == -1) {
        nvmm_fail("nvmm_vcpu_getstate");
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

    if (nvmm_vcpu_setstate(fMach, &fVcpu.vcpu,
                           NVMM_X64_STATE_SEGS | NVMM_X64_STATE_CRS) == -1) {
        nvmm_fail("nvmm_vcpu_setstate");
    }
}


void NvmmX86Vcpu::IoCallback(struct nvmm_io *io)
{
    NvmmVcpu *v = reinterpret_cast<NvmmVcpu *>(io->vcpu);
    X86HypervisorTarget &target = v->owner->fOwner.fTarget;
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


void NvmmX86Vcpu::MemCallback(struct nvmm_mem *mem)
{
    NvmmVcpu *v = reinterpret_cast<NvmmVcpu *>(mem->vcpu);
    X86HypervisorTarget &target = v->owner->fOwner.fTarget;

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


/* With the lock held, before a run: CR8 takes a task priority the guest
   wrote to the local APIC's page. */
void NvmmX86Vcpu::SyncTaskPriority()
{
    struct nvmm_vcpu *vcpu = &fVcpu.vcpu;
    int tpr = fOwner.fTarget.TaskPriority();

    if (tpr == fTaskPriority)
        return;
    if (nvmm_vcpu_getstate(fMach, vcpu, NVMM_X64_STATE_CRS) == -1)
        nvmm_fail("nvmm_vcpu_getstate");
    vcpu->state->crs[NVMM_X64_CR_CR8] = tpr;
    if (nvmm_vcpu_setstate(fMach, vcpu, NVMM_X64_STATE_CRS) == -1)
        nvmm_fail("nvmm_vcpu_setstate");
    fTaskPriority = tpr;
}


/* With the lock held, before a run: hands the processor the interrupt the
   8259s or its local APIC raise when it can take one, and otherwise asks to
   exit once it can. False while it stays halted. */
bool NvmmX86Vcpu::InjectInterrupt()
{
    struct nvmm_vcpu *vcpu = &fVcpu.vcpu;
    X86HypervisorTarget &target = fOwner.fTarget;
    bool intr = target.InterruptRequested();

    if (intr && !fEventPending && !fInterruptShadow &&
        (fRflags & RFLAGS_IF)) {
        vcpu->event->type = NVMM_VCPU_EVENT_INTR;
        vcpu->event->vector = target.AcknowledgeInterrupt();
        if (nvmm_vcpu_inject(fMach, vcpu) == -1)
            nvmm_fail("nvmm_vcpu_inject");
        fEventPending = true;
        fHalted = false;
        intr = target.InterruptRequested();
    }
    if (fHalted)
        return false;
    if (intr && !fWindowRequested) {
        if (nvmm_vcpu_getstate(fMach, vcpu, NVMM_X64_STATE_INTR) == -1)
            nvmm_fail("nvmm_vcpu_getstate");
        vcpu->state->intr.int_window_exiting = 1;
        if (nvmm_vcpu_setstate(fMach, vcpu, NVMM_X64_STATE_INTR) == -1)
            nvmm_fail("nvmm_vcpu_setstate");
        fWindowRequested = true;
    }
    return true;
}


void NvmmX86Vcpu::InjectException(int vector)
{
    struct nvmm_vcpu *vcpu = &fVcpu.vcpu;

    vcpu->event->type = NVMM_VCPU_EVENT_EXCP;
    vcpu->event->vector = vector;
    vcpu->event->u.excp.error = 0;
    if (nvmm_vcpu_inject(fMach, vcpu) == -1)
        nvmm_fail("nvmm_vcpu_inject");
}


#define MSR_IA32_APIC_BASE 0x1b

/* IA32_APIC_BASE goes to the machine's local APIC. Another MSR the kernel
   does not handle reads as 0 and ignores writes, rather than faulting a
   guest that probes it. */
void NvmmX86Vcpu::ExitMsr(const struct nvmm_vcpu_exit *ctx)
{
    struct nvmm_x64_state *state = fVcpu.vcpu.state;
    X86HypervisorTarget &target = fOwner.fTarget;
    bool apic = fOwner.fLocalApic;

    if (nvmm_vcpu_getstate(fMach, &fVcpu.vcpu, NVMM_X64_STATE_GPRS) == -1)
        nvmm_fail("nvmm_vcpu_getstate");
    if (ctx->reason == NVMM_VCPU_EXIT_RDMSR) {
        uint64_t val = 0;
        if (apic && ctx->u.rdmsr.msr == MSR_IA32_APIC_BASE) {
            DeviceLocker locker(fOwner.fLock);
            val = target.ApicBase();
        }
        state->gprs[NVMM_X64_GPR_RAX] = (uint32_t)val;
        state->gprs[NVMM_X64_GPR_RDX] = val >> 32;
        state->gprs[NVMM_X64_GPR_RIP] = ctx->u.rdmsr.npc;
    } else {
        if (apic && ctx->u.wrmsr.msr == MSR_IA32_APIC_BASE) {
            DeviceLocker locker(fOwner.fLock);
            if (!target.SetApicBase(ctx->u.wrmsr.val)) {
                InjectException(13); /* #GP */
                return;
            }
        }
        state->gprs[NVMM_X64_GPR_RIP] = ctx->u.wrmsr.npc;
    }
    if (nvmm_vcpu_setstate(fMach, &fVcpu.vcpu, NVMM_X64_STATE_GPRS) == -1)
        nvmm_fail("nvmm_vcpu_setstate");
}


/* The state the vcpu was created with, less the time stamp counter, which
   an INIT leaves running. */
void NvmmX86Vcpu::Init()
{
    struct nvmm_vcpu *vcpu = &fVcpu.vcpu;

    if (nvmm_vcpu_getstate(fMach, vcpu, NVMM_X64_STATE_MSRS) == -1)
        nvmm_fail("nvmm_vcpu_getstate");
    uint64_t tsc = vcpu->state->msrs[NVMM_X64_MSR_TSC];
    *vcpu->state = fResetState;
    vcpu->state->msrs[NVMM_X64_MSR_TSC] = tsc;
    if (nvmm_vcpu_setstate(fMach, vcpu, NVMM_X64_STATE_ALL) == -1)
        nvmm_fail("nvmm_vcpu_setstate");

    fRflags = 0x2;
    fInterruptShadow = false;
    fEventPending = false;
    fWindowRequested = false;
    fTaskPriority = 0;
    fHalted = false;
}


void NvmmX86Vcpu::Startup(int vector)
{
    struct nvmm_vcpu *vcpu = &fVcpu.vcpu;
    struct nvmm_x64_state *state = vcpu->state;

    if (nvmm_vcpu_getstate(fMach, vcpu,
                           NVMM_X64_STATE_SEGS | NVMM_X64_STATE_GPRS) == -1) {
        nvmm_fail("nvmm_vcpu_getstate");
    }
    state->segs[NVMM_X64_SEG_CS].selector = vector << 8;
    state->segs[NVMM_X64_SEG_CS].base = (uint64_t)vector << 12;
    state->gprs[NVMM_X64_GPR_RIP] = 0;
    if (nvmm_vcpu_setstate(fMach, vcpu,
                           NVMM_X64_STATE_SEGS | NVMM_X64_STATE_GPRS) == -1) {
        nvmm_fail("nvmm_vcpu_setstate");
    }
}


void NvmmX86Vcpu::ThreadStarted()
{
    sigset_t set;

    fThread = pthread_self();
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}


/* A kick sets fKicked before it looks at fInRun, and a run sets fInRun
   before it looks at fKicked, so either the run does not start or the kick
   signals it. */
void NvmmX86Vcpu::InterruptRun()
{
    fKicked.store(true);
    if (fInRun.load())
        pthread_kill(fThread, SIGALRM);
}


void NvmmX86Vcpu::Run()
{
    struct nvmm_vcpu *vcpu = &fVcpu.vcpu;
    const struct nvmm_vcpu_exit *ctx = vcpu->exit;
    DeviceLock &lock = fOwner.fLock;

    fKicked.store(false);
    {
        DeviceLocker locker(lock);
        if (fOwner.fLocalApic)
            SyncTaskPriority();
        if (!InjectInterrupt())
            return;
    }

    fInRun.store(true);
    if (fKicked.load()) {
        fInRun.store(false);
        return;
    }
    int ret = nvmm_vcpu_run(fMach, vcpu);
    fInRun.store(false);
    if (ret == -1) {
        if (errno == EINTR || errno == EAGAIN)
            return;
        nvmm_fail("nvmm_vcpu_run");
    }

    fRflags = ctx->exitstate.rflags;
    fInterruptShadow = ctx->exitstate.int_shadow;
    fEventPending = ctx->exitstate.evt_pending;
    fWindowRequested = ctx->exitstate.int_window_exiting;
    /* the local APIC learns of a write to CR8 before anything else sees
       it */
    if (fOwner.fLocalApic && (int)ctx->exitstate.cr8 != fTaskPriority) {
        fTaskPriority = ctx->exitstate.cr8;
        DeviceLocker locker(lock);
        fOwner.fTarget.SetTaskPriority(fTaskPriority);
    }

    switch (ctx->reason) {
    case NVMM_VCPU_EXIT_NONE:
    case NVMM_VCPU_EXIT_INT_READY:
    case NVMM_VCPU_EXIT_TPR_CHANGED:
        break;
    case NVMM_VCPU_EXIT_MEMORY: {
        DeviceLocker locker(lock);
        if (nvmm_assist_mem(fMach, vcpu) == -1) {
            fprintf(stderr, "NVMM: cannot emulate the access to 0x%" PRIx64
                    ": %s\n", (uint64_t)ctx->u.mem.gpa, strerror(errno));
            exit(1);
        }
        break;
    }
    case NVMM_VCPU_EXIT_IO: {
        DeviceLocker locker(lock);
        if (nvmm_assist_io(fMach, vcpu) == -1) {
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
    case NVMM_VCPU_EXIT_SHUTDOWN: {
        DeviceLocker locker(lock);
        fprintf(stderr, "NVMM: the guest shut down (triple fault)\n");
        fShutdown = true;
        fOwner.fTarget.ProcessorShutdown();
        break;
    }
    default:
        fprintf(stderr, "NVMM: unsupported exit reason 0x%" PRIx64 "\n",
                (uint64_t)ctx->reason);
        exit(1);
    }
}


bool NvmmX86Vcpu::Idle(bool intr)
{
    return fShutdown || (fHalted && !(intr && (fRflags & RFLAGS_IF)));
}


std::unique_ptr<HostX86Hypervisor> host_x86_hypervisor_open(
    X86HypervisorTarget &target, DeviceLock &lock,
    const HostX86Options &options)
{
    if (nvmm_init() == -1) {
        fprintf(stderr, "NVMM not available: %s\n", strerror(errno));
        return nullptr;
    }

    auto nvmm = std::make_unique<NvmmX86Hypervisor>(target, lock, options);
    if (!nvmm->Init())
        return nullptr;
    return nvmm;
}
