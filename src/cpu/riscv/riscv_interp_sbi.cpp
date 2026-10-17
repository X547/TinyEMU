/*
 * RISC-V SBI for interpreted harts
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
#include <atomic>
#include <thread>

#include "device_lock.h"
#include "riscv_interp_sbi.h"
#include "riscv_sbi_defs.h"
#include "riscv_vcpu.h"

#define SBI_SPEC_VERSION ((2 << 24) | 0) /* 2.0 */
/* not a registered implementation ID: "TEMU" */
#define SBI_IMPL_ID      0x54454d55
#define SBI_IMPL_VERSION 1


RISCVSbi::RISCVSbi(RISCVSbiHarts &harts, RISCVCpuTarget &target,
                   DeviceLock &device_lock):
    fHarts(harts),
    fTarget(target),
    fDeviceLock(device_lock)
{
}


static uint64_t xlen_ones(int xlen)
{
    return xlen == 32 ? UINT32_MAX : UINT64_MAX;
}


bool RISCVSbi::HartSet(int xlen, uint64_t mask, uint64_t base, uint64_t *set)
{
    int count = fHarts.HartCount();
    uint64_t all = count == 64 ? UINT64_MAX : ((uint64_t)1 << count) - 1;

    if (base == xlen_ones(xlen)) {
        *set = all;
        return true;
    }
    *set = 0;
    for (int i = 0; i < 64 && (mask >> i) != 0; i++) {
        if (!((mask >> i) & 1))
            continue;
        if (base >= (uint64_t)count || (uint64_t)i >= count - base)
            return false;
        *set |= (uint64_t)1 << (base + i);
    }
    return true;
}


/* The legacy calls pass the address of a mask in S-mode memory, or zero for
   every hart. */
bool RISCVSbi::LegacyHartSet(RISCVCPU *cpu, uint64_t mask_addr, uint64_t *set)
{
    uint64_t mask;

    if (mask_addr == 0)
        return HartSet(64, 0, UINT64_MAX, set);
    if (!cpu->ReadVirtual(mask_addr, &mask))
        return false;
    return HartSet(64, mask, 0, set);
}


void RISCVSbi::SendIpi(uint64_t set)
{
    for (int hart = 0; set != 0; hart++, set >>= 1) {
        if (set & 1) {
            fHarts.Hart(hart)->PostSoftIrq();
            fTarget.WakeHart(hart);
        }
    }
}


/* Returns once every hart in 'set' has flushed its TLB. A hart that waits
   here takes in the flushes others ask of it meanwhile, so that two harts
   fencing each other do not wait for ever. */
void RISCVSbi::RemoteSfence(RISCVCPU *cpu, uint64_t set)
{
    uint64_t tickets[64];
    int self = cpu->HartId();

    for (int hart = 0; hart < 64; hart++) {
        if (!((set >> hart) & 1))
            continue;
        if (hart == self) {
            cpu->FlushTlb();
        } else {
            tickets[hart] = fHarts.Hart(hart)->PostTlbFlush();
            fTarget.WakeHart(hart);
        }
    }
    for (int hart = 0; hart < 64; hart++) {
        if (!((set >> hart) & 1) || hart == self)
            continue;
        while (!fHarts.Hart(hart)->TlbFlushDone(tickets[hart])) {
            if (fTarget.Stopping())
                return;
            cpu->TakePostedTlbFlush();
            std::this_thread::yield();
        }
    }
}


int64_t RISCVSbi::Base(uint32_t fid, uint64_t a[8], uint64_t *value)
{
    switch (fid) {
    case 0: /* get_spec_version */
        *value = SBI_SPEC_VERSION;
        break;
    case 1: /* get_impl_id */
        *value = SBI_IMPL_ID;
        break;
    case 2: /* get_impl_version */
        *value = SBI_IMPL_VERSION;
        break;
    case 3: /* probe_extension */
        switch (a[0]) {
        case SBI_EXT_BASE:
        case SBI_EXT_TIME:
        case SBI_EXT_IPI:
        case SBI_EXT_RFNC:
        case SBI_EXT_HSM:
        case SBI_EXT_SRST:
        case SBI_EXT_DBCN:
            *value = 1;
            break;
        default:
            *value = a[0] <= SBI_EXT_LEGACY_LAST;
            break;
        }
        break;
    case 4: /* get_mvendorid */
    case 5: /* get_marchid */
    case 6: /* get_mimpid */
        /* what the CSRs read */
        *value = 0;
        break;
    default:
        return SBI_ERR_NOT_SUPPORTED;
    }
    return SBI_SUCCESS;
}


int64_t RISCVSbi::Hsm(RISCVCPU *cpu, uint32_t fid, uint64_t a[8],
                      uint64_t *value)
{
    switch (fid) {
    case 0: /* hart_start */
        if (a[0] >= (uint64_t)fHarts.HartCount())
            return SBI_ERR_INVALID_PARAM;
        if (!fHarts.Hart(a[0])->HsmStart(a[1], a[2]))
            return SBI_ERR_ALREADY_AVAILABLE;
        fTarget.WakeHart(a[0]);
        return SBI_SUCCESS;
    case 1: /* hart_stop */
        cpu->HsmStop();
        return SBI_SUCCESS;
    case 2: /* hart_get_status */
        if (a[0] >= (uint64_t)fHarts.HartCount())
            return SBI_ERR_INVALID_PARAM;
        *value = fHarts.Hart(a[0])->HsmState();
        return SBI_SUCCESS;
    case 3: /* hart_suspend */
        if (a[0] == SBI_HSM_SUSPEND_RETENTIVE) {
            cpu->HsmSuspend(true, 0, 0);
        } else if (a[0] == SBI_HSM_SUSPEND_NON_RETENTIVE) {
            cpu->HsmSuspend(false, a[1], a[2]);
        } else {
            return SBI_ERR_INVALID_PARAM;
        }
        return SBI_SUCCESS;
    default:
        return SBI_ERR_NOT_SUPPORTED;
    }
}


