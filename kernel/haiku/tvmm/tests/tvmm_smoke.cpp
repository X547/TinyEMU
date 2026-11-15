// Runs small real mode guests on tvmm and checks each kind of exit, the
// FPU state on both sides of a run, and kicks.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <OS.h>

#include "TvmmClient.h"


static constexpr uint64 kRamSize = 64 * 1024;
static constexpr uint32 kCodeBase = 0x1000;
static constexpr uint32 kDataBase = 0x3000;

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


static const uint8 kCode[] = {
	0xd9, 0x3e, 0x00, 0x30,         // 0x1000 fnstcw [0x3000]
	0xba, 0x80, 0x00,               // 0x1004 mov dx, 0x80
	0xb0, 0x42,                     // 0x1007 mov al, 0x42
	0xee,                           // 0x1009 out dx, al
	0xec,                           // 0x100a in al, dx
	0xee,                           // 0x100b out dx, al
	0xdb, 0xe3,                     // 0x100c fninit
	0xd9, 0x3e, 0x02, 0x30,         // 0x100e fnstcw [0x3002]
	0xee,                           // 0x1012 out dx, al
	0xeb, 0xfd,                     // 0x1013 jmp 0x1012
};

static const uint8 kSpin[] = {0xeb, 0xfe};                         // jmp $
static const uint8 kHalt[] = {0xf4};                               // hlt
static const uint8 kCpuid[] = {0x0f, 0xa2, 0xf4};                  // cpuid; hlt
static const uint8 kMmio[] = {
	0xb8, 0x00, 0x20,                       // mov ax, 0x2000
	0x8e, 0xd8,                             // mov ds, ax
	0x8e, 0xc0,                             // mov es, ax
	0xa0, 0x00, 0x00,                       // mov al, [0]
	0xa2, 0x01, 0x00,                       // mov [1], al
	0xc7, 0x06, 0x02, 0x00, 0x34, 0x12,     // mov word [2], 0x1234
	0x66, 0x0f, 0xbf, 0x1e, 0x04, 0x00,     // movsx ebx, word [4]
	0xbf, 0x10, 0x00,                       // mov di, 0x10
	0xb0, 0x77,                             // mov al, 0x77
	0xaa,                                   // stosb
	0xf4,
};
static const uint8 kStringIo[] = {
	0x31, 0xc0,                             // xor ax, ax
	0x8e, 0xd8,                             // mov ds, ax
	0x8e, 0xc0,                             // mov es, ax
	0xbe, 0x00, 0x31,                       // mov si, 0x3100
	0xb9, 0x03, 0x00,                       // mov cx, 3
	0xba, 0x80, 0x00,                       // mov dx, 0x80
	0xf3, 0x6e,                             // rep outsb
	0xbf, 0x10, 0x31,                       // mov di, 0x3110
	0xb9, 0x02, 0x00,                       // mov cx, 2
	0xf3, 0x6c,                             // rep insb
	0xf4,
};
static const uint8 kMsr[] = {
	0x66, 0xb9, 0x77, 0x02, 0x00, 0x00,     // mov ecx, 0x277
	0x0f, 0x32,                             // rdmsr
	0x66, 0x89, 0xc6,                       // mov esi, eax
	0x66, 0xb9, 0x34, 0x12, 0x00, 0x00,     // mov ecx, 0x1234
	0x0f, 0x32,                             // rdmsr
	0xf4,
};

static const uint32 kSpinAt = 0x1100;
static const uint32 kHaltAt = 0x1110;
static const uint32 kCpuidAt = 0x1120;
static const uint32 kMmioAt = 0x11c0;
static const uint32 kMsrAt = 0x1140;
static const uint32 kStringIoAt = 0x1180;


struct Guest {
	TvmmDevice device;
	uint8 *ram = nullptr;
	TvmmRun *run = nullptr;
	TvmmX86State state {};
};


