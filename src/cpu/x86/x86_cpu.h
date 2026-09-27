/*
 * x86 CPU emulator
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
#pragma once

#include "iomem.h"

typedef struct X86CPUState X86CPUState;

/* the physical address width, that of PAE paging */
#define X86_CPU_PHYS_ADDRESS_BITS 36

/* get_reg/set_reg additional constants */
#define X86_CPU_REG_EIP 8
#define X86_CPU_REG_CR0 9
#define X86_CPU_REG_CR2 10

#define X86_CPU_SEG_ES 0
#define X86_CPU_SEG_CS 1
#define X86_CPU_SEG_SS 2
#define X86_CPU_SEG_DS 3
#define X86_CPU_SEG_FS 4
#define X86_CPU_SEG_GS 5
#define X86_CPU_SEG_LDT 6
#define X86_CPU_SEG_TR 7
#define X86_CPU_SEG_GDT 8
#define X86_CPU_SEG_IDT 9

typedef struct {
    uint16_t sel;
    uint16_t flags;
    uint64_t base;
    uint32_t limit;
} X86CPUSeg;

X86CPUState *x86_cpu_init(PhysMemoryMap *mem_map);
void x86_cpu_end(X86CPUState *s);
void x86_cpu_interp(X86CPUState *s, int max_cycles1);
/* any thread */
void x86_cpu_set_irq(X86CPUState *s, bool set);
void x86_cpu_set_reg(X86CPUState *s, int reg, uint32_t val);
uint32_t x86_cpu_get_reg(X86CPUState *s, int reg);
void x86_cpu_set_seg(X86CPUState *s, int seg, const X86CPUSeg *sd);
/* Implemented by the machine, which owns the interrupt controller. */
class X86HardIntnoSource {
public:
    virtual ~X86HardIntnoSource() = default;

    virtual int HardIntno() = 0;
};

/* Implemented by the machine, which owns the time base. */
class X86TscSource {
public:
    virtual ~X86TscSource() = default;

    virtual uint64_t Tsc() = 0;
};

/* Implemented by the machine when the processor has a local APIC, which
   CPUID then reports. Called with the device lock held. */
class X86LocalApicTarget {
public:
    virtual ~X86LocalApicTarget() = default;

    virtual uint32_t ApicId() = 0;
    /* IA32_APIC_BASE; SetApicBase() is false for a value to refuse */
    virtual uint64_t ApicBase() = 0;
    virtual bool SetApicBase(uint64_t val) = 0;
    /* CR8 */
    virtual int TaskPriority() = 0;
    virtual void SetTaskPriority(int val) = 0;
};

void x86_cpu_set_hard_intno_source(X86CPUState *s, X86HardIntnoSource *source);
void x86_cpu_set_local_apic(X86CPUState *s, X86LocalApicTarget *apic);
void x86_cpu_set_tsc_source(X86CPUState *s, X86TscSource *source);
void x86_cpu_set_port_io(X86CPUState *s, DeviceIO *port_io);
/* taken around every device access and interrupt acknowledge */
void x86_cpu_set_device_lock(X86CPUState *s, DeviceLock *lock);
int64_t x86_cpu_get_cycles(X86CPUState *s);
/* Halted with no interrupt that could wake it. */
bool x86_cpu_get_power_down(X86CPUState *s);
void x86_cpu_flush_tlb_write_range_ram(X86CPUState *s,
                                       uint8_t *ram_ptr, size_t ram_size);
