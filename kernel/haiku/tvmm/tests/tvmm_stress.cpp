// Runs many vcpus at once against what can change under them: RAM mapped,
// moved, unmapped and freed, kicks without pause, and the device closed.
// Passes when nothing hangs, every operation succeeds, and every exit is one
// the guest's accesses explain.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <algorithm>

#include <OS.h>

#include "TvmmClient.h"


static constexpr uint64 kRamSize = 64 * 1024;
static constexpr uint32 kCodeAt = 0x1000;
// the slot that changes, where the guest's ES and DS point
static constexpr uint32 kSlot = 1;
static constexpr uint64 kSlotGpa = 0x10000;
static constexpr uint64 kSlotMovedGpa = 0x20000;
static constexpr uint64 kSlotSize = 4 * 4096;
static constexpr uint32 kMaxVcpus = 32;
static constexpr bigtime_t kDuration = 3000000;
// SVM's exit code of IN and OUT, as an INS or OUTS to memory that is not RAM
// comes back Unhandled
static constexpr uint64 kSvmExitIoio = 0x7b;

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
			sFailures++; \
		} \
	} while (false)


// Real mode: a store and a load in the slot, a REP INSB into it and a REP
// OUTSB from it, which the kernel does, and again.
static const uint8 kLoop[] = {
	0xfc,                                   // cld
	0xb8, 0x00, 0x10,                       // mov ax, 0x1000
	0x8e, 0xc0,                             // mov es, ax
	0x8e, 0xd8,                             // mov ds, ax
	0xa2, 0x00, 0x01,                       // mov [0x100], al
	0xa0, 0x00, 0x21,                       // mov al, [0x2100]
	0xbf, 0x00, 0x30,                       // mov di, 0x3000
	0xb9, 0x40, 0x00,                       // mov cx, 64
	0xba, 0x80, 0x00,                       // mov dx, 0x80
	0xf3, 0x6c,                             // rep insb
	0xbe, 0x00, 0x10,                       // mov si, 0x1000
	0xb9, 0x40, 0x00,                       // mov cx, 64
	0xf3, 0x6e,                             // rep outsb
	0x40,                                   // inc ax
	0xeb, 0xe4,                             // jmp mov [0x100]
};


struct Machine {
	TvmmDevice device;
	uint32 vcpus = 0;
	uint8 *ram = nullptr;
	TvmmRun *runs[kMaxVcpus] {};
};


struct VcpuThread {
	Machine *machine = nullptr;
	uint32 index = 0;
	volatile bool *stop = nullptr;
	thread_id thread = -1;
	// what the runs came back with
	uint64 exits = 0;
	uint64 io = 0;
	uint64 mmio = 0;
	uint64 canceled = 0;
	uint64 unhandled = 0;
	// the first unexpected exit reason, or run status
	uint32 badExit = 0;
	status_t result = B_OK;
};


static void Jump(TvmmDevice &device, uint32 vcpu, uint32 ip)
{
	static TvmmX86State state;
	CHECK_OK(device.GetState(vcpu, TvmmX86StateAll, state));
	state.segs[TvmmX86SegCs].selector = 0;
	state.segs[TvmmX86SegCs].base = 0;
	state.rip = ip;
	CHECK_OK(device.SetState(vcpu, TvmmX86StateGprs | TvmmX86StateSegs, state));
}


static bool Setup(Machine &machine, uint32 vcpus, uint32 flags)
{
	machine.vcpus = vcpus;
	if (machine.device.Open() < B_OK || machine.device.CreateVm(flags) < B_OK) {
		printf("FAIL: no VM\n");
		sFailures++;
		return false;
	}
	uint32 ramId = 0;
	CHECK_OK(machine.device.AllocRam(kRamSize, ramId, machine.ram));
	memcpy(machine.ram + kCodeAt, kLoop, sizeof(kLoop));
	CHECK_OK(machine.device.MapRam(0, 0, kRamSize, ramId, 0, 0));
	for (uint32 i = 0; i < vcpus; i++) {
		CHECK_OK(machine.device.CreateVcpu(i, machine.runs[i]));
		Jump(machine.device, i, kCodeAt);
	}
	return sFailures == 0;
}