static uint16 HostFcw()
{
	uint16 fcw;
	asm volatile("fnstcw %0" : "=m" (fcw));
	return fcw;
}


static uint32 HostMxcsr()
{
	uint32 mxcsr;
	asm volatile("stmxcsr %0" : "=m" (mxcsr));
	return mxcsr;
}


static void Jump(Guest &guest, uint32 ip)
{
	CHECK_OK(guest.device.GetState(0, TvmmX86StateAll, guest.state));
	guest.state.segs[TvmmX86SegCs].selector = 0;
	guest.state.segs[TvmmX86SegCs].base = 0;
	guest.state.segs[TvmmX86SegDs].selector = 0;
	guest.state.segs[TvmmX86SegDs].base = 0;
	guest.state.rip = ip;
	CHECK_OK(guest.device.SetState(0, TvmmX86StateGprs | TvmmX86StateSegs, guest.state));
}


static TvmmExit Run(Guest &guest)
{
	CHECK_OK(guest.device.Run(0));
	return guest.run->exitReason;
}


static void TestInfo(TvmmDevice &device)
{
	TvmmInfo info;
	CHECK_OK(device.GetInfo(info));
	printf("abi %u, arch %u, host %s, features %#x, max vcpus %u, slots %u\n",
		info.abiVersion, static_cast<uint32>(info.arch), strerror(info.hostStatus),
		info.features, info.maxVcpus, info.maxSlots);
	CHECK(info.abiVersion == kTvmmAbiVersion);
	if (info.hostStatus < B_OK) {
		printf("the host cannot run guests\n");
		exit(1);
	}
}


static void TestIoAndFpu(Guest &guest)
{
	printf("== port I/O and guest FPU\n");
	Jump(guest, kCodeBase);

	CHECK(Run(guest) == TvmmExit::Io);
	CHECK(guest.run->exit.io.port == 0x80 && !guest.run->exit.io.in && guest.run->exit.io.data == 0x42);

	CHECK(Run(guest) == TvmmExit::Io);
	CHECK(guest.run->exit.io.port == 0x80 && guest.run->exit.io.in && guest.run->exit.io.size == 1);
	guest.run->exit.io.data = 0x55;

	CHECK(Run(guest) == TvmmExit::Io);
	CHECK(!guest.run->exit.io.in && guest.run->exit.io.data == 0x55);

	// the loop: out dx, al; jmp
	CHECK(Run(guest) == TvmmExit::Io);
	uint16 fcw[2];
	memcpy(fcw, guest.ram + kDataBase, sizeof(fcw));
	printf("guest FCW at reset %#x, after FNINIT %#x\n", fcw[0], fcw[1]);
	CHECK(fcw[0] == 0x0040);
	CHECK(fcw[1] == 0x037f);
}


static void TestFpuIsolation(Guest &guest)
{
	printf("== host FPU across runs\n");
	const uint16 hostFcw = 0x027f;
	const uint32 hostMxcsr = 0x9fc0;
	asm volatile("fldcw %0" :: "m" (hostFcw));
	asm volatile("ldmxcsr %0" :: "m" (hostMxcsr));

	const int count = 1000000;
	const bigtime_t start = system_time();
	for (int i = 0; i < count; i++) {
		if (Run(guest) != TvmmExit::Io) {
			CHECK(false);
			break;
		}
	}
	const bigtime_t elapsed = system_time() - start;
	const uint16 fcwAfter = HostFcw();
	const uint32 mxcsrAfter = HostMxcsr();
	printf("%d port exits in %" B_PRId64 " us: %.2f us each\n", count, elapsed, double(elapsed) / count);
	printf("host FCW %#x, MXCSR %#x after the runs\n", fcwAfter, mxcsrAfter);

	CHECK(fcwAfter == hostFcw);
	CHECK(mxcsrAfter == hostMxcsr);

	const uint16 defaultFcw = 0x037f;
	const uint32 defaultMxcsr = 0x1f80;
	asm volatile("fldcw %0" :: "m" (defaultFcw));
	asm volatile("ldmxcsr %0" :: "m" (defaultMxcsr));
}


