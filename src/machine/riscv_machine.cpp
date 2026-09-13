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
#include <fcntl.h>
#include <errno.h>
#include <unistd.h>
#include <time.h>
#ifdef __HAIKU__
#include <OS.h>
#endif

#include "cutils.h"
#include "iomem.h"
#include "riscv_cpu.h"
#include "uart.h"
#include "virtio.h"
#include "machine.h"
#include "device.h"
#include "devices.h"
#include "fdt.h"
#include "pci.h"
#include "plic.h"
#include "aplic.h"

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
    public TlbFlushTarget,
    public RtcTimeSource,
    public SerialOutput {
public:
    PhysMemoryMap *mem_map = nullptr;
    SystemBus *bus = nullptr;
    int max_xlen = 0;
    int hart_count = 0;
    RISCVCPU *cpus[RISCV_MAX_HARTS] {};
    RISCVIntcType intc_type = RISCV_INTC_PLIC;
    uint64_t ram_size = 0;
    /* RTC */
    bool rtc_real_time = false;
    uint64_t rtc_start_time = 0;
    uint64_t timecmp[RISCV_MAX_HARTS] {};
    /* the one selected by intc_type */
    PLIC *plic = nullptr;
    APLIC *aplic = nullptr;
    /* bits of the hart number in an IMSIC page address */
    int imsic_hart_bits = 0;
    /* HTIF */
    uint64_t htif_tohost = 0, htif_fromhost = 0;

    /* Whichever devices the configuration gave the keyboard and the pointer
       roles to; null when it declared none. */
    InputEventTarget *keyboard = nullptr;
    InputEventTarget *mouse = nullptr;

    ~RISCVMachine() override;

    /* memory-mapped register blocks */
    uint32_t HtifRead(uint32_t offset, int size_log2);
    void HtifWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t ClintRead(uint32_t offset, int size_log2);
    void ClintWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t ImsicRead(uint32_t offset, int size_log2);
    void ImsicWriteM(uint32_t offset, uint32_t val, int size_log2);
    void ImsicWriteS(uint32_t offset, uint32_t val, int size_log2);

    DeviceIOAdapter<RISCVMachine, &RISCVMachine::HtifRead,
                    &RISCVMachine::HtifWrite> fHtifIo {*this};
    DeviceIOAdapter<RISCVMachine, &RISCVMachine::ClintRead,
                    &RISCVMachine::ClintWrite> fClintIo {*this};
    DeviceIOAdapter<RISCVMachine, &RISCVMachine::ImsicRead,
                    &RISCVMachine::ImsicWriteM> fImsicIoM {*this};
    DeviceIOAdapter<RISCVMachine, &RISCVMachine::ImsicRead,
                    &RISCVMachine::ImsicWriteS> fImsicIoS {*this};

    /* HartIrqTarget */
    void SetExternalIrq(int hart, bool supervisor, int level) override;

    /* PCIMsiTarget */
    void SendMsi(uint64_t addr, uint32_t data) override;

    /* TlbFlushTarget */
    void FlushTlbWriteRange(uint8_t *ram_addr, size_t ram_size) override;

    /* RtcTimeSource */
    uint64_t RtcTime() override;

    /* SerialOutput */
    void WriteData(const uint8_t *buf, int buf_len) override;

    /* VirtMachine */
    int GetSleepDuration(int delay) override;
    void Interp(int max_exec_cycle) override;
    bool MouseIsAbsolute() override;
    void SendMouseEvent(int dx, int dy, int dz, unsigned int buttons) override;
    void SendKeyEvent(bool is_down, uint16_t key_code) override;
};

#define LOW_RAM_SIZE   0x00010000 /* 64KB */
#define RAM_BASE_ADDR  0x80000000
#define CLINT_BASE_ADDR 0x02000000
/* The size of a SiFive CLINT. It must be exactly this: firmware splits the
   region into its ACLINT MSWI and MTIMER halves by size, so declaring a
   larger one moves mtimecmp somewhere this CLINT does not decode and the
   timer silently never fires. */
#define CLINT_SIZE      0x00010000
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

