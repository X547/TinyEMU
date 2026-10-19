/*
 * RISCV CPU emulator private definitions
 * 
 * Copyright (c) 2016-2017 Fabrice Bellard
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
#ifndef RISCV_CPU_PRIV_H
#define RISCV_CPU_PRIV_H

/* included inside a namespace, so <atomic>, <bit> and <type_traits> come from
   the includer */
#include "riscv_cpu.h"
#include "bits.h"
#include "cutils.h"

#define __exception __attribute__((warn_unused_result))

#define CONFIG_EXT_C /* compressed instructions */

/* the integer types of a width in bits */
template <int Bits> struct IntOfWidth;
template <> struct IntOfWidth<32> { typedef uint32_t U; typedef int32_t S; };
template <> struct IntOfWidth<64> { typedef uint64_t U; typedef int64_t S; };
#ifdef HAVE_INT128
template <> struct IntOfWidth<128> { typedef uint128_t U; typedef int128_t S; };
#endif

template <int Bits> using uintn_t = typename IntOfWidth<Bits>::U;
template <int Bits> using intn_t = typename IntOfWidth<Bits>::S;

/* The decoder keeps the rounding mode in a plain int because get_insn_rm()
   reports an invalid encoding as -1. Every use site rejects the negative case
   before reaching softfp, so the narrowing to RoundingModeEnum is safe there. */
#define RM(rm) static_cast<RoundingModeEnum>(rm)

#define TLB_BITS 8
#define TLB_SIZE (1 << TLB_BITS)

#define CAUSE_MISALIGNED_FETCH    0x0
#define CAUSE_FAULT_FETCH         0x1
#define CAUSE_ILLEGAL_INSTRUCTION 0x2
#define CAUSE_BREAKPOINT          0x3
#define CAUSE_MISALIGNED_LOAD     0x4
#define CAUSE_FAULT_LOAD          0x5
#define CAUSE_MISALIGNED_STORE    0x6
#define CAUSE_FAULT_STORE         0x7
#define CAUSE_USER_ECALL          0x8
#define CAUSE_SUPERVISOR_ECALL    0x9
#define CAUSE_HYPERVISOR_ECALL    0xa
#define CAUSE_MACHINE_ECALL       0xb
#define CAUSE_FETCH_PAGE_FAULT    0xc
#define CAUSE_LOAD_PAGE_FAULT     0xd
#define CAUSE_STORE_PAGE_FAULT    0xf

/* Note: converted to correct bit position at runtime */
#define CAUSE_INTERRUPT  bit_at(31)

#define PRV_U 0
#define PRV_S 1
#define PRV_H 2
#define PRV_M 3

/* misa CSR */
#define MCPUID_SUPER   bit_at('S' - 'A')
#define MCPUID_USER    bit_at('U' - 'A')
#define MCPUID_I       bit_at('I' - 'A')
#define MCPUID_M       bit_at('M' - 'A')
#define MCPUID_A       bit_at('A' - 'A')
#define MCPUID_F       bit_at('F' - 'A')
#define MCPUID_D       bit_at('D' - 'A')
#define MCPUID_Q       bit_at('Q' - 'A')
#define MCPUID_C       bit_at('C' - 'A')

/* mstatus CSR */

#define MSTATUS_SIE_SHIFT 1
#define MSTATUS_MIE_SHIFT 3
#define MSTATUS_SPIE_SHIFT 5
#define MSTATUS_MPIE_SHIFT 7
#define MSTATUS_SPP_SHIFT 8
#define MSTATUS_MPP_SHIFT 11
#define MSTATUS_FS_SHIFT 13
#define MSTATUS_UXL_SHIFT 32
#define MSTATUS_SXL_SHIFT 34

/* masks of the register's own width, the target_ulong of the RISCVCPUState
   member using them, so that complementing one keeps the fields above bit 31
   of a 64 bit mstatus */
