/*
 * PC emulator
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
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include <algorithm>
#include <thread>
#include <vector>

#include "bits.h"
#include "cutils.h"
#include "host_thread.h"
#include "host_time.h"
#include "host_x86_hypervisor.h"
#include "iomem.h"
#include "devices.h"
#include "hpet.h"
#include "simplefb.h"
#include "virtio.h"
#include "uart.h"
#include "x86_cpu.h"
#include "machine.h"
#include "pci.h"
#include "pci_host_i440fx.h"
#include "pc_acpi.h"
#include "i8254.h"
#include "i8259.h"
#include "ioapic.h"
#include "lapic.h"
#include "mc146818.h"

//#define DEBUG_BIOS
//#define DUMP_IOPORT

#ifdef DEBUG_BIOS

#endif

class PCMachine;

/* With a hypervisor the interrupt controller is the host's, so device IRQs go
   straight there rather than through the emulated 8259s. */
class HypervisorIRQTarget final: public IRQTarget {
private:
    PCMachine &fMachine;

public:
    HypervisorIRQTarget(PCMachine &machine): fMachine(machine) {}

    void SetIRQ(int irq_num, int level) override;
};

/* With an IOAPIC, each line reaches both it and the 8259s, as on a PIIX,
   and the PIT's IRQ 0 is IOAPIC input 2. */
class PCIrqFanout final: public IRQTarget {
private:
    PCMachine &fMachine;

public:
    PCIrqFanout(PCMachine &machine): fMachine(machine) {}

    void SetIRQ(int irq_num, int level) override;
};

class PCMachine final:
    public VirtMachine,
    public CPUIRQTarget,
    public X86CpuTarget,
    public PCIMsiTarget,
    public LocalApicHost {
public:
    /* The processors, by APIC ID: the host hypervisor's when it has one,
       or else the interpreter's. They go last, after everything that uses
       their memory map. */
    std::unique_ptr<X86CpuBackend> fCpus;
    /* RAM from 0 up to the PCI hole, and the rest from 4 GB */
    uint64_t fLowRamSize;
    uint64_t fHighRamSize;
    /* the processors' */
    PhysMemoryMap *mem_map;
    PhysMemoryMap *port_map;

    std::unique_ptr<I8259Pair> pic_state;
    /* the lines of the 8259s, then the IOAPIC's inputs no 8259 has */
    IRQSignal irq_lines[IOAPIC_PINS];
    std::unique_ptr<I8254> pit_state;
    std::unique_ptr<MC146818> cmos_state;
    /* what the ACPI tables of a kernel boot describe */
    std::unique_ptr<AcpiPmBlock> fAcpiPm;

    /* The configuration's devices, and what realizing them produced. */
    SystemBus *bus = nullptr;
    I440FXState *i440fx_state = nullptr;
    FBDevice *fb_dev = nullptr;
    /* The device answering the VMware backdoor port, and where it is. */
    VMPortTarget *vmport = nullptr;
    uint64_t fb_base = 0;

    /* the processors have local APICs, and MSIs go to them */
    bool fLocalApic = false;
    /* the hypervisor has the 8259s and the 8254 rather than the machine */
    bool fHypervisorIrqchip = false;
    HypervisorIRQTarget fHypervisorIrqTarget {*this};
    /* the machine's IOAPIC, when the 8259s are the machine's */
    std::unique_ptr<IOAPIC> fIoApic;
    /* the machine's local APICs, when the processors have none of their
       own, and what each requests */
    std::unique_ptr<LocalApicBus> fLapics;
    std::unique_ptr<std::atomic<bool>[]> fApicIrq;
    PCIrqFanout fIrqFanout {*this};
    /* the configuration's HPET, if it has one */
    HPET *fHpet = nullptr;
    /* the physical address width the processors report */
    int fPhysAddressBits = 0;
    /* Otherwise the machine's: INTR as the 8259s drive it, read without the
       lock. */
    std::atomic<bool> fCpuIrq {false};
    /* Each processor runs on a thread of its own, which waits on its wakeup
       while the processor is halted; the machine thread keeps the timers
       and the screen. */
    int fCpuCount = 1;
    std::vector<std::thread> fVcpuThreads;
    std::unique_ptr<HostWakeup[]> fVcpuWakeups;
    /* The INIT and STARTUP IPIs a processor with the machine's local APIC
       has yet to take, and whether it waits for a STARTUP. */
    struct CpuStart {
        std::atomic<bool> init {false};
        std::atomic<int> startup {-1};
        bool waiting = false;
    };
    std::unique_ptr<CpuStart[]> fCpuStarts;

    void VcpuThread(int index);
    /* On the thread of processor 'index': acts out the IPIs it was sent,
       and false while it waits for a STARTUP. */
    bool TakeCpuStart(int index);
    /* Makes processor 'index' end its run or its wait, to look at its
       interrupts, its IPIs or a stop. Any thread. */
    void KickVcpu(int index);
    /* The processor of the calling thread: its own, or the first on the
       machine thread. */
    int CurrentCpu() const;
    LocalApic &CurrentApic() {return fLapics->Apic(CurrentCpu());}

    /* fixed-function port stubs and the VMware backdoor port */
    uint32_t Port80Read(uint32_t offset, int size_log2);
    void Port80Write(uint32_t offset, uint32_t val, int size_log2);
    uint32_t Port92Read(uint32_t offset, int size_log2);
    void Port92Write(uint32_t offset, uint32_t val, int size_log2);
    uint32_t VmPortRead(uint32_t addr, int size_log2);
    void VmPortWrite(uint32_t addr, uint32_t val, int size_log2);
    uint32_t BiosDebugRead(uint32_t offset, int size_log2);
    void BiosDebugWrite(uint32_t offset, uint32_t val, int size_log2);
    /* X86CpuTarget */
    uint32_t PortRead(uint32_t port, int size_log2) override;
    void PortWrite(uint32_t port, uint32_t val, int size_log2) override;
    void MmioRead(uint64_t addr, uint8_t *data, int len) override;
    void MmioWrite(uint64_t addr, const uint8_t *data, int len) override;
    bool InterruptRequested() override;
    int AcknowledgeInterrupt() override;
    void ApicEoi(int vector) override;
    void ProcessorShutdown() override;
    /* PCIMsiTarget */
    void SendMsi(uint64_t addr, uint32_t data) override;
    /* LocalApicHost; ApicEoi() above serves both */
    void SetApicInterrupt(uint32_t id, bool pending) override;
    void ApicInit(uint32_t id) override;
    void ApicStartup(uint32_t id, int vector) override;
    void ApicTimerChanged(uint64_t due) override;
    /* X86CpuTarget again: the calling processor's local APIC */
    uint64_t ApicBase() override {return CurrentApic().Base();}
    bool SetApicBase(uint64_t val) override
        {return CurrentApic().SetBase(val);}
    int TaskPriority() override {return CurrentApic().TaskPriority();}
    void SetTaskPriority(int val) override
        {CurrentApic().SetTaskPriority(val);}
    /* the local APICs' page, each processor seeing its own */
    uint32_t LapicRead(uint32_t offset, int size_log2)
        {return CurrentApic().DeviceRead(offset, size_log2);}
    void LapicWrite(uint32_t offset, uint32_t val, int size_log2)
        {CurrentApic().DeviceWrite(offset, val, size_log2);}

    DeviceIOAdapter<PCMachine, &PCMachine::Port80Read,
                    &PCMachine::Port80Write> fPort80Io {*this};
    DeviceIOAdapter<PCMachine, &PCMachine::Port92Read,
                    &PCMachine::Port92Write> fPort92Io {*this};
    DeviceIOAdapter<PCMachine, &PCMachine::VmPortRead,
                    &PCMachine::VmPortWrite> fVmPortIo {*this};
    DeviceIOAdapter<PCMachine, &PCMachine::BiosDebugRead,
                    &PCMachine::BiosDebugWrite> fBiosDebugIo {*this};
    DeviceIOAdapter<PCMachine, &PCMachine::LapicRead,
                    &PCMachine::LapicWrite> fLapicIo {*this};

    ~PCMachine() override;

    DeviceLock &Lock() {return *fDeviceLock;}

    /* CPUIRQTarget */
    void SetCPUIRQ(int level) override;

    /* VirtMachine */
    void StartProcessors() override;
    void JoinProcessors() override;
    int64_t RunTimers() override;
    void InterruptExecution() override;
};

