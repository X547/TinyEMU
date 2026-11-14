#pragma once

#include <SupportDefs.h>

#include <lock.h>

#include "GuestMemory.h"

class ArchVm;
class Vcpu;
struct TvmmAllocRam;
struct TvmmCreateVcpu;
struct TvmmMapRam;
struct TvmmSetCpuid;


class Vm {
public:
	static constexpr uint32 kMaxVcpus = 256;

private:
	team_id fTeam;
	mutex fLock;
	ArchVm *fArch = nullptr;
	bool fArchAcquired = false;
	GuestMemory fMemory;
	Vcpu *fVcpus[kMaxVcpus] {};
	int64 fTlbGeneration = 1;
	int32 fClosing = 0;

	void FlushGuestTlbs();

public:
	explicit Vm(team_id team);
	~Vm();

	status_t Init(uint32 flags);

	team_id Team() const {return fTeam;}
	ArchVm &Arch() const {return *fArch;}
	int64 TlbGeneration() const;
	bool Closing() const;

	status_t AllocRam(TvmmAllocRam &args);
	status_t FreeRam(uint32 id);
	status_t MapRam(const TvmmMapRam &args);
	status_t CreateVcpu(TvmmCreateVcpu &args);
	// nullptr for an index with no vcpu
	Vcpu *GetVcpu(uint32 index);
	status_t SetCpuid(const TvmmSetCpuid &args);

	// Guest RAM, for what the kernel emulates; false where the range is not
	// RAM, or for a write, not writable RAM.
	bool IsRam(uint64 gpa, bool write);
	bool ReadGuest(uint64 gpa, void *buffer, size_t size);
	bool WriteGuest(uint64 gpa, const void *buffer, size_t size);

	// Every vcpu leaves its run, and none enters again.
	void Close();
};