#define MSTATUS_SIE bit_at<target_ulong>(MSTATUS_SIE_SHIFT)
#define MSTATUS_MIE bit_at<target_ulong>(MSTATUS_MIE_SHIFT)
#define MSTATUS_SPIE bit_at<target_ulong>(MSTATUS_SPIE_SHIFT)
#define MSTATUS_MPIE bit_at<target_ulong>(MSTATUS_MPIE_SHIFT)
#define MSTATUS_SPP bit_at<target_ulong>(MSTATUS_SPP_SHIFT)
#define MSTATUS_MPP field_mask<target_ulong>(MSTATUS_MPP_SHIFT, 2)
#define MSTATUS_FS field_mask<target_ulong>(MSTATUS_FS_SHIFT, 2)
#define MSTATUS_XS field_mask<target_ulong>(15, 2)
#define MSTATUS_MPRV bit_at<target_ulong>(17)
#define MSTATUS_SUM bit_at<target_ulong>(18)
#define MSTATUS_MXR bit_at<target_ulong>(19)
#define MSTATUS_TVM bit_at<target_ulong>(20)
#define MSTATUS_TW bit_at<target_ulong>(21)
#define MSTATUS_TSR bit_at<target_ulong>(22)
#define MSTATUS_UXL_MASK field_mask<uint64_t>(MSTATUS_UXL_SHIFT, 2)
#define MSTATUS_SXL_MASK field_mask<uint64_t>(MSTATUS_SXL_SHIFT, 2)

/* menvcfg and senvcfg CSRs. Both are 64 bit whatever XLEN is */
#define ENVCFG_FIOM  bit_at<uint64_t>(0)
#define MENVCFG_ADUE bit_at<uint64_t>(61)
#define MENVCFG_STCE bit_at<uint64_t>(63)

#define PG_SHIFT 12
#define PG_MASK bit_mask<target_ulong>(PG_SHIFT)

#define IMSIC_WORDS ((RISCV_IMSIC_NUM_IDS + 64) / 64)

/* An IMSIC interrupt file. Bit n of the arrays is identity n. */
typedef struct {
    uint32_t eidelivery;
    uint32_t eithreshold;
    uint64_t eip[IMSIC_WORDS];
    uint64_t eie[IMSIC_WORDS];
} ImsicFile;

#define IMSIC_FILE_M 0
#define IMSIC_FILE_S 1

/* log2 of the size of an access of type T */
template <typename T>
static constexpr int access_size_log2 = std::countr_zero(sizeof(T));

/* A hart whose XLEN can be at most MaxXlen. */
template <int MaxXlen>
struct RISCVCPUState: public RISCVCPU {
    static constexpr int MAX_XLEN = MaxXlen;
    /* the floating point register width */
    static constexpr int FLEN = MaxXlen == 128 ? 128 : 64;
    /* the maximum memory access width */
    static constexpr int MLEN = FLEN;

    typedef uintn_t<MAX_XLEN> target_ulong;
    typedef intn_t<MAX_XLEN> target_long;
    typedef uintn_t<FLEN> fp_uint;
    typedef uintn_t<MLEN> mem_uint_t;

    struct TLBEntry {
        target_ulong vaddr;
        uintptr_t mem_addend;
    };

    /* Bits 63:54 of a 64 bit PTE are claimed by Svnapot (63), Svpbmt (62:61)
       and future standard use (60:54). None of them are implemented here, so
       a PTE that sets any of them is malformed and must fault. */
    static constexpr target_ulong PTE_RESERVED_MASK =
        MAX_XLEN >= 64 ? field_mask<target_ulong>(54, 10) : 0;

    target_ulong pc;
    target_ulong reg[32];

    fp_uint fp_reg[32];
    uint32_t fflags;
    uint8_t frm;

    uint8_t cur_xlen;  /* current XLEN value, <= MAX_XLEN */
    uint8_t priv; /* see PRV_x */
    uint8_t fs; /* MSTATUS_FS value */
    uint8_t mxl; /* MXL field in MISA register */
    
    int32_t n_cycles; /* only used inside the CPU loop */
    uint64_t insn_counter;
    /* mcycle and minstret are writable and both derive from insn_counter.
       While a counter runs its field holds the difference between the counter
       and insn_counter; while mcountinhibit freezes it, the field holds the
       counter's value outright. */
    uint64_t mcycle_base;
    uint64_t minstret_base;
    bool power_down_flag;
    int pending_exception; /* used during MMU exception handling */
    /* the machine's real time counter, shared with its timer device */
    RtcTimeSource *rtc_time_source;
    target_ulong pending_tval;
    
    /* CSRs */
    target_ulong mstatus;
    target_ulong mtvec;
    target_ulong mscratch;
    target_ulong mepc;
    target_ulong mcause;
    target_ulong mtval;
    target_ulong mhartid; /* ro */
    uint32_t misa;
    uint32_t mie;
    uint32_t mip;
    uint32_t medeleg;
    uint32_t mideleg;
    uint32_t mcounteren;
    uint32_t mcountinhibit;
    uint64_t menvcfg;

