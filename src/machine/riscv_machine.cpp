/*
 * RISCV machine
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
#include <stdlib.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include <string>

#include "bits.h"
#include "cutils.h"
#include "host_console.h"
#include "host_riscv_hypervisor.h"
#include "host_thread.h"
#include "host_time.h"
#include "iomem.h"
#include "riscv_cpu.h"
#include "riscv_sbi_defs.h"
#include "riscv_vcpu.h"
#include "uart.h"
#include "virtio.h"
#include "machine.h"
#include "device.h"
#include "devices.h"
#include "fdt.h"
#include "pci.h"
#include "plic.h"
#include "aplic.h"
#include "clint.h"
#include "htif.h"

/* RISCV machine */

#define RISCV_MAX_HARTS 64

/* the external interrupt controllers a configuration can choose */
enum RISCVIntcType {
    RISCV_INTC_PLIC,
    RISCV_INTC_APLIC,
    RISCV_INTC_APLIC_IMSIC,
};

class RISCVMachine final:
    public VirtMachine,
    public HartIrqTarget,
    public PCIMsiTarget,
    public ClintHost,
    public RISCVCpuTarget {
public:
    /* the harts and the memory map everything else is in, so it goes
       last */
    std::unique_ptr<RISCVCpuBackend> fCpus;
    PhysMemoryMap *mem_map = nullptr;
    int max_xlen = 0;
    int hart_count = 0;
    RISCVIntcType intc_type = RISCV_INTC_PLIC;
    uint64_t ram_size = 0;
    /* the harts answer the SBI, with no firmware in M-mode */
    bool native_sbi = false;
    /* the configuration's "compatible", before the machine's own; empty
       when it gave none */
    std::string compatible;
    HostConsole *fConsole = nullptr;
    std::unique_ptr<Clint> clint;
    std::unique_ptr<HTIF> htif;
    /* the one selected by intc_type */
    std::unique_ptr<PLIC> plic;
    std::unique_ptr<APLIC> aplic;
    /* after everything its devices use, so that it goes first */
    std::unique_ptr<SystemBus> bus;
    /* bits of the hart number in an IMSIC page address */
    int imsic_hart_bits = 0;
    /* Each hart runs on a thread of its own, which waits on its wakeup while
       the hart waits for an interrupt. The machine thread keeps the timers,
       the harts' Sstc deadlines among them, and the screen. */
    std::vector<std::thread> fHartThreadList;
    std::unique_ptr<HostWakeup[]> fHartWakeups;
    /* the Sstc deadline each hart was last woken for; machine thread */
    uint64_t fSTimerKicked[RISCV_MAX_HARTS] {};

    ~RISCVMachine() override;

    void HartThread(int hart);
    /* Wakes the machine thread for a timer due at 'deadline' on the time
       counter. Any thread. */
    void KickTimersBefore(uint64_t deadline);
    /* time counter ticks in host microseconds */
    uint64_t TicksToUs(uint64_t ticks);

    /* the debug console's write and the legacy putchar */
    int64_t ConsoleWrite(uint64_t len, uint64_t addr, uint64_t *value);
    void ConsolePutchar(uint8_t c);

    /* memory-mapped register blocks */
    uint32_t ImsicRead(uint32_t offset, int size_log2);
    void ImsicWriteM(uint32_t offset, uint32_t val, int size_log2);
    void ImsicWriteS(uint32_t offset, uint32_t val, int size_log2);

    DeviceIOAdapter<RISCVMachine, &RISCVMachine::ImsicRead,
                    &RISCVMachine::ImsicWriteM> fImsicIoM {*this};
    DeviceIOAdapter<RISCVMachine, &RISCVMachine::ImsicRead,
                    &RISCVMachine::ImsicWriteS> fImsicIoS {*this};

    /* HartIrqTarget */
    void SetExternalIrq(int hart, bool supervisor, int level) override;

    /* PCIMsiTarget */
    void SendMsi(uint64_t addr, uint32_t data) override;

    /* RISCVCpuTarget */
    bool SbiCall(int hart, int xlen, uint64_t a[8]) override;
    void SystemReset(bool reboot, bool failure) override;
    void HartFault(int hart, uint64_t cause, uint64_t pc,
                   uint64_t tval) override;
    /* Makes 'hart' look at its interrupts again. */
    void WakeHart(int hart) override;
    void TimerChanged(uint64_t deadline) override;
    bool Stopping() override {return StopRequested();}

    /* ClintHost */
    uint64_t ClintTime() override;
    void SetClintIrq(int hart, bool timer, bool level) override;
    void ClintTimerChanged(uint64_t deadline) override;

    /* VirtMachine */
    void StartProcessors() override;
    void JoinProcessors() override;
    int64_t RunTimers() override;
    void InterruptExecution() override;
};

#define LOW_RAM_SIZE   0x00010000 /* 64KB */
#define RAM_BASE_ADDR  0x80000000
#define CLINT_BASE_ADDR 0x02000000
#define HTIF_BASE_ADDR 0x40008000
#define HTIF_SIZE      0x00001000
#define PLIC_BASE_ADDR 0x40100000
#define APLIC_M_BASE_ADDR 0x0c000000
#define APLIC_S_BASE_ADDR 0x0d000000
/* One page per hart from each base, in blocks aligned to their size as the
   AIA specification asks. */