int64_t RISCVSbi::Reset(RISCVCPU *cpu, uint64_t type, uint64_t reason)
{
    bool reboot;

    if (reason != SBI_SRST_REASON_NONE && reason != SBI_SRST_REASON_FAILURE)
        return SBI_ERR_INVALID_PARAM;
    switch (type) {
    case SBI_SRST_SHUTDOWN:
        reboot = false;
        break;
    case SBI_SRST_COLD_REBOOT:
    case SBI_SRST_WARM_REBOOT:
        reboot = true;
        break;
    default:
        return SBI_ERR_INVALID_PARAM;
    }
    {
        DeviceLocker locker(fDeviceLock);
        fTarget.SystemReset(reboot, reason == SBI_SRST_REASON_FAILURE);
    }
    /* the call does not return */
    cpu->HsmStop();
    return SBI_SUCCESS;
}


bool RISCVSbi::TargetCall(RISCVCPU *cpu, int xlen, uint64_t a[8])
{
    DeviceLocker locker(fDeviceLock);

    return fTarget.SbiCall(cpu->HartId(), xlen, a);
}


/* The SBI v0.1 calls return their result in a0 and keep a1. */
void RISCVSbi::Legacy(RISCVCPU *cpu, int xlen, uint32_t eid, uint64_t a[8])
{
    int64_t ret = SBI_SUCCESS;
    uint64_t set;

    switch (eid) {
    case SBI_LEGACY_SET_TIMER:
        cpu->SetTimer(xlen == 32 ? a[0] | a[1] << 32 : a[0]);
        break;
    case SBI_LEGACY_PUTCHAR:
    case SBI_LEGACY_GETCHAR:
        if (TargetCall(cpu, xlen, a))
            return;
        ret = SBI_ERR_NOT_SUPPORTED;
        break;
    case SBI_LEGACY_CLEAR_IPI:
        cpu->ClearSoftIrq();
        break;
    case SBI_LEGACY_SEND_IPI:
    case SBI_LEGACY_FENCE_I:
    case SBI_LEGACY_SFENCE_VMA:
    case SBI_LEGACY_SFENCE_ASID:
        if (!LegacyHartSet(cpu, a[0], &set)) {
            ret = SBI_ERR_INVALID_PARAM;
        } else if (eid == SBI_LEGACY_SEND_IPI) {
            SendIpi(set);
        } else if (eid == SBI_LEGACY_FENCE_I) {
            std::atomic_thread_fence(std::memory_order_seq_cst);
        } else {
            RemoteSfence(cpu, set);
        }
        break;
    case SBI_LEGACY_SHUTDOWN:
        Reset(cpu, SBI_SRST_SHUTDOWN, SBI_SRST_REASON_NONE);
        break;
    }
    a[0] = ret;
}


void RISCVSbi::Ecall(RISCVCPU *cpu, int xlen, uint64_t a[8])
{
    uint32_t eid = a[7];
    uint32_t fid = a[6];
    uint64_t value = 0;
    uint64_t set;
    int64_t err;

    if (a[7] <= SBI_EXT_LEGACY_LAST) {
        Legacy(cpu, xlen, eid, a);
        return;
    }
    if (a[7] == SBI_EXT_DBCN && TargetCall(cpu, xlen, a))
        return;
    if (a[7] != eid) {
        err = SBI_ERR_NOT_SUPPORTED;
    } else switch (eid) {
    case SBI_EXT_BASE:
        err = Base(fid, a, &value);
        break;
    case SBI_EXT_TIME:
        if (fid != 0) {
            err = SBI_ERR_NOT_SUPPORTED;
            break;
        }
        cpu->SetTimer(xlen == 32 ? a[0] | a[1] << 32 : a[0]);
        err = SBI_SUCCESS;
        break;
    case SBI_EXT_IPI:
        if (fid != 0) {
            err = SBI_ERR_NOT_SUPPORTED;
        } else if (!HartSet(xlen, a[0], a[1], &set)) {
            err = SBI_ERR_INVALID_PARAM;
        } else {
            SendIpi(set);
            err = SBI_SUCCESS;
        }
        break;
    case SBI_EXT_RFNC:
        /* the hypervisor fences need the H extension */
        if (fid > 2) {
            err = SBI_ERR_NOT_SUPPORTED;
        } else if (!HartSet(xlen, a[0], a[1], &set)) {
            err = SBI_ERR_INVALID_PARAM;
        } else {
            /* nothing caches decoded instructions, so FENCE.I needs no more
               than the ordering */
            if (fid == 0)
                std::atomic_thread_fence(std::memory_order_seq_cst);
            else
                RemoteSfence(cpu, set);
            err = SBI_SUCCESS;
        }
        break;
    case SBI_EXT_HSM:
        err = Hsm(cpu, fid, a, &value);
        break;
    case SBI_EXT_SRST:
        err = fid == 0 ? Reset(cpu, a[0], a[1]) : SBI_ERR_NOT_SUPPORTED;
        break;
    default:
        err = SBI_ERR_NOT_SUPPORTED;
        break;
    }
    a[0] = err;
    a[1] = value;
}


void RISCVSbi::MachineTrap(RISCVCPU *cpu, uint64_t cause, uint64_t pc,
                           uint64_t tval)
{
    DeviceLocker locker(fDeviceLock);

    fTarget.HartFault(cpu->HartId(), cause, pc, tval);
}