/* The processor the calling thread runs, or -1 on any other thread. */
static thread_local int sCurrentVcpu = -1;

struct PcMemoryRange {
    uint64_t addr;
    uint64_t size;
    uint32_t type; /* E820_* */
};
#define PC_MEMORY_RANGES_MAX 6

static void copy_kernel(PCMachine *s, const uint8_t *buf, int buf_len,
                        const char *cmd_line, uint32_t bios_size);
static int pc_memory_ranges(PCMachine *s, uint32_t bios_size,
                            PcMemoryRange *ranges);
static void pc_acpi_setup(PCMachine *s);
static uint8_t *get_ram_range_ptr(PCMachine *s, uint64_t addr, uint64_t len);
static void set_flat_gdt(uint8_t *gdt);
static void start_flat_protected_mode(PCMachine *s, uint32_t gdt_addr,
                                      uint32_t entry, int reg,
                                      uint32_t reg_val);
static void map_pci_interrupts(PCMachine *s);
static bool is_elf(const uint8_t *buf, int buf_len);
static bool pvh_load(PCMachine *s, const uint8_t *buf, int buf_len,
                     const uint8_t *initrd, int initrd_len,
                     const char *cmd_line, uint32_t bios_size,
                     uint64_t rsdp);

void PCMachine::BiosDebugWrite(uint32_t offset, uint32_t val, int size_log2)
{
    (void)offset;
    (void)size_log2;
    putchar(get_bits(val, 0, 8));
}

uint32_t PCMachine::BiosDebugRead(uint32_t offset, int size_log2)
{
    (void)offset;
    (void)size_log2;
    return 0;
}

void PCMachine::Port80Write(uint32_t offset, uint32_t val, int size_log2)
{
    (void)offset;
    (void)val;
    (void)size_log2;
}

uint32_t PCMachine::Port80Read(uint32_t offset, int size_log2)
{
    (void)offset;
    (void)size_log2;
    return 0xff;
}

/* Bit 0 is the fast reset, which the FADT also names as the ACPI reset
   register. */
void PCMachine::Port92Write(uint32_t offset, uint32_t val, int size_log2)
{
    (void)size_log2;
    if (offset == 0 && (val & 1))
        RequestReset();
}

uint32_t PCMachine::Port92Read(uint32_t offset, int size_log2)
{
    (void)offset;
    (void)size_log2;
    int a20 = 1; /* A20=0 is not supported */
    return a20 << 1;
}

#define VMPORT_MAGIC   0x564D5868
#define REG_EAX 0
#define REG_EBX 1
#define REG_ECX 2
#define REG_EDX 3
#define REG_ESI 4
#define REG_EDI 5

uint32_t PCMachine::VmPortRead(uint32_t addr, int size_log2)
{
    PCMachine *s = this;
    X86Vcpu &vcpu = s->fCpus->Vcpu(CurrentCpu());
    uint32_t regs[6];
    X86Regs r;

    vcpu.GetRegs(&r);
    regs[REG_EAX] = r.gpr[0];
    regs[REG_EBX] = r.gpr[3];
    regs[REG_ECX] = r.gpr[1];
    regs[REG_EDX] = r.gpr[2];
    regs[REG_ESI] = r.gpr[6];
    regs[REG_EDI] = r.gpr[7];

    if (regs[REG_EAX] == VMPORT_MAGIC) {

        s->vmport->VMPortCommand(regs);

        /* Note: in 64 bits the high parts are reset to zero
           in all cases. */
        r.gpr[0] = regs[REG_EAX];
        r.gpr[3] = regs[REG_EBX];
        r.gpr[1] = regs[REG_ECX];
        r.gpr[2] = regs[REG_EDX];
        r.gpr[6] = regs[REG_ESI];
        r.gpr[7] = regs[REG_EDI];
        vcpu.SetRegs(r);
    }
    return regs[REG_EAX];
}

void PCMachine::VmPortWrite(uint32_t addr, uint32_t val, int size_log2)
{
    (void)addr;
    (void)val;
    (void)size_log2;
}

void PCMachine::SetCPUIRQ(int level)
{
    if (fLapics) {
        /* the other processors' LINT0 is left masked */
        fLapics->Apic(0).SetLint0(level);
        return;
    }
    bool was = fCpuIrq.exchange(level != 0);
    bool raised = level && !was;
    fCpus->Vcpu(0).SetInterruptRequest(level != 0);
    /* Processor 0 looks at INTR again before its next run, so only a request
       from another thread has to stop a run in progress or end a wait. */
    if (raised && sCurrentVcpu != 0)
        KickVcpu(0);
}

int PCMachine::CurrentCpu() const
{
    return sCurrentVcpu < 0 ? 0 : sCurrentVcpu;
}

void PCMachine::SetApicInterrupt(uint32_t id, bool pending)
{
    /* as with INTR */
    fApicIrq[id].store(pending);
    fCpus->Vcpu(id).SetInterruptRequest(pending);
    if (pending && sCurrentVcpu != (int)id)
        KickVcpu(id);
}

/* The processor takes these between runs, on its own thread. */
void PCMachine::ApicInit(uint32_t id)
{
    fCpuStarts[id].init.store(true);
    KickVcpu(id);
}

void PCMachine::ApicStartup(uint32_t id, int vector)
{
    fCpuStarts[id].startup.store(vector);
    KickVcpu(id);
}

/* The machine thread may be asleep until a later deadline. */
void PCMachine::ApicTimerChanged(uint64_t due)
{
    KickBefore(due);
}

bool PCMachine::InterruptRequested()
{
    if (fLapics)
        return fApicIrq[CurrentCpu()].load();
    return CurrentCpu() == 0 && fCpuIrq.load();
}

int PCMachine::AcknowledgeInterrupt()
{
    if (fLapics) {
        int vector = CurrentApic().Acknowledge();
        if (vector != LAPIC_EXTINT)
            return vector;
    }
    return pic_state->Acknowledge();
}

/* Only a write to the local APICs' page is an interrupt; anything else is
   the memory write an MSI really is. */
void PCMachine::SendMsi(uint64_t addr, uint32_t data)
{
    if ((addr >> 20) == 0xfee) {
        if (fLapics)
            fLapics->Deliver(addr, data);
        else
            fCpus->SendMsi(addr, data);
    } else
        mem_map->IoWrite(addr, data, 2);
}

/* The two PICs give sixteen lines, which an IOAPIC follows with eight of its
   own; the port space is what a 16 bit port number can name. */
#define PC_IRQ_COUNT 16
/* xAPIC IDs 0 to 254 */
#define PC_MAX_CPUS 255
#define PC_IO_SPACE_SIZE 0x10000

/* Where a device that wants host address space rather than ports is placed.
   A PC has almost nothing of the kind -- the framebuffer of a machine booting
   a kernel without firmware is the one thing -- so the window is what is
   left between the PCI hole and the IOAPIC. */
#define PC_DEVICE_WINDOW_BASE 0xf8000000
#define PC_DEVICE_WINDOW_SIZE (IOAPIC_ADDR - PC_DEVICE_WINDOW_BASE)

/* RAM beyond this goes above 4 GB, leaving the rest of the 32 bit space to
   PCI and the platform devices. */
#define PC_LOW_RAM_MAX 0xc0000000ULL
#define PC_HIGH_RAM_BASE 0x100000000ULL
#define PC_LOCAL_APIC_ADDR 0xfee00000
#define PC_LOCAL_APIC_SIZE 0x100000 /* with the MSI addresses */

static uint8_t *get_ram_ptr(PCMachine *s, uint64_t paddr)
{
    PhysMemoryRange *pr;
    pr = s->mem_map->FindRange(paddr);
    if (!pr || !pr->is_ram)
        return NULL;
    return pr->phys_mem + (uintptr_t)(paddr - pr->addr);
}

#ifdef DUMP_IOPORT
static bool dump_port(int port)
{
    return !((port >= 0x1f0 && port <= 0x1f7) ||
             (port >= 0x20 && port <= 0x21) ||
             (port >= 0xa0 && port <= 0xa1));
}
#endif

