/*
 * The RISC-V harts on Linux KVM
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
#include <stddef.h>
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
#include "host_riscv_hypervisor.h"
#include "hypervisor_memory_map.h"
#include "iomem.h"
#include "riscv_sbi_defs.h"


#define KVM_REG_U64(type, index) \
    (KVM_REG_RISCV | KVM_REG_SIZE_U64 | (type) | (index))
#define KVM_CONFIG_REG(name) \
    KVM_REG_U64(KVM_REG_RISCV_CONFIG, KVM_REG_RISCV_CONFIG_REG(name))
#define KVM_CORE_REG(name) \
    KVM_REG_U64(KVM_REG_RISCV_CORE, KVM_REG_RISCV_CORE_REG(name))
#define KVM_TIMER_REG(name) \
    KVM_REG_U64(KVM_REG_RISCV_TIMER, KVM_REG_RISCV_TIMER_REG(name))
#define KVM_ISA_EXT_REG(id) KVM_REG_U64(KVM_REG_RISCV_ISA_EXT, (id))
#define KVM_SBI_EXT_REG(id) KVM_REG_U64(KVM_REG_RISCV_SBI_EXT, (id))

#define MISA_S bit_at('S' - 'A')
#define MISA_U bit_at('U' - 'A')


/* The multi-letter extensions by their KVM numbers, as the device tree
   names them; the single letter ones are in misa. */
static const struct {
    int id;
    const char *name;
} kvm_isa_exts[] = {
    {KVM_RISCV_ISA_EXT_ZICSR, "zicsr"},
    {KVM_RISCV_ISA_EXT_ZIFENCEI, "zifencei"},
    {KVM_RISCV_ISA_EXT_ZICNTR, "zicntr"},
    {KVM_RISCV_ISA_EXT_ZIHPM, "zihpm"},
    {KVM_RISCV_ISA_EXT_ZICBOM, "zicbom"},
    {KVM_RISCV_ISA_EXT_ZICBOZ, "zicboz"},
    {KVM_RISCV_ISA_EXT_ZICOND, "zicond"},
    {KVM_RISCV_ISA_EXT_ZIHINTNTL, "zihintntl"},
    {KVM_RISCV_ISA_EXT_ZIHINTPAUSE, "zihintpause"},
    {KVM_RISCV_ISA_EXT_ZFA, "zfa"},
    {KVM_RISCV_ISA_EXT_ZFH, "zfh"},
    {KVM_RISCV_ISA_EXT_ZFHMIN, "zfhmin"},
    {KVM_RISCV_ISA_EXT_ZBA, "zba"},
    {KVM_RISCV_ISA_EXT_ZBB, "zbb"},
    {KVM_RISCV_ISA_EXT_ZBC, "zbc"},
    {KVM_RISCV_ISA_EXT_ZBKB, "zbkb"},
    {KVM_RISCV_ISA_EXT_ZBKC, "zbkc"},
    {KVM_RISCV_ISA_EXT_ZBKX, "zbkx"},
    {KVM_RISCV_ISA_EXT_ZBS, "zbs"},
    {KVM_RISCV_ISA_EXT_ZKND, "zknd"},
    {KVM_RISCV_ISA_EXT_ZKNE, "zkne"},
    {KVM_RISCV_ISA_EXT_ZKNH, "zknh"},
    {KVM_RISCV_ISA_EXT_ZKR, "zkr"},
    {KVM_RISCV_ISA_EXT_ZKSED, "zksed"},
    {KVM_RISCV_ISA_EXT_ZKSH, "zksh"},
    {KVM_RISCV_ISA_EXT_ZKT, "zkt"},
    {KVM_RISCV_ISA_EXT_ZVBB, "zvbb"},
    {KVM_RISCV_ISA_EXT_ZVBC, "zvbc"},
    {KVM_RISCV_ISA_EXT_ZVFH, "zvfh"},
    {KVM_RISCV_ISA_EXT_ZVFHMIN, "zvfhmin"},
    {KVM_RISCV_ISA_EXT_ZVKB, "zvkb"},
    {KVM_RISCV_ISA_EXT_ZVKG, "zvkg"},
    {KVM_RISCV_ISA_EXT_ZVKNED, "zvkned"},
    {KVM_RISCV_ISA_EXT_ZVKNHA, "zvknha"},
    {KVM_RISCV_ISA_EXT_ZVKNHB, "zvknhb"},
    {KVM_RISCV_ISA_EXT_ZVKSED, "zvksed"},
    {KVM_RISCV_ISA_EXT_ZVKSH, "zvksh"},
    {KVM_RISCV_ISA_EXT_ZVKT, "zvkt"},
    {KVM_RISCV_ISA_EXT_SMSTATEEN, "smstateen"},
    {KVM_RISCV_ISA_EXT_SSAIA, "ssaia"},
    {KVM_RISCV_ISA_EXT_SSTC, "sstc"},
    {KVM_RISCV_ISA_EXT_SVINVAL, "svinval"},
    {KVM_RISCV_ISA_EXT_SVNAPOT, "svnapot"},
    {KVM_RISCV_ISA_EXT_SVPBMT, "svpbmt"},
};


