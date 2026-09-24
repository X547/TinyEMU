/*
 * Host hypervisor running the x86 processor: KVM
 *
 * Copyright (c) 2011-2017 Fabrice Bellard
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
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/kvm.h>

#include <atomic>
#include <memory>
#include <vector>

#include "bits.h"
#include "device_lock.h"
#include "host_memory.h"
#include "host_x86_hypervisor.h"


#define CPUID_APIC bit_at(9)
#define CPUID_ACPI bit_at(22)


class KvmX86Hypervisor;

/* One vcpu. The in-kernel local APIC waits at HLT, so a run lasts until an
   access for the target or InterruptRun(). */
class KvmX86Vcpu final: public HostX86Vcpu {
private:
    KvmX86Hypervisor &fOwner;
    int fFd = -1;
    int fRunSize = 0;
    struct kvm_run *fRun = nullptr;
    /* the thread that runs it, for signalling it out of KVM_RUN */
    pthread_t fThread {};
    std::atomic<bool> fThreadKnown {false};
    /* shut down, until the machine ends */
    bool fShutdown = false;

    void SetCpuid(int index);
    void ExitIo();
    void ExitMmio();

public:
    KvmX86Vcpu(KvmX86Hypervisor &owner, int index);
    ~KvmX86Vcpu() override;

    void GetRegs(HostX86Regs *regs) override;
    void SetRegs(const HostX86Regs &regs) override;
    void SetFlatProtectedMode(uint32_t gdt_base, uint16_t gdt_limit,
                              uint16_t code_sel, uint16_t data_sel) override;
    void ThreadStarted() override;
    void Run() override;
    bool Idle(bool intr) override {return fShutdown;}
    void InterruptRun() override;
};


/* The VM keeps the 8259s, the 8254, the IOAPIC and the local APICs in the
   kernel. */
class KvmX86Hypervisor final: public HostX86Hypervisor {
private:
    friend class KvmX86Vcpu;

    X86HypervisorTarget &fTarget;
    DeviceLock &fLock;
    /* the in-kernel local APIC is shown to the guest */
    bool fLocalApic;
    int fCpuCount;
    int fKvmFd;
    int fVmFd = -1;
    std::vector<std::unique_ptr<KvmX86Vcpu>> fVcpus;

    void SetGsiRouting();

public:
    KvmX86Hypervisor(X86HypervisorTarget &target, DeviceLock &lock,
                     const HostX86Options &options, int kvm_fd):
        fTarget(target), fLock(lock), fLocalApic(options.local_apic),
        fCpuCount(options.cpu_count), fKvmFd(kvm_fd) {}
    ~KvmX86Hypervisor() override;

    void Init();