void PCMachine::PortWrite(uint32_t port, uint32_t val, int size_log2)
{
#ifdef DUMP_IOPORT
    if (dump_port(port))
        printf("write port=0x%x val=0x%x s=%d\n", port, val, 1 << size_log2);
#endif
    port_map->IoWrite(port, val, size_log2);
    /* Programming the PIT or the RTC can bring a timer the machine thread
       sleeps on forward. */
    int64_t delay = -1;
    if (port - 0x40 < 4)
        delay = pit_state->DelayUs();
    else if (port - 0x70 < 2)
        delay = cmos_state->DelayUs();
    if (delay >= 0)
        KickBefore(host_monotonic_us() + delay);
}

uint32_t PCMachine::PortRead(uint32_t port, int size_log2)
{
    uint32_t val = port_map->IoRead(port, size_log2);
#ifdef DUMP_IOPORT
    if (dump_port(port))
        printf("read port=0x%x val=0x%x s=%d\n", port, val, 1 << size_log2);
#endif
    return val;
}

void HypervisorIRQTarget::SetIRQ(int irq_num, int level)
{
    fMachine.fCpus->SetIRQ(irq_num, level);
}

void PCIrqFanout::SetIRQ(int irq_num, int level)
{
    fMachine.pic_state->SetIRQ(irq_num, level);
    if (irq_num != 2)
        fMachine.fIoApic->SetIRQ(irq_num == 0 ? 2 : irq_num, level);
}

/* A PC resets on a triple fault. */
void PCMachine::ProcessorShutdown()
{
    RequestReset();
}

void PCMachine::ApicEoi(int vector)
{
    if (fIoApic)
        fIoApic->Eoi(vector);
}

void PCMachine::MmioWrite(uint64_t paddr, const uint8_t *data, int len)
{
    PhysMemoryRange *pr;
    uint64_t addr;

    pr = mem_map->FindRange(paddr);
    if (!pr)
        return;
    addr = paddr - pr->addr;
    if (pr->is_ram) {
        if (!(pr->devram_flags & DEVRAM_FLAG_ROM) && addr + len <= pr->size) {
            memcpy(mem_map->GetRamPtr(paddr, true), data, len);
        }
        return;
    }
    switch(len) {
    case 1:
        if (pr->devio_flags & DEVIO_SIZE8) {
            pr->io->DeviceWrite(addr, *(const uint8_t *)data, 0);
        }
        break;
    case 2:
        if (pr->devio_flags & DEVIO_SIZE16) {
            pr->io->DeviceWrite(addr, *(const uint16_t *)data, 1);
        }
        break;
    case 4:
        if (pr->devio_flags & DEVIO_SIZE32) {
            pr->io->DeviceWrite(addr, *(const uint32_t *)data, 2);
        }
        break;
    case 8:
        if (pr->devio_flags & DEVIO_SIZE32) {
            pr->io->DeviceWrite(addr, *(const uint32_t *)data, 2);
            pr->io->DeviceWrite(addr + 4, *(const uint32_t *)(data + 4), 2);
        }
        break;
    default:
        abort();
    }
}

void PCMachine::MmioRead(uint64_t paddr, uint8_t *data, int len)
{
    PhysMemoryRange *pr;
    uint64_t addr;

    pr = mem_map->FindRange(paddr);
    if (!pr)
        goto no_dev;
    addr = paddr - pr->addr;
    if (pr->is_ram) {
        if (addr + len > pr->size)
            goto no_dev;
        memcpy(data, pr->phys_mem + addr, len);
        return;
    }
    switch(len) {
    case 1:
        if (!(pr->devio_flags & DEVIO_SIZE8))
            goto no_dev;
        *(uint8_t *)data = pr->io->DeviceRead(addr, 0);
        break;
    case 2:
        if (!(pr->devio_flags & DEVIO_SIZE16))
            goto no_dev;
        *(uint16_t *)data = pr->io->DeviceRead(addr, 1);
        break;
    case 4:
        if (!(pr->devio_flags & DEVIO_SIZE32))
            goto no_dev;
        *(uint32_t *)data = pr->io->DeviceRead(addr, 2);
        break;
    case 8:
        if (!(pr->devio_flags & DEVIO_SIZE32))
            goto no_dev;
        *(uint32_t *)data = pr->io->DeviceRead(addr, 2);
        *(uint32_t *)(data + 4) = pr->io->DeviceRead(addr + 4, 2);
        break;
    default:
        abort();
    }
    return;
 no_dev:
    memset(data, 0, len);
}

/* What a host bridge may decode: from the end of low RAM up to the device
   window, and above RAM as far as the processors address. */
static PcPciApertures pc_pci_space(PCMachine *s)
{
    PcPciApertures space;

    space.mmio_base = (s->fLowRamSize + 0xfffff) & ~(uint64_t)0xfffff;
    space.mmio_end = PC_DEVICE_WINDOW_BASE;
    space.mmio64_base = PC_HIGH_RAM_BASE + s->fHighRamSize;
    space.mmio64_end = std::max(space.mmio64_base,
                                (uint64_t)1 << s->fPhysAddressBits);
    return space;
}

/* The addresses and lines the chipset above holds. They are reserved so that
   a device the configuration declares cannot be placed on top of one of
   them. A host bridge reserves its apertures itself. */
static bool pc_claim_fixed_ranges(PCMachine *s, bool acpi, uint32_t bios_size)
{
    RangeAllocator &mmio = s->bus->MmioAlloc();
    RangeAllocator &io = s->bus->IoAlloc();
    RangeAllocator &irq = s->bus->IrqAlloc();

    return mmio.Claim(0, 0x100000, "low memory") &&
        mmio.Claim(0x100000, s->fLowRamSize - 0x100000, "ram") &&
        mmio.Claim(IOAPIC_ADDR, IOAPIC_SIZE, "ioapic") &&
        mmio.Claim(PC_LOCAL_APIC_ADDR, PC_LOCAL_APIC_SIZE, "local apic") &&
        mmio.Claim(PC_HIGH_RAM_BASE - bios_size, bios_size, "bios") &&
        mmio.Claim(PC_HIGH_RAM_BASE, s->fHighRamSize, "high ram") &&
        (!acpi || io.Claim(ACPI_PM_BASE, ACPI_PM_SIZE, "acpi")) &&
        (!acpi || irq.Claim(ACPI_SCI_IRQ, 1, "acpi sci")) &&
        io.Claim(0x20, 2, "pic") && io.Claim(0xa0, 2, "pic") &&
        io.Claim(0x4d0, 2, "elcr") &&
        io.Claim(0x40, 4, "pit") && io.Claim(0x61, 1, "pit") &&
        io.Claim(0x70, 2, "cmos") &&
        io.Claim(0x80, 2, "port80") && io.Claim(0x92, 2, "port92") &&
#ifdef DEBUG_BIOS
        io.Claim(0x402, 2, "bios debug") &&
#endif
        irq.Claim(0, 1, "pit") && irq.Claim(2, 1, "cascade") &&
        irq.Claim(8, 1, "rtc") && irq.Claim(13, 1, "fpu");
}