class KvmRISCVHypervisor;

/* One vcpu. The kernel answers most of the SBI and waits at WFI itself, so
   a run lasts until an access for the machine or InterruptRun(). */
class KvmRISCVVcpu final: public RISCVVcpu {
private:
    KvmRISCVHypervisor &fOwner;
    int fIndex;
    int fFd = -1;
    int fRunSize = 0;
    struct kvm_run *fRun = nullptr;
    /* the thread that runs it, for signalling it out of KVM_RUN */
    pthread_t fThread {};
    std::atomic<bool> fThreadKnown {false};
    /* the guest asked for a reset or a shutdown, after which it runs no
       more */
    bool fEnded = false;

    void ExitMmio();
    void ExitSbi();
    void ExitSystemEvent();

public:
    KvmRISCVVcpu(KvmRISCVHypervisor &owner, int index);
    ~KvmRISCVVcpu() override;

    bool GetReg(uint64_t id, uint64_t *val);
    void SetReg(uint64_t id, uint64_t val);
    void SetMpState(uint32_t state);

    /* RISCVVcpu */
    void ThreadStarted() override;
    void Run() override;
    bool Idle() override {return fEnded;}
    void InterruptRun() override;
    void SetIrqLine(uint32_t mask, bool level) override;
    /* there is no IMSIC */
    void ImsicSetPending(bool supervisor, uint32_t id) override {abort();}
    uint64_t TimerDeadline() override {return UINT64_MAX;}
    void Start(uint64_t entry, uint64_t opaque) override;
};


/* The VM. The kernel has no interrupt controller for it, so the machine
   drives each hart's supervisor external interrupt. */
class KvmRISCVHypervisor final: public RISCVCpuBackend,
                                public HostHypervisorRam {
private:
    friend class KvmRISCVVcpu;

    RISCVCpuTarget &fTarget;
    DeviceLock &fLock;
    int fKvmFd;
    int fVmFd = -1;
    std::vector<std::unique_ptr<KvmRISCVVcpu>> fVcpus;
    std::unique_ptr<HypervisorPhysMemoryMap> fMemoryMap;
    RISCVHartIsa fIsa;
    uint64_t fTimebaseFrequency = 0;
    /* the guest's time counter less the host's */
    uint64_t fTimeOffset = 0;

    void ReadIsa();

public:
    KvmRISCVHypervisor(RISCVCpuTarget &target, DeviceLock &lock,
                       int kvm_fd):
        fTarget(target), fLock(lock), fKvmFd(kvm_fd) {}
    ~KvmRISCVHypervisor() override;

    void Init(const RISCVCpuOptions &options);

    /* RISCVCpuBackend */
    PhysMemoryMap &MemoryMap() override {return *fMemoryMap;}
    RISCVVcpu &Vcpu(int index) override {return *fVcpus[index];}
    const RISCVHartIsa &Isa() override {return fIsa;}
    uint64_t Time() override;
    void SetTime(uint64_t time) override;
    uint64_t TimebaseFrequency() override {return fTimebaseFrequency;}
    bool RealTimeClock() override {return true;}

    /* HostHypervisorRam */
    uint8_t *AllocRam(size_t size) override {return host_ram_alloc(size);}
    void FreeRam(uint8_t *ptr, size_t size) override
    {
        host_ram_free(ptr, size);
    }
    void MapRam(int slot, uint64_t addr, uint64_t size, uint8_t *host_mem,
                bool read_only, bool log_dirty) override;
    void GetDirtyLog(int slot, uint32_t *bitmap) override;
};


