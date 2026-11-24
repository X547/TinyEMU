#include "SvmHost.h"

#include <stdlib.h>
#include <string.h>

#include <algorithm>
#include <new>

#include <KernelExport.h>

#include <arch/x86/arch_cpu.h>
#include <cpu.h>
#include <smp.h>
#include <util/AutoLock.h>

#include "Arch.h"
#include "Common.h"
#include "KernelArea.h"
#include "SvmVm.h"
#include "Vm.h"
#include "tvmm_abi.h"


static constexpr uint32 kMsrVmCr = 0xc0010114;
static constexpr uint32 kMsrVmHsavePa = 0xc0010117;
static constexpr uint64 kVmCrSvmDisable = 1 << 4;
static constexpr uint64 kEferSvme = 1 << 12;

static constexpr uint32 kCpuidSvm = 1 << 2;          // 0x80000001 ECX
static constexpr uint32 kCpuidXsave = 1 << 26;       // 1 ECX
static constexpr uint32 kCpuidOsxsave = 1 << 27;     // 1 ECX

// 0x8000000A EDX
static constexpr uint32 kSvmCpuidNp = 1 << 0;
static constexpr uint32 kSvmCpuidNrips = 1 << 3;
static constexpr uint32 kSvmCpuidVmcbClean = 1 << 5;
static constexpr uint32 kSvmCpuidFlushByAsid = 1 << 6;
static constexpr uint32 kSvmCpuidDecodeAssists = 1 << 7;
static constexpr uint32 kSvmCpuidPauseFilter = 1 << 10;
static constexpr uint32 kSvmCpuidAvic = 1 << 13;
static constexpr uint32 kSvmCpuidVgif = 1 << 16;
static constexpr uint32 kSvmCpuidSpecCtrl = 1 << 20;

static constexpr uint32 kMaxAsids = 32768;


namespace {


struct HostCpuState {
	phys_addr_t hsavePage;
	phys_addr_t hostSavePage;
	// what the CPU had before, which Release() puts back
	bool svmeWasSet;
	uint64 previousHsavePa;
	// TSC minus CPU 0's
	int64 tscSkew;
};


// CPU 0 and one other hand a timestamp back and forth through one cache
// line. Each one-way time is the line's latency plus or minus the skew, so
// the skew is half the difference of the fastest each way.
struct SkewMeasurement {
	int32 target;
	int32 turn;
	uint64 stamp;
	int64 minThere;
	int64 minBack;
	bool timedOut;
};


struct Probed {
	bool done = false;
	TvmmArch arch = TvmmArch::None;
	status_t status = B_NOT_SUPPORTED;
	uint32 features = 0;
	uint32 archId[4] {};
	uint32 asidCount = 0;
	uint64 hostXcr0 = 0;
	size_t xsaveSize = 0;
	uint32 mxcsrMask = 0;
};


} // namespace


static Probed sProbed;

static mutex sHostLock = MUTEX_INITIALIZER("tvmm svm host");
static int32 sAcquireCount = 0;
static KernelArea *sHostPages = nullptr;
static HostCpuState *sCpuStates = nullptr;
static uint64 sAsidBitmap[kMaxAsids / 64];


static void Cpuid(uint32 leaf, uint32 subleaf, uint32 regs[4])
{
	asm volatile(
		"cpuid"
		: "=a" (regs[0]), "=b" (regs[1]), "=c" (regs[2]), "=d" (regs[3])
		: "a" (leaf), "c" (subleaf)
	);
}


static uint64 ReadXcr0()
{
	uint32 low;
	uint32 high;
	asm volatile("xgetbv" : "=a" (low), "=d" (high) : "c" (0));
	return (uint64(high) << 32) | low;
}


static uint32 ReadMxcsrMask()
{
	alignas(16) uint8 area[512] {};
	asm volatile("fxsave64 %0" : "=m" (area));
	uint32 mask;
	memcpy(&mask, area + 28, sizeof(mask));
	return mask != 0 ? mask : 0xffbf;
}