static void TestHaltCpuidMsr(Guest &guest)
{
	printf("== HLT, CPUID, MSR\n");
	Jump(guest, kHaltAt);
	CHECK(Run(guest) == TvmmExit::Halted);

	Jump(guest, kCpuidAt);
	guest.state.gprs[TvmmX86GprRax] = 0;
	CHECK_OK(guest.device.SetState(0, TvmmX86StateGprs, guest.state));
	CHECK(Run(guest) == TvmmExit::Halted);
	CHECK_OK(guest.device.GetState(0, TvmmX86StateGprs, guest.state));
	char vendor[13] {};
	memcpy(vendor, &guest.state.gprs[TvmmX86GprRbx], 4);
	memcpy(vendor + 4, &guest.state.gprs[TvmmX86GprRdx], 4);
	memcpy(vendor + 8, &guest.state.gprs[TvmmX86GprRcx], 4);
	printf("guest CPUID vendor %s\n", vendor);
	CHECK(strcmp(vendor, "AuthenticAMD") == 0);

	Jump(guest, kMsrAt);
	CHECK(Run(guest) == TvmmExit::Msr);
	CHECK(guest.run->exit.msr.msr == 0x1234 && !guest.run->exit.msr.write);
	guest.run->exit.msr.value = 0xdeadbeef12345678ull;
	CHECK(Run(guest) == TvmmExit::Halted);
	CHECK_OK(guest.device.GetState(0, TvmmX86StateGprs, guest.state));
	CHECK(uint32(guest.state.gprs[TvmmX86GprRsi]) == 0x00070406);
	CHECK(guest.state.gprs[TvmmX86GprRax] == 0x12345678);
	CHECK(guest.state.gprs[TvmmX86GprRdx] == 0xdeadbeef);

}


static bool IsMmio(Guest &guest, uint64 gpa, uint8 size, bool write)
{
	const TvmmExitMmio &mmio = guest.run->exit.mmio;
	const bool ok = guest.run->exitReason == TvmmExit::Mmio && mmio.gpa == gpa
		&& mmio.size == size && (mmio.write != 0) == write;
	if (!ok) {
		printf("exit %u: gpa %#" B_PRIx64 " size %u write %u\n",
			static_cast<uint32>(guest.run->exitReason), mmio.gpa, mmio.size, mmio.write);
	}
	return ok;
}


static void TestMmio(Guest &guest)
{
	printf("== memory mapped I/O\n");
	Jump(guest, kMmioAt);

	Run(guest);
	CHECK(IsMmio(guest, 0x20000, 1, false));
	guest.run->exit.mmio.data[0] = 0x5a;

	Run(guest);
	CHECK(IsMmio(guest, 0x20001, 1, true) && guest.run->exit.mmio.data[0] == 0x5a);

	Run(guest);
	CHECK(IsMmio(guest, 0x20002, 2, true));
	uint16 word;
	memcpy(&word, guest.run->exit.mmio.data, sizeof(word));
	CHECK(word == 0x1234);

	Run(guest);
	CHECK(IsMmio(guest, 0x20004, 2, false));
	word = 0x8001;
	memcpy(guest.run->exit.mmio.data, &word, sizeof(word));

	Run(guest);
	CHECK(IsMmio(guest, 0x20010, 1, true) && guest.run->exit.mmio.data[0] == 0x77);

	CHECK(Run(guest) == TvmmExit::Halted);
	CHECK_OK(guest.device.GetState(0, TvmmX86StateGprs, guest.state));
	CHECK(guest.state.gprs[TvmmX86GprRbx] == 0xffff8001);
	CHECK((guest.state.gprs[TvmmX86GprRdi] & 0xffff) == 0x11);
}


