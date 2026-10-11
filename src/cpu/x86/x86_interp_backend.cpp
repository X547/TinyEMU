/*
 * The x86 interpreter as the machine's processors
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

#include <algorithm>
#include <memory>
#include <vector>

#include "device_lock.h"
#include "host_time.h"
#include "iomem.h"
#include "x86_cpu.h"
#include "x86_cpu_priv.h"


/* the time stamp counter's rate, counted from host time */
#define TSC_FREQ 100000000


/* One interpreted processor. Devices are the target's; RAM it reaches
   through the back end's memory map. */
class X86InterpVcpu final:
    public X86Vcpu,
    public X86HardIntnoSource,
    public X86TscSource,
    public X86LocalApicTarget,
    public X86ShutdownTarget {
private:
    X86CpuTarget &fTarget;
    DeviceLock &fLock;
    int fIndex;
    X86CPUState *fCpu;

    uint32_t PortRead(uint32_t port, int size_log2)
        {return fTarget.PortRead(port, size_log2);}
    void PortWrite(uint32_t port, uint32_t val, int size_log2)
        {fTarget.PortWrite(port, val, size_log2);}
    DeviceIOAdapter<X86InterpVcpu, &X86InterpVcpu::PortRead,
                    &X86InterpVcpu::PortWrite> fPortIo {*this};

public:
    X86InterpVcpu(X86CpuTarget &target, DeviceLock &lock,
                  const X86CpuOptions &options, PhysMemoryMap &mem_map,
                  int index);
    ~X86InterpVcpu() override;

    X86CPUState *Cpu() {return fCpu;}

    /* X86Vcpu */
    void GetRegs(X86Regs *regs) override;
    void SetRegs(const X86Regs &regs) override;
    void SetFlatProtectedMode(uint32_t gdt_base, uint16_t gdt_limit,
                              uint16_t code_sel, uint16_t data_sel) override;
    void ThreadStarted() override {}
    void Run() override {x86_cpu_run(fCpu);}
    bool Idle(bool intr) override;
    void InterruptRun() override {x86_cpu_request_exit(fCpu);}
    void SetInterruptRequest(bool level) override
        {x86_cpu_set_irq(fCpu, level);}
    void Init() override {x86_cpu_reset(fCpu);}
    void Startup(int vector) override {x86_cpu_startup(fCpu, vector);}

    /* X86HardIntnoSource */
    int HardIntno() override {return fTarget.AcknowledgeInterrupt();}
    /* X86TscSource */
    uint64_t Tsc() override;
    /* X86LocalApicTarget: the processor's index is its APIC ID */
    uint32_t ApicId() override {return fIndex;}
    uint64_t ApicBase() override {return fTarget.ApicBase();}
    bool SetApicBase(uint64_t val) override
        {return fTarget.SetApicBase(val);}
    int TaskPriority() override {return fTarget.TaskPriority();}
    void SetTaskPriority(int val) override {fTarget.SetTaskPriority(val);}
    /* X86ShutdownTarget */
    void ProcessorShutdown() override;
};


/* The interpreter has no interrupt controllers; the machine keeps them
   all. */
class X86InterpBackend final: public X86CpuBackend, public TlbFlushTarget {
private:
    int fPhysAddressBits;
    /* before the processors, which use it until they go */
    PhysMemoryMap fMemoryMap;
    std::vector<std::unique_ptr<X86InterpVcpu>> fVcpus;

public:
    X86InterpBackend(X86CpuTarget &target, DeviceLock &lock,
                     const X86CpuOptions &options);

    bool HasInterruptControllers() override {return false;}
    bool HasLocalApics() override {return false;}
    int PhysAddressBits() override {return fPhysAddressBits;}
    PhysMemoryMap &MemoryMap() override {return fMemoryMap;}
    /* the machine keeps the 8259s and the local APICs, so nothing comes
       here */
    void SetIRQ(int irq, int level) override {abort();}
    void SendMsi(uint64_t addr, uint32_t data) override {abort();}
    X86Vcpu &Vcpu(int index) override {return *fVcpus[index];}

    /* TlbFlushTarget, for the RAM dirty bits */
    void FlushTlbWriteRange(uint8_t *ram_addr, size_t ram_size) override;
};


