#include "RiscVHost.h"

#include <stdlib.h>
#include <string.h>

#include <new>

#include <KernelExport.h>

#include <smp.h>
#include <util/AutoLock.h>
#include <util/atomic.h>

#include "Arch.h"
#include "Common.h"
#include "RiscVContext.h"
#include "RiscVCsr.h"
#include "RiscVVm.h"
#include "Vm.h"
#include "tvmm_abi.h"


// The device tree the kernel booted with; the FDT bus manager moves it to the
// heap.
extern void *gFDT;


namespace {


constexpr uint32 kFdtMagic = 0xd00dfeed;
constexpr uint32 kFdtHeaderSize = 40;

enum {
	kFdtBeginNode = 1,
	kFdtEndNode = 2,
	kFdtProp = 3,
	kFdtNop = 4,
	kFdtEnd = 9,
};


// The first hart of the device tree, as it describes itself.
struct HartDescription {
	uint64 timebase;
	char deviceType[16];
	char status[16];
	char isa[256];
	// riscv,isa-extensions: strings, each ended by a NUL
	char extensions[2048];
	uint32 extensionsSize;
	char mmuType[32];
	uint32 cbomBlockSize;
	uint32 cbozBlockSize;
};


struct Probed {
	bool done = false;
	TvmmArch arch = TvmmArch::None;
	status_t status = B_NOT_SUPPORTED;
	uint32 features = 0;
	uint64 hgatpMode = 0;
	uint32 vmidBits = 0;
	uint64 henvcfg = 0;
	uint64 timebase = 0;
	// what the host's harts have
	uint32 misa = 0;
	char extensions[2048] {};
	uint32 extensionsSize = 0;
	uint32 satpMode = 0;
	uint32 cbomBlockSize = 0;
	uint32 cbozBlockSize = 0;
};


} // namespace


static Probed sProbed;
static HartDescription sHart;

static mutex sHostLock = MUTEX_INITIALIZER("tvmm riscv host");
static int32 sAcquireCount = 0;
static int64 *sLoadedVcpus = nullptr;
static int64 sNextVcpuId = 0;


//#pragma mark - device tree


static uint32 Be32(const uint8 *bytes)
{
	return (uint32(bytes[0]) << 24) | (uint32(bytes[1]) << 16) | (uint32(bytes[2]) << 8) | bytes[3];
}


static uint32 Align4(uint32 value)
{
	return (value + 3) & ~3u;
}


// The tree, if it is where the kernel says and still mapped there.
static bool FdtBlob(const uint8 *&blob, uint32 &size)
{
	const auto *fdt = static_cast<const uint8*>(gFDT);
	if (fdt == nullptr) {
		return false;
	}
	area_info info;
	const area_id area = area_for(const_cast<uint8*>(fdt));
	if (area < 0 || get_area_info(area, &info) < B_OK) {
		return false;
	}
	const addr_t end = addr_t(info.address) + info.size;
	if (addr_t(fdt) + kFdtHeaderSize > end || Be32(fdt) != kFdtMagic) {
		return false;
	}
	size = Be32(fdt + 4);
	if (size < kFdtHeaderSize || size > end - addr_t(fdt)) {
		return false;
	}
	blob = fdt;
	return true;
}


// A string property, NUL terminated in 'buffer' even if it is not in the
// tree.
static void CopyString(char *buffer, size_t bufferSize, const uint8 *value, uint32 length)
{
	const size_t count = length < bufferSize - 1 ? length : bufferSize - 1;
	memcpy(buffer, value, count);
	buffer[count] = '\0';
}


static uint64 ReadCells(const uint8 *value, uint32 length)
{
	if (length == 4) {
		return Be32(value);
	}
	if (length == 8) {
		return (uint64(Be32(value)) << 32) | Be32(value + 4);
	}
	return 0;
}


