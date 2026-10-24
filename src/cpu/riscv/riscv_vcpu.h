/*
 * The RISC-V harts as the machine runs them: in hardware or interpreted
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
#include <string>
#include <vector>

#include "riscv_cpu.h"

class DeviceLock;
class PhysMemoryMap;


/* satp MODE values */
#define RISCV_SATP_SV32 1
#define RISCV_SATP_SV39 8
#define RISCV_SATP_SV48 9
#define RISCV_SATP_SV57 10


/* What every hart implements, as the device tree describes it. */
struct RISCVHartIsa {
    /* the single letter extensions, as misa holds them */
    uint32_t misa = 0;
    /* the multi-letter ones, in the order the tree lists them */
    std::vector<std::string> extensions;
    /* the widest address translation, as satp's MODE names it */
    int satp_mode = RISCV_SATP_SV39;
    /* the bytes a Zicbom or Zicboz instruction covers, 0 if not given */
    uint32_t cbom_block_size = 0;
    uint32_t cboz_block_size = 0;
};


/* What the machine needs from the harts, fixed when their back end is
   opened. */
struct RISCVCpuOptions {
    /* 32, 64 or 128 */
    int xlen = 64;
    /* harts, numbered from 0 */
    int hart_count = 1;
    RISCVInterruptArch interrupt_arch = RISCV_INTR_BASE;
    /* Firmware runs in M-mode. Without, the harts start in S-mode and the
       back end answers their SBI calls, passing the ones RISCVCpuTarget
       lists to the machine. */
    bool machine_mode = true;
    /* the time counter follows host time, rather than the instructions
       run, where the back end can choose */
    bool rtc_real_time = true;
};


/* The machine half. Called with the device lock held. */
class RISCVCpuTarget {
public:
    virtual ~RISCVCpuTarget() = default;

    /* An SBI call of an extension the machine answers whatever runs the
       harts: the debug console, and the legacy putchar and getchar. a[]
       holds a0 to a7, zero extended from 'xlen' bits; a[0] and a[1] are
       written back, only a[0] for a legacy call. False if the call is not
       one of these. */
    virtual bool SbiCall(int hart, int xlen, uint64_t a[8]) = 0;
    /* A system reset or shutdown the guest asked for. */
    virtual void SystemReset(bool reboot, bool failure) = 0;
    /* A trap nothing in the guest is there to take; the hart stops. */
    virtual void HartFault(int hart, uint64_t cause, uint64_t pc,
                           uint64_t tval) = 0;

    /* These are called without the lock, from any thread. */

    /* Ends the wait of 'hart' after Idle(), for it to look at what was
       posted to it. */
    virtual void WakeHart(int hart) = 0;
    /* A timer of the back end's moved earlier, to 'deadline' on the time
       counter, which the machine's timers wake the hart for. */
    virtual void TimerChanged(uint64_t deadline) = 0;
    /* The harts' threads are ending, so nothing posted is taken in any
       more. */
    virtual bool Stopping() = 0;
};


/* One hart, run on a host thread of its own.

   ThreadStarted(), Run() and Idle() are called on that thread without the
   device lock. InterruptRun(), SetIrqLine() and ImsicSetPending() may be
   called from any thread; whoever calls them then wakes the hart. */
class RISCVVcpu {
public:
    virtual ~RISCVVcpu() = default;

    /* Before the first Run(). */
    virtual void ThreadStarted() = 0;
    /* Runs until the hart waits for an interrupt or InterruptRun(). */
    virtual void Run() = 0;
    /* Whether the hart waits for an interrupt, or for a start while
       stopped. Always false when the hypervisor itself waits. */
    virtual bool Idle() = 0;
    /* Makes Run() return soon. */
    virtual void InterruptRun() = 0;

    /* The level of the interrupt lines in 'mask' that a device drives:
       MSIP, MTIP, and MEIP and SEIP unless an IMSIC drives those. Without
       M-mode only SEIP. */
    virtual void SetIrqLine(uint32_t mask, bool level) = 0;
    /* A write of 'id' to the seteipnum register of the hart's machine or
       supervisor level IMSIC interrupt file. */
    virtual void ImsicSetPending(bool supervisor, uint32_t id) = 0;
    /* When on the time counter a timer the back end keeps for the hart
       falls due, UINT64_MAX for none or one the hypervisor keeps itself.
       The machine then interrupts the run and wakes the hart. */
    virtual uint64_t TimerDeadline() = 0;

    /* Only without M-mode, before the hart runs: starts it in S-mode at
       'entry', with a0 holding its hart id and a1 'opaque'. The others wait
       stopped for an SBI start. */
    virtual void Start(uint64_t entry, uint64_t opaque) = 0;
};


/* What runs the machine's harts: a host hypervisor or the interpreter.
   These methods are called with the device lock held. */
class RISCVCpuBackend {
public:
    virtual ~RISCVCpuBackend() = default;

    /* Guest physical memory, which the machine fills with RAM and devices.
       It is the back end's because the harts reach RAM through it without
       the machine. */
    virtual PhysMemoryMap &MemoryMap() = 0;

    /* Hart 'index', below RISCVCpuOptions::hart_count. */
    virtual RISCVVcpu &Vcpu(int index) = 0;

    virtual const RISCVHartIsa &Isa() = 0;

    /* The time counter the 'time' CSR reads and every timer runs on. Any
       thread. */
    virtual uint64_t Time() = 0;
    /* mtime written: the counter goes on from 'time'. With the device
       lock held. */
    virtual void SetTime(uint64_t time) = 0;
    virtual uint64_t TimebaseFrequency() = 0;
    /* Whether the counter follows host time. */
    virtual bool RealTimeClock() = 0;
};


/* nullptr if 'options.xlen' is not one this build interprets. */
std::unique_ptr<RISCVCpuBackend> riscv_interp_open(
    RISCVCpuTarget &target, DeviceLock &lock, const RISCVCpuOptions &options);
