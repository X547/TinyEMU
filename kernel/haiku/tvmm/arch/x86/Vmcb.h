#pragma once

#include <stddef.h>

#include <SupportDefs.h>


// The SVM virtual machine control block, from the AMD64 Architecture
// Programmer's Manual, volume 2, appendix B.


// VmcbControl::interceptCr
constexpr uint32 kSvmInterceptCr8Write  = 1u << (16 + 8);

// VmcbControl::interceptMisc1
enum {
	SvmInterceptIntr       = 1u << 0,
	SvmInterceptNmi        = 1u << 1,
	SvmInterceptSmi        = 1u << 2,
	SvmInterceptInit       = 1u << 3,
	SvmInterceptVintr      = 1u << 4,
	SvmInterceptCpuid      = 1u << 18,
	SvmInterceptInvd       = 1u << 22,
	SvmInterceptPause      = 1u << 23,
	SvmInterceptHlt        = 1u << 24,
	SvmInterceptInvlpga    = 1u << 26,
	SvmInterceptIoioProt   = 1u << 27,
	SvmInterceptMsrProt    = 1u << 28,
	SvmInterceptShutdown   = 1u << 31,
};


// VmcbControl::interceptMisc2
enum {
	SvmInterceptVmrun      = 1u << 0,
	SvmInterceptVmmcall    = 1u << 1,
	SvmInterceptVmload     = 1u << 2,
	SvmInterceptVmsave     = 1u << 3,
	SvmInterceptStgi       = 1u << 4,
	SvmInterceptClgi       = 1u << 5,
	SvmInterceptSkinit     = 1u << 6,
	SvmInterceptMonitor    = 1u << 10,
	SvmInterceptMwait      = 1u << 11,
	SvmInterceptMwaitCond  = 1u << 12,
	SvmInterceptXsetbv     = 1u << 13,
	SvmInterceptRdpru      = 1u << 14,
};


// VmcbControl::vintr
constexpr uint64 kSvmVintrTprMask       = 0xff;
constexpr uint64 kSvmVintrIrq           = 1ull << 8;
constexpr uint64 kSvmVintrPrioShift     = 16;
constexpr uint64 kSvmVintrIgnoreTpr     = 1ull << 20;
constexpr uint64 kSvmVintrMasking       = 1ull << 24;

// VmcbControl::tlbControl
constexpr uint8 kSvmTlbNothing          = 0;
constexpr uint8 kSvmTlbFlushAll         = 1;
constexpr uint8 kSvmTlbFlushAsid        = 3;

// VmcbControl::eventInj and exitIntInfo
constexpr uint64 kSvmEventValid         = 1ull << 31;
constexpr uint64 kSvmEventErrorValid    = 1ull << 11;
constexpr uint64 kSvmEventTypeShift     = 8;
constexpr uint64 kSvmEventTypeIntr      = 0;
constexpr uint64 kSvmEventTypeNmi       = 2;
constexpr uint64 kSvmEventTypeException = 3;
constexpr uint64 kSvmEventTypeSoftInt   = 4;

// VmcbControl::npControl
constexpr uint64 kSvmNpEnable           = 1ull << 0;


// VmcbControl::exitCode
enum : uint64 {
	SvmExitWriteCr8     = 0x18,
	SvmExitReadDr0      = 0x20,
	SvmExitWriteDr0     = 0x30,
	SvmExitException0   = 0x40,
	SvmExitIntr         = 0x60,
	SvmExitNmi          = 0x61,
	SvmExitSmi          = 0x62,
	SvmExitInit         = 0x63,
	SvmExitVintr        = 0x64,
	SvmExitCpuid        = 0x72,
	SvmExitInvd         = 0x76,
	SvmExitPause        = 0x77,
	SvmExitHlt          = 0x78,
	SvmExitInvlpga      = 0x7a,
	SvmExitIoio         = 0x7b,
	SvmExitMsr          = 0x7c,
	SvmExitShutdown     = 0x7f,
	SvmExitVmrun        = 0x80,
	SvmExitVmmcall      = 0x81,
	SvmExitVmload       = 0x82,
	SvmExitVmsave       = 0x83,
	SvmExitStgi         = 0x84,
	SvmExitClgi         = 0x85,
	SvmExitSkinit       = 0x86,
	SvmExitMonitor      = 0x8a,
	SvmExitMwait        = 0x8b,
	SvmExitMwaitCond    = 0x8c,
	SvmExitXsetbv       = 0x8d,
	SvmExitRdpru        = 0x8e,
	SvmExitNpf          = 0x400,
	SvmExitInvalid      = ~0ull,
};


// IOIO exit: EXITINFO1
constexpr uint64 kSvmIoioIn             = 1ull << 0;
constexpr uint64 kSvmIoioString         = 1ull << 2;
constexpr uint64 kSvmIoioRep            = 1ull << 3;
constexpr uint64 kSvmIoioSizeShift      = 4;
constexpr uint64 kSvmIoioPortShift      = 16;


struct VmcbSegment {
	uint16 selector;
	uint16 attrib;
	uint32 limit;
	uint64 base;
};

static_assert(sizeof(VmcbSegment) == 16);