#define IMSIC_M_BASE_ADDR 0x08000000
#define IMSIC_S_BASE_ADDR 0x09000000
#define IMSIC_PAGE_SIZE   0x1000
#define IMSIC_SETEIPNUM_LE 0x000

/* Wired interrupt lines, whichever controller takes them; line 0 does not
   exist. */
#define RISCV_IRQ_LINES PLIC_NUM_SOURCES

/* Everything the configuration adds is placed in here. It is the one hole in
   the architectural layout large enough for an ECAM window and a PCI aperture
   as well as the MMIO devices, and it stays below 4 GB because that is where
   anything a 32 bit BAR must reach has to live. */
#define DEVICE_WINDOW_BASE 0x10000000
#define DEVICE_WINDOW_SIZE 0x30000000

/* And a second window for the few things that may sit above 4 GB: a PCI host
   bridge's 64 bit aperture is the only one. It starts far enough up that no
   plausible amount of RAM reaches it, and overlaps are reported all the same
   because every claim goes in the same map. */
#define HIGH_DEVICE_WINDOW_BASE 0x1000000000ull
#define HIGH_DEVICE_WINDOW_SIZE 0x1000000000ull

/* The machine's PCI I/O port space, which every host bridge's I/O aperture is
   a slice of. This processor has no port instructions, so nothing addresses
   it directly: a bridge reaches its own slice through a memory window and
   says so in its device tree "ranges".

   It starts at port 0, so a machine with one host bridge -- which is what
   nearly every configuration is -- gets the 0 to 0xffff a guest expects to
   find. It runs well past 64 KB only so that a second bridge has somewhere
   to put its aperture rather than colliding with the first. */
#define PCI_IO_WINDOW_BASE 0
#define PCI_IO_WINDOW_SIZE 0x1000000 /* 16 MB */

uint64_t RISCVMachine::TicksToUs(uint64_t ticks)
{
    uint64_t freq = fCpus->TimebaseFrequency();

    if (freq == 1000000)
        return ticks;
    if (ticks > UINT64_MAX / 1000000)
        return ticks / freq * 1000000;
    return ticks * 1000000 / freq;
}

/* A counter that does not follow host time can only be watched. */
void RISCVMachine::KickTimersBefore(uint64_t deadline)
{
    if (!fCpus->RealTimeClock()) {
        Kick();
        return;
    }
    uint64_t now = fCpus->Time();
    uint64_t host_now = host_monotonic_us();
    uint64_t us = deadline > now ? TicksToUs(deadline - now) : 0;
    if (us <= UINT64_MAX - host_now)
        KickBefore(host_now + us);
}

void RISCVMachine::WakeHart(int hart)
{
    fHartWakeups[hart].Kick();
}

void RISCVMachine::TimerChanged(uint64_t deadline)
{
    KickTimersBefore(deadline);
}

/* The memory regions of the IMSIC interrupt files. The interrupt files
   themselves are part of the harts; a page only takes the MSI writes. */
uint32_t RISCVMachine::ImsicRead(uint32_t offset, int size_log2)
{
    (void)offset;
    (void)size_log2;
    return 0;
}

void RISCVMachine::ImsicWriteM(uint32_t offset, uint32_t val, int size_log2)
{
    uint32_t hart = offset / IMSIC_PAGE_SIZE;

    if (offset % IMSIC_PAGE_SIZE == IMSIC_SETEIPNUM_LE &&
        hart < (uint32_t)hart_count) {
        fCpus->Vcpu(hart).ImsicSetPending(false, val);
        WakeHart(hart);
    }
}

void RISCVMachine::ImsicWriteS(uint32_t offset, uint32_t val, int size_log2)
{
    uint32_t hart = offset / IMSIC_PAGE_SIZE;

    if (offset % IMSIC_PAGE_SIZE == IMSIC_SETEIPNUM_LE &&
        hart < (uint32_t)hart_count) {
        fCpus->Vcpu(hart).ImsicSetPending(true, val);
        WakeHart(hart);
    }
}

/* PCI MSIs are written straight to the IMSIC pages. */
void RISCVMachine::SendMsi(uint64_t addr, uint32_t data)
{
    mem_map->IoWrite(addr, data, 2);
}

static uint64_t imsic_region_size(RISCVMachine *m)
{
    return (uint64_t)IMSIC_PAGE_SIZE << m->imsic_hart_bits;
}

void RISCVMachine::SetExternalIrq(int hart, bool supervisor, int level)
{
    fCpus->Vcpu(hart).SetIrqLine(supervisor ? MIP_SEIP : MIP_MEIP, level);
    WakeHart(hart);
}

static uint8_t *get_ram_ptr(RISCVMachine *s, uint64_t paddr, bool is_rw)
{
    return s->mem_map->GetRamPtr(paddr, is_rw);
}

/* FDT machine description */

/* The interrupt files of one privilege level; returns the node's phandle.
   The hart index width is left for the guest to derive from the number of
   harts listed, which gives the layout the pages are placed in. */
