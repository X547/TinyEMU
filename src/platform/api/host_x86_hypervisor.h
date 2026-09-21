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

    /* Whether the interrupt controller raises INTR. */
    virtual bool InterruptRequested() = 0;
    /* Acknowledges the request and returns its vector. */
    virtual int AcknowledgeInterrupt() = 0;
};


/* One processor the host runs in hardware.

   A hypervisor with interrupt controllers of its own provides the 8259s and
   the 8254, takes the lines through SetIRQ() and waits in Run() while the
   processor is halted. One without them takes interrupts from the target, and
   the machine waits while Idle() says so.

   Run(), ProcessorThreadStarted(), Idle() and InterruptRun() are called
   without the device lock; Run() takes it around each call to the target. The
   other methods are called with the lock held. */
class HostX86Hypervisor {
public:
    virtual ~HostX86Hypervisor() = default;

    virtual bool HasInterruptControllers() = 0;

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

    /* SetRegs() leaves the registers HostX86Regs does not hold alone. */
    virtual void GetRegs(HostX86Regs *regs) = 0;
    virtual void SetRegs(const HostX86Regs &regs) = 0;
    /* Protected mode with CS and the data segments covering the whole 4 GB,
       as a boot loader leaves it. */
    virtual void SetFlatProtectedMode(uint32_t gdt_base, uint16_t gdt_limit,
                                      uint16_t code_sel,
                                      uint16_t data_sel) = 0;

    /* On the processor thread, before the first Run(). */
    virtual void ProcessorThreadStarted() = 0;
    /* Runs until an access for the target, a halt, or 'timeout_us'; -1 is
       about 10 ms. */
    virtual void Run(int64_t timeout_us) = 0;
    /* Halted with no interrupt it could take while INTR is at 'intr'. Always
       false with interrupt controllers of its own. */
    virtual bool Idle(bool intr) = 0;
    /* Any thread: makes Run() return soon. */
    virtual void InterruptRun() = 0;
};


/* Implemented once per host; the build picks the file. nullptr when the host
   has no hypervisor, having said why if it should have had one. */
std::unique_ptr<HostX86Hypervisor> host_x86_hypervisor_open(
    X86HypervisorTarget &target, DeviceLock &lock);