static std::unique_ptr<VirtMachine> pc_machine_init(const VirtMachineParams *p)
{
    std::unique_ptr<PCMachine> owned;
    PCMachine *s;
    DeviceContext ctx;

    if (strcmp(p->machine_name, "pc") != 0) {
        vm_error("unsupported machine: %s\n", p->machine_name);
        return nullptr;
    }
    X86CpuOptions options;
    if (p->interrupt_controller == nullptr ||
        strcmp(p->interrupt_controller, "pic") == 0) {
        options.local_apic = false;
    } else if (strcmp(p->interrupt_controller, "apic") == 0) {
        options.local_apic = true;
    } else {
        vm_error("pc: interrupt_controller must be \"pic\" or \"apic\", "
                 "not \"%s\"\n", p->interrupt_controller);
        return nullptr;
    }
    /* The processors beyond the first are started through their local
       APICs, whose xAPIC IDs stop short of the broadcast one. */
    if (p->cpu_count < 1 || p->cpu_count > PC_MAX_CPUS) {
        vm_error("pc: cpus must be between 1 and %d\n", PC_MAX_CPUS);
        return nullptr;
    }
    if (p->cpu_count > 1 && !options.local_apic) {
        vm_error("pc: more than one processor needs interrupt_controller "
                 "\"apic\"\n");
        return nullptr;
    }
    options.cpu_count = p->cpu_count;
    options.hypervisor_interrupt_controllers = p->hypervisor_intc;
    if (p->hypervisor_cpuid != nullptr &&
        strcmp(p->hypervisor_cpuid, "interpreter") == 0) {
        options.cpuid = x86_cpu_hypervisor_cpuid();
    } else if (p->hypervisor_cpuid != nullptr &&
               strcmp(p->hypervisor_cpuid, "host") != 0) {
        vm_error("pc: hypervisor_cpuid must be \"host\" or \"interpreter\", "
                 "not \"%s\"\n", p->hypervisor_cpuid);
        return nullptr;
    }
    if (p->hypervisor_pat != nullptr &&
        strcmp(p->hypervisor_pat, "write-back") == 0) {
        options.pat = 0x0606060606060606;
    } else if (p->hypervisor_pat != nullptr &&
               strcmp(p->hypervisor_pat, "default") != 0) {
        vm_error("pc: hypervisor_pat must be \"default\" or \"write-back\", "
                 "not \"%s\"\n", p->hypervisor_pat);
        return nullptr;
    }

    assert(p->ram_size >= (1 << 20));

    owned = std::make_unique<PCMachine>();
    s = owned.get();
    s->SetHost(p);
    s->vmc = p->vmc;
    s->fLowRamSize = std::min<uint64_t>(p->ram_size, PC_LOW_RAM_MAX);
    s->fHighRamSize = p->ram_size - s->fLowRamSize;

    s->port_map = new PhysMemoryMap();

    if (p->accel_enable)
        s->fCpus = host_x86_hypervisor_open(*s, *p->device_lock, options);
    if (!s->fCpus)
        s->fCpus = x86_interp_open(*s, *p->device_lock, options);
    s->fLocalApic = options.local_apic;
    bool machine_lapics = s->fLocalApic && !s->fCpus->HasLocalApics();
    s->fCpuCount = options.cpu_count;
    s->fVcpuWakeups = std::make_unique<HostWakeup[]>(s->fCpuCount);
    s->fPhysAddressBits = s->fCpus->PhysAddressBits();
    if (s->fHighRamSize != 0 && PC_HIGH_RAM_BASE + s->fHighRamSize >
        (uint64_t)1 << s->fPhysAddressBits) {
        vm_error("pc: %llu MB of RAM is beyond the processors' %d bit "
                 "physical addresses\n",
                 (unsigned long long)(p->ram_size >> 20),
                 s->fPhysAddressBits);
        return nullptr;
    }

    s->mem_map = &s->fCpus->MemoryMap();
    s->fHypervisorIrqchip = s->fCpus->HasInterruptControllers();
    if (s->fHypervisorIrqchip) {
        for (int i = 0; i < 16; i++) {
            s->irq_lines[i].Init(&s->fHypervisorIrqTarget, i);
        }
    }
    /* the others wait for their STARTUP IPIs */
    if (machine_lapics) {
        s->fCpuStarts = std::make_unique<PCMachine::CpuStart[]>(s->fCpuCount);
        for (int i = 1; i < s->fCpuCount; i++)
            s->fCpuStarts[i].waiting = true;
    }

    /* set the RAM mapping and leave the VGA addresses empty */
    s->mem_map->RegisterRam(0xc0000, s->fLowRamSize - 0xc0000, 0);
    s->mem_map->RegisterRam(0, 0xa0000, 0);
    if (s->fHighRamSize != 0)
        s->mem_map->RegisterRam(PC_HIGH_RAM_BASE, s->fHighRamSize, 0);
    
    /* devices */
    s->port_map->RegisterDevice(0x80, 2, &s->fPort80Io, DEVIO_SIZE8);
    s->port_map->RegisterDevice(0x92, 2, &s->fPort92Io, DEVIO_SIZE8);
    
    /* setup the bios */
    if (p->files[VM_FILE_BIOS].len > 0) {
        int bios_size, bios_size1;
        uint8_t *bios_buf, *ptr;
        uint32_t bios_addr;
        
        bios_size = p->files[VM_FILE_BIOS].len;
        bios_buf = p->files[VM_FILE_BIOS].buf;
        assert((bios_size % 65536) == 0 && bios_size != 0);
        bios_addr = -bios_size;
        /* at the top of the 4GB memory */
        s->mem_map->RegisterRam(bios_addr, bios_size, DEVRAM_FLAG_ROM);
        ptr = get_ram_ptr(s, bios_addr);
        memcpy(ptr, bios_buf, bios_size);
        /* in the lower 1MB memory (currently set as RAM) */
        bios_size1 = min_int(bios_size, 128 * 1024);
        ptr = get_ram_ptr(s, 0x100000 - bios_size1);
        memcpy(ptr, bios_buf + bios_size - bios_size1, bios_size1);
#ifdef DEBUG_BIOS
        s->port_map->RegisterDevice(0x402, 2, &s->fBiosDebugIo, DEVIO_SIZE8);
#endif
    }

    if (!s->fHypervisorIrqchip) {
        s->pic_state = std::make_unique<I8259Pair>(s->port_map, 0x20, 0xa0,
                                                   0x4d0, 0x4d1, *s,
                                                   s->irq_lines);
        if (s->fLocalApic) {
            s->fIoApic = std::make_unique<IOAPIC>(*s, 0);
            s->mem_map->RegisterDevice(IOAPIC_ADDR, IOAPIC_SIZE,
                                       s->fIoApic.get(), DEVIO_SIZE32);
            for (int i = 0; i < PC_IRQ_COUNT; i++)
                s->irq_lines[i].Init(&s->fIrqFanout, i);
        }
        /* the 8259s then reach the first processor through its LINT0 */
        if (machine_lapics) {
            s->fApicIrq = std::make_unique<std::atomic<bool>[]>(s->fCpuCount);
            s->fLapics = std::make_unique<LocalApicBus>(*s, s->fCpuCount);
            s->mem_map->RegisterDevice(LAPIC_BASE, LAPIC_SIZE, &s->fLapicIo,
                                       DEVIO_SIZE32);
        }
        s->pit_state = std::make_unique<I8254>(s->port_map, 0x40, 0x61,
                                               &s->irq_lines[0]);
    }
    IRQTarget *ioapic = nullptr;
    if (s->fIoApic)
        ioapic = s->fIoApic.get();
    else if (s->fHypervisorIrqchip && s->fLocalApic)
        ioapic = &s->fHypervisorIrqTarget;
    for (int i = PC_IRQ_COUNT; ioapic != nullptr && i < IOAPIC_PINS; i++)
        s->irq_lines[i].Init(ioapic, i);

    s->cmos_state = std::make_unique<MC146818>(s->port_map, 0x70,
                                               &s->irq_lines[8],
                                               p->rtc_local_time);

    /* various cmos data */
    {
        int size;
        /* memory size */
        size = min_int((s->fLowRamSize - (1 << 20)) >> 10, 65535);
        s->cmos_state->SetNvram(0x30, size);
        s->cmos_state->SetNvram(0x31, size >> 8);
        if (s->fLowRamSize >= (16 << 20)) {
            size = min_int((s->fLowRamSize - (16 << 20)) >> 16, 65535);
            s->cmos_state->SetNvram(0x34, size);
            s->cmos_state->SetNvram(0x35, size >> 8);
        }
        /* above 4 GB, in 64 KB units */
        uint64_t high = s->fHighRamSize >> 16;
        s->cmos_state->SetNvram(0x5b, high);
        s->cmos_state->SetNvram(0x5c, high >> 8);
        s->cmos_state->SetNvram(0x5d, high >> 16);
        s->cmos_state->SetNvram(0x14, 0x06); /* mouse + FPU present */
    }
    
    /* The devices the configuration declares. Everything above this point is
       what a PC has before any of them exists: RAM, the interrupt
       controllers, the timer and the clock. */
    s->bus = new SystemBus(s->mem_map, s->port_map, s->irq_lines,
                           ioapic != nullptr ? IOAPIC_PINS : PC_IRQ_COUNT);
    if (s->fLocalApic)
        s->bus->SetMsiTarget(s);
    s->bus->IoAlloc().SetWindow(0, PC_IO_SPACE_SIZE);
    /* A device that names no line gets one of the IOAPIC's own, which no
       ISA device and no 8259 shares. */
    if (ioapic != nullptr)
        s->bus->IrqAlloc().SetWindow(PC_IRQ_COUNT,
                                     IOAPIC_PINS - PC_IRQ_COUNT);
    s->bus->MmioAlloc().SetWindow(PC_DEVICE_WINDOW_BASE,
                                  PC_DEVICE_WINDOW_SIZE);

    PcPciApertures pci_space = pc_pci_space(s);
    ctx.params = p;
    ctx.platform = p->platform;
    ctx.machine = s;
    ctx.pc_pci_space = &pci_space;

    /* The nesting in the file is the nesting of the buses, so the root one
       has to be the kind this machine provides: "pc" for devices only
       firmware knows of, "acpi" for devices the machine describes in the
       ACPI tables it builds for a kernel. */
    SystemBus *root = s->bus;
    bool acpi = false;
    if (!bus_config_attach(p->root_bus, "the pc machine",
                           [root, &acpi](const char *type) -> Bus * {
                               acpi = strcmp(type, "acpi") == 0;
                               root->SetAcpi(acpi);
                               return acpi || strcmp(type, "pc") == 0 ?
                                   root : nullptr;
                           }, &ctx)) {
        return nullptr;
    }
    if (acpi && p->files[VM_FILE_BIOS].len > 0) {
        vm_error("pc: the \"acpi\" root bus is for a kernel the machine "
                 "loads itself; firmware builds tables of its own, so use "
                 "\"pc\"\n");
        return nullptr;
    }
    if (!acpi && p->files[VM_FILE_KERNEL].buf != nullptr &&
        (s->fLocalApic || s->fCpuCount > 1)) {
        vm_error("pc: a kernel finds APICs and other processors only in the "
                 "ACPI tables, which the machine builds on the \"acpi\" root "
                 "bus\n");
        return nullptr;
    }
    if (!pc_claim_fixed_ranges(s, acpi, p->files[VM_FILE_BIOS].len) ||
        !s->bus->AllocateAll() || !s->bus->RealizeAll()) {
        return nullptr;
    }

    device_context_connect(&ctx);
    s->fb_dev = ctx.fb_dev;
    s->fb_base = ctx.fb_base;

    /* The VMware backdoor is read through the processor's registers, so the
       machine owns the port and the device only interprets the call. */
    s->vmport = ctx.vmport;
    if (s->vmport != NULL) {
        s->port_map->RegisterDevice(ctx.vmport_base, 1, &s->fVmPortIo,
                                    DEVIO_SIZE32);
    }

    /* With no firmware to program the PIRQ registers, copy_kernel() routes
       the INTx lines itself, which needs the bridge the tree built. */
    for (int i = 0; i < s->bus->DeviceCount(); i++) {
        I440FXState *fx = i440fx_node_state(s->bus->DeviceAt(i));
        if (fx != NULL) {
            s->i440fx_state = fx;
            break;
        }
    }

    /* The HPET's legacy replacement lines are the PIT's and the RTC's.
       Routed, it may use the IOAPIC inputs no ISA line reaches and no
       device took, when the guest is told of an IOAPIC. */
    uint32_t free_pins = 0;
    for (int i = PC_IRQ_COUNT; ioapic != nullptr && i < IOAPIC_PINS; i++) {
        if (s->bus->IrqAlloc().FindOverlap(i, 1) == nullptr)
            free_pins |= bit_at(i);
    }
    for (int i = 0; i < s->bus->DeviceCount(); i++) {
        HPET *hpet = hpet_node_state(s->bus->DeviceAt(i));
        if (hpet == nullptr)
            continue;
        hpet->SetOutputs(&s->irq_lines[0], &s->irq_lines[8], ioapic,
                         free_pins);
        s->fHpet = hpet;
        break;
    }

    if (acpi) {
        pc_acpi_setup(s);
    }

    if (p->files[VM_FILE_KERNEL].buf) {
        const VMFileEntry &kernel = p->files[VM_FILE_KERNEL];
        const VMFileEntry &initrd = p->files[VM_FILE_INITRD];

        if (is_elf(kernel.buf, kernel.len)) {
            if (!pvh_load(s, kernel.buf, kernel.len, initrd.buf, initrd.len,
                          p->cmdline ? p->cmdline : "",
                          p->files[VM_FILE_BIOS].len, PC_ACPI_ADDR)) {
                return nullptr;
            }
        } else {
            copy_kernel(s, kernel.buf, kernel.len,
                        p->cmdline ? p->cmdline : "",
                        p->files[VM_FILE_BIOS].len);
        }
    }

    return owned;
}