static void HartProperty(HartDescription &hart, const char *name, const uint8 *value, uint32 length)
{
	if (strcmp(name, "device_type") == 0) {
		CopyString(hart.deviceType, sizeof(hart.deviceType), value, length);
	} else if (strcmp(name, "status") == 0) {
		CopyString(hart.status, sizeof(hart.status), value, length);
	} else if (strcmp(name, "riscv,isa") == 0) {
		CopyString(hart.isa, sizeof(hart.isa), value, length);
	} else if (strcmp(name, "riscv,isa-extensions") == 0) {
		if (length <= sizeof(hart.extensions)) {
			memcpy(hart.extensions, value, length);
			hart.extensionsSize = length;
		}
	} else if (strcmp(name, "mmu-type") == 0) {
		CopyString(hart.mmuType, sizeof(hart.mmuType), value, length);
	} else if (strcmp(name, "riscv,cbom-block-size") == 0) {
		hart.cbomBlockSize = uint32(ReadCells(value, length));
	} else if (strcmp(name, "riscv,cboz-block-size") == 0) {
		hart.cbozBlockSize = uint32(ReadCells(value, length));
	} else if (strcmp(name, "timebase-frequency") == 0) {
		hart.timebase = ReadCells(value, length);
	}
}


// The first hart under /cpus that is there, with the timebase of /cpus
// unless it has its own.
static bool ParseFdt(HartDescription &hart)
{
	const uint8 *fdt = nullptr;
	uint32 size = 0;
	if (!FdtBlob(fdt, size)) {
		return false;
	}
	const uint32 structOffset = Be32(fdt + 8);
	const uint32 stringsOffset = Be32(fdt + 12);
	const uint32 stringsSize = Be32(fdt + 32);
	const uint32 structSize = Be32(fdt + 36);
	if (structOffset > size || structSize > size - structOffset
		|| stringsOffset > size || stringsSize > size - stringsOffset) {
		return false;
	}
	const auto *strings = reinterpret_cast<const char*>(fdt + stringsOffset);
	const uint8 *p = fdt + structOffset;
	const uint8 *end = p + structSize;

	// the root node is at depth 1
	int depth = 0;
	int cpusDepth = -1;
	uint64 cpusTimebase = 0;
	bool collecting = false;
	bool found = false;
	HartDescription candidate;
	while (end - p >= 4) {
		const uint32 token = Be32(p);
		p += 4;
		switch (token) {
			case kFdtBeginNode: {
				const auto *name = reinterpret_cast<const char*>(p);
				const size_t nameLength = strnlen(name, end - p);
				if (nameLength == size_t(end - p)) {
					return false;
				}
				p += Align4(nameLength + 1);
				depth++;
				if (depth == 2 && strcmp(name, "cpus") == 0) {
					cpusDepth = depth;
				} else if (cpusDepth > 0 && depth == cpusDepth + 1 && !found
					&& strncmp(name, "cpu", 3) == 0) {
					candidate = {};
					collecting = true;
				}
				break;
			}
			case kFdtEndNode:
				if (collecting && depth == cpusDepth + 1) {
					collecting = false;
					if (strcmp(candidate.deviceType, "cpu") == 0
						&& (candidate.status[0] == '\0' || strcmp(candidate.status, "okay") == 0)) {
						hart = candidate;
						found = true;
					}
				}
				if (depth == cpusDepth) {
					cpusDepth = -1;
				}
				depth--;
				break;
			case kFdtProp: {
				if (end - p < 8) {
					return false;
				}
				const uint32 length = Be32(p);
				const uint32 nameOffset = Be32(p + 4);
				p += 8;
				if (length > uint32(end - p) || nameOffset >= stringsSize
					|| strnlen(strings + nameOffset, stringsSize - nameOffset) == stringsSize - nameOffset) {
					return false;
				}
				const char *name = strings + nameOffset;
				if (depth == cpusDepth && strcmp(name, "timebase-frequency") == 0) {
					cpusTimebase = ReadCells(p, length);
				} else if (collecting && depth == cpusDepth + 1) {
					HartProperty(candidate, name, p, length);
				}
				p += Align4(length);
				if (p > end) {
					return false;
				}
				break;
			}
			case kFdtNop:
				break;
			case kFdtEnd:
				end = p;
				break;
			default:
				return false;
		}
	}
	if (found && hart.timebase == 0) {
		hart.timebase = cpusTimebase;
	}
	return found;
}


