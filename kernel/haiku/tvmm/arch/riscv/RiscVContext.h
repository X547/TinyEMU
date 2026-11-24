#pragma once

// What the world switch saves and loads, shared with RiscVEntry.S.

// the guest's general registers, x0's slot unused
#define TVMM_CTX_GUEST_X(n)    ((n) * 8)
#define TVMM_CTX_GUEST_PC      256
// sstatus and hstatus to enter with, then as the exit left them
#define TVMM_CTX_GUEST_SSTATUS 264
#define TVMM_CTX_GUEST_HSTATUS 272
// the exit
#define TVMM_CTX_SCAUSE        280
#define TVMM_CTX_STVAL         288
#define TVMM_CTX_HTVAL         296
#define TVMM_CTX_HTINST        304
// the host's callee saved registers and the trap CSRs
#define TVMM_CTX_HOST_RA       312
#define TVMM_CTX_HOST_SP       320
#define TVMM_CTX_HOST_GP       328
#define TVMM_CTX_HOST_TP       336
#define TVMM_CTX_HOST_S(n)     (344 + (n) * 8)
#define TVMM_CTX_HOST_SSTATUS  440
#define TVMM_CTX_HOST_HSTATUS  448
#define TVMM_CTX_HOST_STVEC    456
#define TVMM_CTX_HOST_SSCRATCH 464
#define TVMM_CTX_SIZE          472


#ifdef __cplusplus

#include <stddef.h>

#include <SupportDefs.h>


struct RiscVContext {
	uint64 x[32];
	uint64 pc;
	uint64 sstatus;
	uint64 hstatus;
	uint64 scause;
	uint64 stval;
	uint64 htval;
	uint64 htinst;
	uint64 hostRa;
	uint64 hostSp;
	uint64 hostGp;
	uint64 hostTp;
	uint64 hostS[12];
	uint64 hostSstatus;
	uint64 hostHstatus;
	uint64 hostStvec;
	uint64 hostSscratch;
};

static_assert(offsetof(RiscVContext, pc) == TVMM_CTX_GUEST_PC);
static_assert(offsetof(RiscVContext, sstatus) == TVMM_CTX_GUEST_SSTATUS);
static_assert(offsetof(RiscVContext, hstatus) == TVMM_CTX_GUEST_HSTATUS);
static_assert(offsetof(RiscVContext, scause) == TVMM_CTX_SCAUSE);
static_assert(offsetof(RiscVContext, htinst) == TVMM_CTX_HTINST);
static_assert(offsetof(RiscVContext, hostRa) == TVMM_CTX_HOST_RA);
static_assert(offsetof(RiscVContext, hostS) == TVMM_CTX_HOST_S(0));
static_assert(offsetof(RiscVContext, hostSstatus) == TVMM_CTX_HOST_SSTATUS);
static_assert(offsetof(RiscVContext, hostSscratch) == TVMM_CTX_HOST_SSCRATCH);
static_assert(sizeof(RiscVContext) == TVMM_CTX_SIZE);


// The floating point registers, as fsd and fsrcr store them.
struct RiscVFpu {
	uint64 f[32];
	uint64 fcsr;
};


// A trap that an access of the hypervisor's to the guest took instead.
struct RiscVTrap {
	uint64 taken;
	uint64 scause;
	uint64 stval;
};


extern "C" {

// Interrupts disabled: runs the guest until it traps to the hypervisor.
void tvmm_riscv_run(RiscVContext *context);

// Interrupts disabled, right after an exit and with its hgatp and vsatp: the
// instruction at the guest's 'pc', as it would fetch it. Sets 'trap' if the
// guest's translation faulted.
uint32 tvmm_riscv_read_insn(uint64 pc, RiscVTrap *trap);

// Interrupts disabled: writes 'value' to henvcfg and returns what it holds,
// or sets 'trap' if there is none.
uint64 tvmm_riscv_probe_henvcfg(uint64 value, RiscVTrap *trap);

void tvmm_riscv_save_fpu(RiscVFpu *fpu);
void tvmm_riscv_load_fpu(const RiscVFpu *fpu);

// hfence.gvma and hfence.vvma for every address, with hgatp's VMID
void tvmm_riscv_hfence_all();

}

#endif // __cplusplus