// Until told to stop or a run fails: each exit answered as a device would.
static status_t RunVcpu(void *data)
{
	auto &self = *static_cast<VcpuThread*>(data);
	Machine &machine = *self.machine;
	TvmmRun &run = *machine.runs[self.index];
	uint8 *runData = reinterpret_cast<uint8*>(&run) + kTvmmRunDataOffset;
	while (!*self.stop) {
		const status_t res = machine.device.Run(self.index);
		if (res < B_OK) {
			self.result = res;
			break;
		}
		self.exits++;
		switch (run.exitReason) {
			case TvmmExit::Io:
				if (run.exit.io.in) {
					if (run.exit.io.count != 0) {
						memset(runData, 0x5a, run.exit.io.count * run.exit.io.size);
					} else {
						run.exit.io.data = 0x5a;
					}
				}
				self.io++;
				break;
			case TvmmExit::Mmio:
				if (!run.exit.mmio.write) {
					memset(run.exit.mmio.data, 0xff, sizeof(run.exit.mmio.data));
				}
				self.mmio++;
				break;
			case TvmmExit::Canceled:
				self.canceled++;
				break;
			case TvmmExit::Unhandled:
				if (run.exit.unhandled.code == kSvmExitIoio) {
					self.unhandled++;
					break;
				}
				[[fallthrough]];
			default:
				if (self.badExit == 0) {
					self.badExit = uint32(run.exitReason);
				}
				break;
		}
	}
	return B_OK;
}


static void StartVcpus(Machine &machine, VcpuThread *threads, uint32 count, volatile bool *stop)
{
	for (uint32 i = 0; i < count; i++) {
		VcpuThread &t = threads[i];
		t.machine = &machine;
		t.index = i;
		t.stop = stop;
		t.thread = spawn_thread(RunVcpu, "tvmm stress vcpu", B_NORMAL_PRIORITY, &t);
		resume_thread(t.thread);
	}
}


// Each run returns within a few seconds, or something hangs.
static void JoinVcpus(Machine &machine, VcpuThread *threads, uint32 count, bool kick)
{
	for (uint32 i = 0; i < count; i++) {
		if (kick) {
			machine.device.Kick(i);
		}
	}
	for (uint32 i = 0; i < count; i++) {
		status_t res;
		const status_t waited = wait_for_thread_etc(threads[i].thread, B_RELATIVE_TIMEOUT, 5000000, &res);
		if (waited == B_TIMED_OUT) {
			printf("FAIL: vcpu %u hangs\n", i);
			sFailures++;
		}
	}
}


static void Report(const VcpuThread *threads, uint32 count, bool requireIo)
{
	uint64 exits = 0, io = 0, mmio = 0, canceled = 0, unhandled = 0;
	for (uint32 i = 0; i < count; i++) {
		const VcpuThread &t = threads[i];
		exits += t.exits;
		io += t.io;
		mmio += t.mmio;
		canceled += t.canceled;
		unhandled += t.unhandled;
		if (t.badExit != 0) {
			printf("FAIL: vcpu %u had exit %u\n", i, t.badExit);
			sFailures++;
		}
		if (requireIo && t.io == 0) {
			printf("FAIL: vcpu %u made no progress\n", i);
			sFailures++;
		}
	}
	printf("%u vcpus: %" B_PRIu64 " exits: %" B_PRIu64 " I/O, %" B_PRIu64 " MMIO, %" B_PRIu64
		" canceled, %" B_PRIu64 " string I/O to no RAM\n", count, exits, io, mmio, canceled, unhandled);
}


static uint32 HostCpus()
{
	system_info info;
	get_system_info(&info);
	return info.cpu_count;
}


// The slot's RAM comes and goes, moves, and has its dirty log taken, while
// twice as many vcpus as there are CPUs use it.
static void TestMapping()
{
	printf("== mapping while vcpus run\n");
	const uint32 vcpus = std::min<uint32>(2 * HostCpus(), kMaxVcpus);
	Machine machine;
	if (!Setup(machine, vcpus, 0)) {
		return;
	}
	volatile bool stop = false;
	VcpuThread threads[kMaxVcpus];
	StartVcpus(machine, threads, vcpus, &stop);

	uint64 cycles = 0;
	uint64 dirtyPages = 0;
	const bigtime_t end = system_time() + kDuration;
	while (system_time() < end && sFailures == 0) {
		uint32 ramId = 0;
		uint8 *ram = nullptr;
		CHECK_OK(machine.device.AllocRam(kSlotSize, ramId, ram));
		CHECK_OK(machine.device.MapRam(kSlot, kSlotGpa, kSlotSize, ramId, 0, TvmmMapLogDirty));
		snooze(cycles % 7 * 50);
		uint64 bitmap = 0;
		CHECK_OK(machine.device.GetDirtyLog(kSlot, &bitmap, 1));
		snooze(cycles % 5 * 50);
		CHECK_OK(machine.device.GetDirtyLog(kSlot, &bitmap, 1));
		dirtyPages += __builtin_popcountll(bitmap);
		if (cycles % 2 != 0) {
			// moved away from where the guest looks
			CHECK_OK(machine.device.MapRam(kSlot, kSlotMovedGpa, kSlotSize, ramId, 0, 0));
		}
		CHECK_OK(machine.device.MapRam(kSlot, 0, 0, 0, 0, 0));
		CHECK_OK(machine.device.FreeRam(ramId, ram));
		cycles++;
	}
	stop = true;
	JoinVcpus(machine, threads, vcpus, true);
	printf("%" B_PRIu64 " times mapped, %" B_PRIu64 " pages logged written\n", cycles, dirtyPages);
	Report(threads, vcpus, true);
}