//#pragma mark - the host's harts


static bool HasExtension(const char *name)
{
	for (uint32 i = 0; i < sProbed.extensionsSize; i += strlen(sProbed.extensions + i) + 1) {
		if (strcmp(sProbed.extensions + i, name) == 0) {
			return true;
		}
	}
	return false;
}


static void AddExtension(const char *name, size_t length)
{
	char buffer[64];
	if (length == 0 || length >= sizeof(buffer)) {
		return;
	}
	memcpy(buffer, name, length);
	buffer[length] = '\0';
	if (HasExtension(buffer) || sProbed.extensionsSize + length + 1 > sizeof(sProbed.extensions)) {
		return;
	}
	memcpy(sProbed.extensions + sProbed.extensionsSize, buffer, length + 1);
	sProbed.extensionsSize += length + 1;
}


// riscv,isa-extensions if there is one, riscv,isa otherwise: the single
// letters go to misa, the others to the list, lower case.
static void ReadIsa(const HartDescription &hart)
{
	auto addLetter = [](char letter) {
		if (letter >= 'a' && letter <= 'z') {
			sProbed.misa |= 1u << (letter - 'a');
		}
	};
	if (hart.extensionsSize > 0) {
		for (uint32 i = 0; i < hart.extensionsSize;) {
			const char *name = hart.extensions + i;
			const size_t length = strnlen(name, hart.extensionsSize - i);
			if (length == 1) {
				addLetter(name[0]);
			} else {
				AddExtension(name, length);
			}
			i += length + 1;
		}
		return;
	}
	const char *isa = hart.isa;
	if (strncmp(isa, "rv64", 4) != 0) {
		return;
	}
	isa += 4;
	while (*isa != '\0' && *isa != '_') {
		addLetter(*isa++);
	}
	while (*isa == '_') {
		const char *name = ++isa;
		while (*isa != '\0' && *isa != '_') {
			isa++;
		}
		AddExtension(name, isa - name);
	}
}


// With interrupts disabled. hgatp takes a mode it has, and as many VMID bits
// as it has; a mode it does not have leaves it as it was.
static bool ProbeHgatpMode(uint64 mode, uint32 &vmidBits)
{
	TVMM_CSR_WRITE(TVMM_CSR_HGATP, (mode << RiscV::kHgatpModeShift) | RiscV::kHgatpVmidMask);
	const uint64 value = TVMM_CSR_READ(TVMM_CSR_HGATP);
	TVMM_CSR_WRITE(TVMM_CSR_HGATP, 0);
	if ((value >> RiscV::kHgatpModeShift) != mode) {
		return false;
	}
	// the bits are the low ones of the field; libgcc's popcount is not the
	// kernel's
	vmidBits = 0;
	for (uint64 vmid = (value & RiscV::kHgatpVmidMask) >> RiscV::kHgatpVmidShift; vmid != 0; vmid >>= 1) {
		vmidBits += vmid & 1;
	}
	return true;
}


