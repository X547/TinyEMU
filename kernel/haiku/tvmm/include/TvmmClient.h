/*
 * tvmm: the client side of the kernel add-on's interface
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

#include <devctl.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>

#include <OS.h>

#include <AutoDeleterPosix.h>

#include "tvmm_abi.h"


// An open tvmm device, which holds at most one VM. The methods return what
// the kernel returned; the descriptor is closed with the object, which ends
// the VM.
class TvmmDevice {
private:
	FileDescriptorCloser fFd;

	template <typename Args>
	status_t Op(TvmmOp op, Args &args) const
	{
		return posix_devctl(fFd.Get(), op, &args, sizeof(args), nullptr);
	}

public:
	status_t Open()
	{
		fFd.SetTo(open(kTvmmDevicePath, O_RDWR | O_CLOEXEC));
		if (!fFd.IsSet()) {
			return errno;
		}
		return B_OK;
	}

	bool IsOpen() const {return fFd.IsSet();}

	status_t GetInfo(TvmmInfo &info) const
	{
		info = {};
		return Op(TvmmOpGetInfo, info);
	}

	// 'flags' are TvmmVm* flags.
	status_t CreateVm(uint32 flags = 0) const
	{
		TvmmCreateVm args {};
		args.flags = flags;
		return Op(TvmmOpCreateVm, args);
	}

	status_t SendMsi(uint64 address, uint32 data) const
	{
		TvmmSendMsi args {};
		args.address = address;
		args.data = data;
		return Op(TvmmOpSendMsi, args);
	}

	status_t ResetVcpu(uint32 vcpu) const
	{
		TvmmVcpuOp args {};
		args.vcpu = vcpu;
		return Op(TvmmOpResetVcpu, args);
	}

	// 'address' is where the RAM is mapped in this team.
	status_t AllocRam(uint64 size, uint32 &ramId, uint8 *&address) const
	{
		TvmmAllocRam args {};
		args.size = size;
		const status_t res = Op(TvmmOpAllocRam, args);
		if (res < B_OK) {
			return res;
		}
		ramId = args.ramId;
		address = reinterpret_cast<uint8*>(addr_t(args.address));
		return B_OK;
	}

	// Also removes this team's mapping of it.
	status_t FreeRam(uint32 ramId, uint8 *address) const
	{
		TvmmFreeRam args {};
		args.ramId = ramId;
		const status_t res = Op(TvmmOpFreeRam, args);
		if (res < B_OK) {
			return res;
		}
		const area_id area = area_for(address);
		if (area >= 0) {
			delete_area(area);
		}
		return B_OK;
	}

	status_t MapRam(uint32 slot, uint64 gpa, uint64 size, uint32 ramId, uint64 offset, uint32 flags) const
	{
		TvmmMapRam args {};
		args.slot = slot;
		args.flags = flags;
		args.gpa = gpa;
		args.size = size;
		args.ramId = ramId;
		args.offset = offset;
		return Op(TvmmOpMapRam, args);
	}

	status_t CreateVcpu(uint32 index, TvmmRun *&run) const
	{
		TvmmCreateVcpu args {};
		args.index = index;
		const status_t res = Op(TvmmOpCreateVcpu, args);
		if (res < B_OK) {
			return res;
		}
		run = reinterpret_cast<TvmmRun*>(addr_t(args.runAddress));
		return B_OK;
	}

	status_t Run(uint32 vcpu) const
	{
		TvmmVcpuOp args {};
		args.vcpu = vcpu;
		return Op(TvmmOpRun, args);
	}

	status_t Kick(uint32 vcpu) const
	{
		TvmmVcpuOp args {};
		args.vcpu = vcpu;
		return Op(TvmmOpKick, args);
	}

	// The classes not asked for keep what 'state' held.
	status_t GetState(uint32 vcpu, uint32 classes, TvmmX86State &state) const
	{
		TvmmStateOp args {};
		args.vcpu = vcpu;
		args.classes = classes;
		args.stateAddress = addr_t(&state);
		args.stateSize = sizeof(state);
		return Op(TvmmOpGetState, args);
	}

	// Any thread, while the vcpu runs too.
	status_t GetStats(uint32 vcpu, TvmmVcpuStats &stats) const
	{
		TvmmGetStats args {};
		args.vcpu = vcpu;
		const status_t res = Op(TvmmOpGetStats, args);
		if (res < B_OK) {
			return res;
		}
		stats = args.stats;
		return B_OK;
	}

	status_t SetCpuid(const TvmmCpuidEntry *entries, uint32 count) const
	{
		TvmmSetCpuid args {};
		args.count = count;
		args.entriesAddress = addr_t(entries);
		return Op(TvmmOpSetCpuid, args);
	}

	status_t SetState(uint32 vcpu, uint32 classes, const TvmmX86State &state) const
	{
		TvmmStateOp args {};
		args.vcpu = vcpu;
		args.classes = classes;
		args.stateAddress = addr_t(&state);
		args.stateSize = sizeof(state);
		return Op(TvmmOpSetState, args);
	}
};