static uint32_t riscv_build_imsic_fdt(RISCVMachine *m, FDTBuilder &fdt,
                                      uint64_t base,
                                      const uint32_t *intc_phandle,
                                      bool supervisor)
{
    uint32_t phandle = fdt.AllocPhandle();

    fdt.BeginNodeNum("interrupt-controller", base);
    fdt.PropStr("compatible", "riscv,imsics");
    /* only M-mode firmware has a use for the machine level files */
    if (!supervisor && m->native_sbi)
        fdt.PropStr("status", "disabled");
    fdt.PropU64Range("reg", base, imsic_region_size(m));
    for (int hart = 0; hart < m->hart_count; hart++) {
        fdt.AddCellU32(intc_phandle[hart]);
        fdt.AddCellU32(supervisor ? 9 : 11); /* S or M ext irq */
    }
    fdt.PropCells("interrupts-extended");
    fdt.PropEmpty("interrupt-controller");
    fdt.PropU32("#interrupt-cells", 0);
    fdt.PropEmpty("msi-controller");
    fdt.PropU32("#msi-cells", 0);
    fdt.PropU32("riscv,num-ids", RISCV_IMSIC_NUM_IDS);
    fdt.PropU32("phandle", phandle);
    fdt.EndNode();
    return phandle;
}

/* 'reserved_start' and 'reserved_size' are the RAM the guest must keep out
   of: the firmware, or the tree itself without one. */
