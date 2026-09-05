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

/* RISCV machine */

class RISCVMachine final:
    public VirtMachine,
    public IRQTarget,
    public TlbFlushTarget,
    public SerialOutput {
public:
    PhysMemoryMap *mem_map = nullptr;
    SystemBus *bus = nullptr;
    int max_xlen = 0;
    RISCVCPU *cpu_state = nullptr;
    uint64_t ram_size = 0;
    /* RTC */
    bool rtc_real_time = false;
    uint64_t rtc_start_time = 0;
    uint64_t timecmp = 0;
    /* PLIC */
    uint32_t plic_pending_irq = 0, plic_served_irq = 0;
    /* HTIF */
    uint64_t htif_tohost = 0, htif_fromhost = 0;

    VIRTIODevice *keyboard_dev = nullptr;
    VIRTIODevice *mouse_dev = nullptr;

    ~RISCVMachine() override;

    /* memory-mapped register blocks */
    uint32_t HtifRead(uint32_t offset, int size_log2);
    void HtifWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t ClintRead(uint32_t offset, int size_log2);
    void ClintWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t PlicRead(uint32_t offset, int size_log2);
    void PlicWrite(uint32_t offset, uint32_t val, int size_log2);

    DeviceIOAdapter<RISCVMachine, &RISCVMachine::HtifRead,
                    &RISCVMachine::HtifWrite> fHtifIo {*this};
    DeviceIOAdapter<RISCVMachine, &RISCVMachine::ClintRead,
                    &RISCVMachine::ClintWrite> fClintIo {*this};
    DeviceIOAdapter<RISCVMachine, &RISCVMachine::PlicRead,
                    &RISCVMachine::PlicWrite> fPlicIo {*this};

    /* IRQTarget */
    void SetIRQ(int irq_num, int level) override;

    /* TlbFlushTarget */
    void FlushTlbWriteRange(uint8_t *ram_addr, size_t ram_size) override;

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
#define CLINT_SIZE      0x000c0000
#define HTIF_BASE_ADDR 0x40008000
#define HTIF_SIZE      0x00001000
#define PLIC_BASE_ADDR 0x40100000
#define PLIC_SIZE      0x00400000

/* Everything the configuration adds is placed in here. It is the one hole in
   the architectural layout large enough for an ECAM window and a PCI aperture
   as well as the MMIO devices, and it stays below 4 GB because PCI BARs are
   32 bit. */
#define DEVICE_WINDOW_BASE 0x10000000
#define DEVICE_WINDOW_SIZE 0x30000000

/* PLIC input lines; line 0 does not exist. */
#define PLIC_NUM_SOURCES 32

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
        val = m->cpu_state->Cycles() / RTC_FREQ_DIV;
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

    device = s->htif_tohost >> 56;
    cmd = (s->htif_tohost >> 48) & 0xff;
    if (s->htif_tohost == 1) {
        /* shuthost */
        printf("\nPower off.\n");
        exit(0);
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

uint32_t RISCVMachine::ClintRead(uint32_t offset, int size_log2)
{
    RISCVMachine *m = this;
    uint32_t val;

    assert(size_log2 == 2);
    switch(offset) {
    case 0xbff8:
        val = rtc_get_time(m);
        break;
    case 0xbffc:
        val = rtc_get_time(m) >> 32;
        break;
    case 0x4000:
        val = m->timecmp;
        break;
    case 0x4004:
        val = m->timecmp >> 32;
        break;
    default:
        val = 0;
        break;
    }
    return val;
}

void RISCVMachine::ClintWrite(uint32_t offset, uint32_t val, int size_log2)
{
    RISCVMachine *m = this;

    assert(size_log2 == 2);
    switch(offset) {
    case 0x4000:
        m->timecmp = (m->timecmp & ~0xffffffff) | val;
        m->cpu_state->ResetMip(MIP_MTIP);
        break;
    case 0x4004:
        m->timecmp = (m->timecmp & 0xffffffff) | ((uint64_t)val << 32);
        m->cpu_state->ResetMip(MIP_MTIP);
        break;
    default:
        break;
    }
}

static void plic_update_mip(RISCVMachine *s)
{
    RISCVCPU *cpu = s->cpu_state;
    uint32_t mask;
    mask = s->plic_pending_irq & ~s->plic_served_irq;
    if (mask) {
        cpu->SetMip(MIP_MEIP | MIP_SEIP);
    } else {
        cpu->ResetMip(MIP_MEIP | MIP_SEIP);
    }
}

#define PLIC_HART_BASE 0x200000
#define PLIC_HART_SIZE 0x1000

uint32_t RISCVMachine::PlicRead(uint32_t offset, int size_log2)
{
    RISCVMachine *s = this;
    uint32_t val, mask;
    int i;
    assert(size_log2 == 2);
    switch(offset) {
    case PLIC_HART_BASE:
    case PLIC_HART_BASE + PLIC_HART_SIZE:
        val = 0;
        break;
    case PLIC_HART_BASE + 4:
    case PLIC_HART_BASE + PLIC_HART_SIZE + 4:
        mask = s->plic_pending_irq & ~s->plic_served_irq;
        if (mask != 0) {
            i = ctz32(mask);
            s->plic_served_irq |= 1 << i;
            plic_update_mip(s);
            val = i + 1;
        } else {
            val = 0;
        }
        break;
    default:
        val = 0;
        break;
    }
    return val;
}

void RISCVMachine::PlicWrite(uint32_t offset, uint32_t val, int size_log2)
{
    RISCVMachine *s = this;

    assert(size_log2 == 2);
    switch(offset) {
    case PLIC_HART_BASE + 4:
    case PLIC_HART_BASE + PLIC_HART_SIZE + 4:
        val--;
        if (val < 32) {
            s->plic_served_irq &= ~(1 << val);
            plic_update_mip(s);
        }
        break;
    default:
        break;
    }
}

void RISCVMachine::SetIRQ(int irq_num, int level)
{
    uint32_t mask = 1 << (irq_num - 1);
    if (level) {
        plic_pending_irq |= mask;
    } else {
        plic_pending_irq &= ~mask;
    }
    plic_update_mip(this);
}

static uint8_t *get_ram_ptr(RISCVMachine *s, uint64_t paddr, bool is_rw)
{
    return s->mem_map->GetRamPtr(paddr, is_rw);
}

/* FDT machine description */

static int riscv_build_fdt(RISCVMachine *m, uint8_t *dst,
                           uint64_t kernel_start, uint64_t kernel_size,
                           uint64_t initrd_start, uint64_t initrd_size,
                           const char *cmd_line)
{
    FDTBuilder fdt;
    FDTContext ctx;
    int size, max_xlen, i;
    char isa_string[128], *q;
    uint32_t misa;
    uint32_t tab[4];

    ctx.fdt = &fdt;

    fdt.BeginNode("");
    fdt.PropU32("#address-cells", 2);
    fdt.PropU32("#size-cells", 2);
    fdt.PropStr("compatible", "ucbbar,riscvemu-bar_dev");
    fdt.PropStr("model", "ucbbar,riscvemu-bare");

    /* CPU list */
    fdt.BeginNode("cpus");
    fdt.PropU32("#address-cells", 1);
    fdt.PropU32("#size-cells", 0);
    fdt.PropU32("timebase-frequency", RTC_FREQ);

    /* cpu */
    fdt.BeginNodeNum("cpu", 0);
    fdt.PropStr("device_type", "cpu");
    fdt.PropU32("reg", 0);
    fdt.PropStr("status", "okay");
    fdt.PropStr("compatible", "riscv");

    max_xlen = m->max_xlen;
    misa = m->cpu_state->Misa();
    q = isa_string;
    q += snprintf(isa_string, sizeof(isa_string), "rv%d", max_xlen);
    for(i = 0; i < 26; i++) {
        if (misa & (1 << i))
            *q++ = 'a' + i;
    }
    *q = '\0';
    fdt.PropStr("riscv,isa", isa_string);

    fdt.PropStr("mmu-type", max_xlen <= 32 ? "riscv,sv32" : "riscv,sv48");
    fdt.PropU32("clock-frequency", 2000000000);

    fdt.BeginNode("interrupt-controller");
    fdt.PropU32("#interrupt-cells", 1);
    fdt.PropEmpty("interrupt-controller");
    fdt.PropStr("compatible", "riscv,cpu-intc");
    ctx.intc_phandle = fdt.AllocPhandle();
    fdt.PropU32("phandle", ctx.intc_phandle);
    fdt.EndNode(); /* interrupt-controller */

    fdt.EndNode(); /* cpu */

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

    tab[0] = ctx.intc_phandle;
    tab[1] = 3; /* M IPI irq */
    tab[2] = ctx.intc_phandle;
    tab[3] = 7; /* M timer irq */
    fdt.PropTabU32("interrupts-extended", tab, 4);

    fdt.PropU64Range("reg", CLINT_BASE_ADDR, CLINT_SIZE);

    fdt.EndNode(); /* clint */

    fdt.BeginNodeNum("plic", PLIC_BASE_ADDR);
    fdt.PropU32("#interrupt-cells", 1);
    /* Needed so that an "interrupt-map" naming this controller as the parent
       has an unambiguous parent specifier length. */
    fdt.PropU32("#address-cells", 0);
    fdt.PropEmpty("interrupt-controller");
    fdt.PropStr("compatible", "riscv,plic0");
    fdt.PropU32("riscv,ndev", PLIC_NUM_SOURCES - 1);
    fdt.PropU64Range("reg", PLIC_BASE_ADDR, PLIC_SIZE);

    tab[0] = ctx.intc_phandle;
    tab[1] = 9; /* S ext irq */
    tab[2] = ctx.intc_phandle;
    tab[3] = 11; /* M ext irq */
    fdt.PropTabU32("interrupts-extended", tab, 4);

    ctx.plic_phandle = fdt.AllocPhandle();
    fdt.PropU32("phandle", ctx.plic_phandle);

    fdt.EndNode(); /* plic */

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
    uint32_t fdt_addr, align, kernel_base, initrd_base;
    uint8_t *ram_ptr;
    uint32_t *q;

    if (buf_len > s->ram_size) {
        vm_error("BIOS too big\n");
        exit(1);
    }

    ram_ptr = get_ram_ptr(s, RAM_BASE_ADDR, true);
    memcpy(ram_ptr, buf, buf_len);

    kernel_base = 0;
    if (kernel_buf_len > 0) {
        /* copy the kernel if present */
        if (s->max_xlen == 32)
            align = 4 << 20; /* 4 MB page align */
        else
            align = 2 << 20; /* 2 MB page align */
        kernel_base = (buf_len + align - 1) & ~(align - 1);
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

    riscv_build_fdt(s, ram_ptr + fdt_addr,
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

void RISCVMachine::FlushTlbWriteRange(uint8_t *ram_addr, size_t ram_size)
{
    cpu_state->FlushTlbWriteRangeRam(ram_addr, ram_size);
}

/* Reserve the parts of the map the architecture fixes, so that anything the
   configuration places is checked against them. */
static bool riscv_claim_fixed_ranges(RISCVMachine *s)
{
    RangeAllocator &mmio = s->bus->MmioAlloc();

    return mmio.Claim(0, LOW_RAM_SIZE, "low ram") &&
        mmio.Claim(CLINT_BASE_ADDR, CLINT_SIZE, "clint") &&
        mmio.Claim(HTIF_BASE_ADDR, HTIF_SIZE, "htif") &&
        mmio.Claim(PLIC_BASE_ADDR, PLIC_SIZE, "plic") &&
        mmio.Claim(RAM_BASE_ADDR, s->ram_size, "ram");
}

static VirtMachine *riscv_machine_init(const VirtMachineParams *p)
{
    RISCVMachine *s;
    int max_xlen, ram_flags;
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

    s = new RISCVMachine();
    s->vmc = p->vmc;
    s->ram_size = p->ram_size;
    s->max_xlen = max_xlen;
    s->mem_map = new PhysMemoryMap();
    /* needed to handle the RAM dirty bits */
    s->mem_map->SetTlbFlushTarget(s);

    s->cpu_state = riscv_cpu_create(s->mem_map, max_xlen);
    if (!s->cpu_state) {
        vm_error("unsupported max_xlen=%d\n", max_xlen);
        /* XXX: should free resources */
        return NULL;
    }
    /* RAM */
    ram_flags = 0;
    s->mem_map->RegisterRam(RAM_BASE_ADDR, p->ram_size, ram_flags);
    s->mem_map->RegisterRam(0x00000000, LOW_RAM_SIZE, 0);
    s->rtc_real_time = p->rtc_real_time;
    if (p->rtc_real_time) {
        s->rtc_start_time = rtc_get_real_time(s);
    }

    s->mem_map->RegisterDevice(CLINT_BASE_ADDR, CLINT_SIZE, &s->fClintIo,
                               DEVIO_SIZE32);
    s->mem_map->RegisterDevice(PLIC_BASE_ADDR, PLIC_SIZE, &s->fPlicIo,
                               DEVIO_SIZE32);
    s->mem_map->RegisterDevice(HTIF_BASE_ADDR, 16, &s->fHtifIo, DEVIO_SIZE32);
    s->console = p->console;

    s->bus = new SystemBus(s->mem_map, s, PLIC_NUM_SOURCES);
    s->bus->MmioAlloc().SetWindow(DEVICE_WINDOW_BASE, DEVICE_WINDOW_SIZE);
    if (!riscv_claim_fixed_ranges(s)) {
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
    s->keyboard_dev = ctx.keyboard_dev;
    s->mouse_dev = ctx.mouse_dev;
    s->fb_dev = ctx.fb_dev;
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
    delete cpu_state;
    delete bus;
    delete mem_map;
}

/* in ms */
int RISCVMachine::GetSleepDuration(int delay)
{
    RISCVCPU *s = cpu_state;
    int64_t delay1;

    /* wait for an event: the only asynchronous event is the RTC timer */
    if (!(s->Mip() & MIP_MTIP)) {
        delay1 = timecmp - rtc_get_time(this);
        if (delay1 <= 0) {
            s->SetMip(MIP_MTIP);
            delay = 0;
        } else {
            /* convert delay to ms */
            delay1 = delay1 / (RTC_FREQ / 1000);
            if (delay1 < delay)
                delay = delay1;
        }
    }
    if (!s->PowerDown())
        delay = 0;
    return delay;
}

void RISCVMachine::Interp(int max_exec_cycle)
{
    cpu_state->Interp(max_exec_cycle);
}

void RISCVMachine::SendKeyEvent(bool is_down, uint16_t key_code)
{
    if (keyboard_dev != nullptr) {
        virtio_input_send_key_event(keyboard_dev, is_down, key_code);
    }
}

bool RISCVMachine::MouseIsAbsolute()
{
    return true;
}

void RISCVMachine::SendMouseEvent(int dx, int dy, int dz, unsigned int buttons)
{
    if (mouse_dev != nullptr) {
        virtio_input_send_mouse_event(mouse_dev, dx, dy, dz, buttons);
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