X86InterpVcpu::X86InterpVcpu(X86CpuTarget &target, DeviceLock &lock,
                             const X86CpuOptions &options,
                             PhysMemoryMap &mem_map, int index):
    fTarget(target), fLock(lock), fIndex(index)
{
    fCpu = x86_cpu_init(&mem_map);
    x86_cpu_set_hard_intno_source(fCpu, this);
    x86_cpu_set_tsc_source(fCpu, this);
    x86_cpu_set_shutdown_target(fCpu, this);
    x86_cpu_set_port_io(fCpu, &fPortIo);
    x86_cpu_set_device_lock(fCpu, &lock);
    x86_cpu_set_smp(fCpu, options.cpu_count > 1);
    if (options.local_apic)
        x86_cpu_set_local_apic(fCpu, this);
}


X86InterpVcpu::~X86InterpVcpu()
{
    x86_cpu_end(fCpu);
}


void X86InterpVcpu::GetRegs(X86Regs *regs)
{
    for (int i = 0; i < 8; i++)
        regs->gpr[i] = fCpu->regs[i];
    regs->rip = fCpu->rip;
    regs->rflags = get_eflags(fCpu);
}


/* Called in the middle of an instruction, as for the VMware backdoor, the
   decoder holds RIP and the flags as it goes, so only a change is
   written. */
void X86InterpVcpu::SetRegs(const X86Regs &regs)
{
    for (int i = 0; i < 8; i++)
        fCpu->regs[i] = regs.gpr[i];
    if (regs.rip != fCpu->rip)
        fCpu->rip = regs.rip;
    if (regs.rflags != get_eflags(fCpu))
        cpu_set_eflags(fCpu, regs.rflags, UINT32_MAX);
}


void X86InterpVcpu::SetFlatProtectedMode(uint32_t gdt_base,
                                         uint16_t gdt_limit,
                                         uint16_t code_sel,
                                         uint16_t data_sel)
{
    X86CPUSeg sd;

    x86_cpu_set_reg(fCpu, X86_CPU_REG_CR0,
                    x86_cpu_get_reg(fCpu, X86_CPU_REG_CR0) | 1); /* CR0_PE */

    sd.base = gdt_base;
    sd.limit = gdt_limit;
    x86_cpu_set_seg(fCpu, X86_CPU_SEG_GDT, &sd);

    sd.base = 0;
    sd.limit = 0xffffffff;
    sd.sel = code_sel;
    sd.flags = 0xc09b; /* code */
    x86_cpu_set_seg(fCpu, X86_CPU_SEG_CS, &sd);

    sd.sel = data_sel;
    sd.flags = 0xc093; /* data */
    for (int i = 0; i < 6; i++) {
        if (i != X86_CPU_SEG_CS)
            x86_cpu_set_seg(fCpu, i, &sd);
    }
}


bool X86InterpVcpu::Idle(bool intr)
{
    /* the processor latched INTR itself */
    (void)intr;
    return x86_cpu_get_power_down(fCpu);
}


uint64_t X86InterpVcpu::Tsc()
{
    uint64_t us = host_monotonic_us();

    return (us / 1000000) * TSC_FREQ + (us % 1000000) * (TSC_FREQ / 1000000);
}


/* From the processor's thread, without the lock. */
void X86InterpVcpu::ProcessorShutdown()
{
    DeviceLocker locker(fLock);
    fTarget.ProcessorShutdown();
}


X86InterpBackend::X86InterpBackend(X86CpuTarget &target, DeviceLock &lock,
                                   const X86CpuOptions &options):
    fPhysAddressBits(std::min(X86_CPU_PHYS_ADDRESS_BITS,
                              options.max_phys_address_bits))
{
    fMemoryMap.SetTlbFlushTarget(this);
    for (int i = 0; i < options.cpu_count; i++) {
        fVcpus.push_back(std::make_unique<X86InterpVcpu>(target, lock, options,
                                                         fMemoryMap, i));
    }
}


void X86InterpBackend::FlushTlbWriteRange(uint8_t *ram_addr, size_t ram_size)
{
    for (std::unique_ptr<X86InterpVcpu> &vcpu : fVcpus)
        x86_cpu_flush_tlb_write_range_ram(vcpu->Cpu(), ram_addr, ram_size);
}


std::unique_ptr<X86CpuBackend> x86_interp_open(X86CpuTarget &target,
                                               DeviceLock &lock,
                                               const X86CpuOptions &options)
{
    return std::make_unique<X86InterpBackend>(target, lock, options);
}