static void ProbeOnce()
{
	if (sProbed.done) {
		return;
	}
	sProbed.done = true;

	if (!ParseFdt(sHart)) {
		TVMM_LOG("no hart in the device tree\n");
		return;
	}
	sProbed.features |= TvmmRiscVFeatureFdt;
	ReadIsa(sHart);
	if ((sProbed.misa & (1u << ('h' - 'a'))) == 0) {
		return;
	}
	sProbed.arch = TvmmArch::RiscVH;

	if (sHart.timebase == 0) {
		TVMM_LOG("the device tree has no timebase\n");
		return;
	}
	sProbed.timebase = sHart.timebase;
	// the FPU state is switched with the D extension's instructions
	if ((sProbed.misa & (1u << ('d' - 'a'))) == 0) {
		return;
	}

	uint64 henvcfgWanted = 0;
	if (HasExtension("zicbom")) {
		henvcfgWanted |= RiscV::kEnvcfgCbieFlush | RiscV::kEnvcfgCbcfe;
	}
	if (HasExtension("zicboz")) {
		henvcfgWanted |= RiscV::kEnvcfgCbze;
	}

	RiscVTrap trap {};
	uint64 henvcfg = 0;
	bool g39 = false;
	bool g48 = false;
	uint32 vmid39 = 0;
	uint32 vmid48 = 0;
	{
		const cpu_status state = disable_interrupts();
		g48 = ProbeHgatpMode(RiscV::kHgatpModeSv48x4, vmid48);
		g39 = ProbeHgatpMode(RiscV::kHgatpModeSv39x4, vmid39);
		henvcfg = tvmm_riscv_probe_henvcfg(henvcfgWanted, &trap);
		if (!trap.taken) {
			tvmm_riscv_probe_henvcfg(0, &trap);
		}
		restore_interrupts(state);
	}
	if (trap.taken) {
		TVMM_LOG("henvcfg is missing (scause %#" B_PRIx64 ")\n", trap.scause);
		return;
	}
	sProbed.henvcfg = henvcfg & henvcfgWanted;
	if (g48) {
		sProbed.hgatpMode = RiscV::kHgatpModeSv48x4;
		sProbed.vmidBits = vmid48;
		sProbed.features |= TvmmRiscVFeatureSv48x4;
	} else if (g39) {
		sProbed.hgatpMode = RiscV::kHgatpModeSv39x4;
		sProbed.vmidBits = vmid39;
		sProbed.features |= TvmmRiscVFeatureSv39x4;
	} else {
		TVMM_LOG("hgatp has no G-stage translation\n");
		return;
	}
	if ((sProbed.henvcfg & RiscV::kEnvcfgCbcfe) != 0 || (sProbed.henvcfg & RiscV::kEnvcfgCbze) != 0) {
		sProbed.features |= TvmmRiscVFeatureCbo;
	}

	if (strcmp(sHart.mmuType, "riscv,sv57") == 0) {
		sProbed.satpMode = 10;
	} else if (strcmp(sHart.mmuType, "riscv,sv48") == 0) {
		sProbed.satpMode = 9;
	} else {
		sProbed.satpMode = 8;
	}
	sProbed.cbomBlockSize = sHart.cbomBlockSize;
	sProbed.cbozBlockSize = sHart.cbozBlockSize;
	sProbed.status = B_OK;
}


//#pragma mark - Arch


void Arch::Probe(TvmmInfo &info)
{
	ProbeOnce();
	info.arch = sProbed.arch;
	info.hostStatus = sProbed.status;
	info.features = sProbed.features;
	info.maxVcpus = MaxVcpus();
	info.archId[0] = sProbed.vmidBits;
	info.archId[1] = sProbed.hgatpMode == RiscV::kHgatpModeSv48x4 ? 50
		: sProbed.hgatpMode == RiscV::kHgatpModeSv39x4 ? 41 : 0;
}


status_t Arch::HostStatus()
{
	ProbeOnce();
	return sProbed.status;
}


// What a guest can have as the host has it, in hardware and with nothing of
// the hypervisor's: no H, no vector state, no counters but the base ones,
// and no timer compare of its own.
static const char *const kGuestExtensions[] = {
	"zicsr", "zifencei", "zicntr", "zihintpause", "zihintntl", "zicond",
	"zimop", "zcmop", "zacas", "zabha",
	"zba", "zbb", "zbc", "zbs", "zbkb", "zbkc", "zbkx",
	"zknd", "zkne", "zknh", "zksed", "zksh", "zkt",
	"zfa", "zfh", "zfhmin", "zca", "zcb", "zcd",
	"zicbop", "ziccif", "ziccrse", "ziccamoa", "zicclsm", "za64rs", "za128rs", "zic64b",
	"svade", "svnapot", "svinval", "svvptc",
	"ssccptr", "sstvecd", "sstvala", "sscounterenw",
};