static int riscv_build_fdt(RISCVMachine *m, uint8_t *dst,
                           uint64_t reserved_start, uint64_t reserved_size,
                           uint64_t kernel_start, uint64_t kernel_size,
                           uint64_t initrd_start, uint64_t initrd_size,
                           const char *cmd_line)
{
    FDTBuilder fdt;
    FDTContext ctx;
    int size, max_xlen, i;
    std::string isa_string;
    std::string ext_list;
    uint32_t misa;
    uint32_t intc_phandle[RISCV_MAX_HARTS];

    ctx.fdt = &fdt;

    /* No PMP protects firmware here: only this keeps a kernel from
       allocating over the M mode trap handler it depends on. */
    fdt.AddReservation(reserved_start, reserved_size);

    fdt.BeginNode("");
    fdt.PropU32("#address-cells", 2);
    fdt.PropU32("#size-cells", 2);
    if (m->compatible.empty())
        fdt.PropStr("compatible", "ucbbar,riscvemu-bar_dev");
    else
        fdt.PropStrList("compatible", m->compatible.c_str(),
                        "ucbbar,riscvemu-bar_dev", NULL);
    fdt.PropStr("model", "ucbbar,riscvemu-bare");

    max_xlen = m->max_xlen;
    const RISCVHartIsa &isa = m->fCpus->Isa();
    misa = isa.misa;

    isa_string = "rv" + std::to_string(max_xlen);
    /* The single letter extensions must appear in the canonical order given
       by the ISA specification, not in misa bit order: Linux rejects a hart
       whose riscv,isa does not begin with "rv<xlen>ima" and refuses to boot.
       Any bit outside the canonical list is appended afterwards so that no
       extension is silently dropped. */
    {
        static const char canonical[] = "iemafdgqlcbkjtpvnhsu";
        uint32_t emitted = 0;
        for (const char *p = canonical; *p != '\0'; p++) {
            uint32_t bit = 1 << (*p - 'a');
            if (misa & bit) {
                isa_string += *p;
                emitted |= bit;
            }
        }
        for(i = 0; i < 26; i++) {
            if (get_bit(misa, i) && !get_bit(emitted, i))
                isa_string += 'a' + i;
        }
    }
    /* Multi-letter extensions follow the single letter ones, each introduced
       by an underscore. */
    for (const std::string &ext : isa.extensions)
        isa_string += "_" + ext;

    {
        /* A packed list of NUL terminated strings. The privilege modes 'S'
           and 'U' are not extensions and have no place here, unlike in the
           "riscv,isa" string above. */
        static const char canonical[] = "imafdqch";
        for (const char *c = canonical; *c != '\0'; c++) {
            if (misa & (1 << (*c - 'a'))) {
                ext_list += *c;
                ext_list += '\0';
            }
        }
        for (const std::string &ext : isa.extensions) {
            ext_list += ext;
            ext_list += '\0';
        }
    }

    const char *mmu_type;
    switch (isa.satp_mode) {
    case RISCV_SATP_SV32: mmu_type = "riscv,sv32"; break;
    case RISCV_SATP_SV39: mmu_type = "riscv,sv39"; break;
    case RISCV_SATP_SV48: mmu_type = "riscv,sv48"; break;
    default: mmu_type = "riscv,sv57"; break;
    }

    /* CPU list */
    fdt.BeginNode("cpus");
    fdt.PropU32("#address-cells", 1);
    fdt.PropU32("#size-cells", 0);
    fdt.PropU32("timebase-frequency", m->fCpus->TimebaseFrequency());

    for (int hart = 0; hart < m->hart_count; hart++) {
        fdt.BeginNodeNum("cpu", hart);
        fdt.PropStr("device_type", "cpu");
        fdt.PropU32("reg", hart);
        fdt.PropStr("status", "okay");
        fdt.PropStr("compatible", "riscv");

        fdt.PropStr("riscv,isa", isa_string.c_str());
        /* Linux 6.6 and later parse "riscv,isa-base" plus
           "riscv,isa-extensions" instead, and a kernel built without
           CONFIG_RISCV_ISA_FALLBACK (Ubuntu's generic riscv64 kernel, for
           one) discards any hart that carries only the deprecated
           "riscv,isa". With every hart discarded there is no boot CPU left
           and of_parse_and_init_cpus() hits a BUG() before the console is
           even up, so both forms are emitted. */
        fdt.PropStr("riscv,isa-base", max_xlen <= 32 ? "rv32i" : "rv64i");
        fdt.Prop("riscv,isa-extensions", ext_list.data(), ext_list.size());
        /* without the block sizes a guest leaves the extensions unused */
        if (isa.cbom_block_size != 0)
            fdt.PropU32("riscv,cbom-block-size", isa.cbom_block_size);
        if (isa.cboz_block_size != 0)
            fdt.PropU32("riscv,cboz-block-size", isa.cboz_block_size);

        fdt.PropStr("mmu-type", mmu_type);
        fdt.PropU32("clock-frequency", 2000000000);

        fdt.BeginNode("interrupt-controller");
        fdt.PropU32("#interrupt-cells", 1);
        fdt.PropEmpty("interrupt-controller");
        fdt.PropStr("compatible", "riscv,cpu-intc");
        intc_phandle[hart] = fdt.AllocPhandle();
        fdt.PropU32("phandle", intc_phandle[hart]);
        fdt.EndNode(); /* interrupt-controller */

        fdt.EndNode(); /* cpu */
    }

    fdt.EndNode(); /* cpus */

    fdt.BeginNodeNum("memory", RAM_BASE_ADDR);
    fdt.PropStr("device_type", "memory");
    fdt.PropU64Range("reg", RAM_BASE_ADDR, m->ram_size);
    fdt.EndNode(); /* memory */

    /* The binding lists fromhost first and tohost second; here tohost is
       the register at the base and fromhost the one after it. */
    fdt.BeginNodeNum("htif", HTIF_BASE_ADDR + 8);
    fdt.PropStr("compatible", "ucb,htif0");
    fdt.AddCellU64(HTIF_BASE_ADDR + 8);
    fdt.AddCellU64(8);
    fdt.AddCellU64(HTIF_BASE_ADDR);
    fdt.AddCellU64(8);
    fdt.PropCells("reg");
    fdt.EndNode(); /* htif */

    fdt.BeginNode("soc");
    fdt.PropU32("#address-cells", 2);
    fdt.PropU32("#size-cells", 2);
    fdt.PropStrList("compatible",
                    "ucbbar,riscvemu-bar-soc", "simple-bus", NULL);
    fdt.PropEmpty("ranges");

    fdt.BeginNodeNum("clint", CLINT_BASE_ADDR);
    fdt.PropStr("compatible", "riscv,clint0");

    for (int hart = 0; hart < m->hart_count; hart++) {
        fdt.AddCellU32(intc_phandle[hart]);
        fdt.AddCellU32(3); /* M IPI irq */
        fdt.AddCellU32(intc_phandle[hart]);
        fdt.AddCellU32(7); /* M timer irq */
    }
    fdt.PropCells("interrupts-extended");

    fdt.PropU64Range("reg", CLINT_BASE_ADDR, CLINT_SIZE);

    fdt.EndNode(); /* clint */

    if (m->intc_type == RISCV_INTC_PLIC) {
        ctx.irq_phandle = m->plic->BuildFDT(fdt, PLIC_BASE_ADDR, intc_phandle,
                                            m->native_sbi);
    } else {
        uint32_t imsic_m_phandle = 0, imsic_s_phandle = 0;

        if (m->intc_type == RISCV_INTC_APLIC_IMSIC) {
            imsic_m_phandle = riscv_build_imsic_fdt(m, fdt, IMSIC_M_BASE_ADDR,
                                                    intc_phandle, false);
            imsic_s_phandle = riscv_build_imsic_fdt(m, fdt, IMSIC_S_BASE_ADDR,
                                                    intc_phandle, true);
            ctx.msi_phandle = imsic_s_phandle;
        }
        ctx.irq_phandle = m->aplic->BuildFDT(fdt, APLIC_M_BASE_ADDR,
                                             APLIC_S_BASE_ADDR, intc_phandle,
                                             imsic_m_phandle, imsic_s_phandle,
                                             m->native_sbi);
        ctx.irq_cells = 2;
    }

    /* Every configured device describes itself from the resources it was
       actually given, so the tree cannot drift from the mapping. */
    m->bus->BuildFDTAll(ctx);

    fdt.EndNode(); /* soc */

    fdt.BeginNode("chosen");
    if (ctx.stdout_path[0] != '\0') {
        fdt.PropStr("stdout-path", ctx.stdout_path);
    }
    fdt.PropStr("bootargs", cmd_line ? cmd_line : "");
    if (kernel_size > 0) {
        fdt.PropU64("riscv,kernel-start", kernel_start);
        fdt.PropU64("riscv,kernel-end", kernel_start + kernel_size);
    }
    if (initrd_size > 0) {
        fdt.PropU64("linux,initrd-start", initrd_start);
        fdt.PropU64("linux,initrd-end", initrd_start + initrd_size);
    }

    fdt.EndNode(); /* chosen */

    fdt.EndNode(); /* / */

    size = fdt.Output(dst);
#if 1
    {
        FILE *f;
        f = fopen("/tmp/riscvemu.dtb", "wb");
        if (f != NULL) {
            fwrite(dst, 1, size, f);
            fclose(f);
        }
    }
#endif
    return size;
}