struct Kicker {
	Machine *machine;
	volatile bool *stop;
	uint64 kicks;
};


static status_t Kick(void *data)
{
	auto &self = *static_cast<Kicker*>(data);
	uint32 next = 0;
	while (!*self.stop) {
		self.machine->device.Kick(next);
		next = (next * 7 + 3) % self.machine->vcpus;
		self.kicks++;
	}
	return B_OK;
}


// Kicks without pause, at any point of the vcpus' entries and exits.
static void TestKicks()
{
	printf("== kick storm\n");
	const uint32 vcpus = std::min<uint32>(HostCpus(), kMaxVcpus);
	Machine machine;
	if (!Setup(machine, vcpus, 0)) {
		return;
	}
	uint32 ramId = 0;
	uint8 *ram = nullptr;
	CHECK_OK(machine.device.AllocRam(kSlotSize, ramId, ram));
	CHECK_OK(machine.device.MapRam(kSlot, kSlotGpa, kSlotSize, ramId, 0, 0));

	volatile bool stop = false;
	VcpuThread threads[kMaxVcpus];
	StartVcpus(machine, threads, vcpus, &stop);
	Kicker kicker = {&machine, &stop, 0};
	const thread_id kickThread = spawn_thread(Kick, "tvmm stress kicker", B_NORMAL_PRIORITY, &kicker);
	resume_thread(kickThread);
	snooze(kDuration);
	stop = true;
	status_t res;
	wait_for_thread(kickThread, &res);
	JoinVcpus(machine, threads, vcpus, true);
	printf("%" B_PRIu64 " kicks\n", kicker.kicks);
	Report(threads, vcpus, true);
}


// The device closes while vcpu 0 runs and the others wait in the kernel for
// a STARTUP: every run returns.
static void TestClose()
{
	printf("== close while vcpus run and wait\n");
	const uint32 vcpus = std::min<uint32>(HostCpus(), kMaxVcpus);
	Machine machine;
	if (!Setup(machine, vcpus, TvmmVmLocalApic)) {
		return;
	}
	uint32 ramId = 0;
	uint8 *ram = nullptr;
	CHECK_OK(machine.device.AllocRam(kSlotSize, ramId, ram));
	CHECK_OK(machine.device.MapRam(kSlot, kSlotGpa, kSlotSize, ramId, 0, 0));
	for (uint32 i = 0; i < vcpus; i++) {
		CHECK_OK(machine.device.ResetVcpu(i));
	}
	Jump(machine.device, 0, kCodeAt);

	volatile bool stop = false;
	VcpuThread threads[kMaxVcpus];
	StartVcpus(machine, threads, vcpus, &stop);
	snooze(200000);
	const bigtime_t start = system_time();
	machine.device.Close();
	JoinVcpus(machine, threads, vcpus, false);
	printf("every run returned %" B_PRId64 " us after the close\n", system_time() - start);
	uint32 ended = 0;
	for (uint32 i = 0; i < vcpus; i++) {
		if (threads[i].result < B_OK) {
			ended++;
		}
	}
	CHECK(ended == vcpus);
	Report(threads, vcpus, false);
	CHECK(threads[0].io != 0);
}


int main()
{
	setvbuf(stdout, nullptr, _IONBF, 0);
	printf("%u host CPUs\n", HostCpus());

	TestMapping();
	TestKicks();
	TestClose();

	// the driver still works
	TvmmDevice device;
	TvmmInfo info;
	CHECK_OK(device.Open());
	CHECK_OK(device.GetInfo(info));

	if (sFailures != 0) {
		printf("%d failures\n", sFailures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