#define RTC_FREQ 1000000
#define RTC_FREQ_DIV 16 /* arbitrary, relative to CPU freq to have a
                           10 MHz frequency */

static uint64_t rtc_get_real_time(RISCVMachine *s)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * RTC_FREQ +
        (ts.tv_nsec / (1000000000 / RTC_FREQ));
}

static uint64_t rtc_get_time(RISCVMachine *m)
{
    uint64_t val;
    if (m->rtc_real_time) {
        val = rtc_get_real_time(m) - m->rtc_start_time;
    } else {
        /* the sum keeps advancing while some harts wait for interrupts */
        val = 0;
        for (int i = 0; i < m->hart_count; i++)
            val += m->cpus[i]->Cycles();
        val /= RTC_FREQ_DIV;
    }
    //    printf("rtc_time=%" PRId64 "\n", val);
    return val;
}

uint32_t RISCVMachine::HtifRead(uint32_t offset, int size_log2)
{
    RISCVMachine *s = this;
    uint32_t val;

    assert(size_log2 == 2);
    switch(offset) {
    case 0:
        val = s->htif_tohost;
        break;
    case 4:
        val = s->htif_tohost >> 32;
        break;
    case 8:
        val = s->htif_fromhost;
        break;
    case 12:
        val = s->htif_fromhost >> 32;
        break;
    default:
        val = 0;
        break;
    }
    return val;
}

static void htif_handle_cmd(RISCVMachine *s)
{
    uint32_t device, cmd;

    /* the guest keeps running until the current step ends and may repeat
       the power off request meanwhile */
    if (s->shutdown_requested)
        return;

    device = s->htif_tohost >> 56;
    cmd = (s->htif_tohost >> 48) & 0xff;
    if (device == 0 && cmd == 0 && (s->htif_tohost & 1)) {
        /* Power off, using the spike/riscv-tests convention: the guest writes
           (code << 1) | 1, so a plain 1 is a success exit. The exit status is
           what makes a guest usable as an automated test: it reports pass/fail
           without the harness having to grep the console log. */
        uint64_t code = (s->htif_tohost & 0xffffffffffff) >> 1;
        if (code == 0) {
            printf("\nPower off.\n");
        } else {
            printf("\nPower off, exit code %" PRIu64 ".\n", code);
        }
        /* Only the low 8 bits survive wait(2), and 0 there would turn a failure
           into a pass, so codes that do not fit are reported as 255. */
        s->RequestShutdown(code < 256 ? (int)code : 255);
    } else if (device == 1 && cmd == 1) {
        uint8_t buf[1];
        buf[0] = s->htif_tohost & 0xff;
        s->console->WriteData(buf, 1);
        s->htif_tohost = 0;
        s->htif_fromhost = ((uint64_t)device << 56) | ((uint64_t)cmd << 48);
    } else if (device == 1 && cmd == 0) {
        /* request keyboard interrupt */
        s->htif_tohost = 0;
#ifdef __HAIKU__
    } else if (device == 2 && cmd == 0) {
    	// get calendar time
    	s->htif_fromhost = real_time_clock_usecs();
#endif
    } else {
        printf("HTIF: unsupported tohost=0x%016" PRIx64 "\n", s->htif_tohost);
    }
}

void RISCVMachine::HtifWrite(uint32_t offset, uint32_t val, int size_log2)
{
    RISCVMachine *s = this;

    assert(size_log2 == 2);
    switch(offset) {
    case 0:
        s->htif_tohost = (s->htif_tohost & ~0xffffffff) | val;
        break;
    case 4:
        s->htif_tohost = (s->htif_tohost & 0xffffffff) | ((uint64_t)val << 32);
        htif_handle_cmd(s);
        break;
    case 8:
        s->htif_fromhost = (s->htif_fromhost & ~0xffffffff) | val;
        break;
    case 12:
        s->htif_fromhost = (s->htif_fromhost & 0xffffffff) |
            (uint64_t)val << 32;
        break;
    default:
        break;
    }
}

