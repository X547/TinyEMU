/*
 * Host hypervisor running the x86 processor
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
#pragma once

#include <stddef.h>
#include <stdint.h>

#include <memory>

class DeviceLock;


/* The general registers in encoding order: eax, ecx, edx, ebx, esp, ebp, esi,
   edi. */
struct HostX86Regs {
    uint64_t gpr[8];
    uint64_t rip;
    uint64_t rflags;
};


/* What the machine needs from the hypervisor, fixed when it is opened. */
struct HostX86Options {
    /* local APICs: the hypervisor's, reached by SendMsi(), or the machine's
       when it has none */
    bool local_apic = false;
    /* processors, numbered from 0 */
    int cpu_count = 1;
    /* the widest physical address the processors report in CPUID
       0x80000008, in bits */
    int max_phys_address_bits = 40;
};


/* The machine half: the port and memory accesses the processor makes to
   devices, and the interrupt controller when the hypervisor has none. Called
   with the device lock held. */
class X86HypervisorTarget {
public:
    virtual ~X86HypervisorTarget() = default;

    virtual uint32_t PortRead(uint32_t port, int size_log2) = 0;
    virtual void PortWrite(uint32_t port, uint32_t val, int size_log2) = 0;
    /* Guest physical memory, RAM included for a hypervisor that emulates an
       instruction through here. 'len' is 1, 2, 4 or 8. */
    virtual void MmioRead(uint64_t addr, uint8_t *data, int len) = 0;
    virtual void MmioWrite(uint64_t addr, const uint8_t *data, int len) = 0;

    /* Whether the calling processor has an interrupt to take: the 8259s'
       INTR, which only processor 0 takes, or with the machine's local
       APICs, its own APIC's. */
    virtual bool InterruptRequested() = 0;
    /* Acknowledges the request and returns its vector. */
    virtual int AcknowledgeInterrupt() = 0;

    /* The calling processor's local APIC, when the machine has them:
       IA32_APIC_BASE, false for a value the APIC cannot take, and CR8. */
    virtual uint64_t ApicBase() = 0;
    virtual bool SetApicBase(uint64_t val) = 0;
    virtual int TaskPriority() = 0;
    virtual void SetTaskPriority(int cr8) = 0;

    /* A local APIC the hypervisor emulates ended a level triggered
       interrupt with this vector, for the IOAPIC the machine may have. */
    virtual void ApicEoi(int vector) = 0;
    /* A processor shut down, as on a triple fault; it runs no more. */
    virtual void ProcessorShutdown() = 0;
};


/* One processor the host runs in hardware, on a host thread of its own.

   ThreadStarted(), Run() and Idle() are called on that thread without the
   device lock; Run() takes it around each call to the target. The register
   methods are called with the lock held, either before the processor first
   runs or on its thread. InterruptRun() is called from any thread. */
class HostX86Vcpu {
public:
    virtual ~HostX86Vcpu() = default;

    /* SetRegs() leaves the registers HostX86Regs does not hold alone. */
    virtual void GetRegs(HostX86Regs *regs) = 0;
    virtual void SetRegs(const HostX86Regs &regs) = 0;
    /* Protected mode with CS and the data segments covering the whole 4 GB,
       as a boot loader leaves it. */
    virtual void SetFlatProtectedMode(uint32_t gdt_base, uint16_t gdt_limit,
                                      uint16_t code_sel,
                                      uint16_t data_sel) = 0;

    /* Before the first Run(). */
    virtual void ThreadStarted() = 0;
    /* Runs until an access for the target, a halt, or InterruptRun(). */
    virtual void Run() = 0;
    /* Halted with no interrupt it could take while INTR is at 'intr'. Always
       false when the hypervisor itself waits at HLT. */
    virtual bool Idle(bool intr) = 0;
    /* Makes Run() return soon. */
    virtual void InterruptRun() = 0;

    /* Only with the machine's local APICs, on the processor's thread: an
       INIT, after which the processor is as at reset, and a STARTUP, which
       starts it in real mode at 'vector' << 12. */
    virtual void Init() = 0;
    virtual void Startup(int vector) = 0;
};


/* The machine the processors belong to.

   A hypervisor with interrupt controllers of its own provides the 8259s and
   the 8254 and takes the lines through SetIRQ(). One without them takes
   interrupts from the target, and a processor waits while its Idle() says
   so. These methods are called with the device lock held. */
class HostX86Hypervisor {
public:
    virtual ~HostX86Hypervisor() = default;

    virtual bool HasInterruptControllers() = 0;
    /* Whether it emulates the local APICs HostX86Options::local_apic asks
       for; if not, the machine has them and the processors reach them
       through the target. */
    virtual bool HasLocalApics() = 0;
    /* The physical address width the processors report: the host's, cut to
       HostX86Options::max_phys_address_bits. */
    virtual int PhysAddressBits() = 0;

    /* Memory for guest RAM, zeroed, as host_ram_alloc() gives it. It comes
       from here because a hypervisor may have to set it up before anything
       is written to it; nullptr on failure. */
    virtual uint8_t *AllocRam(size_t size) = 0;
    virtual void FreeRam(uint8_t *ptr, size_t size) = 0;

    /* Guest RAM at 'addr' backed by 'host_mem'. 'slot' names the range from
       then on; mapping it again moves it, and a size of 0 removes it. */
    virtual void MapRam(int slot, uint64_t addr, uint64_t size,
                        uint8_t *host_mem, bool read_only,
                        bool log_dirty) = 0;
    /* The pages of 'slot' written since the last call, one bit per page in
       64 bit words; the log starts over. */
    virtual void GetDirtyLog(int slot, uint32_t *bitmap) = 0;

    /* Only with interrupt controllers of its own. */
    virtual void SetIRQ(int irq, int level) = 0;
    /* Only with local APICs: delivers the interrupt message a write of
       'data' to 'addr' in the 0xfee00000 page describes. Any thread. */
    virtual void SendMsi(uint64_t addr, uint32_t data) = 0;

    /* Processor 'index', below HostX86Options::cpu_count. */
    virtual HostX86Vcpu &Vcpu(int index) = 0;
};


/* Implemented once per host; the build picks the file. nullptr when the host
   has no hypervisor or cannot provide 'options', having said why if it
   should have had one. */
std::unique_ptr<HostX86Hypervisor> host_x86_hypervisor_open(
    X86HypervisorTarget &target, DeviceLock &lock,
    const HostX86Options &options);