status_t Arch::GetHostIsa(TvmmHostIsa &isa)
{
	ProbeOnce();
	CHECK_RET(sProbed.status);

	const uint32 letters = (1u << ('i' - 'a')) | (1u << ('m' - 'a')) | (1u << ('a' - 'a'))
		| (1u << ('f' - 'a')) | (1u << ('d' - 'a')) | (1u << ('c' - 'a'));
	isa.misa = (sProbed.misa & letters) | (1u << ('s' - 'a')) | (1u << ('u' - 'a'));
	isa.timebaseFrequency = sProbed.timebase;
	isa.satpMode = sProbed.satpMode;

	size_t used = 0;
	auto add = [&isa, &used](const char *name) {
		const size_t length = strlen(name);
		// room for this one's NUL and the list's
		if (used + length + 2 > sizeof(isa.extensions)) {
			return;
		}
		memcpy(isa.extensions + used, name, length + 1);
		used += length + 1;
	};
	const bool bitmanip = (sProbed.misa & (1u << ('b' - 'a'))) != 0;
	for (const char *name: kGuestExtensions) {
		const bool impliedByB = bitmanip
			&& (strcmp(name, "zba") == 0 || strcmp(name, "zbb") == 0 || strcmp(name, "zbs") == 0);
		if (HasExtension(name) || impliedByB) {
			add(name);
		}
	}
	const uint64 cbom = RiscV::kEnvcfgCbieFlush | RiscV::kEnvcfgCbcfe;
	if (HasExtension("zicbom") && (sProbed.henvcfg & cbom) == cbom) {
		add("zicbom");
		isa.cbomBlockSize = sProbed.cbomBlockSize;
	}
	if (HasExtension("zicboz") && (sProbed.henvcfg & RiscV::kEnvcfgCbze) != 0) {
		add("zicboz");
		isa.cbozBlockSize = sProbed.cbozBlockSize;
	}
	isa.extensions[used] = '\0';
	return B_OK;
}


uint32 Arch::MaxVcpus()
{
	return Vm::kMaxVcpus;
}


status_t Arch::Acquire()
{
	MutexLocker locker(sHostLock);
	if (sAcquireCount > 0) {
		sAcquireCount++;
		return B_OK;
	}
	sLoadedVcpus = static_cast<int64*>(calloc(smp_get_num_cpus(), sizeof(int64)));
	if (sLoadedVcpus == nullptr) {
		return B_NO_MEMORY;
	}
	sAcquireCount = 1;
	return B_OK;
}


void Arch::Release()
{
	MutexLocker locker(sHostLock);
	if (--sAcquireCount > 0) {
		return;
	}
	free(sLoadedVcpus);
	sLoadedVcpus = nullptr;
}


uint64 Arch::Cycles()
{
	return RiscV::ReadTime();
}


status_t Arch::CreateVm(Vm &vm, uint32 flags, ArchVm *&arch)
{
	if (flags != 0) {
		return B_BAD_VALUE;
	}
	auto *riscVVm = new(std::nothrow) RiscVVm(vm);
	if (riscVVm == nullptr) {
		return B_NO_MEMORY;
	}
	const status_t res = riscVVm->Init();
	if (res < B_OK) {
		delete riscVVm;
		return res;
	}
	arch = riscVVm;
	return B_OK;
}


//#pragma mark - RiscVHost


uint64 RiscVHost::HgatpMode()
{
	return sProbed.hgatpMode;
}


uint64 RiscVHost::Henvcfg()
{
	return sProbed.henvcfg;
}


uint64 RiscVHost::TimebaseFrequency()
{
	return sProbed.timebase;
}


int64 RiscVHost::NewVcpuId()
{
	return atomic_add64(&sNextVcpuId, 1) + 1;
}


int64 RiscVHost::LoadedVcpu(int32 cpu)
{
	return sLoadedVcpus[cpu];
}


void RiscVHost::SetLoadedVcpu(int32 cpu, int64 id)
{
	sLoadedVcpus[cpu] = id;
}