    bool HasInterruptControllers() override {return true;}
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


static void sigalrm_handler(int sig)
{
}


KvmX86Hypervisor::~KvmX86Hypervisor()
{
    fVcpus.clear();
    if (fVmFd >= 0)
        close(fVmFd);
    close(fKvmFd);
}


/* The IOAPIC wired as a PC has it, and as the machine's MADT describes it:
   the 8254's GSI 0 arrives on input 2, and GSI 2 goes nowhere. */
void KvmX86Hypervisor::SetGsiRouting()
{
    struct kvm_irq_routing *routing;
    struct kvm_irq_routing_entry *e;
    int n = 0;

    routing = static_cast<struct kvm_irq_routing *>(
        calloc(1, sizeof(*routing) + 40 * sizeof(routing->entries[0])));
    for (int gsi = 0; gsi < 24; gsi++) {
        if (gsi == 2)
            continue;
        if (gsi < 16) {
            e = &routing->entries[n++];
            e->gsi = gsi;
            e->type = KVM_IRQ_ROUTING_IRQCHIP;
            e->u.irqchip.irqchip = gsi < 8 ? KVM_IRQCHIP_PIC_MASTER :
                KVM_IRQCHIP_PIC_SLAVE;
            e->u.irqchip.pin = gsi & 7;
        }
        e = &routing->entries[n++];
        e->gsi = gsi;
        e->type = KVM_IRQ_ROUTING_IRQCHIP;
        e->u.irqchip.irqchip = KVM_IRQCHIP_IOAPIC;
        e->u.irqchip.pin = gsi == 0 ? 2 : gsi;
    }
    routing->nr = n;
    if (ioctl(fVmFd, KVM_SET_GSI_ROUTING, routing) < 0) {
        perror("KVM_SET_GSI_ROUTING");
        exit(1);
    }
    free(routing);
}


void KvmX86Hypervisor::Init()
{
    struct sigaction act;
    struct kvm_pit_config pit_config;
    uint64_t base_addr;

    fVmFd = ioctl(fKvmFd, KVM_CREATE_VM, 0);
    if (fVmFd < 0) {
        perror("KVM_CREATE_VM");
        exit(1);
    }

    /* just before the BIOS */
    base_addr = 0xfffbc000;
    if (ioctl(fVmFd, KVM_SET_IDENTITY_MAP_ADDR, &base_addr) < 0) {
        perror("KVM_SET_IDENTITY_MAP_ADDR");
        exit(1);
    }

    if (ioctl(fVmFd, KVM_SET_TSS_ADDR, (long)(base_addr + 0x1000)) < 0) {
        perror("KVM_SET_TSS_ADDR");
        exit(1);
    }

    if (ioctl(fVmFd, KVM_CREATE_IRQCHIP, 0) < 0) {
        perror("KVM_CREATE_IRQCHIP");
        exit(1);
    }
    if (fLocalApic)
        SetGsiRouting();

    memset(&pit_config, 0, sizeof(pit_config));
    pit_config.flags = KVM_PIT_SPEAKER_DUMMY;
    if (ioctl(fVmFd, KVM_CREATE_PIT2, &pit_config)) {
        perror("KVM_CREATE_PIT2");
        exit(1);
    }

    for (int i = 0; i < fCpuCount; i++)
        fVcpus.push_back(std::make_unique<KvmX86Vcpu>(*this, i));

    act.sa_handler = sigalrm_handler;
    sigemptyset(&act.sa_mask);
    act.sa_flags = 0;
    sigaction(SIGALRM, &act, NULL);
    /* The signal is for the vcpu threads, which unblock it. Every thread
       started after this inherits the mask. */
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    pthread_sigmask(SIG_BLOCK, &set, NULL);
}


void KvmX86Hypervisor::MapRam(int slot, uint64_t addr, uint64_t size,
                              uint8_t *host_mem, bool read_only,
                              bool log_dirty)
{
    struct kvm_userspace_memory_region region;

    region.slot = slot;
    region.flags = 0;
    if (read_only)
        region.flags |= KVM_MEM_READONLY;
    if (log_dirty)
        region.flags |= KVM_MEM_LOG_DIRTY_PAGES;
    region.guest_phys_addr = addr;
    region.memory_size = size;
    region.userspace_addr = (uintptr_t)host_mem;
    if (ioctl(fVmFd, KVM_SET_USER_MEMORY_REGION, &region) < 0) {
        perror("KVM_SET_USER_MEMORY_REGION");
        exit(1);
    }
}


void KvmX86Hypervisor::GetDirtyLog(int slot, uint32_t *bitmap)
{
    struct kvm_dirty_log dlog;

    memset(&dlog, 0, sizeof(dlog));
    dlog.slot = slot;
    dlog.dirty_bitmap = bitmap;
    if (ioctl(fVmFd, KVM_GET_DIRTY_LOG, &dlog) < 0) {
        perror("KVM_GET_DIRTY_LOG");
        exit(1);
    }
}


void KvmX86Hypervisor::SetIRQ(int irq, int level)
{
    struct kvm_irq_level irq_level;

    irq_level.irq = irq;
    irq_level.level = level;
    if (ioctl(fVmFd, KVM_IRQ_LINE, &irq_level) < 0) {
        perror("KVM_IRQ_LINE");
        exit(1);
    }
}


void KvmX86Hypervisor::SendMsi(uint64_t addr, uint32_t data)
{
    struct kvm_msi msi;

    memset(&msi, 0, sizeof(msi));
    msi.address_lo = (uint32_t)addr;
    msi.address_hi = (uint32_t)(addr >> 32);
    msi.data = data;
    if (ioctl(fVmFd, KVM_SIGNAL_MSI, &msi) < 0) {
        perror("KVM_SIGNAL_MSI");
        exit(1);
    }
}


KvmX86Vcpu::KvmX86Vcpu(KvmX86Hypervisor &owner, int index):
    fOwner(owner)
{
    fFd = ioctl(fOwner.fVmFd, KVM_CREATE_VCPU, index);
    if (fFd < 0) {
        perror("KVM_CREATE_VCPU");
        exit(1);
    }

    SetCpuid(index);

    /* map the kvm_run structure */
    fRunSize = ioctl(fOwner.fKvmFd, KVM_GET_VCPU_MMAP_SIZE, NULL);
    if (fRunSize < 0) {
        perror("KVM_GET_VCPU_MMAP_SIZE");
        exit(1);
    }

    void *run = mmap(NULL, fRunSize, PROT_READ | PROT_WRITE, MAP_SHARED,
                     fFd, 0);
    if (run == MAP_FAILED) {
        perror("mmap kvm_run");
        exit(1);
    }
    fRun = static_cast<struct kvm_run *>(run);
}


KvmX86Vcpu::~KvmX86Vcpu()
{
    if (fRun != nullptr)
        munmap(fRun, fRunSize);
    if (fFd >= 0)
        close(fFd);
}


void KvmX86Vcpu::SetCpuid(int index)
{
    struct kvm_cpuid2 *kvm_cpuid;
    int n_ent_max, i;
    struct kvm_cpuid_entry2 *ent;

    n_ent_max = 128;
    kvm_cpuid = static_cast<struct kvm_cpuid2 *>(
        calloc(1, sizeof(struct kvm_cpuid2) +
               n_ent_max * sizeof(kvm_cpuid->entries[0])));

    kvm_cpuid->nent = n_ent_max;
    if (ioctl(fOwner.fKvmFd, KVM_GET_SUPPORTED_CPUID, kvm_cpuid) < 0) {
        perror("KVM_GET_SUPPORTED_CPUID");
        exit(1);
    }

    for(i = 0; i < kvm_cpuid->nent; i++) {
        ent = &kvm_cpuid->entries[i];
        /* remove the APIC (unless the machine has one) & ACPI to be in
           sync with the emulator */
        if (ent->function == 1 || ent->function == 0x80000001) {
            ent->edx &= ~CPUID_ACPI;
            if (!fOwner.fLocalApic)
                ent->edx &= ~CPUID_APIC;
        }
        /* The table is the host's; the APIC ID it reports is the vcpu's,
           which is its index. */
        switch (ent->function) {
        case 1:
            ent->ebx = set_bits(ent->ebx, 24, 8, (uint32_t)index);
            break;
        case 0xb:
        case 0x1f:
            ent->edx = index;
            break;
        case 0x8000001e:
            ent->eax = index;
            break;
        }
    }

    if (ioctl(fFd, KVM_SET_CPUID2, kvm_cpuid) < 0) {
        perror("KVM_SET_CPUID2");
        exit(1);
    }
    free(kvm_cpuid);
}


void KvmX86Vcpu::GetRegs(HostX86Regs *regs)
{
    struct kvm_regs r;

    if (ioctl(fFd, KVM_GET_REGS, &r) < 0) {
        perror("KVM_GET_REGS");
        exit(1);
    }
    regs->gpr[0] = r.rax;
    regs->gpr[1] = r.rcx;
    regs->gpr[2] = r.rdx;
    regs->gpr[3] = r.rbx;
    regs->gpr[4] = r.rsp;
    regs->gpr[5] = r.rbp;
    regs->gpr[6] = r.rsi;
    regs->gpr[7] = r.rdi;
    regs->rip = r.rip;
    regs->rflags = r.rflags;
}


void KvmX86Vcpu::SetRegs(const HostX86Regs &regs)
{
    struct kvm_regs r;

    if (ioctl(fFd, KVM_GET_REGS, &r) < 0) {
        perror("KVM_GET_REGS");
        exit(1);
    }
    r.rax = regs.gpr[0];
    r.rcx = regs.gpr[1];
    r.rdx = regs.gpr[2];
    r.rbx = regs.gpr[3];
    r.rsp = regs.gpr[4];
    r.rbp = regs.gpr[5];
    r.rsi = regs.gpr[6];
    r.rdi = regs.gpr[7];
    r.rip = regs.rip;
    r.rflags = regs.rflags;
    if (ioctl(fFd, KVM_SET_REGS, &r) < 0) {
        perror("KVM_SET_REGS");
        exit(1);
    }
}


void KvmX86Vcpu::SetFlatProtectedMode(uint32_t gdt_base, uint16_t gdt_limit,
                                      uint16_t code_sel, uint16_t data_sel)
{
    struct kvm_sregs sregs;
    struct kvm_segment seg;

    if (ioctl(fFd, KVM_GET_SREGS, &sregs) < 0) {
        perror("KVM_GET_SREGS");
        exit(1);
    }

    sregs.cr0 |= (1 << 0); /* CR0_PE */
    sregs.gdt.base = gdt_base;
    sregs.gdt.limit = gdt_limit;

    memset(&seg, 0, sizeof(seg));
    seg.limit = 0xffffffff;
    seg.present = 1;
    seg.db = 1;
    seg.s = 1; /* code/data */
    seg.g = 1; /* 4KB granularity */

    seg.type = 0xb; /* code */
    seg.selector = code_sel;
    sregs.cs = seg;

    seg.type = 0x3; /* data */
    seg.selector = data_sel;
    sregs.ds = seg;
    sregs.es = seg;
    sregs.ss = seg;
    sregs.fs = seg;
    sregs.gs = seg;

    if (ioctl(fFd, KVM_SET_SREGS, &sregs) < 0) {
        perror("KVM_SET_SREGS");
        exit(1);
    }
}


void KvmX86Vcpu::ThreadStarted()
{
    sigset_t set;

    fThread = pthread_self();
    fThreadKnown.store(true);
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}


void KvmX86Vcpu::ExitIo()
{
    X86HypervisorTarget &target = fOwner.fTarget;
    struct kvm_run *run = fRun;
    uint8_t *ptr;
    int i;

    ptr = (uint8_t *)run + run->io.data_offset;

    for(i = 0; i < run->io.count; i++) {
        if (run->io.direction == KVM_EXIT_IO_OUT) {
            switch(run->io.size) {
            case 1:
                target.PortWrite(run->io.port, *(uint8_t *)ptr, 0);
                break;
            case 2:
                target.PortWrite(run->io.port, *(uint16_t *)ptr, 1);
                break;
            case 4:
                target.PortWrite(run->io.port, *(uint32_t *)ptr, 2);
                break;
            default:
                abort();
            }
        } else {
            switch(run->io.size) {
            case 1:
                *(uint8_t *)ptr = target.PortRead(run->io.port, 0);
                break;
            case 2:
                *(uint16_t *)ptr = target.PortRead(run->io.port, 1);
                break;
            case 4:
                *(uint32_t *)ptr = target.PortRead(run->io.port, 2);
                break;
            default:
                abort();
            }
        }
        ptr += run->io.size;
    }
}


void KvmX86Vcpu::ExitMmio()
{
    X86HypervisorTarget &target = fOwner.fTarget;
    struct kvm_run *run = fRun;

    if (run->mmio.is_write) {
        target.MmioWrite(run->mmio.phys_addr, run->mmio.data, run->mmio.len);
    } else {
        target.MmioRead(run->mmio.phys_addr, run->mmio.data, run->mmio.len);
    }
}


void KvmX86Vcpu::Run()
{
    struct kvm_run *run = fRun;
    int ret;

    ret = ioctl(fFd, KVM_RUN, 0);
    /* a request to return that came in while running has done its job */
    run->immediate_exit = 0;
    if (ret < 0) {
        if (errno == EINTR || errno == EAGAIN)
            return;
        perror("KVM_RUN");
        exit(1);
    }
    switch(run->exit_reason) {
    case KVM_EXIT_HLT:
        break;
    case KVM_EXIT_IO: {
        DeviceLocker locker(fOwner.fLock);
        ExitIo();
        break;
    }
    case KVM_EXIT_MMIO: {
        DeviceLocker locker(fOwner.fLock);
        ExitMmio();
        break;
    }
    case KVM_EXIT_SHUTDOWN: {
        DeviceLocker locker(fOwner.fLock);
        fShutdown = true;
        fOwner.fTarget.ProcessorShutdown();
        break;
    }
    case KVM_EXIT_FAIL_ENTRY:
        fprintf(stderr, "KVM_EXIT_FAIL_ENTRY: reason=0x%" PRIx64 "\n",
                (uint64_t)run->fail_entry.hardware_entry_failure_reason);
        exit(1);
    case KVM_EXIT_INTERNAL_ERROR:
        fprintf(stderr, "KVM_EXIT_INTERNAL_ERROR: suberror=0x%x\n",
                (uint32_t)run->internal.suberror);
        exit(1);
    default:
        fprintf(stderr, "KVM: unsupported exit_reason=%d\n", run->exit_reason);
        exit(1);
    }
}


void KvmX86Vcpu::InterruptRun()
{
    /* immediate_exit covers a signal that lands before KVM_RUN */
    fRun->immediate_exit = 1;
    if (fThreadKnown.load())
        pthread_kill(fThread, SIGALRM);
}


std::unique_ptr<HostX86Hypervisor> host_x86_hypervisor_open(
    X86HypervisorTarget &target, DeviceLock &lock,
    const HostX86Options &options)
{
    int kvm_fd, ret;

    kvm_fd = open("/dev/kvm", O_RDWR);
    if (kvm_fd < 0) {
        fprintf(stderr, "KVM not available\n");
        return nullptr;
    }
    ret = ioctl(kvm_fd, KVM_GET_API_VERSION, 0);
    if (ret < 0) {
        perror("KVM_GET_API_VERSION");
        exit(1);
    }
    if (ret != 12) {
        fprintf(stderr, "Unsupported KVM version\n");
        close(kvm_fd);
        return nullptr;
    }

    auto kvm = std::make_unique<KvmX86Hypervisor>(target, lock, options,
                                                  kvm_fd);
    kvm->Init();
    return kvm;
}