static void copy_bios(RISCVMachine *s, const uint8_t *buf, int buf_len,
                      const uint8_t *kernel_buf, int kernel_buf_len,
                      const uint8_t *initrd_buf, int initrd_buf_len,
                      const char *cmd_line)
{
    uint32_t fdt_addr, align, kernel_base, initrd_base, firmware_size;
    uint8_t *ram_ptr;
    uint32_t *q;

    if (buf_len > s->ram_size) {
        vm_error("BIOS too big\n");
        exit(1);
    }

    ram_ptr = get_ram_ptr(s, RAM_BASE_ADDR, true);
    memcpy(ram_ptr, buf, buf_len);

    /* The firmware occupies everything up to the kernel: its BSS and heap
       reach past the end of the image, so reserve the whole aligned block
       rather than just the bytes that were copied. */
    if (s->max_xlen == 32)
        align = 4 << 20; /* 4 MB page align */
    else
        align = 2 << 20; /* 2 MB page align */
    firmware_size = (buf_len + align - 1) & ~(align - 1);

    kernel_base = 0;
    if (kernel_buf_len > 0) {
        /* copy the kernel if present */
        kernel_base = firmware_size;
        memcpy(ram_ptr + kernel_base, kernel_buf, kernel_buf_len);
        if (kernel_buf_len + kernel_base > s->ram_size) {
            vm_error("kernel too big");
            exit(1);
        }
    }

    initrd_base = 0;
    if (initrd_buf_len > 0) {
        /* same allocation as QEMU */
        initrd_base = s->ram_size / 2;
        if (initrd_base > (128 << 20))
            initrd_base = 128 << 20;
        memcpy(ram_ptr + initrd_base, initrd_buf, initrd_buf_len);
        if (initrd_buf_len + initrd_base > s->ram_size) {
            vm_error("initrd too big");
            exit(1);
        }
    }

    ram_ptr = get_ram_ptr(s, 0, true);

    fdt_addr = 0x1000 + 8 * 8;

    riscv_build_fdt(s, ram_ptr + fdt_addr, RAM_BASE_ADDR, firmware_size,
                    RAM_BASE_ADDR + kernel_base, kernel_buf_len,
                    RAM_BASE_ADDR + initrd_base, initrd_buf_len,
                    cmd_line);

    /* jump_addr = 0x80000000 */

    q = (uint32_t *)(ram_ptr + 0x1000);
    q[0] = 0x297 + 0x80000000 - 0x1000; /* auipc t0, jump_addr */
    q[1] = 0x597; /* auipc a1, dtb */
    q[2] = 0x58593 + ((fdt_addr - 4) << 20); /* addi a1, a1, dtb */
    q[3] = 0xf1402573; /* csrr a0, mhartid */
    q[4] = 0x00028067; /* jalr zero, t0, jump_addr */
}

/* The tree's room at the top of RAM when no firmware places it. */
#define NATIVE_FDT_SIZE 0x10000

/* Without M-mode firmware the kernel takes the start of RAM and hart 0 enters
   it in S-mode, with a1 pointing to the device tree as firmware would leave
   it. Without a kernel hart 0 enters 'flash_base' instead, where S-mode
   firmware runs in place. */
static bool load_kernel_native(RISCVMachine *s, uint64_t flash_base,
                               const uint8_t *kernel_buf, int kernel_buf_len,
                               const uint8_t *initrd_buf, int initrd_buf_len,
                               const char *cmd_line)
{
    uint64_t fdt_base, initrd_base = 0;
    uint64_t entry = kernel_buf_len > 0 ? RAM_BASE_ADDR : flash_base;
    uint8_t *ram_ptr;
    int fdt_size;

    if (entry == 0) {
        vm_error("the native sbi needs a kernel or a cfi-flash to run\n");
        return false;
    }

    if (s->ram_size < 2 * NATIVE_FDT_SIZE) {
        vm_error("not enough RAM\n");
        return false;
    }
    fdt_base = s->ram_size - NATIVE_FDT_SIZE;
    if ((uint64_t)kernel_buf_len > fdt_base) {
        vm_error("kernel too big\n");
        return false;
    }
    ram_ptr = get_ram_ptr(s, RAM_BASE_ADDR, true);
    memcpy(ram_ptr, kernel_buf, kernel_buf_len);

    if (initrd_buf_len > 0) {
        /* same allocation as QEMU */
        initrd_base = s->ram_size / 2;
        if (initrd_base > (128 << 20))
            initrd_base = 128 << 20;
        if (initrd_base < (uint64_t)kernel_buf_len ||
            (uint64_t)initrd_buf_len > fdt_base - initrd_base) {
            vm_error("initrd too big\n");
            return false;
        }
        memcpy(ram_ptr + initrd_base, initrd_buf, initrd_buf_len);
    }

    /* built aside, since nothing bounds its size beforehand */
    std::vector<uint8_t> fdt(1 << 20);
    fdt_size = riscv_build_fdt(s, fdt.data(),
                               RAM_BASE_ADDR + fdt_base, NATIVE_FDT_SIZE,
                               RAM_BASE_ADDR, kernel_buf_len,
                               RAM_BASE_ADDR + initrd_base, initrd_buf_len,
                               cmd_line);
    if (fdt_size > NATIVE_FDT_SIZE) {
        vm_error("device tree too big\n");
        return false;
    }
    memcpy(ram_ptr + fdt_base, fdt.data(), fdt_size);

    s->fCpus->Vcpu(0).Start(entry, RAM_BASE_ADDR + fdt_base);
    return true;
}

uint64_t RISCVMachine::ClintTime()
{
    return fCpus->Time();
}