void RISCVMachine::WriteData(const uint8_t *buf, int buf_len)
{
    if (console != nullptr) {
        console->WriteData(buf, buf_len);
    }
}

/* CLINT register layout: one msip word and one 64 bit mtimecmp per hart */
#define CLINT_MSIP_BASE     0x0000
#define CLINT_MTIMECMP_BASE 0x4000
#define CLINT_MTIME         0xbff8

uint32_t RISCVMachine::ClintRead(uint32_t offset, int size_log2)
{
    RISCVMachine *m = this;
    uint32_t val = 0;
    uint32_t hart;

    assert(size_log2 == 2);
    if (offset < CLINT_MTIMECMP_BASE) {
        hart = (offset - CLINT_MSIP_BASE) / 4;
        if (hart < (uint32_t)m->hart_count)
            val = (m->cpus[hart]->Mip() & MIP_MSIP) != 0;
    } else if (offset < CLINT_MTIME) {
        hart = (offset - CLINT_MTIMECMP_BASE) / 8;
        if (hart < (uint32_t)m->hart_count) {
            if ((offset - CLINT_MTIMECMP_BASE) % 8 == 0)
                val = m->timecmp[hart];
            else
                val = m->timecmp[hart] >> 32;
        }
    } else if (offset == CLINT_MTIME) {
        val = rtc_get_time(m);
    } else if (offset == CLINT_MTIME + 4) {
        val = rtc_get_time(m) >> 32;
    }
    return val;
}

void RISCVMachine::ClintWrite(uint32_t offset, uint32_t val, int size_log2)
{
    RISCVMachine *m = this;
    uint32_t hart;

    assert(size_log2 == 2);
    if (offset < CLINT_MTIMECMP_BASE) {
        /* a software interrupt to that hart */
        hart = (offset - CLINT_MSIP_BASE) / 4;
        if (hart >= (uint32_t)m->hart_count)
            return;
        if (val & 1) {
            m->cpus[hart]->SetMip(MIP_MSIP);
        } else {
            m->cpus[hart]->ResetMip(MIP_MSIP);
        }
    } else if (offset < CLINT_MTIME) {
        hart = (offset - CLINT_MTIMECMP_BASE) / 8;
        if (hart >= (uint32_t)m->hart_count)
            return;
        uint64_t &cmp = m->timecmp[hart];
        if ((offset - CLINT_MTIMECMP_BASE) % 8 == 0)
            cmp = (cmp & ~0xffffffffull) | val;
        else
            cmp = (cmp & 0xffffffff) | ((uint64_t)val << 32);
        m->cpus[hart]->ResetMip(MIP_MTIP);
    }
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
        hart < (uint32_t)hart_count)
        cpus[hart]->ImsicSetPending(false, val);
}

void RISCVMachine::ImsicWriteS(uint32_t offset, uint32_t val, int size_log2)
{
    uint32_t hart = offset / IMSIC_PAGE_SIZE;

    if (offset % IMSIC_PAGE_SIZE == IMSIC_SETEIPNUM_LE &&
        hart < (uint32_t)hart_count)
        cpus[hart]->ImsicSetPending(true, val);
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
    uint32_t mask = supervisor ? MIP_SEIP : MIP_MEIP;
    if (level) {
        cpus[hart]->SetMip(mask);
    } else {
        cpus[hart]->ResetMip(mask);
    }
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
    uint32_t tab[2 * RISCV_MAX_HARTS];
    uint32_t phandle = fdt.AllocPhandle();

    fdt.BeginNodeNum("interrupt-controller", base);
    fdt.PropStr("compatible", "riscv,imsics");
    fdt.PropU64Range("reg", base, imsic_region_size(m));
    for (int hart = 0; hart < m->hart_count; hart++) {
        tab[2 * hart] = intc_phandle[hart];
        tab[2 * hart + 1] = supervisor ? 9 : 11; /* S or M ext irq */
    }
    fdt.PropTabU32("interrupts-extended", tab, 2 * m->hart_count);
    fdt.PropEmpty("interrupt-controller");
    fdt.PropU32("#interrupt-cells", 0);
    fdt.PropEmpty("msi-controller");
    fdt.PropU32("#msi-cells", 0);
    fdt.PropU32("riscv,num-ids", RISCV_IMSIC_NUM_IDS);
    fdt.PropU32("phandle", phandle);
    fdt.EndNode();
    return phandle;
}