    target_ulong stvec;
    target_ulong sscratch;
    target_ulong sepc;
    target_ulong scause;
    target_ulong stval;
    /* currently 64 bit physical addresses max */
    std::conditional_t<MAX_XLEN == 32, uint32_t, uint64_t> satp;
    uint32_t scounteren;
    uint64_t senvcfg;
    uint64_t stimecmp;

    /* Advanced Interrupt Architecture */
    RISCVInterruptArch intr_arch;
    target_ulong miselect;
    target_ulong siselect;
    ImsicFile imsic[2]; /* IMSIC_FILE_M, IMSIC_FILE_S */

    /* Posted from other threads and taken in by process_inbox(). */
    std::atomic<bool> inbox_pending;
    /* Interp() is to return; posted with inbox_pending */
    std::atomic<bool> exit_request;
    /* when the Sstc comparator next fires, for the machine's timers */
    std::atomic<uint64_t> stimer_deadline;
    std::atomic<uint32_t> irq_lines;
    std::atomic<uint64_t> msi_pending[2][IMSIC_WORDS];
    uint32_t irq_line_mask; /* the mip bits irq_lines drives */

    /* SBI implemented by the machine; null when firmware runs in M-mode */
    RISCVSbiHandler *sbi;
    /* only HsmStart() leaves this state; interrupts do not */
    bool hsm_stopped;
    /* set by HsmSuspend() until the hart wakes up again */
    bool hsm_suspended;
    bool hsm_resume; /* non-retentive: resume at hsm_addr */
    std::atomic<int> hsm_state; /* RISCVHsmState, for other threads */
    /* where a start or a resume goes: written by whoever claimed the start,
       before it posts hsm_start_posted */
    target_ulong hsm_addr;
    target_ulong hsm_opaque;
    std::atomic<bool> hsm_start_posted;
    std::atomic<bool> ssip_posted;
    std::atomic<uint64_t> tlb_flush_req;
    std::atomic<uint64_t> tlb_flush_done;

    target_ulong load_res; /* for atomic LR/SC */
    /* what LR read: with other harts, SC stores only if memory still holds
       it */
    mem_uint_t load_val;
    bool smp;

    PhysMemoryMap *mem_map;
    DeviceLock *device_lock;

    TLBEntry tlb_read[TLB_SIZE];
    TLBEntry tlb_write[TLB_SIZE];
    TLBEntry tlb_code[TLB_SIZE];