void RISCVMachine::SetClintIrq(int hart, bool timer, bool level)
{
    fCpus->Vcpu(hart).SetIrqLine(timer ? MIP_MTIP : MIP_MSIP, level);
    WakeHart(hart);
}

void RISCVMachine::ClintTimerChanged(uint64_t deadline)
{
    KickTimersBefore(deadline);
}

/* Writes from guest physical memory, a page at a time since RAM may end at
   any page. */
int64_t RISCVMachine::ConsoleWrite(uint64_t len, uint64_t addr,
                                   uint64_t *value)
{
    uint64_t done = 0;

    while (done < len) {
        uint64_t n = DEVRAM_PAGE_SIZE - (addr + done) % DEVRAM_PAGE_SIZE;
        if (n > len - done)
            n = len - done;
        uint8_t *ptr = mem_map->GetRamPtr(addr + done, false);
        if (ptr == nullptr)
            break;
        if (fConsole != nullptr)
            fConsole->WriteData(ptr, n);
        done += n;
    }
    if (done == 0 && len != 0)
        return SBI_ERR_INVALID_PARAM;
    *value = done;
    return SBI_SUCCESS;
}

void RISCVMachine::ConsolePutchar(uint8_t c)
{
    if (fConsole != nullptr)
        fConsole->WriteData(&c, 1);
}

bool RISCVMachine::SbiCall(int hart, int xlen, uint64_t a[8])
{
    uint32_t fid = a[6];
    uint64_t value = 0;
    int64_t err;

    (void)hart;
    switch (a[7]) {
    case SBI_LEGACY_PUTCHAR:
        ConsolePutchar(a[0]);
        a[0] = SBI_SUCCESS;
        return true;
    case SBI_LEGACY_GETCHAR:
        /* console input goes to the guest's console devices */
        a[0] = SBI_ERR_FAILED;
        return true;
    case SBI_EXT_DBCN:
        break;
    default:
        return false;
    }

    switch (fid) {
    case 0: /* write */
        if (xlen != 32 && a[2] != 0)
            err = SBI_ERR_INVALID_PARAM;
        else
            err = ConsoleWrite(a[0], xlen == 32 ? a[1] | a[2] << 32 : a[1],
                               &value);
        break;
    case 1: /* read */
        /* console input goes to the guest's console devices */
        err = SBI_SUCCESS;
        break;
    case 2: /* write_byte */
        ConsolePutchar(a[0]);
        err = SBI_SUCCESS;
        break;
    default:
        err = SBI_ERR_NOT_SUPPORTED;
        break;
    }
    a[0] = err;
    a[1] = value;
    return true;
}

void RISCVMachine::SystemReset(bool reboot, bool failure)
{
    if (reboot) {
        RequestReset();
    } else if (!ShutdownRequested()) {
        if (failure) {
            printf("\nPower off, system failure.\n");
            RequestShutdown(1);
        } else {
            printf("\nPower off.\n");
            RequestShutdown(0);
        }
    }
}

void RISCVMachine::HartFault(int hart, uint64_t cause, uint64_t pc,
                             uint64_t tval)
{
    vm_error("hart %d: trap with no M-mode to take it: cause=0x%" PRIx64
             " pc=0x%" PRIx64 " tval=0x%" PRIx64 "\n", hart, cause, pc, tval);
    RequestShutdown(255);
}

/* Reserve the parts of the map the architecture fixes, so that anything the
   configuration places is checked against them. */
static bool riscv_claim_fixed_ranges(RISCVMachine *s)
{
    RangeAllocator &mmio = s->bus->MmioAlloc();

    if (!mmio.Claim(0, LOW_RAM_SIZE, "low ram") ||
        !mmio.Claim(CLINT_BASE_ADDR, CLINT_SIZE, "clint") ||
        !mmio.Claim(HTIF_BASE_ADDR, HTIF_SIZE, "htif") ||
        !mmio.Claim(RAM_BASE_ADDR, s->ram_size, "ram")) {
        return false;
    }
    if (s->intc_type == RISCV_INTC_PLIC) {
        return mmio.Claim(PLIC_BASE_ADDR, PLIC_SIZE, "plic");
    }
    if (!mmio.Claim(APLIC_M_BASE_ADDR, APLIC_SIZE, "aplic-m") ||
        !mmio.Claim(APLIC_S_BASE_ADDR, APLIC_SIZE, "aplic-s")) {
        return false;
    }
    if (s->intc_type == RISCV_INTC_APLIC_IMSIC) {
        return mmio.Claim(IMSIC_M_BASE_ADDR, imsic_region_size(s), "imsic-m") &&
            mmio.Claim(IMSIC_S_BASE_ADDR, imsic_region_size(s), "imsic-s");
    }
    return true;
}