static void sigalrm_handler(int sig)
{
}


static uint64_t host_rdtime()
{
    uint64_t val;

    asm volatile("rdtime %0" : "=r"(val));
    return val;
}


KvmRISCVHypervisor::~KvmRISCVHypervisor()
{
    fMemoryMap.reset();
    fVcpus.clear();
    if (fVmFd >= 0)
        close(fVmFd);
    close(fKvmFd);
}


/* What the guest is shown is what KVM gives the first vcpu, which it gives
   them all. */
void KvmRISCVHypervisor::ReadIsa()
{
    KvmRISCVVcpu &vcpu = *fVcpus[0];
    uint64_t val;

    if (!vcpu.GetReg(KVM_CONFIG_REG(isa), &val)) {
        perror("KVM_GET_ONE_REG isa");
        exit(1);
    }
    fIsa.misa = (uint32_t)val | MISA_S | MISA_U;
    for (const auto &ext : kvm_isa_exts) {
        if (vcpu.GetReg(KVM_ISA_EXT_REG(ext.id), &val) && val != 0)
            fIsa.extensions.push_back(ext.name);
    }
    if (vcpu.GetReg(KVM_CONFIG_REG(satp_mode), &val))
        fIsa.satp_mode = val;
    else
        fIsa.satp_mode = RISCV_SATP_SV39;
    if (vcpu.GetReg(KVM_CONFIG_REG(zicbom_block_size), &val))
        fIsa.cbom_block_size = val;
    if (vcpu.GetReg(KVM_CONFIG_REG(zicboz_block_size), &val))
        fIsa.cboz_block_size = val;
}