static void ProbeOnce()
{
	if (sProbed.done) {
		return;
	}
	sProbed.done = true;

	uint32 regs[4];
	Cpuid(0x80000000, 0, regs);
	if (regs[0] < 0x8000000a) {
		return;
	}
	Cpuid(0x80000001, 0, regs);
	if ((regs[2] & kCpuidSvm) == 0) {
		return;
	}
	sProbed.arch = TvmmArch::X86Svm;

	Cpuid(0x8000000a, 0, regs);
	memcpy(sProbed.archId, regs, sizeof(regs));
	sProbed.asidCount = std::min<uint32>(regs[1], kMaxAsids);
	const uint32 edx = regs[3];
	const struct {
		uint32 cpuid;
		uint32 feature;
	} featureMap[] = {
		{kSvmCpuidNp, TvmmSvmFeatureNpt},
		{kSvmCpuidNrips, TvmmSvmFeatureNrips},
		{kSvmCpuidVmcbClean, TvmmSvmFeatureVmcbClean},
		{kSvmCpuidFlushByAsid, TvmmSvmFeatureFlushByAsid},
		{kSvmCpuidDecodeAssists, TvmmSvmFeatureDecodeAssist},
		{kSvmCpuidPauseFilter, TvmmSvmFeaturePauseFilter},
		{kSvmCpuidAvic, TvmmSvmFeatureAvic},
		{kSvmCpuidVgif, TvmmSvmFeatureVgif},
		{kSvmCpuidSpecCtrl, TvmmSvmFeatureSpecCtrl},
	};
	for (const auto &entry: featureMap) {
		if ((edx & entry.cpuid) != 0) {
			sProbed.features |= entry.feature;
		}
	}

	if ((x86_read_msr(kMsrVmCr) & kVmCrSvmDisable) != 0) {
		sProbed.status = B_NOT_ALLOWED;
		return;
	}
	// Exits come without the instruction decoded or a next RIP otherwise,
	// which this has no emulator for.
	const uint32 required = TvmmSvmFeatureNpt | TvmmSvmFeatureNrips | TvmmSvmFeatureDecodeAssist;
	if ((sProbed.features & required) != required) {
		sProbed.status = B_NOT_SUPPORTED;
		return;
	}

	Cpuid(1, 0, regs);
	if ((regs[2] & kCpuidXsave) == 0 || (regs[2] & kCpuidOsxsave) == 0) {
		sProbed.status = B_NOT_SUPPORTED;
		return;
	}
	sProbed.hostXcr0 = ReadXcr0();
	Cpuid(0xd, 0, regs);
	sProbed.xsaveSize = regs[1];
	if (sProbed.xsaveSize > sizeof(TvmmX86State::fpu)) {
		sProbed.status = B_NOT_SUPPORTED;
		return;
	}
	sProbed.mxcsrMask = ReadMxcsrMask();
	sProbed.status = B_OK;
}


static constexpr int kSkewRounds = 1000;
// how long either CPU waits for the other, in TSC cycles, before giving up
static constexpr uint64 kSkewTimeout = 1000000000;


// rdtscp waits for what came before it, unlike rdtsc.
static uint64 ReadTscOrdered()
{
	uint32 low;
	uint32 high;
	uint32 aux;
	asm volatile("rdtscp" : "=a" (low), "=d" (high), "=c" (aux) :: "memory");
	return (uint64(high) << 32) | low;
}


// Waits until 'turn' is 'value'; false after kSkewTimeout.
static bool WaitTurn(SkewMeasurement &measurement, int32 value)
{
	const uint64 start = ReadTscOrdered();
	while (atomic_get(&measurement.turn) != value) {
		if (ReadTscOrdered() - start > kSkewTimeout) {
			measurement.timedOut = true;
			return false;
		}
		cpu_pause();
	}
	return true;
}


