/*
 * tvmm: the interface between the kernel add-on and its clients
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

#include <Drivers.h>
#include <SupportDefs.h>


// Every operation takes one argument structure, passed with its size, and
// returns a status_t. Addresses in the client are carried as uint64.
// The structures have the same layout in the kernel and in the client, which
// the static_asserts below hold to.

constexpr uint32 kTvmmAbiVersion = 6;
constexpr const char *kTvmmDevicePath = "/dev/misc/tvmm";


enum TvmmOp: uint32 {
	TvmmOpGetInfo = B_DEVICE_OP_CODES_END + 1,
	TvmmOpCreateVm,
	TvmmOpAllocRam,
	TvmmOpFreeRam,
	TvmmOpMapRam,
	TvmmOpCreateVcpu,
	TvmmOpRun,
	TvmmOpKick,
	TvmmOpGetState,
	TvmmOpSetState,
	TvmmOpSetCpuid,
	TvmmOpSendMsi,
	TvmmOpResetVcpu,
	TvmmOpGetStats,
};


enum class TvmmArch: uint32 {
	None,
	X86Svm,
	X86Vmx,
	RiscVH,
};


// TvmmInfo::features, x86 SVM
enum {
	TvmmSvmFeatureNpt          = 1 << 0,
	TvmmSvmFeatureNrips        = 1 << 1,
	TvmmSvmFeatureVmcbClean    = 1 << 2,
	TvmmSvmFeatureFlushByAsid  = 1 << 3,
	TvmmSvmFeatureDecodeAssist = 1 << 4,
	TvmmSvmFeaturePauseFilter  = 1 << 5,
	TvmmSvmFeatureAvic         = 1 << 6,
	TvmmSvmFeatureVgif         = 1 << 7,
	TvmmSvmFeatureSpecCtrl     = 1 << 8,
};


struct TvmmInfo {
	uint32 abiVersion;
	TvmmArch arch;
	// why the host cannot run guests, B_OK when it can
	status_t hostStatus;
	uint32 maxVcpus;
	uint32 maxSlots;
	uint32 features;
	// the processor's own description of its virtualization, for diagnostics
	uint32 archId[4];
};


// TvmmCreateVm::flags
enum {
	// x86: the kernel has a local APIC for each vcpu, which handles HLT and
	// starts the processors beyond the first with INIT and STARTUP. The
	// client's 8259s inject around it while it takes ExtINT, and a level
	// triggered EOI exits for the client's IOAPIC.
	TvmmVmLocalApic = 1 << 0,
};


struct TvmmCreateVm {
	uint32 flags;
};


// x86 with TvmmVmLocalApic: the message a write of 'data' to 'address' in
// the 0xfee00000 page describes. Any thread.
struct TvmmSendMsi {
	uint64 address;
	uint32 data;
	uint32 reserved;
};


// RAM the kernel owns, wired, zeroed, and mapped into the caller.
struct TvmmAllocRam {
	uint64 size;
	// out
	uint32 ramId;
	uint32 reserved;
	uint64 address;
};


struct TvmmFreeRam {
	uint32 ramId;
};


enum {
	TvmmMapReadOnly = 1 << 0,
	TvmmMapLogDirty = 1 << 1,
};


// Guest physical memory at 'gpa' backed by 'ramId' from 'offset'. Mapping a
// slot again moves it, and a size of 0 removes it.
struct TvmmMapRam {
	uint32 slot;
	uint32 flags;
	uint64 gpa;
	uint64 size;
	uint32 ramId;
	uint32 reserved;
	uint64 offset;
};


// The vcpu's run page (TvmmRun) is mapped into the caller.
struct TvmmCreateVcpu {
	uint32 index;
	uint32 reserved;
	// out
	uint64 runAddress;
};


// TvmmOpRun, TvmmOpKick, and TvmmOpResetVcpu, which puts the vcpu as at
// power on, its local APIC included; with TvmmVmLocalApic, a vcpu beyond the
// first then waits for a STARTUP.
struct TvmmVcpuOp {
	uint32 vcpu;
};


struct TvmmStateOp {
	uint32 vcpu;
	// TvmmX86State* classes
	uint32 classes;
	uint64 stateAddress;
	uint64 stateSize;
};


// x86: a CPUID leaf, every subleaf of it, reported as the host's with the bits
// of 'del' cleared and those of 'set' set. Registers in the order EAX, EBX,
// ECX, EDX. The kernel then fills in what differs per processor: the APIC
// IDs, OSXSAVE, and the XSAVE area sizes.
struct TvmmCpuidEntry {
	uint32 leaf;
	uint32 del[4];
	uint32 set[4];
};


// The whole table, which replaces the one before. For every vcpu; set before
// they run.
struct TvmmSetCpuid {
	uint32 count;
	uint32 reserved;
	uint64 entriesAddress;
};

constexpr uint32 kTvmmMaxCpuidEntries = 64;


//#pragma mark - run page


enum class TvmmExit: uint32 {
	None,
	// TvmmOpKick, or the VM is closing
	Canceled,
	Io,
	Mmio,
	Msr,
	Halted,
	InterruptWindow,
	// the guest wrote CR8, with TvmmRun::tprExits; TvmmRun::cr8 has the value
	TprChanged,
	// the kernel's local APIC ended a level triggered interrupt
	Eoi,
	// the kernel's local APIC began to take the 8259s' INTR, which the client
	// may have waiting
	ExtIntAccepted,
	// triple fault; the state is undefined until it is set again
	Shutdown,
	// the processor refused the state
	FailEntry,
	// an exit the kernel has no handling for
	Unhandled,
};


struct TvmmExitIo {
	uint16 port;
	// 1, 2 or 4
	uint8 size;
	uint8 in;
	// out: the value written; in: the client stores the value read
	uint32 data;
};


struct TvmmExitMmio {
	uint64 gpa;
	// 1, 2, 4, 8 or 16
	uint8 size;
	uint8 write;
	uint8 reserved[6];
	// write: the value written; read: the client stores the value read
	uint8 data[16];
};


struct TvmmExitMsr {
	uint32 msr;
	uint8 write;
	// the client clears it to raise #GP
	uint8 ok;
	uint8 reserved[2];
	// write: the value written; read: the client stores the value read
	uint64 value;
};


struct TvmmExitEoi {
	uint32 vector;
};


struct TvmmExitFailEntry {
	uint64 code;
};


struct TvmmExitUnhandled {
	uint64 code;
	uint64 info1;
	uint64 info2;
	uint64 rip;
	uint8 insnLength;
	uint8 insnBytes[15];
};


// TvmmRun::inject.type
enum {
	TvmmInjectNone = 0,
	// external interrupt, which needs the guest's IF and no interrupt shadow
	TvmmInjectInterrupt = 1,
	TvmmInjectNmi = 2,
};


// Shared between the kernel and the client. The client fills in the request
// before TvmmOpRun, the kernel the exit before returning; the kernel reads
// each request field once.
struct TvmmRun {
	// request
	uint8 requestInterruptWindow;
	uint8 injectType;
	uint8 injectVector;
	// x86: exit with TprChanged when the guest writes CR8
	uint8 tprExits;
	// x86: CR8 takes 'cr8Value' before the guest runs
	uint8 setCr8;
	uint8 cr8Value;
	uint8 reserved0[2];

	// exit
	TvmmExit exitReason;
	uint32 reserved1;
	uint64 rflags;
	uint64 cr8;
	// the guest cannot take an interrupt for one instruction
	uint8 interruptShadow;
	// an event is already queued for delivery
	uint8 eventPending;
	// with TvmmVmLocalApic: the 8259s' INTR reaches the processor, its APIC
	// being disabled or taking ExtINT on LINT0
	uint8 extIntAccepted;
	uint8 reserved2[5];

	union {
		TvmmExitIo io;
		TvmmExitMmio mmio;
		TvmmExitMsr msr;
		TvmmExitEoi eoi;
		TvmmExitFailEntry failEntry;
		TvmmExitUnhandled unhandled;
		uint8 raw[256];
	} exit;
};

static_assert(offsetof(TvmmRun, exitReason) == 8);
static_assert(offsetof(TvmmRun, exit) == 40);
static_assert(sizeof(TvmmRun) == 296);


//#pragma mark - statistics


// How often something happened, and the host TSC cycles it took.
struct TvmmStat {
	uint64 count;
	uint64 cycles;
};


// TvmmVcpuStats::archExits, x86 SVM: the processor's exits by kind
enum {
	// a host interrupt, NMI, SMI or INIT, kicks and timers included
	TvmmX86StatIntr,
	TvmmX86StatInterruptWindow,
	TvmmX86StatPause,
	TvmmX86StatHlt,
	TvmmX86StatCpuid,
	TvmmX86StatXsetbv,
	TvmmX86StatCr8,
	TvmmX86StatDr,
	// IN and OUT
	TvmmX86StatIo,
	// INS and OUTS, one element each
	TvmmX86StatStringIo,
	// an MSR the kernel has: EFER, PAT, TSC
	TvmmX86StatMsr,
	// IA32_APIC_BASE and the x2APIC registers of the kernel's local APIC
	TvmmX86StatMsrApic,
	TvmmX86StatMsrClient,
	// the register page of the kernel's local APIC
	TvmmX86StatMmioApic,
	TvmmX86StatMmioClient,
	// a nested page fault on what is RAM by now, which the guest retries
	TvmmX86StatNpfRam,
	// an instruction the guest gets #UD or #AC for
	TvmmX86StatException,
	TvmmX86StatShutdown,
	TvmmX86StatFailEntry,
	// anything else, the exits the client gets as Unhandled included
	TvmmX86StatOther,
	TvmmX86StatCount
};


constexpr uint32 kTvmmExitCount = uint32(TvmmExit::Unhandled) + 1;
constexpr uint32 kTvmmMaxClientExits = 16;
constexpr uint32 kTvmmMaxArchExits = 32;
// x86: the local APIC's registers, by xAPIC offset / 16 (x2APIC MSR - 0x800)
constexpr uint32 kTvmmApicRegCount = 64;
static_assert(kTvmmExitCount <= kTvmmMaxClientExits);
static_assert(TvmmX86StatCount <= kTvmmMaxArchExits);


// What a vcpu did since it was created. A run's cycles are counted when it
// returns, the cycles of what the client does when the next run starts.
struct TvmmVcpuStats {
	// TvmmOpRun, from the start of the operation to its return
	TvmmStat runs;
	// in the guest, from just before the guest is entered to just after it
	// exits, the switch of the FPU state included
	TvmmStat entries;
	// waits in the kernel, at HLT or for a STARTUP
	TvmmStat waits;
	// the returns to the client by TvmmRun::exitReason, and the cycles the
	// client took until the next TvmmOpRun
	TvmmStat clientExits[kTvmmMaxClientExits];
	// the processor's exits by kind, TvmmX86Stat* on x86, and the cycles from
	// the exit to the end of its handling in the kernel; the host interrupt
	// that made an exit is taken in that time
	TvmmStat archExits[kTvmmMaxArchExits];
	// x86 with TvmmVmLocalApic: the guest's accesses to its local APIC's
	// registers, through the page or the x2APIC MSRs, and the cycles of each
	// exit as in archExits
	TvmmStat apicReads[kTvmmApicRegCount];
	TvmmStat apicWrites[kTvmmApicRegCount];
	// x86 with TvmmVmLocalApic: those accesses taking the VM's lock of the
	// APICs, and the cycles until they had it
	TvmmStat apicLock;
	// the kernel's reads and writes of guest memory, and its looks at what is
	// RAM, for exits: each under the VM's lock
	TvmmStat guestMemory;
	// x86: instructions read from guest memory, as the processor gave no
	// bytes of them, page table walk included
	TvmmStat insnFetches;
};


// Any thread, while the vcpu runs too. Each counter is read whole; the
// counters together may be of moments apart.
struct TvmmGetStats {
	uint32 vcpu;
	uint32 reserved;
	// out
	TvmmVcpuStats stats;
};

static_assert(sizeof(TvmmGetStats)
	== 8 + 16 * (6 + kTvmmMaxClientExits + kTvmmMaxArchExits + 2 * kTvmmApicRegCount));


//#pragma mark - x86 state


enum {
	TvmmX86StateGprs = 1 << 0,
	TvmmX86StateSegs = 1 << 1,
	TvmmX86StateCrs  = 1 << 2,
	TvmmX86StateDrs  = 1 << 3,
	TvmmX86StateMsrs = 1 << 4,
	TvmmX86StateIntr = 1 << 5,
	TvmmX86StateFpu  = 1 << 6,
	TvmmX86StateAll  = (1 << 7) - 1,
};


// encoding order
enum {
	TvmmX86GprRax, TvmmX86GprRcx, TvmmX86GprRdx, TvmmX86GprRbx,
	TvmmX86GprRsp, TvmmX86GprRbp, TvmmX86GprRsi, TvmmX86GprRdi,
	TvmmX86GprR8,  TvmmX86GprR9,  TvmmX86GprR10, TvmmX86GprR11,
	TvmmX86GprR12, TvmmX86GprR13, TvmmX86GprR14, TvmmX86GprR15,
	TvmmX86GprCount
};


enum {
	TvmmX86SegEs, TvmmX86SegCs, TvmmX86SegSs, TvmmX86SegDs,
	TvmmX86SegFs, TvmmX86SegGs, TvmmX86SegGdt, TvmmX86SegLdt,
	TvmmX86SegIdt, TvmmX86SegTr,
	TvmmX86SegCount
};


enum {
	TvmmX86MsrStar, TvmmX86MsrLstar, TvmmX86MsrCstar, TvmmX86MsrSfmask,
	TvmmX86MsrKernelGsBase, TvmmX86MsrSysenterCs, TvmmX86MsrSysenterEsp,
	TvmmX86MsrSysenterEip, TvmmX86MsrPat, TvmmX86MsrTsc, TvmmX86MsrTscAux,
	TvmmX86MsrCount
};


struct TvmmX86Segment {
	uint64 base;
	uint32 limit;
	uint16 selector;
	// type, S, DPL, P in bits 0-7; AVL, L, D/B, G in bits 8-11
	uint16 attrib;
};


struct TvmmX86State {
	// Gprs
	uint64 gprs[TvmmX86GprCount];
	uint64 rip;
	uint64 rflags;

	// Segs
	TvmmX86Segment segs[TvmmX86SegCount];

	// Crs
	uint64 cr0;
	uint64 cr2;
	uint64 cr3;
	uint64 cr4;
	uint64 cr8;
	uint64 xcr0;
	uint64 efer;

	// Drs
	uint64 dr[4];
	uint64 dr6;
	uint64 dr7;

	// Msrs
	uint64 msrs[TvmmX86MsrCount];

	// Intr
	uint64 interruptShadow;
	// an event queued for delivery, in the format of the SVM EVENTINJ field
	uint64 pendingEvent;

	uint8 reserved[48];

	// Fpu: the XSAVE standard (uncompacted) format
	alignas(64) uint8 fpu[4096];
};

static_assert(offsetof(TvmmX86State, segs) == 144);
static_assert(offsetof(TvmmX86State, cr0) == 304);
static_assert(offsetof(TvmmX86State, fpu) % 64 == 0);