struct VmcbControl {
	uint32 interceptCr;            // 0x000: reads in 0-15, writes in 16-31
	uint32 interceptDr;            // 0x004
	uint32 interceptExceptions;    // 0x008
	uint32 interceptMisc1;         // 0x00c
	uint32 interceptMisc2;         // 0x010
	uint32 interceptMisc3;         // 0x014
	uint8 reserved1[0x03c - 0x018];
	uint16 pauseFilterThreshold;   // 0x03c
	uint16 pauseFilterCount;       // 0x03e
	uint64 iopmBasePa;             // 0x040
	uint64 msrpmBasePa;            // 0x048
	uint64 tscOffset;              // 0x050
	uint32 asid;                   // 0x058
	uint8 tlbControl;              // 0x05c
	uint8 reserved2[3];
	uint64 vintr;                  // 0x060
	uint64 interruptShadow;        // 0x068
	uint64 exitCode;               // 0x070
	uint64 exitInfo1;              // 0x078
	uint64 exitInfo2;              // 0x080
	uint64 exitIntInfo;            // 0x088
	uint64 npControl;              // 0x090
	uint64 avicApicBar;            // 0x098
	uint64 ghcbPa;                 // 0x0a0
	uint64 eventInj;               // 0x0a8
	uint64 nCr3;                   // 0x0b0
	uint64 virtExt;                // 0x0b8
	uint32 cleanBits;              // 0x0c0
	uint32 reserved3;
	uint64 nextRip;                // 0x0c8
	uint8 insnLength;              // 0x0d0
	uint8 insnBytes[15];           // 0x0d1
	uint64 avicBackingPage;        // 0x0e0
	uint8 reserved4[8];
	uint64 avicLogicalTable;       // 0x0f0
	uint64 avicPhysicalTable;      // 0x0f8
	uint8 reserved5[8];
	uint64 vmsaPa;                 // 0x108
	uint8 reserved6[0x400 - 0x110];
};

static_assert(offsetof(VmcbControl, pauseFilterThreshold) == 0x03c);
static_assert(offsetof(VmcbControl, iopmBasePa) == 0x040);
static_assert(offsetof(VmcbControl, asid) == 0x058);
static_assert(offsetof(VmcbControl, vintr) == 0x060);
static_assert(offsetof(VmcbControl, exitCode) == 0x070);
static_assert(offsetof(VmcbControl, npControl) == 0x090);
static_assert(offsetof(VmcbControl, eventInj) == 0x0a8);
static_assert(offsetof(VmcbControl, nCr3) == 0x0b0);
static_assert(offsetof(VmcbControl, cleanBits) == 0x0c0);
static_assert(offsetof(VmcbControl, nextRip) == 0x0c8);
static_assert(offsetof(VmcbControl, insnLength) == 0x0d0);
static_assert(offsetof(VmcbControl, avicBackingPage) == 0x0e0);
static_assert(offsetof(VmcbControl, vmsaPa) == 0x108);
static_assert(sizeof(VmcbControl) == 0x400);


struct VmcbSave {
	VmcbSegment es;                // 0x400
	VmcbSegment cs;
	VmcbSegment ss;
	VmcbSegment ds;
	VmcbSegment fs;
	VmcbSegment gs;
	VmcbSegment gdtr;
	VmcbSegment ldtr;
	VmcbSegment idtr;
	VmcbSegment tr;
	uint8 reserved1[0x4cb - 0x4a0];
	uint8 cpl;                     // 0x4cb
	uint32 reserved2;
	uint64 efer;                   // 0x4d0
	uint8 reserved3[0x548 - 0x4d8];
	uint64 cr4;                    // 0x548
	uint64 cr3;
	uint64 cr0;
	uint64 dr7;
	uint64 dr6;
	uint64 rflags;
	uint64 rip;                    // 0x578
	uint8 reserved4[0x5d8 - 0x580];
	uint64 rsp;                    // 0x5d8
	uint64 sCet;
	uint64 ssp;
	uint64 isstAddr;
	uint64 rax;                    // 0x5f8
	uint64 star;                   // 0x600
	uint64 lstar;
	uint64 cstar;
	uint64 sfmask;
	uint64 kernelGsBase;
	uint64 sysenterCs;
	uint64 sysenterEsp;
	uint64 sysenterEip;
	uint64 cr2;                    // 0x640
	uint8 reserved5[0x668 - 0x648];
	uint64 gPat;                   // 0x668
	uint64 dbgCtl;
	uint64 brFrom;
	uint64 brTo;
	uint64 lastExceptionFrom;
	uint64 lastExceptionTo;
	uint8 reserved6[0x6e0 - 0x698];
	uint64 specCtrl;               // 0x6e0
	uint8 reserved7[0x1000 - 0x6e8];
};

static_assert(offsetof(VmcbSave, cpl) == 0x4cb - 0x400);
static_assert(offsetof(VmcbSave, efer) == 0x4d0 - 0x400);
static_assert(offsetof(VmcbSave, cr4) == 0x548 - 0x400);
static_assert(offsetof(VmcbSave, rip) == 0x578 - 0x400);
static_assert(offsetof(VmcbSave, rsp) == 0x5d8 - 0x400);
static_assert(offsetof(VmcbSave, rax) == 0x5f8 - 0x400);
static_assert(offsetof(VmcbSave, star) == 0x600 - 0x400);
static_assert(offsetof(VmcbSave, cr2) == 0x640 - 0x400);
static_assert(offsetof(VmcbSave, gPat) == 0x668 - 0x400);
static_assert(offsetof(VmcbSave, specCtrl) == 0x6e0 - 0x400);


struct Vmcb {
	VmcbControl control;
	VmcbSave save;
};

static_assert(sizeof(Vmcb) == 0x1000);