// On every CPU at once, with interrupts disabled; all but two return.
static void MeasureSkew(void *cookie, int cpu)
{
	auto &measurement = *static_cast<SkewMeasurement*>(cookie);
	if (cpu == 0) {
		for (int i = 0; i < kSkewRounds && !measurement.timedOut; i++) {
			const uint64 sent = ReadTscOrdered();
			atomic_set64(reinterpret_cast<int64*>(&measurement.stamp), int64(sent));
			atomic_set(&measurement.turn, 1);
			if (!WaitTurn(measurement, 2)) {
				break;
			}
			const uint64 now = ReadTscOrdered();
			const int64 back = int64(now - uint64(atomic_get64(reinterpret_cast<int64*>(&measurement.stamp))));
			const int64 there = int64(now - sent) - back;
			measurement.minThere = std::min(measurement.minThere, there);
			measurement.minBack = std::min(measurement.minBack, back);
		}
		atomic_set(&measurement.turn, 3);
	} else if (cpu == measurement.target) {
		for (;;) {
			const uint64 start = ReadTscOrdered();
			int32 turn;
			while ((turn = atomic_get(&measurement.turn)) != 1 && turn != 3) {
				if (ReadTscOrdered() - start > kSkewTimeout) {
					measurement.timedOut = true;
					return;
				}
				cpu_pause();
			}
			if (turn == 3) {
				return;
			}
			atomic_set64(reinterpret_cast<int64*>(&measurement.stamp), int64(ReadTscOrdered()));
			atomic_set(&measurement.turn, 2);
		}
	}
}


// Haiku resets every CPU's TSC at boot, one after the other, which leaves
// them some hundred cycles apart; a guest that sees them so finds its TSC
// unusable.
static void MeasureTscSkews(HostCpuState *states, int32 cpuCount)
{
	states[0].tscSkew = 0;
	for (int32 cpu = 1; cpu < cpuCount; cpu++) {
		SkewMeasurement measurement {};
		measurement.target = cpu;
		measurement.minThere = INT64_MAX;
		measurement.minBack = INT64_MAX;
		call_all_cpus_sync(MeasureSkew, &measurement);
		if (measurement.timedOut || measurement.minThere == INT64_MAX) {
			TVMM_LOG("cannot measure the TSC of CPU %" B_PRId32 "\n", cpu);
			states[cpu].tscSkew = 0;
			continue;
		}
		// there = latency + skew, back = latency - skew
		states[cpu].tscSkew = (measurement.minThere - measurement.minBack) / 2;
		TVMM_LOG("TSC of CPU %" B_PRId32 ": %+" B_PRId64 " cycles against CPU 0\n", cpu, states[cpu].tscSkew);
	}
}


static void EnableCpu(void *cookie, int cpu)
{
	(void)cookie;
	HostCpuState &state = sCpuStates[cpu];
	const uint64 efer = x86_read_msr(IA32_MSR_EFER);
	state.svmeWasSet = (efer & kEferSvme) != 0;
	state.previousHsavePa = x86_read_msr(kMsrVmHsavePa);
	x86_write_msr(IA32_MSR_EFER, efer | kEferSvme);
	x86_write_msr(kMsrVmHsavePa, state.hsavePage);
}


static void DisableCpu(void *cookie, int cpu)
{
	(void)cookie;
	const HostCpuState &state = sCpuStates[cpu];
	if (state.svmeWasSet) {
		// someone else uses SVM, with its own host save area
		x86_write_msr(kMsrVmHsavePa, state.previousHsavePa);
		return;
	}
	x86_write_msr(kMsrVmHsavePa, 0);
	x86_write_msr(IA32_MSR_EFER, x86_read_msr(IA32_MSR_EFER) & ~kEferSvme);
}


//#pragma mark - Arch

void Arch::Probe(TvmmInfo &info)
{
	ProbeOnce();
	info.arch = sProbed.arch;
	info.hostStatus = sProbed.status;
	info.features = sProbed.features;
	info.maxVcpus = MaxVcpus();
	memcpy(info.archId, sProbed.archId, sizeof(info.archId));
}


status_t Arch::HostStatus()
{
	ProbeOnce();
	return sProbed.status;
}