void KvmRISCVHypervisor::Init(const RISCVCpuOptions &options)
{
    struct sigaction act;
    uint64_t val;

    fVmFd = ioctl(fKvmFd, KVM_CREATE_VM, 0);
    if (fVmFd < 0) {
        perror("KVM_CREATE_VM");
        exit(1);
    }

    for (int i = 0; i < options.hart_count; i++)
        fVcpus.push_back(std::make_unique<KvmRISCVVcpu>(*this, i));
    fMemoryMap = std::make_unique<HypervisorPhysMemoryMap>(*this);
    ReadIsa();

    KvmRISCVVcpu &vcpu = *fVcpus[0];
    if (!vcpu.GetReg(KVM_TIMER_REG(frequency), &fTimebaseFrequency)) {
        perror("KVM_GET_ONE_REG timer frequency");
        exit(1);
    }
    /* the guest's counter started with the VM */
    if (!vcpu.GetReg(KVM_TIMER_REG(time), &val)) {
        perror("KVM_GET_ONE_REG timer time");
        exit(1);
    }
    fTimeOffset = val - host_rdtime();

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


uint64_t KvmRISCVHypervisor::Time()
{
    return host_rdtime() + fTimeOffset;
}


/* The guest's counter is the kernel's, which only a vcpu's own thread may
   set; a write to mtime is left undone. */
void KvmRISCVHypervisor::SetTime(uint64_t time)
{
    static bool warned;

    (void)time;
    if (!warned) {
        warned = true;
        fprintf(stderr, "riscv kvm: writes to mtime are not supported\n");
    }
}


void KvmRISCVHypervisor::MapRam(int slot, uint64_t addr, uint64_t size,
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


void KvmRISCVHypervisor::GetDirtyLog(int slot, uint32_t *bitmap)
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


KvmRISCVVcpu::KvmRISCVVcpu(KvmRISCVHypervisor &owner, int index):
    fOwner(owner), fIndex(index)
{
    fFd = ioctl(fOwner.fVmFd, KVM_CREATE_VCPU, index);
    if (fFd < 0) {
        perror("KVM_CREATE_VCPU");
        exit(1);
    }

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

    /* The kernel answers the debug console only by passing it here; one
       without the extension leaves the guest to find it missing. */
    uint64_t on = 1;
    struct kvm_one_reg reg = {KVM_SBI_EXT_REG(KVM_RISCV_SBI_EXT_DBCN),
                              (uint64_t)(uintptr_t)&on};
    ioctl(fFd, KVM_SET_ONE_REG, &reg);

    /* every hart waits for an SBI start, the first for Start() */
    SetMpState(KVM_MP_STATE_STOPPED);
}


KvmRISCVVcpu::~KvmRISCVVcpu()
{
    if (fRun != nullptr)
        munmap(fRun, fRunSize);
    if (fFd >= 0)
        close(fFd);
}


bool KvmRISCVVcpu::GetReg(uint64_t id, uint64_t *val)
{
    struct kvm_one_reg reg = {id, (uint64_t)(uintptr_t)val};

    return ioctl(fFd, KVM_GET_ONE_REG, &reg) == 0;
}


void KvmRISCVVcpu::SetReg(uint64_t id, uint64_t val)
{
    struct kvm_one_reg reg = {id, (uint64_t)(uintptr_t)&val};

    if (ioctl(fFd, KVM_SET_ONE_REG, &reg) < 0) {
        perror("KVM_SET_ONE_REG");
        exit(1);
    }
}


void KvmRISCVVcpu::SetMpState(uint32_t state)
{
    struct kvm_mp_state mp_state = {state};

    if (ioctl(fFd, KVM_SET_MP_STATE, &mp_state) < 0) {
        perror("KVM_SET_MP_STATE");
        exit(1);
    }
}


void KvmRISCVVcpu::Start(uint64_t entry, uint64_t opaque)
{
    SetReg(KVM_CORE_REG(regs.pc), entry);
    SetReg(KVM_CORE_REG(regs.a0), fIndex);
    SetReg(KVM_CORE_REG(regs.a1), opaque);
    SetReg(KVM_CORE_REG(mode), KVM_RISCV_MODE_S);
    SetMpState(KVM_MP_STATE_RUNNABLE);
}


void KvmRISCVVcpu::ThreadStarted()
{
    sigset_t set;

    fThread = pthread_self();
    fThreadKnown.store(true);
    sigemptyset(&set);
    sigaddset(&set, SIGALRM);
    pthread_sigmask(SIG_UNBLOCK, &set, NULL);
}


/* With the lock held. The kernel decodes the access; a write to a ROM
   range goes to the device behind it, as a flash takes its commands. */
void KvmRISCVVcpu::ExitMmio()
{
    PhysMemoryMap &map = *fOwner.fMemoryMap;
    struct kvm_run *run = fRun;
    uint64_t paddr = run->mmio.phys_addr;
    int len = run->mmio.len;
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

    if (run->mmio.is_write) {
        memcpy(&val, run->mmio.data, len);
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
        memcpy(run->mmio.data, &val, len);
    }
}


/* With the lock held: an SBI call the kernel passed on, which the kernel
   then returns in a0 and a1. A legacy call keeps a1. */
void KvmRISCVVcpu::ExitSbi()
{
    struct kvm_run *run = fRun;
    uint64_t a[8];

    for (int i = 0; i < 6; i++)
        a[i] = run->riscv_sbi.args[i];
    a[6] = run->riscv_sbi.function_id;
    a[7] = run->riscv_sbi.extension_id;
    if (!fOwner.fTarget.SbiCall(fIndex, 64, a)) {
        a[0] = (uint64_t)(int64_t)SBI_ERR_NOT_SUPPORTED;
        a[1] = 0;
    }
    run->riscv_sbi.ret[0] = a[0];
    run->riscv_sbi.ret[1] = a[1];
}


/* With the lock held. The kernel stops the other vcpus itself. */
void KvmRISCVVcpu::ExitSystemEvent()
{
    struct kvm_run *run = fRun;
    bool failure = run->system_event.ndata > 0 &&
        run->system_event.data[0] == SBI_SRST_REASON_FAILURE;

    fEnded = true;
    switch (run->system_event.type) {
    case KVM_SYSTEM_EVENT_SHUTDOWN:
        fOwner.fTarget.SystemReset(false, failure);
        break;
    case KVM_SYSTEM_EVENT_RESET:
        fOwner.fTarget.SystemReset(true, failure);
        break;
    default:
        fprintf(stderr, "KVM: unsupported system event %u\n",
                run->system_event.type);
        exit(1);
    }
}


void KvmRISCVVcpu::Run()
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
    switch (run->exit_reason) {
    case KVM_EXIT_MMIO: {
        DeviceLocker locker(fOwner.fLock);
        ExitMmio();
        break;
    }
    case KVM_EXIT_RISCV_SBI: {
        DeviceLocker locker(fOwner.fLock);
        ExitSbi();
        break;
    }
    case KVM_EXIT_SYSTEM_EVENT: {
        DeviceLocker locker(fOwner.fLock);
        ExitSystemEvent();
        break;
    }
    case KVM_EXIT_INTR:
        break;
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


void KvmRISCVVcpu::InterruptRun()
{
    /* immediate_exit covers a signal that lands before KVM_RUN */
    fRun->immediate_exit = 1;
    if (fThreadKnown.load())
        pthread_kill(fThread, SIGALRM);
}


/* KVM_INTERRUPT is one of the vcpu calls that need not wait for a run to
   end, and it wakes the vcpu from WFI. */
void KvmRISCVVcpu::SetIrqLine(uint32_t mask, bool level)
{
    struct kvm_interrupt irq;

    if (!(mask & MIP_SEIP))
        return;
    irq.irq = level ? KVM_INTERRUPT_SET : KVM_INTERRUPT_UNSET;
    if (ioctl(fFd, KVM_INTERRUPT, &irq) < 0) {
        perror("KVM_INTERRUPT");
        exit(1);
    }
}


bool host_riscv_hypervisor_open(RISCVCpuTarget &target, DeviceLock &lock,
                                const RISCVCpuOptions &options,
                                std::unique_ptr<RISCVCpuBackend> *backend)
{
    int kvm_fd, ret, max_vcpus;

    kvm_fd = open("/dev/kvm", O_RDWR);
    if (kvm_fd < 0) {
        fprintf(stderr, "KVM not available\n");
        return true;
    }
    ret = ioctl(kvm_fd, KVM_GET_API_VERSION, 0);
    if (ret != 12) {
        fprintf(stderr, "Unsupported KVM version\n");
        close(kvm_fd);
        return true;
    }

    const char *why = nullptr;
    max_vcpus = ioctl(kvm_fd, KVM_CHECK_EXTENSION, KVM_CAP_MAX_VCPUS);
    if (options.machine_mode)
        why = "cannot run M-mode firmware; set sbi: \"native\"";
    else if (options.xlen != 64)
        why = "runs only riscv64 guests";
    else if (options.interrupt_arch == RISCV_INTR_AIA_IMSIC)
        why = "has no IMSIC to give the guest";
    else if (max_vcpus > 0 && options.hart_count > max_vcpus)
        why = "cannot run that many harts";
    if (why != nullptr) {
        fprintf(stderr, "KVM %s, or accel: \"none\" for the interpreter\n",
                why);
        close(kvm_fd);
        return false;
    }

    auto kvm = std::make_unique<KvmRISCVHypervisor>(target, lock, kvm_fd);
    kvm->Init(options);
    *backend = std::move(kvm);
    return true;
}
