#pragma once

#include <SupportDefs.h>


// What the host processors provide for SVM, and their per-CPU state while a
// VM holds them (Arch::Acquire()).
namespace SvmHost {


uint32 Features();
// TvmmSvmFeature*
bool HasFeature(uint32 feature);

// XCR0 as the host runs with it, and the XSAVE area it needs.
uint64 HostXcr0();
size_t XsaveSize();
// MXCSR bits the processor implements
uint32 MxcsrMask();

// While held: the per-CPU page VMSAVE keeps the host's state in.
phys_addr_t HostSavePage(int32 cpu);

// While held: how far 'cpu's TSC is ahead of CPU 0's. The host's TSCs need
// not agree, but a guest's processors must.
int64 TscSkew(int32 cpu);
// The TSC as CPU 0 has it; any thread.
uint64 ReadTsc0();

// An address space identifier no other vcpu has, 0 when none is left.
uint32 AllocAsid();
void FreeAsid(uint32 asid);


} // SvmHost