status_t Arch::GetHostIsa(TvmmHostIsa &isa)
{
	(void)isa;
	return B_NOT_SUPPORTED;
}


uint32 Arch::MaxVcpus()
{
	return std::min<uint32>(Vm::kMaxVcpus, sProbed.asidCount > 0 ? sProbed.asidCount - 1 : 0);
}


status_t Arch::Acquire()
{
	MutexLocker locker(sHostLock);
	if (sAcquireCount > 0) {
		sAcquireCount++;
		return B_OK;
	}

	const int32 cpuCount = smp_get_num_cpus();
	auto *pages = new(std::nothrow) KernelArea();
	auto *states = new(std::nothrow) HostCpuState[cpuCount];
	if (pages == nullptr || states == nullptr) {
		delete pages;
		delete[] states;
		return B_NO_MEMORY;
	}
	status_t res = pages->Create("tvmm svm host", 2 * cpuCount * kPageSize, B_FULL_LOCK);
	for (int32 cpu = 0; res >= B_OK && cpu < cpuCount; cpu++) {
		res = pages->PhysicalAddress(2 * cpu * kPageSize, states[cpu].hsavePage);
		if (res >= B_OK) {
			res = pages->PhysicalAddress((2 * cpu + 1) * kPageSize, states[cpu].hostSavePage);
		}
	}
	if (res < B_OK) {
		delete pages;
		delete[] states;
		return res;
	}

	MeasureTscSkews(states, cpuCount);
	sHostPages = pages;
	sCpuStates = states;
	call_all_cpus_sync(EnableCpu, nullptr);
	sAcquireCount = 1;
	return B_OK;
}


void Arch::Release()
{
	MutexLocker locker(sHostLock);
	if (--sAcquireCount > 0) {
		return;
	}
	call_all_cpus_sync(DisableCpu, nullptr);
	delete sHostPages;
	sHostPages = nullptr;
	delete[] sCpuStates;
	sCpuStates = nullptr;
}


uint64 Arch::Cycles()
{
	return SvmHost::ReadTsc0();
}


status_t Arch::CreateVm(Vm &vm, uint32 flags, ArchVm *&arch)
{
	auto *svmVm = new(std::nothrow) SvmVm(vm, flags);
	if (svmVm == nullptr) {
		return B_NO_MEMORY;
	}
	const status_t res = svmVm->Init();
	if (res < B_OK) {
		delete svmVm;
		return res;
	}
	arch = svmVm;
	return B_OK;
}


//#pragma mark - SvmHost

uint32 SvmHost::Features()
{
	return sProbed.features;
}


bool SvmHost::HasFeature(uint32 feature)
{
	return (sProbed.features & feature) != 0;
}


uint64 SvmHost::HostXcr0()
{
	return sProbed.hostXcr0;
}


size_t SvmHost::XsaveSize()
{
	return sProbed.xsaveSize;
}


uint32 SvmHost::MxcsrMask()
{
	return sProbed.mxcsrMask;
}


phys_addr_t SvmHost::HostSavePage(int32 cpu)
{
	return sCpuStates[cpu].hostSavePage;
}


int64 SvmHost::TscSkew(int32 cpu)
{
	return sCpuStates[cpu].tscSkew;
}


uint64 SvmHost::ReadTsc0()
{
	const cpu_status state = disable_interrupts();
	const uint64 tsc = ReadTscOrdered() - uint64(TscSkew(smp_get_current_cpu()));
	restore_interrupts(state);
	return tsc;
}


uint32 SvmHost::AllocAsid()
{
	MutexLocker locker(sHostLock);
	for (uint32 asid = 1; asid < sProbed.asidCount; asid++) {
		uint64 &word = sAsidBitmap[asid / 64];
		const uint64 bit = 1ull << (asid % 64);
		if ((word & bit) == 0) {
			word |= bit;
			return asid;
		}
	}
	return 0;
}


void SvmHost::FreeAsid(uint32 asid)
{
	MutexLocker locker(sHostLock);
	sAsidBitmap[asid / 64] &= ~(1ull << (asid % 64));
}