static int riscv_build_fdt(RISCVMachine *m, uint8_t *dst,
                           uint64_t firmware_size,
                           uint64_t kernel_start, uint64_t kernel_size,
                           uint64_t initrd_start, uint64_t initrd_size,
                           const char *cmd_line)
{
    FDTBuilder fdt;
    FDTContext ctx;
    int size, max_xlen, i;
    char isa_string[128], *q;
    char ext_list[256], *ext_end;
    uint32_t misa;
    uint32_t intc_phandle[RISCV_MAX_HARTS];
    uint32_t tab[4 * RISCV_MAX_HARTS];

    ctx.fdt = &fdt;

    /* Keep the guest out of the firmware image. There is no PMP here, so
       nothing else stops a kernel from allocating over the M mode trap
       handler it depends on. */
    fdt.AddReservation(RAM_BASE_ADDR, firmware_size);

    fdt.BeginNode("");
    fdt.PropU32("#address-cells", 2);
    fdt.PropU32("#size-cells", 2);
    fdt.PropStr("compatible", "ucbbar,riscvemu-bar_dev");
    fdt.PropStr("model", "ucbbar,riscvemu-bare");

    max_xlen = m->max_xlen;
    misa = m->cpus[0]->Misa();

    /* Extensions implemented outside of misa, which only has room for the
       single letter ones. */
    const char *multi_letter_ext[16];
    int ext_count = 0;
    multi_letter_ext[ext_count++] = "zicsr";
    multi_letter_ext[ext_count++] = "zifencei";
    multi_letter_ext[ext_count++] = "zicntr";
    if (m->intc_type != RISCV_INTC_PLIC) {
        multi_letter_ext[ext_count++] = "smaia";
        multi_letter_ext[ext_count++] = "ssaia";
    }
    multi_letter_ext[ext_count++] = "sstc";
    multi_letter_ext[ext_count++] = "svadu";
    multi_letter_ext[ext_count++] = "svinval";

    q = isa_string;
    q += snprintf(isa_string, sizeof(isa_string), "rv%d", max_xlen);
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
                *q++ = *p;
                emitted |= bit;
            }
        }
        for(i = 0; i < 26; i++) {
            if ((misa & (1 << i)) && !(emitted & (1 << i)))
                *q++ = 'a' + i;
        }
    }
    /* Multi-letter extensions follow the single letter ones, each introduced
       by an underscore. */
    for (i = 0; i < ext_count; i++) {
        q += snprintf(q, sizeof(isa_string) - (q - isa_string), "_%s",
                      multi_letter_ext[i]);
    }

    {
        /* A packed list of NUL terminated strings. The privilege modes 'S'
           and 'U' are not extensions and have no place here, unlike in the
           "riscv,isa" string above. */
        static const char canonical[] = "imafdqch";
        char *p = ext_list;
        for (const char *c = canonical; *c != '\0'; c++) {
            if (misa & (1 << (*c - 'a'))) {
                *p++ = *c;
                *p++ = '\0';
            }
        }
        for (i = 0; i < ext_count; i++) {
            size_t len = strlen(multi_letter_ext[i]) + 1;
            memcpy(p, multi_letter_ext[i], len);
            p += len;
        }
        ext_end = p;
    }

    /* CPU list */
    fdt.BeginNode("cpus");
    fdt.PropU32("#address-cells", 1);
    fdt.PropU32("#size-cells", 0);
    fdt.PropU32("timebase-frequency", RTC_FREQ);

    for (int hart = 0; hart < m->hart_count; hart++) {
        fdt.BeginNodeNum("cpu", hart);
        fdt.PropStr("device_type", "cpu");
        fdt.PropU32("reg", hart);
        fdt.PropStr("status", "okay");
        fdt.PropStr("compatible", "riscv");

        fdt.PropStr("riscv,isa", isa_string);
        /* Linux 6.6 and later parse "riscv,isa-base" plus
           "riscv,isa-extensions" instead, and a kernel built without
           CONFIG_RISCV_ISA_FALLBACK (Ubuntu's generic riscv64 kernel, for
           one) discards any hart that carries only the deprecated
           "riscv,isa". With every hart discarded there is no boot CPU left
           and of_parse_and_init_cpus() hits a BUG() before the console is
           even up, so both forms are emitted. */
        fdt.PropStr("riscv,isa-base", max_xlen <= 32 ? "rv32i" : "rv64i");
        fdt.Prop("riscv,isa-extensions", ext_list, ext_end - ext_list);

        fdt.PropStr("mmu-type", max_xlen <= 32 ? "riscv,sv32" : "riscv,sv48");
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

    fdt.BeginNode("htif");
    fdt.PropStr("compatible", "ucb,htif0");
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
        tab[4 * hart] = intc_phandle[hart];
        tab[4 * hart + 1] = 3; /* M IPI irq */
        tab[4 * hart + 2] = intc_phandle[hart];
        tab[4 * hart + 3] = 7; /* M timer irq */
    }
    fdt.PropTabU32("interrupts-extended", tab, 4 * m->hart_count);

    fdt.PropU64Range("reg", CLINT_BASE_ADDR, CLINT_SIZE);

    fdt.EndNode(); /* clint */

    if (m->intc_type == RISCV_INTC_PLIC) {
        ctx.irq_phandle = m->plic->BuildFDT(fdt, PLIC_BASE_ADDR, intc_phandle);
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
                                             imsic_m_phandle, imsic_s_phandle);
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

    riscv_build_fdt(s, ram_ptr + fdt_addr, firmware_size,
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

uint64_t RISCVMachine::RtcTime()
{
    return rtc_get_time(this);
}

void RISCVMachine::FlushTlbWriteRange(uint8_t *ram_addr, size_t ram_size)
{
    for (int hart = 0; hart < hart_count; hart++)
        cpus[hart]->FlushTlbWriteRangeRam(ram_addr, ram_size);
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

static VirtMachine *riscv_machine_init(const VirtMachineParams *p)
{
    RISCVMachine *s;
    int max_xlen, ram_flags;
    RISCVIntcType intc_type;
    DeviceContext ctx;

    if (!strcmp(p->machine_name, "riscv32")) {
        max_xlen = 32;
    } else if (!strcmp(p->machine_name, "riscv64")) {
        max_xlen = 64;
    } else if (!strcmp(p->machine_name, "riscv128")) {
        max_xlen = 128;
    } else {
        vm_error("unsupported machine: %s\n", p->machine_name);
        return NULL;
    }

    if (p->cpu_count < 1 || p->cpu_count > RISCV_MAX_HARTS) {
        vm_error("%s: cpus must be between 1 and %d\n", p->machine_name,
                 RISCV_MAX_HARTS);
        return NULL;
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
        return NULL;
    }

    s = new RISCVMachine();
    s->intc_type = intc_type;
    s->vmc = p->vmc;
    s->ram_size = p->ram_size;
    s->max_xlen = max_xlen;
    s->mem_map = new PhysMemoryMap();
    /* needed to handle the RAM dirty bits */
    s->mem_map->SetTlbFlushTarget(s);

    for (int hart = 0; hart < p->cpu_count; hart++) {
        s->cpus[hart] = riscv_cpu_create(s->mem_map, max_xlen, hart);
        if (!s->cpus[hart]) {
            vm_error("unsupported max_xlen=%d\n", max_xlen);
            /* XXX: should free resources */
            return NULL;
        }
        s->hart_count++;
    }
    /* RAM */
    ram_flags = 0;
    s->mem_map->RegisterRam(RAM_BASE_ADDR, p->ram_size, ram_flags);
    s->mem_map->RegisterRam(0x00000000, LOW_RAM_SIZE, 0);
    s->rtc_real_time = p->rtc_real_time;
    if (p->rtc_real_time) {
        s->rtc_start_time = rtc_get_real_time(s);
    }
    /* the 'time' CSR must read the same counter as the CLINT */
    for (int hart = 0; hart < s->hart_count; hart++)
        s->cpus[hart]->SetRtcTimeSource(s);

    s->mem_map->RegisterDevice(CLINT_BASE_ADDR, CLINT_SIZE, &s->fClintIo,
                               DEVIO_SIZE32);
    s->mem_map->RegisterDevice(HTIF_BASE_ADDR, 16, &s->fHtifIo, DEVIO_SIZE32);
    s->console = p->console;

    IRQTarget *irq_target;
    if (intc_type == RISCV_INTC_PLIC) {
        s->plic = new PLIC(s, s->hart_count);
        s->mem_map->RegisterDevice(PLIC_BASE_ADDR, PLIC_SIZE, s->plic,
                                   DEVIO_SIZE32);
        irq_target = s->plic;
    } else {
        AplicMsiLayout msi;
        RISCVInterruptArch arch = RISCV_INTR_AIA;

        if (intc_type == RISCV_INTC_APLIC_IMSIC) {
            while ((1 << s->imsic_hart_bits) < s->hart_count)
                s->imsic_hart_bits++;
            s->mem_map->RegisterDevice(IMSIC_M_BASE_ADDR,
                                       imsic_region_size(s), &s->fImsicIoM,
                                       DEVIO_SIZE32);
            s->mem_map->RegisterDevice(IMSIC_S_BASE_ADDR,
                                       imsic_region_size(s), &s->fImsicIoS,
                                       DEVIO_SIZE32);
            msi.m_base = IMSIC_M_BASE_ADDR;
            msi.s_base = IMSIC_S_BASE_ADDR;
            msi.hart_index_bits = s->imsic_hart_bits;
            arch = RISCV_INTR_AIA_IMSIC;
        }
        s->aplic = new APLIC(s->mem_map, s, s->hart_count,
                             RISCV_IRQ_LINES - 1,
                             intc_type == RISCV_INTC_APLIC_IMSIC ?
                             &msi : nullptr);
        s->mem_map->RegisterDevice(APLIC_M_BASE_ADDR, APLIC_SIZE,
                                   s->aplic->DomainIO(false), DEVIO_SIZE32);
        s->mem_map->RegisterDevice(APLIC_S_BASE_ADDR, APLIC_SIZE,
                                   s->aplic->DomainIO(true), DEVIO_SIZE32);
        irq_target = s->aplic;
        for (int hart = 0; hart < s->hart_count; hart++)
            s->cpus[hart]->SetInterruptArch(arch);
    }

    s->bus = new SystemBus(s->mem_map, irq_target, RISCV_IRQ_LINES);
    if (intc_type == RISCV_INTC_APLIC_IMSIC) {
        s->bus->SetMsiTarget(s);
    }
    s->bus->MmioAlloc().SetWindow(DEVICE_WINDOW_BASE, DEVICE_WINDOW_SIZE);
    s->bus->MmioAlloc().SetHighWindow(HIGH_DEVICE_WINDOW_BASE,
                                      HIGH_DEVICE_WINDOW_SIZE);
    s->bus->IoAlloc().SetWindow(PCI_IO_WINDOW_BASE, PCI_IO_WINDOW_SIZE);
    if (!riscv_claim_fixed_ranges(s)) {
        return NULL;
    }

    /* The nesting in the file is the nesting of the buses, so the root one
       has to be the kind this machine provides. */
    if (p->root_bus_type != nullptr &&
        strcmp(p->root_bus_type, "fdt") != 0) {
        vm_error("%s: the root bus must be an 'fdt' bus, not '%s'\n",
                 p->machine_name, p->root_bus_type);
        return NULL;
    }

    ctx.params = p;
    ctx.console = p->console;
    ctx.serial_output = s;

    if (!device_build_tree(s->bus, p->root_devices, &ctx)) {
        return NULL;
    }
    if (!s->bus->AllocateAll()) {
        return NULL;
    }
    if (!s->bus->RealizeAll()) {
        return NULL;
    }

    s->console_dev = ctx.console_dev;
    s->keyboard = ctx.keyboard;
    s->mouse = ctx.mouse;
    s->fb_dev = ctx.fb_dev;
    s->serial_console = ctx.serial_console;
    s->net = ctx.net;

    if (!p->files[VM_FILE_BIOS].buf) {
        vm_error("No bios found");
    }

    copy_bios(s, p->files[VM_FILE_BIOS].buf, p->files[VM_FILE_BIOS].len,
              p->files[VM_FILE_KERNEL].buf, p->files[VM_FILE_KERNEL].len,
              p->files[VM_FILE_INITRD].buf, p->files[VM_FILE_INITRD].len,
              p->cmdline);

    return s;
}

RISCVMachine::~RISCVMachine()
{
    /* XXX: stop all */
    for (int hart = 0; hart < hart_count; hart++)
        delete cpus[hart];
    delete bus;
    delete plic;
    delete aplic;
    delete mem_map;
}

/* in ms */
int RISCVMachine::GetSleepDuration(int delay)
{
    uint64_t now = rtc_get_time(this);

    /* wait for an event: the only asynchronous events are the timers */
    for (int hart = 0; hart < hart_count; hart++) {
        RISCVCPU *s = cpus[hart];
        uint64_t stimecmp;

        if (!(s->Mip() & MIP_MTIP)) {
            if (now >= timecmp[hart]) {
                s->SetMip(MIP_MTIP);
                delay = 0;
            } else {
                /* convert delay to ms */
                uint64_t delay1 = (timecmp[hart] - now) / (RTC_FREQ / 1000);
                if (delay1 < (uint64_t)delay)
                    delay = delay1;
            }
        }
        /* the supervisor timer runs off the same counter when Sstc is
           enabled */
        stimecmp = s->UpdateSTimer();
        if (stimecmp != UINT64_MAX) {
            if (now >= stimecmp) {
                delay = 0;
            } else {
                uint64_t delay1 = (stimecmp - now) / (RTC_FREQ / 1000);
                if (delay1 < (uint64_t)delay)
                    delay = delay1;
            }
        }
        if (!s->PowerDown())
            delay = 0;
    }
    return delay;
}

/* Instructions a hart runs before the next one gets its turn. Short enough
   that a hart busy waiting on another, for an IPI to be answered or a lock
   to be dropped, does not waste much. */
#define HART_QUANTUM 10000

void RISCVMachine::Interp(int max_exec_cycle)
{
    if (hart_count == 1) {
        cpus[0]->Interp(max_exec_cycle);
        return;
    }

    /* Round robin until the budget is spent or every hart is waiting for
       an interrupt. */
    uint64_t executed = 0;
    while (executed < (uint64_t)max_exec_cycle && !shutdown_requested) {
        bool ran = false;
        for (int hart = 0; hart < hart_count; hart++) {
            RISCVCPU *cpu = cpus[hart];
            if (cpu->PowerDown())
                continue;
            uint64_t start = cpu->Cycles();
            cpu->Interp(HART_QUANTUM);
            executed += cpu->Cycles() - start;
            ran = true;
        }
        if (!ran)
            break;
    }
}

void RISCVMachine::SendKeyEvent(bool is_down, uint16_t key_code)
{
    if (keyboard != nullptr) {
        keyboard->SendKeyEvent(is_down, key_code);
    }
}

bool RISCVMachine::MouseIsAbsolute()
{
    /* With no pointer the answer only decides which coordinates the front
       end computes and then throws away. */
    return mouse == nullptr || mouse->MouseIsAbsolute();
}

void RISCVMachine::SendMouseEvent(int dx, int dy, int dz, unsigned int buttons)
{
    if (mouse != nullptr) {
        mouse->SendMouseEvent(dx, dy, dz, buttons);
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
        (void)p;
    }

    VirtMachine *Init(const VirtMachineParams *p) const override
    {
        return riscv_machine_init(p);
    }
};

static const RiscvMachineClass sRiscvMachineClass;
const VirtMachineClass &gRiscvMachineClass = sRiscvMachineClass;