static std::unique_ptr<VirtMachine>
riscv_machine_init(const VirtMachineParams *p)
{
    std::unique_ptr<RISCVMachine> s;
    int max_xlen, ram_flags;
    RISCVIntcType intc_type;
    bool native_sbi;
    DeviceContext ctx;

    if (!strcmp(p->machine_name, "riscv32")) {
        max_xlen = 32;
    } else if (!strcmp(p->machine_name, "riscv64")) {
        max_xlen = 64;
    } else if (!strcmp(p->machine_name, "riscv128")) {
        max_xlen = 128;
    } else {
        vm_error("unsupported machine: %s\n", p->machine_name);
        return nullptr;
    }

    if (p->cpu_count < 1 || p->cpu_count > RISCV_MAX_HARTS) {
        vm_error("%s: cpus must be between 1 and %d\n", p->machine_name,
                 RISCV_MAX_HARTS);
        return nullptr;
    }
    if (p->interrupt_controller == nullptr ||
        strcmp(p->interrupt_controller, "plic") == 0) {
        intc_type = RISCV_INTC_PLIC;
    } else if (strcmp(p->interrupt_controller, "aplic") == 0) {
        intc_type = RISCV_INTC_APLIC;
    } else if (strcmp(p->interrupt_controller, "aplic-imsic") == 0) {
        intc_type = RISCV_INTC_APLIC_IMSIC;
    } else {
        vm_error("%s: interrupt_controller must be \"plic\", \"aplic\" or "
                 "\"aplic-imsic\", not \"%s\"\n", p->machine_name,
                 p->interrupt_controller);
        return nullptr;
    }
    if (p->sbi == nullptr || strcmp(p->sbi, "firmware") == 0) {
        native_sbi = false;
    } else if (strcmp(p->sbi, "native") == 0) {
        native_sbi = true;
    } else {
        vm_error("%s: sbi must be \"firmware\" or \"native\", not \"%s\"\n",
                 p->machine_name, p->sbi);
        return nullptr;
    }
    if (native_sbi && p->files[VM_FILE_BIOS].buf) {
        vm_error("%s: no bios runs with the native sbi\n", p->machine_name);
        return nullptr;
    }

    RISCVCpuOptions options;
    options.xlen = max_xlen;
    options.hart_count = p->cpu_count;
    options.interrupt_arch = intc_type == RISCV_INTC_PLIC ? RISCV_INTR_BASE :
        intc_type == RISCV_INTC_APLIC ? RISCV_INTR_AIA : RISCV_INTR_AIA_IMSIC;
    options.machine_mode = !native_sbi;
    options.rtc_real_time = p->rtc_real_time;

    s = std::make_unique<RISCVMachine>();
    s->SetHost(p);
    s->intc_type = intc_type;
    s->vmc = p->vmc;
    s->ram_size = p->ram_size;
    s->max_xlen = max_xlen;
    s->native_sbi = native_sbi;
    if (p->compatible)
        s->compatible = p->compatible;
    s->fConsole = p->platform->Console();

    if (p->accel_enable &&
        !host_riscv_hypervisor_open(*s, *p->device_lock, options, &s->fCpus))
        return nullptr;
    if (!s->fCpus)
        s->fCpus = riscv_interp_open(*s, *p->device_lock, options);
    if (!s->fCpus) {
        vm_error("unsupported max_xlen=%d\n", max_xlen);
        return nullptr;
    }
    s->mem_map = &s->fCpus->MemoryMap();
    s->hart_count = p->cpu_count;
    s->fHartWakeups = std::make_unique<HostWakeup[]>(s->hart_count);
    /* RAM */
    ram_flags = 0;
    s->mem_map->RegisterRam(RAM_BASE_ADDR, p->ram_size, ram_flags);
    s->mem_map->RegisterRam(0x00000000, LOW_RAM_SIZE, 0);

    s->clint = std::make_unique<Clint>(*s, s->hart_count);
    s->mem_map->RegisterDevice(CLINT_BASE_ADDR, CLINT_SIZE, s->clint.get(),
                               DEVIO_SIZE32);
    s->htif = std::make_unique<HTIF>(*s, p->platform->Console());
    s->mem_map->RegisterDevice(HTIF_BASE_ADDR, HTIF_REG_SIZE, s->htif.get(),
                               DEVIO_SIZE32);

    IRQTarget *irq_target;
    if (intc_type == RISCV_INTC_PLIC) {
        s->plic = std::make_unique<PLIC>(s.get(), s->hart_count);
        s->mem_map->RegisterDevice(PLIC_BASE_ADDR, PLIC_SIZE, s->plic.get(),
                                   DEVIO_SIZE32);
        irq_target = s->plic.get();
    } else {
        AplicMsiLayout msi;

        if (intc_type == RISCV_INTC_APLIC_IMSIC) {
            while ((1 << s->imsic_hart_bits) < s->hart_count)
                s->imsic_hart_bits++;
            s->mem_map->RegisterDevice(IMSIC_M_BASE_ADDR,
                                       imsic_region_size(s.get()),
                                       &s->fImsicIoM, DEVIO_SIZE32);
            s->mem_map->RegisterDevice(IMSIC_S_BASE_ADDR,
                                       imsic_region_size(s.get()),
                                       &s->fImsicIoS, DEVIO_SIZE32);
            msi.m_base = IMSIC_M_BASE_ADDR;
            msi.s_base = IMSIC_S_BASE_ADDR;
            msi.hart_index_bits = s->imsic_hart_bits;
        }
        s->aplic = std::make_unique<APLIC>(s->mem_map, s.get(),
                                           s->hart_count, RISCV_IRQ_LINES - 1,
                                           intc_type == RISCV_INTC_APLIC_IMSIC ?
                                           &msi : nullptr);
        s->mem_map->RegisterDevice(APLIC_M_BASE_ADDR, APLIC_SIZE,
                                   s->aplic->DomainIO(false), DEVIO_SIZE32);
        s->mem_map->RegisterDevice(APLIC_S_BASE_ADDR, APLIC_SIZE,
                                   s->aplic->DomainIO(true), DEVIO_SIZE32);
        irq_target = s->aplic.get();
    }

    s->bus = std::make_unique<SystemBus>(s->mem_map, irq_target,
                                         RISCV_IRQ_LINES);
    if (intc_type == RISCV_INTC_APLIC_IMSIC) {
        s->bus->SetMsiTarget(s.get());
    }
    s->bus->MmioAlloc().SetWindow(DEVICE_WINDOW_BASE, DEVICE_WINDOW_SIZE);
    s->bus->MmioAlloc().SetHighWindow(HIGH_DEVICE_WINDOW_BASE,
                                      HIGH_DEVICE_WINDOW_SIZE);
    s->bus->IoAlloc().SetWindow(PCI_IO_WINDOW_BASE, PCI_IO_WINDOW_SIZE);
    if (!riscv_claim_fixed_ranges(s.get())) {
        return nullptr;
    }

    ctx.params = p;
    ctx.platform = p->platform;
    ctx.machine = s.get();

    /* The nesting in the file is the nesting of the buses, so the root one
       has to be the kind this machine provides. */
    std::string owner = std::string("the ") + p->machine_name + " machine";
    SystemBus *root = s->bus.get();
    if (!bus_config_attach(p->root_bus, owner.c_str(),
                           [root](const char *type) -> Bus * {
                               return strcmp(type, "fdt") == 0 ? root
                                                               : nullptr;
                           }, &ctx)) {
        return nullptr;
    }
    if (!s->bus->AllocateAll()) {
        return nullptr;
    }
    if (!s->bus->RealizeAll()) {
        return nullptr;
    }

    device_context_connect(&ctx);

    if (native_sbi) {
        /* what firmware would hand the supervisor domain */
        if (s->aplic)
            s->aplic->DelegateAll();
        if (!load_kernel_native(s.get(), ctx.boot_flash_base,
                                p->files[VM_FILE_KERNEL].buf,
                                p->files[VM_FILE_KERNEL].len,
                                p->files[VM_FILE_INITRD].buf,
                                p->files[VM_FILE_INITRD].len, p->cmdline)) {
            return nullptr;
        }
        return s;
    }

    if (!p->files[VM_FILE_BIOS].buf) {
        vm_error("No bios found");
    }

    copy_bios(s.get(), p->files[VM_FILE_BIOS].buf, p->files[VM_FILE_BIOS].len,
              p->files[VM_FILE_KERNEL].buf, p->files[VM_FILE_KERNEL].len,
              p->files[VM_FILE_INITRD].buf, p->files[VM_FILE_INITRD].len,
              p->cmdline);

    return s;
}

