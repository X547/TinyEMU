#pragma once

// The CSRs by number, so that the assembler needs no H extension.
#define TVMM_CSR_SSTATUS    0x100
#define TVMM_CSR_SIE        0x104
#define TVMM_CSR_STVEC      0x105
#define TVMM_CSR_SCOUNTEREN 0x106
#define TVMM_CSR_SENVCFG    0x10a
#define TVMM_CSR_SSCRATCH   0x140
#define TVMM_CSR_SEPC       0x141
#define TVMM_CSR_SCAUSE     0x142
#define TVMM_CSR_STVAL      0x143
#define TVMM_CSR_SIP        0x144

#define TVMM_CSR_VSSTATUS   0x200
#define TVMM_CSR_VSIE       0x204
#define TVMM_CSR_VSTVEC     0x205
#define TVMM_CSR_VSSCRATCH  0x240
#define TVMM_CSR_VSEPC      0x241
#define TVMM_CSR_VSCAUSE    0x242
#define TVMM_CSR_VSTVAL     0x243
#define TVMM_CSR_VSIP       0x244
#define TVMM_CSR_VSATP      0x280

#define TVMM_CSR_HSTATUS    0x600
#define TVMM_CSR_HEDELEG    0x602
#define TVMM_CSR_HIDELEG    0x603
#define TVMM_CSR_HIE        0x604
#define TVMM_CSR_HTIMEDELTA 0x605
#define TVMM_CSR_HCOUNTEREN 0x606
#define TVMM_CSR_HGEIE      0x607
#define TVMM_CSR_HENVCFG    0x60a
#define TVMM_CSR_HTVAL      0x643
#define TVMM_CSR_HIP        0x644
#define TVMM_CSR_HVIP       0x645
#define TVMM_CSR_HTINST     0x64a
#define TVMM_CSR_HGATP      0x680

#define TVMM_CSR_TIME       0xc01


#define TVMM_STRINGIFY_(x) #x
#define TVMM_STRINGIFY(x) TVMM_STRINGIFY_(x)


#ifdef __cplusplus

#include <SupportDefs.h>

#define TVMM_CSR_READ(csr) ({ \
	uint64 _value; \
	asm volatile("csrr %0, " TVMM_STRINGIFY(csr) : "=r" (_value) :: "memory"); \
	_value; \
})

#define TVMM_CSR_WRITE(csr, value) \
	asm volatile("csrw " TVMM_STRINGIFY(csr) ", %0" :: "r" (uint64(value)) : "memory")

#define TVMM_CSR_SET(csr, value) \
	asm volatile("csrs " TVMM_STRINGIFY(csr) ", %0" :: "r" (uint64(value)) : "memory")

#define TVMM_CSR_CLEAR(csr, value) \
	asm volatile("csrc " TVMM_STRINGIFY(csr) ", %0" :: "r" (uint64(value)) : "memory")


namespace RiscV {


// sstatus and vsstatus
constexpr uint64 kStatusSie = 1ull << 1;
constexpr uint64 kStatusSpie = 1ull << 5;
constexpr uint64 kStatusSpp = 1ull << 8;
constexpr uint64 kStatusFsMask = 3ull << 13;
constexpr uint64 kStatusFsInitial = 1ull << 13;

// hstatus
constexpr uint64 kHstatusGva = 1ull << 6;
constexpr uint64 kHstatusSpv = 1ull << 7;
constexpr uint64 kHstatusSpvp = 1ull << 8;
constexpr uint64 kHstatusHu = 1ull << 9;
constexpr uint64 kHstatusVgeinMask = 0x3full << 12;
constexpr uint64 kHstatusVtvm = 1ull << 20;
constexpr uint64 kHstatusVtw = 1ull << 21;
constexpr uint64 kHstatusVtsr = 1ull << 22;
constexpr uint64 kHstatusVsxlShift = 32;
constexpr uint64 kHstatusVsxlMask = 3ull << kHstatusVsxlShift;
constexpr uint64 kXlen64 = 2;

// hgatp
constexpr int kHgatpModeShift = 60;
constexpr int kHgatpVmidShift = 44;
constexpr uint64 kHgatpVmidMask = 0x3fffull << kHgatpVmidShift;
constexpr uint64 kHgatpModeSv39x4 = 8;
constexpr uint64 kHgatpModeSv48x4 = 9;

// henvcfg
constexpr uint64 kEnvcfgCbieFlush = 1ull << 4;
constexpr uint64 kEnvcfgCbcfe = 1ull << 6;
constexpr uint64 kEnvcfgCbze = 1ull << 7;

// the interrupts, by their bit in hip and hvip
constexpr uint64 kIntVsSoft = 1ull << 2;
constexpr uint64 kIntVsTimer = 1ull << 6;
constexpr uint64 kIntVsExternal = 1ull << 10;
constexpr uint64 kIntVsAll = kIntVsSoft | kIntVsTimer | kIntVsExternal;

// counters the guest may read: cycle, time, instret
constexpr uint64 kCounterenBase = 7;

// scause
constexpr uint64 kCauseInterrupt = 1ull << 63;
enum {
	kCauseInsnMisaligned = 0,
	kCauseInsnAccess = 1,
	kCauseIllegalInsn = 2,
	kCauseBreakpoint = 3,
	kCauseLoadMisaligned = 4,
	kCauseLoadAccess = 5,
	kCauseStoreMisaligned = 6,
	kCauseStoreAccess = 7,
	kCauseUserEcall = 8,
	kCauseSupervisorEcall = 9,
	kCauseVsEcall = 10,
	kCauseInsnPageFault = 12,
	kCauseLoadPageFault = 13,
	kCauseStorePageFault = 15,
	kCauseInsnGuestPageFault = 20,
	kCauseLoadGuestPageFault = 21,
	kCauseVirtualInsn = 22,
	kCauseStoreGuestPageFault = 23,
};

// The guest's own exceptions, which go to it without the hypervisor.
constexpr uint64 kGuestExceptions =
	(1ull << kCauseInsnMisaligned) | (1ull << kCauseIllegalInsn)
	| (1ull << kCauseBreakpoint) | (1ull << kCauseLoadMisaligned)
	| (1ull << kCauseStoreMisaligned) | (1ull << kCauseUserEcall)
	| (1ull << kCauseInsnPageFault) | (1ull << kCauseLoadPageFault)
	| (1ull << kCauseStorePageFault);

constexpr uint32 kInsnWfi = 0x10500073;


inline uint64 ReadTime()
{
	uint64 value;
	asm volatile("rdtime %0" : "=r" (value));
	return value;
}


} // RiscV

#endif // __cplusplus