    static void fprint_target_ulong(FILE *f, target_ulong a);
    static void print_target_ulong(target_ulong a);
    static uint64_t riscv_cpu_rtc_time(RISCVCPUState *s);
    static void wake_on_irq(RISCVCPUState *s);
    static void set_mip(RISCVCPUState *s, uint32_t mask);
    static uint64_t update_stimer(RISCVCPUState *s);
    static void dump_regs(RISCVCPUState *s);
    static __attribute__((unused)) void cpu_abort(RISCVCPUState *s);
    static inline void lr_acquire(RISCVCPUState *s, uint32_t insn);
    static bool update_pte(RISCVCPUState *s, target_ulong pte_addr,
                           target_ulong old, target_ulong pte,
                           int pte_size_log2);
    static int get_phys_addr(RISCVCPUState *s, target_ulong *ppaddr,
                             target_ulong vaddr, int access);
    static bool device_maps(PhysMemoryRange *pr, target_ulong paddr);
    static uint8_t *tlb_fill_write(RISCVCPUState *s, target_ulong addr,
                                   PhysMemoryRange *pr, target_ulong paddr);
    static int target_read_slow(RISCVCPUState *s, mem_uint_t *pval,
                                target_ulong addr, int size_log2);
    static void device_write(RISCVCPUState *s, PhysMemoryRange *pr,
                             target_ulong paddr, mem_uint_t val,
                             int size_log2);
    static int target_write_slow(RISCVCPUState *s, target_ulong addr,
                                 mem_uint_t val, int size_log2);
    static no_inline __exception int target_rmw_ptr(RISCVCPUState *s,
                                                    target_ulong addr,
                                                    int size_log2,
                                                    uint8_t **pptr);
    static inline __exception int rmw_ptr(RISCVCPUState *s, target_ulong addr,
                                          int size_log2, uint8_t **pptr);
    static inline void fence(RISCVCPUState *s, uint32_t insn);
    static no_inline __exception int target_read_insn_slow(RISCVCPUState *s,
                                                           uint8_t **pptr,
                                                           target_ulong addr);
    static inline __exception int target_read_insn_u16(RISCVCPUState *s,
                                                       uint16_t *pinsn,
                                                       target_ulong addr);
    static void tlb_init(RISCVCPUState *s);
    static void tlb_flush_all(RISCVCPUState *s);
    static void tlb_flush_vaddr(RISCVCPUState *s, target_ulong vaddr);
    static void tlb_flush_ram_range(TLBEntry *tlb, uint8_t *ram_ptr,
                                    uint8_t *ram_end);
    static void flush_tlb_write_range_ram(RISCVCPUState *s,
                                          uint8_t *ram_ptr,
                                          size_t ram_size);
    static target_ulong get_mstatus(RISCVCPUState *s, target_ulong mask);
    static void set_mstatus(RISCVCPUState *s, target_ulong val);
    static bool counter_accessible(RISCVCPUState *s, int counter);
    static uint64_t get_mcycle(RISCVCPUState *s);
    static uint64_t get_minstret(RISCVCPUState *s);
    static void set_mcycle(RISCVCPUState *s, uint64_t val);
    static void set_minstret(RISCVCPUState *s, uint64_t val);
    static bool stimecmp_accessible(RISCVCPUState *s);
    static uint64_t csr64_written(RISCVCPUState *s, uint64_t old_val,
                                  target_ulong val, bool high);
    static target_ulong top_interrupt(RISCVCPUState *s, bool supervisor);
    static void imsic_update(RISCVCPUState *s, int file);
    static target_ulong imsic_topei(RISCVCPUState *s, int file);
    static void imsic_claim(RISCVCPUState *s, int file);
    static void take_posted_tlb_flush(RISCVCPUState *s);
    static void process_inbox(RISCVCPUState *s);
    static bool imsic_array_reg(RISCVCPUState *s, ImsicFile *f,
                                target_ulong sel, uint64_t **pword,
                                int *pshift, int *plen);
    static int ireg_read(RISCVCPUState *s, int file, target_ulong sel,
                         target_ulong *pval);
    static int ireg_write(RISCVCPUState *s, int file, target_ulong sel,
                          target_ulong val);
    static int csr_read(RISCVCPUState *s, target_ulong *pval, uint32_t csr,
                        bool will_write);
    static void set_frm(RISCVCPUState *s, unsigned int val);
    static int get_insn_rm(RISCVCPUState *s, unsigned int rm);
    static int csr_write(RISCVCPUState *s, uint32_t csr, target_ulong val);
    static void set_priv(RISCVCPUState *s, int priv);
    static void sbi_enter(RISCVCPUState *s, target_ulong pc,
                          target_ulong opaque);
    static void sbi_stop(RISCVCPUState *s);
    static uint64_t sbi_arg(RISCVCPUState *s, int reg);
    static target_ulong sbi_result(RISCVCPUState *s, uint64_t val);
    static void sbi_ecall(RISCVCPUState *s);
    static void raise_exception2(RISCVCPUState *s, uint32_t cause,
                                 target_ulong tval);
    static void raise_exception(RISCVCPUState *s, uint32_t cause);
    static void clear_mprv_on_return(RISCVCPUState *s, int new_priv);
    static void handle_sret(RISCVCPUState *s);
    static void handle_mret(RISCVCPUState *s);
    static inline uint32_t get_pending_irq_mask(RISCVCPUState *s);
    static __exception int raise_interrupt(RISCVCPUState *s);
    static void riscv_cpu_interp(RISCVCPUState *s);
    static RISCVCPUState *riscv_cpu_init(PhysMemoryMap *mem_map,
                                         uint32_t hart_id);

    template <typename T>
    static T phys_read(RISCVCPUState *s, target_ulong addr);
    template <typename T>
    static void phys_write(RISCVCPUState *s, target_ulong addr,
                           std::type_identity_t<T> val);
    template <typename T>
    static inline target_ulong tlb_access_tag(target_ulong addr);
    template <typename T>
    static force_inline __exception int target_read(RISCVCPUState *s, T *pval,
                                                     target_ulong addr);
    template <typename T>
    static inline __exception int target_write(RISCVCPUState *s,
                                               target_ulong addr,
                                               std::type_identity_t<T> val);
    template <typename U>
    static force_inline int exec_amo(RISCVCPUState *s, uint32_t insn,
                                     uint32_t rs1, uint32_t rs2,
                                     target_ulong *pval);