RISCVMachine::~RISCVMachine() = default;

/* The delay is in host microseconds, the deadlines on the time counter. */
int64_t RISCVMachine::RunTimers()
{
    uint64_t now = fCpus->Time();
    int64_t delay = -1;

    for (int hart = 0; hart < hart_count; hart++) {
        uint64_t next = clint->RunTimer(hart, now);

        /* The supervisor timer runs off the same counter when Sstc is
           enabled. The hart raises it itself once woken, which takes the
           deadline away. */
        RISCVVcpu &vcpu = fCpus->Vcpu(hart);
        uint64_t stimer = vcpu.TimerDeadline();
        if (stimer > now) {
            if (stimer < next)
                next = stimer;
        } else if (stimer != fSTimerKicked[hart]) {
            fSTimerKicked[hart] = stimer;
            vcpu.InterruptRun();
            WakeHart(hart);
        }
        if (next != UINT64_MAX) {
            uint64_t d = next > now ? TicksToUs(next - now) : 0;
            if (d > INT64_MAX)
                d = INT64_MAX;
            if (delay < 0 || (int64_t)d < delay)
                delay = d;
        }
    }
    return delay;
}

void RISCVMachine::StartProcessors()
{
    for (int hart = 0; hart < hart_count; hart++)
        fHartThreadList.emplace_back([this, hart]() {HartThread(hart);});
}

void RISCVMachine::JoinProcessors()
{
    for (std::thread &thread : fHartThreadList)
        thread.join();
    fHartThreadList.clear();
}

void RISCVMachine::InterruptExecution()
{
    for (int hart = 0; hart < hart_count; hart++) {
        fCpus->Vcpu(hart).InterruptRun();
        fHartWakeups[hart].Kick();
    }
}

void RISCVMachine::HartThread(int hart)
{
    RISCVVcpu &vcpu = fCpus->Vcpu(hart);
    char name[16];

    snprintf(name, sizeof(name), "cpu%d", hart);
    host_set_thread_name(name);
    vcpu.ThreadStarted();
    while (!StopRequested()) {
        if (!vcpu.Idle()) {
            vcpu.Run();
            continue;
        }
        /* whatever can wake the hart kicks it */
        fHartWakeups[hart].Wait(-1);
    }
}



//#pragma mark - RiscvMachineClass

class RiscvMachineClass final: public VirtMachineClass {
public:
    const char *MachineNames() const override
    {
        return "riscv32,riscv64,riscv128";
    }

    void SetDefaults(VirtMachineParams *p) const override
    {
        p->accel_enable = true;
    }

    std::unique_ptr<VirtMachine> Init(const VirtMachineParams *p) const override
    {
        return riscv_machine_init(p);
    }
};

static const RiscvMachineClass sRiscvMachineClass;
const VirtMachineClass &gRiscvMachineClass = sRiscvMachineClass;