PCMachine::~PCMachine()
{
    PCMachine *s = this;
    /* XXX: free all */
    delete s->bus;
    delete s->port_map;
}

struct screen_info {
} __attribute__((packed));

/* from plex86 (BSD license) */
struct  __attribute__ ((packed)) linux_params {
    /* screen_info structure */
    uint8_t  orig_x;		/* 0x00 */
    uint8_t  orig_y;		/* 0x01 */
    uint16_t ext_mem_k;	/* 0x02 */
    uint16_t orig_video_page;	/* 0x04 */
    uint8_t  orig_video_mode;	/* 0x06 */
    uint8_t  orig_video_cols;	/* 0x07 */
    uint8_t  flags;		/* 0x08 */
    uint8_t  unused2;		/* 0x09 */
    uint16_t orig_video_ega_bx;/* 0x0a */
    uint16_t unused3;		/* 0x0c */
    uint8_t  orig_video_lines;	/* 0x0e */
    uint8_t  orig_video_isVGA;	/* 0x0f */
    uint16_t orig_video_points;/* 0x10 */
    
    /* VESA graphic mode -- linear frame buffer */
    uint16_t lfb_width;	/* 0x12 */
    uint16_t lfb_height;	/* 0x14 */
    uint16_t lfb_depth;	/* 0x16 */
    uint32_t lfb_base;		/* 0x18 */
    uint32_t lfb_size;		/* 0x1c */
    uint16_t cl_magic, cl_offset; /* 0x20 */
    uint16_t lfb_linelength;	/* 0x24 */
    uint8_t  red_size;		/* 0x26 */
    uint8_t  red_pos;		/* 0x27 */
    uint8_t  green_size;	/* 0x28 */
    uint8_t  green_pos;	/* 0x29 */
    uint8_t  blue_size;	/* 0x2a */
    uint8_t  blue_pos;		/* 0x2b */
    uint8_t  rsvd_size;	/* 0x2c */
    uint8_t  rsvd_pos;		/* 0x2d */
    uint16_t vesapm_seg;	/* 0x2e */
    uint16_t vesapm_off;	/* 0x30 */
    uint16_t pages;		/* 0x32 */
    uint16_t vesa_attributes;	/* 0x34 */
    uint32_t capabilities;     /* 0x36 */
    uint32_t ext_lfb_base;	/* 0x3a */
    uint8_t  _reserved[2];	/* 0x3e */
    
  /* 0x040 */ uint8_t   apm_bios_info[20]; // struct apm_bios_info
  /* 0x054 */ uint8_t   pad2[0x80 - 0x54];

  // Following 2 from 'struct drive_info_struct' in drivers/block/cciss.h.
  // Might be truncated?
  /* 0x080 */ uint8_t   hd0_info[16]; // hd0-disk-parameter from intvector 0x41
  /* 0x090 */ uint8_t   hd1_info[16]; // hd1-disk-parameter from intvector 0x46

  // System description table truncated to 16 bytes
  // From 'struct sys_desc_table_struct' in linux/arch/i386/kernel/setup.c.
  /* 0x0a0 */ uint16_t  sys_description_len;
  /* 0x0a2 */ uint8_t   sys_description_table[14];
                        // [0] machine id
                        // [1] machine submodel id
                        // [2] BIOS revision
                        // [3] bit1: MCA bus