    /* the bits of an FP register above a value of F bits, which NaN-box
       it */
    template <int F>
    static constexpr fp_uint fp_high()
    {
        if constexpr (F < FLEN)
            return (fp_uint)-1 << F;
        else
            return 0;
    }

    template <int F, int Xlen>
    static force_inline int exec_fp_op(RISCVCPUState *s, uint32_t funct5,
                                       uint32_t rd, uint32_t rs1,
                                       uint32_t rs2, int32_t rm);
    template <int Xlen>
    static void no_inline riscv_cpu_interp_x(RISCVCPUState *s,
                                             int n_cycles1);

    void Interp() override;
    void RequestExit() override;
    uint64_t Cycles() override;
    void SetIrqLine(uint32_t mask, bool level) override;
    uint64_t UpdateSTimer() override;
    uint64_t STimerDeadline() override;
    bool PowerDown() override;
    uint32_t Misa() override;
    void FlushTlbWriteRangeRam(uint8_t *ram_ptr, size_t ram_size) override;
    void SetRtcTimeSource(RtcTimeSource *source) override;
    void SetDeviceLock(DeviceLock *lock) override;
    void SetSmp(bool smp) override;
    void SetInterruptArch(RISCVInterruptArch arch) override;
    void ImsicSetPending(bool supervisor, uint32_t id) override;
    uint32_t HartId() override;
    void SetSbiHandler(RISCVSbiHandler *sbi) override;
    RISCVHsmState HsmState() override;
    bool HsmStart(uint64_t addr, uint64_t opaque) override;
    void PostSoftIrq() override;
    uint64_t PostTlbFlush() override;
    bool TlbFlushDone(uint64_t ticket) override;
    void HsmStop() override;
    void HsmSuspend(bool retentive, uint64_t resume_addr,
                    uint64_t opaque) override;
    void SetTimer(uint64_t stime) override;
    void ClearSoftIrq() override;
    void FlushTlb() override;
    void TakePostedTlbFlush() override;
    bool ReadVirtual(uint64_t vaddr, uint64_t *pval) override;
};

/* The page tag an access of type T hits: an access that crosses its page
   matches no TLB entry. */
template <int MaxXlen>
template <typename T>
auto RISCVCPUState<MaxXlen>::tlb_access_tag(target_ulong addr) -> target_ulong
{
    return addr & ~(PG_MASK & ~(target_ulong)(sizeof(T) - 1));
}

/* return 0 if OK, != 0 if exception */
template <int MaxXlen>
template <typename T>
int RISCVCPUState<MaxXlen>::target_read(RISCVCPUState *s, T *pval,
                                        target_ulong addr)
{
    static_assert(sizeof(T) * 8 <= MLEN);
    uint32_t tlb_idx;
    tlb_idx = get_bits(addr, PG_SHIFT, TLB_BITS);
    if (likely(s->tlb_read[tlb_idx].vaddr == tlb_access_tag<T>(addr))) {
        *pval = *(T *)(s->tlb_read[tlb_idx].mem_addend + (uintptr_t)addr);
    } else {
        mem_uint_t val;
        int ret;
        ret = target_read_slow(s, &val, addr, access_size_log2<T>);
        if (ret)
            return ret;
        *pval = val;
    }
    return 0;
}

/* T is given explicitly: the value is usually wider than the access */
template <int MaxXlen>
template <typename T>
int RISCVCPUState<MaxXlen>::target_write(RISCVCPUState *s, target_ulong addr,
                                         std::type_identity_t<T> val)
{
    static_assert(sizeof(T) * 8 <= MLEN);
    uint32_t tlb_idx;
    tlb_idx = get_bits(addr, PG_SHIFT, TLB_BITS);
    if (likely(s->tlb_write[tlb_idx].vaddr == tlb_access_tag<T>(addr))) {
        *(T *)(s->tlb_write[tlb_idx].mem_addend + (uintptr_t)addr) = val;
        return 0;
    } else {
        return target_write_slow(s, addr, val, access_size_log2<T>);
    }
}

#endif /* RISCV_CPU_PRIV_H */
