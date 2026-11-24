#pragma once

#include <SupportDefs.h>


// What the driver found the harts to have, which every CPU has alike, and
// the state of each CPU while a VM is held.
namespace RiscVHost {


// hgatp's MODE for the G-stage translation
uint64 HgatpMode();
// the guest's henvcfg: what the guest's extensions need of it
uint64 Henvcfg();
uint64 TimebaseFrequency();

// A number no other vcpu ever has.
int64 NewVcpuId();
// The vcpu whose translations the CPU's TLBs may hold, by NewVcpuId(); 0
// for none. With interrupts disabled, on that CPU.
int64 LoadedVcpu(int32 cpu);
void SetLoadedVcpu(int32 cpu, int64 id);


} // RiscVHost