static void TestStringIo(Guest &guest)
{
	printf("== string I/O\n");
	memcpy(guest.ram + 0x3100, "abc", 3);
	Jump(guest, kStringIoAt);
	for (int i = 0; i < 3; i++) {
		CHECK(Run(guest) == TvmmExit::Io);
		CHECK(!guest.run->exit.io.in && guest.run->exit.io.size == 1 && guest.run->exit.io.data == uint32('a' + i));
	}
	for (int i = 0; i < 2; i++) {
		CHECK(Run(guest) == TvmmExit::Io);
		CHECK(guest.run->exit.io.in && guest.run->exit.io.size == 1);
		guest.run->exit.io.data = 0x11 * (i + 1);
	}
	CHECK(Run(guest) == TvmmExit::Halted);
	CHECK(guest.ram[0x3110] == 0x11 && guest.ram[0x3111] == 0x22);
	CHECK_OK(guest.device.GetState(0, TvmmX86StateGprs, guest.state));
	CHECK((guest.state.gprs[TvmmX86GprRcx] & 0xffff) == 0);
}


static void TestCpuidTableAndCr8(Guest &guest)
{
	printf("== CPUID table, CR8\n");
	TvmmCpuidEntry entries[1] {};
	entries[0].leaf = 1;
	entries[0].del[2] = 1;          // SSE3
	entries[0].set[3] = 0;
	CHECK_OK(guest.device.SetCpuid(entries, 1));

	Jump(guest, kCpuidAt);
	guest.state.gprs[TvmmX86GprRax] = 1;
	CHECK_OK(guest.device.SetState(0, TvmmX86StateGprs, guest.state));
	guest.run->setCr8 = 1;
	guest.run->cr8Value = 5;
	CHECK(Run(guest) == TvmmExit::Halted);
	guest.run->setCr8 = 0;
	CHECK(guest.run->cr8 == 5);
	CHECK_OK(guest.device.GetState(0, TvmmX86StateGprs | TvmmX86StateCrs, guest.state));
	CHECK((guest.state.gprs[TvmmX86GprRcx] & 1) == 0);
	CHECK((guest.state.gprs[TvmmX86GprRbx] >> 24) == 0);
	CHECK(guest.state.cr8 == 5);
	CHECK_OK(guest.device.SetCpuid(nullptr, 0));
}


struct KickArgs {
	Guest *guest;
	bigtime_t delay;
};


static status_t KickThread(void *data)
{
	auto *args = static_cast<KickArgs*>(data);
	snooze(args->delay);
	return args->guest->device.Kick(0);
}


static void TestKick(Guest &guest)
{
	printf("== kick\n");
	Jump(guest, kSpinAt);
	KickArgs args = {&guest, 100000};
	const thread_id thread = spawn_thread(KickThread, "kick", B_NORMAL_PRIORITY, &args);
	resume_thread(thread);
	const bigtime_t start = system_time();
	CHECK(Run(guest) == TvmmExit::Canceled);
	const bigtime_t elapsed = system_time() - start;
	status_t threadRes;
	wait_for_thread(thread, &threadRes);
	printf("kicked out of a spinning guest after %" B_PRId64 " us\n", elapsed);
	CHECK(threadRes == B_OK);
	CHECK(elapsed >= 90000 && elapsed < 1000000);

	// A kick before the run makes it return at once.
	CHECK_OK(guest.device.Kick(0));
	CHECK(Run(guest) == TvmmExit::Canceled);
}