  /* 0x0b0 */ uint8_t   pad3[0x1e0 - 0xb0];
  /* 0x1e0 */ uint32_t  alt_mem_k;
  /* 0x1e4 */ uint8_t   pad4[4];
  /* 0x1e8 */ uint8_t   e820map_entries;
  /* 0x1e9 */ uint8_t   eddbuf_entries; // EDD_NR
  /* 0x1ea */ uint8_t   pad5[0x1f1 - 0x1ea];
  /* 0x1f1 */ uint8_t   setup_sects; // size of setup.S, number of sectors
  /* 0x1f2 */ uint16_t  mount_root_rdonly; // MOUNT_ROOT_RDONLY (if !=0)
  /* 0x1f4 */ uint16_t  sys_size; // size of compressed kernel-part in the
                                // (b)zImage-file (in 16 byte units, rounded up)
  /* 0x1f6 */ uint16_t  swap_dev; // (unused AFAIK)
  /* 0x1f8 */ uint16_t  ramdisk_flags;
  /* 0x1fa */ uint16_t  vga_mode; // (old one)
  /* 0x1fc */ uint16_t  orig_root_dev; // (high=Major, low=minor)
  /* 0x1fe */ uint8_t   pad6[1];
  /* 0x1ff */ uint8_t   aux_device_info;
  /* 0x200 */ uint16_t  jump_setup; // Jump to start of setup code,
                                  // aka "reserved" field.
  /* 0x202 */ uint8_t   setup_signature[4]; // Signature for SETUP-header, ="HdrS"
  /* 0x206 */ uint16_t  header_format_version; // Version number of header format;
  /* 0x208 */ uint8_t   setup_S_temp0[8]; // Used by setup.S for communication with
                                        // boot loaders, look there.
  /* 0x210 */ uint8_t   loader_type;
                        // 0 for old one.
                        // else 0xTV:
                        //   T=0: LILO
                        //   T=1: Loadlin
                        //   T=2: bootsect-loader
                        //   T=3: SYSLINUX
                        //   T=4: ETHERBOOT
                        //   V=version
  /* 0x211 */ uint8_t   loadflags;
                        // bit0 = 1: kernel is loaded high (bzImage)
                        // bit7 = 1: Heap and pointer (see below) set by boot
                        //   loader.
  /* 0x212 */ uint16_t  setup_S_temp1;
  /* 0x214 */ uint32_t  kernel_start;
  /* 0x218 */ uint32_t  initrd_start;
  /* 0x21c */ uint32_t  initrd_size;
  /* 0x220 */ uint8_t   setup_S_temp2[4];
  /* 0x224 */ uint16_t  setup_S_heap_end_pointer;
  /* 0x226 */ uint16_t  pad70;
  /* 0x228 */ uint32_t  cmd_line_ptr;
  /* 0x22c */ uint8_t   pad7[0x2d0 - 0x22c];

  /* 0x2d0 : Int 15, ax=e820 memory map. */
  // (linux/include/asm-i386/e820.h, 'struct e820entry')
#define E820MAX  32
#define E820_RAM  1
#define E820_RESERVED 2
#define E820_ACPI 3 /* usable as RAM once ACPI tables have been read */
#define E820_NVS  4
  struct __attribute__ ((packed)) {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
    } e820map[E820MAX];

  /* 0x550 */ uint8_t   pad8[0x600 - 0x550];

  // BIOS Enhanced Disk Drive Services.
  // (From linux/include/asm-i386/edd.h, 'struct edd_info')
  // Each 'struct edd_info is 78 bytes, times a max of 6 structs in array.
  /* 0x600 */ uint8_t   eddbuf[0x7d4 - 0x600];

  /* 0x7d4 */ uint8_t   pad9[0x800 - 0x7d4];
  /* 0x800 */ uint8_t   commandline[0x800];

  uint64_t gdt_table[4];
};

static_assert(offsetof(struct linux_params, commandline) == 0x800,
              "the boot parameters do not match the boot protocol");

#define KERNEL_PARAMS_ADDR 0x00090000

static void copy_kernel(PCMachine *s, const uint8_t *buf, int buf_len,
                        const char *cmd_line, uint32_t bios_size)
{
    uint8_t *ram_ptr;
    int setup_sects, header_len, copy_len, setup_hdr_start, setup_hdr_end;
    uint32_t load_address;
    struct linux_params *params;
    FBDevice *fb_dev;
    
    if (buf_len < 1024) {
    too_small:
        fprintf(stderr, "Kernel too small\n");
        exit(1);
    }
    if (buf[0x1fe] != 0x55 || buf[0x1ff] != 0xaa) {
        fprintf(stderr, "Invalid kernel magic\n");
        exit(1);
    }
    setup_sects = buf[0x1f1];
    if (setup_sects == 0)
        setup_sects = 4;
    header_len = (setup_sects + 1) * 512;
    if (buf_len < header_len)
        goto too_small;
    if (memcmp(buf + 0x202, "HdrS", 4) != 0) {
        fprintf(stderr, "Kernel too old\n");
        exit(1);
    }
    load_address = 0x100000; /* we don't support older protocols */

    ram_ptr = get_ram_ptr(s, load_address);
    params = reinterpret_cast<struct linux_params *>(
        get_ram_ptr(s, KERNEL_PARAMS_ADDR));
    if (ram_ptr == NULL || params == NULL) {
        fprintf(stderr, "No RAM to load the kernel into\n");
        exit(1);
    }
    copy_len = buf_len - header_len;
    if (copy_len > (s->fLowRamSize - load_address)) {
        fprintf(stderr, "Not enough RAM\n");
        exit(1);
    }
    memcpy(ram_ptr, buf + header_len, copy_len);

    memset(params, 0, sizeof(struct linux_params));

    /* copy the setup header */
    setup_hdr_start = 0x1f1;
    setup_hdr_end = 0x202 + buf[0x201];
    memcpy((uint8_t *)params + setup_hdr_start, buf + setup_hdr_start,
           setup_hdr_end - setup_hdr_start);

    strcpy((char *)params->commandline, cmd_line);

    params->mount_root_rdonly = 0;
    params->cmd_line_ptr = KERNEL_PARAMS_ADDR +
        offsetof(struct linux_params, commandline);
    params->alt_mem_k = (s->fLowRamSize / 1024) - 1024;
    params->loader_type = 0x01;

    PcMemoryRange ranges[PC_MEMORY_RANGES_MAX];
    int n = pc_memory_ranges(s, bios_size, ranges);
    for (int i = 0; i < n; i++) {
        params->e820map[i].addr = ranges[i].addr;
        params->e820map[i].size = ranges[i].size;
        params->e820map[i].type = ranges[i].type;
    }
    params->e820map_entries = n;
#if 0
    if (initrd_size > 0) {
        params->initrd_start = INITRD_LOAD_ADDR;
        params->initrd_size = initrd_size;
    }
#endif
    params->orig_video_lines = 0;
    params->orig_video_cols = 0;

    fb_dev = s->fb_dev;
    if (fb_dev) {
        
        params->orig_video_isVGA = 0x23; /* VIDEO_TYPE_VLFB */

        params->lfb_depth = 32;
        params->red_size = 8;
        params->red_pos = 16;
        params->green_size = 8;
        params->green_pos = 8;
        params->blue_size = 8;
        params->blue_pos = 0;
        params->rsvd_size = 8;
        params->rsvd_pos = 24;

        params->lfb_width = fb_dev->width;
        params->lfb_height = fb_dev->height;
        params->lfb_linelength = fb_dev->stride;
        params->lfb_size = fb_dev->fb_size;
        params->lfb_base = s->fb_base;
    }
    
    set_flat_gdt((uint8_t *)params + offsetof(struct linux_params, gdt_table));
    start_flat_protected_mode(s, KERNEL_PARAMS_ADDR +
                              offsetof(struct linux_params, gdt_table),
                              load_address, 6, KERNEL_PARAMS_ADDR); /* esi */
    map_pci_interrupts(s);
}

/* The GDT a kernel entered in flat protected mode starts with: code in
   entry 2 and data in entry 3. */
#define BOOT_GDT_ENTRIES 4

static void set_flat_gdt(uint8_t *gdt)
{
    put_le64(gdt, 0);
    put_le64(gdt + 8, 0);
    put_le64(gdt + 16, 0x00cf9b000000ffffULL); /* CS */
    put_le64(gdt + 24, 0x00cf93000000ffffULL); /* DS */
}

