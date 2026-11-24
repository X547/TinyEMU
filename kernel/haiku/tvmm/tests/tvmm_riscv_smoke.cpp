// Runs a small RISC-V guest on tvmm and checks each kind of exit: MMIO
// loads and stores, compressed ones included, SBI calls the kernel answers
// and those it passes on, the timer and WFI, the guest's FPU state against
// the client's, the dirty log, a system reset, and a kick.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>

#include "TvmmClient.h"


static constexpr uint64 kRamBase = 0x80000000;
static constexpr uint64 kRamSize = 64 * 1024;
static constexpr uint64 kMmioBase = 0x10000000;
static constexpr uint64 kLogBase = 0x90000000;
static constexpr uint64 kLogSize = 4 * 4096;

static constexpr uint64 kSbiDbcn = 0x4442434e;
// an experimental extension, which the kernel passes on
static constexpr uint64 kSbiPause = 0x08000000;

static int sFailures = 0;


#define CHECK(cond) \
	do { \
		if (!(cond)) { \
			printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
			sFailures++; \
		} \
	} while (false)

#define CHECK_OK(expr) \
	do { \
		const status_t _res = (expr); \
		if (_res < B_OK) { \
			printf("FAIL %s:%d: %s: %s\n", __FILE__, __LINE__, #expr, strerror(_res)); \
			exit(1); \
		} \
	} while (false)


extern "C" const uint8 tvmm_guest_start[];
extern "C" const uint8 tvmm_guest_spin[];
extern "C" const uint8 tvmm_guest_end[];

// The guest, run in VS-mode without translation at kRamBase.
asm(R"(
	.pushsection .rodata
	.balign 4
	.globl tvmm_guest_start
tvmm_guest_start:
	li t0, 0x10000000
	li t1, 0x12345678
	sw t1, 0(t0)
	lw a2, 4(t0)
	sd a2, 8(t0)
	mv s0, t0
	c.sw a2, 16(s0)

	li a7, 0x4442434e
	li a6, 2
	li a0, 65
	ecall
	sd a1, 24(t0)

	li a7, 0x10
	li a6, 0
	ecall
	sd a1, 32(t0)

	rdtime t2
	li t3, 100000
	add a0, t2, t3
	li a7, 0x54494d45
	li a6, 0
	ecall
	li t4, 0x20
	csrs sie, t4
	wfi
	rdtime t5
	sub t5, t5, t2
	sd t5, 40(t0)

	li t4, 0x2000
	csrs sstatus, t4
	li t6, 0x4045000000000000
	fmv.d.x f1, t6
	fadd.d f2, f1, f1
	fmv.x.d t6, f2
	sd t6, 48(t0)

	li t1, 0x90002000
	sd t1, 0(t1)
	li a7, 0x08000000
	li a6, 0
	ecall

	li a7, 0x53525354
	li a6, 0
	li a0, 0
	li a1, 0
	ecall
1:
	j 1b

	.globl tvmm_guest_spin
tvmm_guest_spin:
2:
	j 2b

	.globl tvmm_guest_end
tvmm_guest_end:
	.popsection
)");


static TvmmDevice sDevice;
static TvmmRun *sRun = nullptr;


static TvmmExit Run()
{
	CHECK_OK(sDevice.Run(0));
	return sRun->exitReason;
}


static void ExpectStore(uint64 offset, uint8 size, uint64 value)
{
	const TvmmExit exit = Run();
	CHECK(exit == TvmmExit::Mmio);
	if (exit != TvmmExit::Mmio) {
		printf("  exit %u at offset %#llx\n", unsigned(exit), (unsigned long long)offset);
		if (exit == TvmmExit::Unhandled) {
			const TvmmExitUnhandled &u = sRun->exit.unhandled;
			printf("  scause %#llx stval %#llx gpa %#llx pc %#llx\n",
				(unsigned long long)u.code, (unsigned long long)u.info1,
				(unsigned long long)u.info2, (unsigned long long)u.rip);
		}
		// the guest is off its course; what follows would only fail too
		::exit(1);
	}
	const TvmmExitMmio &mmio = sRun->exit.mmio;
	uint64 data = 0;
	memcpy(&data, mmio.data, sizeof(data));
	if (mmio.gpa != kMmioBase + offset || mmio.size != size || !mmio.write || data != value) {
		printf("FAIL store: gpa %#llx size %u write %u data %#llx, expected %#llx %u %#llx\n",
			(unsigned long long)mmio.gpa, mmio.size, mmio.write, (unsigned long long)data,
			(unsigned long long)(kMmioBase + offset), size, (unsigned long long)value);
		sFailures++;
	}
}


static uint64 ReadFcsr()
{
	uint64 value;
	asm volatile("frcsr %0" : "=r" (value));
	return value;
}


static void WriteFcsr(uint64 value)
{
	asm volatile("fscsr %0" :: "r" (value));
}


static status_t KickLater(void *cookie)
{
	(void)cookie;
	snooze(100000);
	sDevice.Kick(0);
	return B_OK;
}


static void PrintStats()
{
	static const char *const names[] = {
		"intr", "wfi", "sbi", "sbi client", "gpf ram", "mmio", "virtual insn", "other",
	};
	TvmmVcpuStats stats;
	CHECK_OK(sDevice.GetStats(0, stats));
	printf("  entries %llu, runs %llu, waits %llu\n", (unsigned long long)stats.entries.count,
		(unsigned long long)stats.runs.count, (unsigned long long)stats.waits.count);
	for (uint32 i = 0; i < TvmmRiscVStatCount; i++) {
		if (stats.archExits[i].count != 0) {
			printf("  %-14s %llu\n", names[i], (unsigned long long)stats.archExits[i].count);
		}
	}
}


int main()
{
	CHECK_OK(sDevice.Open());

	TvmmInfo info;
	CHECK_OK(sDevice.GetInfo(info));
	printf("arch %u, host status %s, features %#x, VMID bits %u, guest physical bits %u\n",
		unsigned(info.arch), strerror(info.hostStatus), info.features, info.archId[0], info.archId[1]);
	CHECK(info.abiVersion == kTvmmAbiVersion);
	CHECK(info.arch == TvmmArch::RiscVH);
	CHECK_OK(info.hostStatus);

	auto *isa = new TvmmHostIsa;
	CHECK_OK(sDevice.GetHostIsa(*isa));
	printf("timebase %llu Hz, satp mode %u, cbo %u/%u, misa ",
		(unsigned long long)isa->timebaseFrequency, isa->satpMode, isa->cbomBlockSize,
		isa->cbozBlockSize);
	for (int i = 0; i < 26; i++) {
		if ((isa->misa >> i) & 1) {
			printf("%c", 'a' + i);
		}
	}
	printf(", extensions");
	for (const char *ext = isa->extensions; *ext != '\0'; ext += strlen(ext) + 1) {
		printf(" %s", ext);
	}
	printf("\n");
	CHECK(isa->timebaseFrequency != 0);
	delete isa;

	CHECK_OK(sDevice.CreateVm());
	uint32 ramId;
	uint8 *ram;
	CHECK_OK(sDevice.AllocRam(kRamSize, ramId, ram));
	CHECK_OK(sDevice.MapRam(0, kRamBase, kRamSize, ramId, 0, 0));
	memcpy(ram, tvmm_guest_start, tvmm_guest_end - tvmm_guest_start);

	uint32 logId;
	uint8 *logRam;
	CHECK_OK(sDevice.AllocRam(kLogSize, logId, logRam));
	CHECK_OK(sDevice.MapRam(1, kLogBase, kLogSize, logId, 0, TvmmMapLogDirty));
	uint64 dirty = 0;
	CHECK_OK(sDevice.GetDirtyLog(1, &dirty, 1));
	CHECK(dirty == 0xf);

	CHECK_OK(sDevice.CreateVcpu(0, sRun));
	auto *state = new TvmmRiscVState;
	CHECK_OK(sDevice.GetState(0, TvmmRiscVStateAll, *state));
	CHECK(state->mpState == TvmmRiscVHartStopped);
	state->pc = kRamBase;
	state->mode = TvmmRiscVModeSupervisor;
	state->mpState = TvmmRiscVHartStarted;
	CHECK_OK(sDevice.SetState(0, TvmmRiscVStateCore | TvmmRiscVStateMp, *state));

	// the client's FPU state, which the guest's must not touch
	WriteFcsr(0x60);

	ExpectStore(0, 4, 0x12345678);

	CHECK(Run() == TvmmExit::Mmio);
	CHECK(sRun->exit.mmio.gpa == kMmioBase + 4 && sRun->exit.mmio.size == 4 && !sRun->exit.mmio.write);
	const uint32 loaded = 0xfedcba98;
	memcpy(sRun->exit.mmio.data, &loaded, sizeof(loaded));
	ExpectStore(8, 8, 0xfffffffffedcba98ull);
	ExpectStore(16, 4, 0xfedcba98);

	CHECK(Run() == TvmmExit::Sbi);
	CHECK(sRun->exit.sbi.a[7] == kSbiDbcn && sRun->exit.sbi.a[6] == 2 && sRun->exit.sbi.a[0] == 'A');
	sRun->exit.sbi.a[0] = 0;
	sRun->exit.sbi.a[1] = 0x55;
	ExpectStore(24, 8, 0x55);
	ExpectStore(32, 8, 2 << 24);

	// the timer, 100000 ticks ahead, ends the WFI
	const bigtime_t waitStart = system_time();
	const TvmmExit timerExit = Run();
	const bigtime_t waited = system_time() - waitStart;
	CHECK(timerExit == TvmmExit::Mmio);
	uint64 elapsed = 0;
	memcpy(&elapsed, sRun->exit.mmio.data, sizeof(elapsed));
	printf("WFI: %llu ticks in the guest, %lld us in the client\n", (unsigned long long)elapsed,
		(long long)waited);
	CHECK(sRun->exit.mmio.gpa == kMmioBase + 40 && elapsed >= 100000);

	ExpectStore(48, 8, 0x4055000000000000ull);
	CHECK(ReadFcsr() == 0x60);

	CHECK(Run() == TvmmExit::Sbi);
	CHECK(sRun->exit.sbi.a[7] == kSbiPause);
	dirty = 0;
	CHECK_OK(sDevice.GetDirtyLog(1, &dirty, 1));
	printf("dirty log after one write: %#llx\n", (unsigned long long)dirty);
	CHECK(dirty == 0x4);
	uint64 written = 0;
	memcpy(&written, logRam + 0x2000, sizeof(written));
	CHECK(written == kLogBase + 0x2000);
	sRun->exit.sbi.a[0] = 0;
	sRun->exit.sbi.a[1] = 0;

	CHECK(Run() == TvmmExit::SystemEvent);
	CHECK(sRun->exit.systemEvent.type == 0);
	CHECK_OK(sDevice.GetState(0, TvmmRiscVStateMp | TvmmRiscVStateFp, *state));
	CHECK(state->mpState == TvmmRiscVHartStopped);
	CHECK(state->f[2] == 0x4055000000000000ull);
	PrintStats();

	// a stopped hart waits in the kernel until a kick
	thread_id kicker = spawn_thread(KickLater, "kicker", B_NORMAL_PRIORITY, nullptr);
	resume_thread(kicker);
	CHECK(Run() == TvmmExit::Canceled);
	status_t threadResult;
	wait_for_thread(kicker, &threadResult);

	// a guest that spins is kicked out
	CHECK_OK(sDevice.ResetVcpu(0));
	CHECK_OK(sDevice.GetState(0, TvmmRiscVStateAll, *state));
	state->pc = kRamBase + (tvmm_guest_spin - tvmm_guest_start);
	state->mpState = TvmmRiscVHartStarted;
	CHECK_OK(sDevice.SetState(0, TvmmRiscVStateCore | TvmmRiscVStateMp, *state));
	kicker = spawn_thread(KickLater, "kicker", B_NORMAL_PRIORITY, nullptr);
	resume_thread(kicker);
	CHECK(Run() == TvmmExit::Canceled);
	wait_for_thread(kicker, &threadResult);
	CHECK(ReadFcsr() == 0x60);
	delete state;

	printf("%s: %d failures\n", sFailures == 0 ? "PASS" : "FAIL", sFailures);
	return sFailures == 0 ? 0 : 1;
}