// After the tests above: what they made the vcpu do.
static void TestStats(Guest &guest)
{
	printf("== statistics\n");
	TvmmVcpuStats stats {};
	CHECK_OK(guest.device.GetStats(0, stats));
	CHECK(guest.device.GetStats(1, stats) == B_BAD_VALUE);
	CHECK_OK(guest.device.GetStats(0, stats));

	const TvmmStat &io = stats.archExits[TvmmX86StatIo];
	const TvmmStat &clientIo = stats.clientExits[uint32(TvmmExit::Io)];
	printf("%" B_PRIu64 " runs, %" B_PRIu64 " entries, %" B_PRIu64 " waits\n",
		stats.runs.count, stats.entries.count, stats.waits.count);
	printf("port exits: %" B_PRIu64 ", %.0f cycles in the guest and back, "
		"%.0f handling, %.0f in the client\n",
		io.count, double(stats.entries.cycles) / stats.entries.count,
		double(io.cycles) / io.count, double(clientIo.cycles) / clientIo.count);

	// the FPU test alone ran a million
	CHECK(io.count >= 1000000);
	CHECK(clientIo.count >= io.count);
	CHECK(stats.archExits[TvmmX86StatStringIo].count >= 5);
	CHECK(stats.archExits[TvmmX86StatMmioClient].count >= 5);
	CHECK(stats.archExits[TvmmX86StatMsrClient].count >= 1);
	CHECK(stats.archExits[TvmmX86StatCpuid].count >= 2);
	CHECK(stats.archExits[TvmmX86StatHlt].count >= 6);
	// the kick of the spinning guest, if no timer came first
	CHECK(stats.archExits[TvmmX86StatIntr].count >= 1);
	CHECK(stats.clientExits[uint32(TvmmExit::Canceled)].count == 2);
	CHECK(stats.clientExits[uint32(TvmmExit::Halted)].count >= 6);
	CHECK(stats.runs.cycles > stats.entries.cycles);
	CHECK(stats.waits.count == 0);

	uint64 archCount = 0;
	for (const TvmmStat &stat: stats.archExits) {
		archCount += stat.count;
	}
	CHECK(archCount == stats.entries.count);
	uint64 clientCount = 0;
	for (const TvmmStat &stat: stats.clientExits) {
		clientCount += stat.count;
	}
	CHECK(clientCount == stats.runs.count);
}


int main()
{
	// Each line gets out before whatever comes next can hang the host.
	setvbuf(stdout, nullptr, _IONBF, 0);

	Guest guest;
	CHECK_OK(guest.device.Open());
	TestInfo(guest.device);

	CHECK_OK(guest.device.CreateVm());
	uint32 ramId = 0;
	CHECK_OK(guest.device.AllocRam(kRamSize, ramId, guest.ram));
	for (uint64 i = 0; i < kRamSize; i++) {
		CHECK(guest.ram[i] == 0);
		if (guest.ram[i] != 0) {
			break;
		}
	}
	memcpy(guest.ram + kCodeBase, kCode, sizeof(kCode));
	memcpy(guest.ram + kSpinAt, kSpin, sizeof(kSpin));
	memcpy(guest.ram + kHaltAt, kHalt, sizeof(kHalt));
	memcpy(guest.ram + kCpuidAt, kCpuid, sizeof(kCpuid));
	memcpy(guest.ram + kMmioAt, kMmio, sizeof(kMmio));
	memcpy(guest.ram + kMsrAt, kMsr, sizeof(kMsr));
	memcpy(guest.ram + kStringIoAt, kStringIo, sizeof(kStringIo));
	CHECK_OK(guest.device.MapRam(0, 0, kRamSize, ramId, 0, 0));
	CHECK(guest.device.FreeRam(ramId, guest.ram) == B_BUSY);
	CHECK_OK(guest.device.CreateVcpu(0, guest.run));

	TestIoAndFpu(guest);
	TestFpuIsolation(guest);
	TestHaltCpuidMsr(guest);
	TestMmio(guest);
	TestStringIo(guest);
	TestCpuidTableAndCr8(guest);
	TestKick(guest);
	TestStats(guest);

	CHECK_OK(guest.device.MapRam(0, 0, 0, 0, 0, 0));
	CHECK_OK(guest.device.FreeRam(ramId, guest.ram));

	if (sFailures != 0) {
		printf("%d failures\n", sFailures);
		return 1;
	}
	printf("all passed\n");
	return 0;
}