/* Starts the processor at 'entry' in 32 bit protected mode with paging off,
   the segments flat from the GDT at 'gdt_addr', and general register 'reg'
   holding 'reg_val'. */
static void start_flat_protected_mode(PCMachine *s, uint32_t gdt_addr,
                                      uint32_t entry, int reg,
                                      uint32_t reg_val)
{
    uint16_t gdt_limit = BOOT_GDT_ENTRIES * 8 - 1;
    X86Vcpu &vcpu = s->fCpus->Vcpu(0);
    X86Regs regs;

    vcpu.SetFlatProtectedMode(gdt_addr, gdt_limit, 2 << 3, 3 << 3);

    memset(&regs, 0, sizeof(regs));
    regs.rip = entry;
    regs.gpr[reg] = reg_val;
    regs.rflags = 0x2;
    vcpu.SetRegs(regs);
}

/* With no firmware to program the PIRQ registers, the loader routes the
   INTx lines itself. */
static void map_pci_interrupts(PCMachine *s)
{
    uint8_t elcr[2];

    if (s->i440fx_state == nullptr)
        return;
    i440fx_map_interrupts(s->i440fx_state, elcr);
    /* XXX: hypervisor support */
    if (s->pic_state) {
        s->pic_state->SetElcr(elcr);
    }
}

static void pc_acpi_setup(PCMachine *s)
{
    PcAcpiConfig config;
    uint8_t *mem = get_ram_range_ptr(s, PC_ACPI_ADDR, PC_ACPI_SIZE);

    config.i8042 = s->port_map->FindRange(0x64) != nullptr;
    config.apic = s->fLocalApic;
    config.cpu_count = s->fCpuCount;
    config.hpet_block_id = s->fHpet ? s->fHpet->BlockId() : 0;
    AcpiContext ctx(&config.sb, "\\_SB");
    pc_acpi_chipset(ctx);
    s->bus->BuildAMLAll(ctx);
    assert(mem != NULL);
    memset(mem, 0, PC_ACPI_SIZE);
    pc_acpi_build(mem, config);

    s->fAcpiPm = std::make_unique<AcpiPmBlock>(*s);
    s->port_map->RegisterDevice(ACPI_PM_BASE, ACPI_PM_SIZE, s->fAcpiPm.get(),
                                DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32);
}

/* PVH boot: an ELF kernel with a PHYS32_ENTRY note is entered there in flat
   32 bit protected mode, paging off, with EBX pointing at an hvm_start_info
   (Xen's arch/x86/hvm/start_info.h). */

#define XEN_HVM_START_MAGIC 0x336ec578
#define XEN_ELFNOTE_PHYS32_ENTRY 18

struct HvmStartInfo {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t nr_modules;
    uint64_t modlist_paddr;
    uint64_t cmdline_paddr;
    uint64_t rsdp_paddr;
    uint64_t memmap_paddr;
    uint32_t memmap_entries;
    uint32_t reserved;
};

struct HvmModlistEntry {
    uint64_t paddr;
    uint64_t size;
    uint64_t cmdline_paddr;
    uint64_t reserved;
};

struct HvmMemmapEntry {
    uint64_t addr;
    uint64_t size;
    uint32_t type;
    uint32_t reserved;
};

#define PVH_MEMMAP_MAX 8
#define PVH_CMDLINE_SIZE 2048

/* Everything the kernel is handed besides itself and the initrd, in the page
   below the VGA hole, which the memory map reports as reserved. */
struct PvhBootData {
    HvmStartInfo start_info;
    HvmModlistEntry modules[1];
    HvmMemmapEntry memmap[PVH_MEMMAP_MAX];
    uint64_t gdt[BOOT_GDT_ENTRIES];
    char cmdline[PVH_CMDLINE_SIZE];
};

#define PVH_BOOT_DATA_ADDR 0x9f000
static_assert(sizeof(PvhBootData) <= 0xa0000 - PVH_BOOT_DATA_ADDR,
              "the PVH boot data does not fit below the VGA hole");
static_assert(PC_MEMORY_RANGES_MAX <= PVH_MEMMAP_MAX,
              "the memory map does not fit in the PVH boot data");

/* The RAM there is, less the boot data page, the VGA hole and the BIOS
   area. A kernel booted without firmware is given this map. The VGA hole is
   a gap, as a BIOS leaves it, not reserved: the PCI root decodes it. */
static int pc_memory_ranges(PCMachine *s, uint32_t bios_size,
                            PcMemoryRange *ranges)
{
    int n = 0;
    auto add = [ranges, &n](uint64_t addr, uint64_t size, uint32_t type) {
        assert(n < PC_MEMORY_RANGES_MAX);
        ranges[n++] = {addr, size, type};
    };
    add(0, PVH_BOOT_DATA_ADDR, E820_RAM);
    add(PVH_BOOT_DATA_ADDR, 0xa0000 - PVH_BOOT_DATA_ADDR, E820_RESERVED);
    add(0xc0000, 0x100000 - 0xc0000, E820_RESERVED);
    add(0x100000, s->fLowRamSize - 0x100000, E820_RAM);
    if (bios_size != 0)
        add(PC_HIGH_RAM_BASE - bios_size, bios_size, E820_RESERVED);
    if (s->fHighRamSize != 0)
        add(PC_HIGH_RAM_BASE, s->fHighRamSize, E820_RAM);
    return n;
}

#define ELF_CLASS32 1
#define ELF_CLASS64 2
#define ELF_EM_386 3
#define ELF_EM_X86_64 62
#define ELF_PT_LOAD 1
#define ELF_PT_NOTE 4

static bool is_elf(const uint8_t *buf, int buf_len)
{
    return buf_len >= 4 && memcmp(buf, "\x7f" "ELF", 4) == 0;
}

/* Guest RAM [addr, addr + len) when one writable RAM range holds all of it,
   otherwise NULL. */
static uint8_t *get_ram_range_ptr(PCMachine *s, uint64_t addr, uint64_t len)
{
    PhysMemoryRange *pr = s->mem_map->FindRange(addr);

    if (!pr || !pr->is_ram || (pr->devram_flags & DEVRAM_FLAG_ROM) ||
        len > pr->addr + pr->size - addr)
        return NULL;
    return pr->phys_mem + (uintptr_t)(addr - pr->addr);
}

/* The PHYS32_ENTRY value in the notes 'buf' holds, if any. */
static bool pvh_find_entry(const uint8_t *buf, uint64_t len, uint32_t *entry)
{
    uint64_t pos = 0;

    while (len - pos >= 12) {
        uint32_t namesz = get_le32(buf + pos);
        uint32_t descsz = get_le32(buf + pos + 4);
        uint32_t type = get_le32(buf + pos + 8);
        uint64_t name = pos + 12;
        uint64_t desc = name + ((namesz + 3) & ~3ULL);

        if (desc > len || ((descsz + 3) & ~3ULL) > len - desc)
            return false;
        if (namesz == 4 && memcmp(buf + name, "Xen", 4) == 0 &&
            type == XEN_ELFNOTE_PHYS32_ENTRY && descsz >= 4) {
            *entry = get_le32(buf + desc);
            return true;
        }
        pos = desc + ((descsz + 3) & ~3ULL);
    }
    return false;
}

static bool pvh_load(PCMachine *s, const uint8_t *buf, int buf_len,
                     const uint8_t *initrd, int initrd_len,
                     const char *cmd_line, uint32_t bios_size,
                     uint64_t rsdp)
{
    bool elf64;
    uint64_t phoff;
    int phentsize, phnum, machine;
    uint64_t kernel_end = 0;
    uint32_t entry;
    bool has_entry = false;

    if (buf_len < 0x40 || buf[5] != 1 /* little endian */ ||
        (buf[4] != ELF_CLASS32 && buf[4] != ELF_CLASS64)) {
        vm_error("pc: the kernel is not a little endian ELF file\n");
        return false;
    }
    elf64 = buf[4] == ELF_CLASS64;
    machine = get_le16(buf + 18);
    /* The PVH entry is 32 bit whatever the class, and a 64 bit image may
       say i386 for it. */
    if (machine != ELF_EM_386 && machine != ELF_EM_X86_64) {
        vm_error("pc: the kernel is not an x86 ELF file\n");
        return false;
    }
    if (elf64) {
        phoff = get_le64(buf + 32);
        phentsize = get_le16(buf + 54);
        phnum = get_le16(buf + 56);
    } else {
        phoff = get_le32(buf + 28);
        phentsize = get_le16(buf + 42);
        phnum = get_le16(buf + 44);
    }
    if (phentsize < (elf64 ? 56 : 32) ||
        phoff > (uint64_t)buf_len ||
        (uint64_t)phnum * phentsize > buf_len - phoff) {
        vm_error("pc: the kernel's program headers are truncated\n");
        return false;
    }

    for (int i = 0; i < phnum; i++) {
        const uint8_t *ph = buf + phoff + (uint64_t)i * phentsize;
        uint32_t type = get_le32(ph);
        uint64_t offset, paddr, filesz, memsz;

        if (elf64) {
            offset = get_le64(ph + 8);
            paddr = get_le64(ph + 24);
            filesz = get_le64(ph + 32);
            memsz = get_le64(ph + 40);
        } else {
            offset = get_le32(ph + 4);
            paddr = get_le32(ph + 12);
            filesz = get_le32(ph + 16);
            memsz = get_le32(ph + 20);
        }
        if (offset > (uint64_t)buf_len || filesz > buf_len - offset) {
            vm_error("pc: a kernel segment is past the end of the file\n");
            return false;
        }
        if (type == ELF_PT_NOTE && !has_entry) {
            has_entry = pvh_find_entry(buf + offset, filesz, &entry);
        } else if (type == ELF_PT_LOAD && memsz != 0) {
            uint8_t *ptr = NULL;

            /* below 1 MB is the boot data and the legacy areas */
            if (filesz <= memsz && paddr >= 0x100000)
                ptr = get_ram_range_ptr(s, paddr, memsz);
            if (ptr == NULL) {
                vm_error("pc: no RAM for the kernel segment at 0x%" PRIx64
                         "-0x%" PRIx64 "\n", paddr, paddr + memsz);
                return false;
            }
            memcpy(ptr, buf + offset, filesz);
            memset(ptr + filesz, 0, memsz - filesz);
            kernel_end = std::max(kernel_end, paddr + memsz);
        }
    }
    if (!has_entry) {
        vm_error("pc: the ELF kernel has no PVH entry point\n");
        return false;
    }

    PvhBootData *bd = reinterpret_cast<PvhBootData *>(
        get_ram_range_ptr(s, PVH_BOOT_DATA_ADDR, sizeof(PvhBootData)));
    assert(bd != NULL);
    memset(bd, 0, sizeof(*bd));
    HvmStartInfo *si = &bd->start_info;
    si->magic = XEN_HVM_START_MAGIC;
    si->version = 1;
    si->rsdp_paddr = rsdp;

    if (strlen(cmd_line) >= sizeof(bd->cmdline)) {
        vm_error("pc: the kernel command line is longer than %d bytes\n",
                 PVH_CMDLINE_SIZE - 1);
        return false;
    }
    strcpy(bd->cmdline, cmd_line);
    si->cmdline_paddr = PVH_BOOT_DATA_ADDR + offsetof(PvhBootData, cmdline);

    /* the initrd at the top of the RAM below 4 GB */
    if (initrd_len > 0) {
        uint64_t addr = (s->fLowRamSize - initrd_len) & ~(uint64_t)0xfff;
        uint8_t *ptr = NULL;

        if ((uint64_t)initrd_len < s->fLowRamSize &&
            addr >= ((kernel_end + 0xfff) & ~(uint64_t)0xfff))
            ptr = get_ram_range_ptr(s, addr, initrd_len);
        if (ptr == NULL) {
            vm_error("pc: not enough RAM above the kernel for the initrd\n");
            return false;
        }
        memcpy(ptr, initrd, initrd_len);
        bd->modules[0].paddr = addr;
        bd->modules[0].size = initrd_len;
        si->nr_modules = 1;
        si->modlist_paddr = PVH_BOOT_DATA_ADDR +
            offsetof(PvhBootData, modules);
    }

    PcMemoryRange ranges[PC_MEMORY_RANGES_MAX];
    int n = pc_memory_ranges(s, bios_size, ranges);
    for (int i = 0; i < n; i++) {
        bd->memmap[i].addr = ranges[i].addr;
        bd->memmap[i].size = ranges[i].size;
        bd->memmap[i].type = ranges[i].type;
    }
    si->memmap_paddr = PVH_BOOT_DATA_ADDR + offsetof(PvhBootData, memmap);
    si->memmap_entries = n;

    set_flat_gdt(reinterpret_cast<uint8_t *>(bd->gdt));
    start_flat_protected_mode(s, PVH_BOOT_DATA_ADDR +
                              offsetof(PvhBootData, gdt),
                              entry, 3, PVH_BOOT_DATA_ADDR); /* ebx */
    map_pci_interrupts(s);
    return true;
}

void PCMachine::StartProcessors()
{
    for (int i = 0; i < fCpuCount; i++)
        fVcpuThreads.emplace_back([this, i]() {VcpuThread(i);});
}

void PCMachine::JoinProcessors()
{
    for (std::thread &thread : fVcpuThreads)
        thread.join();
    fVcpuThreads.clear();
}

static void name_cpu_thread(int index)
{
    char name[16];

    snprintf(name, sizeof(name), "cpu%d", index);
    host_set_thread_name(name);
}

/* An INIT stops the processor, and then the first processor starts over
   from the reset vector, the others at their next STARTUP, which may
   already be here. */
bool PCMachine::TakeCpuStart(int index)
{
    CpuStart &start = fCpuStarts[index];
    X86Vcpu &vcpu = fCpus->Vcpu(index);

    if (start.init.exchange(false)) {
        vcpu.Init();
        start.waiting = index != 0;
    }
    int vector = start.startup.exchange(-1);
    if (vector >= 0 && start.waiting) {
        vcpu.Startup(vector);
        start.waiting = false;
    }
    return !start.waiting;
}

void PCMachine::VcpuThread(int index)
{
    X86Vcpu &vcpu = fCpus->Vcpu(index);

    name_cpu_thread(index);
    sCurrentVcpu = index;
    vcpu.ThreadStarted();
    while (!StopRequested()) {
        bool started = !fCpuStarts || TakeCpuStart(index);
        if (!started || vcpu.Idle(InterruptRequested())) {
            fVcpuWakeups[index].Wait(-1);
            continue;
        }
        vcpu.Run();
    }
}

void PCMachine::KickVcpu(int index)
{
    fCpus->Vcpu(index).InterruptRun();
    fVcpuWakeups[index].Kick();
}

/* The sooner of two delays, either of which may be -1 for none. */
static int64_t sooner(int64_t a, int64_t b)
{
    return a < 0 || (b >= 0 && b < a) ? b : a;
}

/* The HPET's legacy replacement mode takes the PIT's and the RTC's lines
   over, except from a PIT the hypervisor has. */
int64_t PCMachine::RunTimers()
{
    PCMachine *s = this;
    bool legacy = s->fHpet && s->fHpet->LegacyReplacement();
    int64_t delay = s->cmos_state->RunTimers(!legacy);

    if (s->fHpet)
        delay = sooner(delay, s->fHpet->RunTimers());
    /* the hypervisor has the PIT */
    if (!s->fHypervisorIrqchip)
        delay = sooner(delay, s->pit_state->RunTimers(!legacy));
    if (s->fLapics)
        delay = sooner(delay, s->fLapics->RunTimers());
    return delay;
}

void PCMachine::InterruptExecution()
{
    for (int i = 0; i < fCpuCount; i++)
        KickVcpu(i);
}

class PcMachineClass final: public VirtMachineClass {
public:
    const char *MachineNames() const override {return "pc";}

    void SetDefaults(VirtMachineParams *p) const override
    {
        p->accel_enable = true;
    }

    std::unique_ptr<VirtMachine> Init(const VirtMachineParams *p) const override
    {
        return pc_machine_init(p);
    }
};

static const PcMachineClass sPcMachineClass;
const VirtMachineClass &gPcMachineClass = sPcMachineClass;
