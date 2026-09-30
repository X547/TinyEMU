/*
 * ICI benchmark: inter-processor interrupt latency on the pc machine.
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

/* CPU 0 finds the processors in the ACPI MADT, starts them with INIT and
   STARTUP, calibrates the TSC against the ACPI PM timer, then times:

   - ping-pong: a fixed IPI to one processor, whose handler sends one back;
     with the target spinning, and with it halted;
   - broadcast: an all-but-self IPI whose handlers each count in memory,
     until all have counted.

   Results go to the serial port. A run ends with an S5 power off (exit
   status 0), or a triple fault on an error (exit status 1). The command line
   may say "iterations=N". */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* keep in sync with start.S */
#define VECTOR_PING 0x40
#define VECTOR_PONG 0x41
#define VECTOR_ACK 0x42
#define VECTOR_WAKE 0x43
#define VECTOR_SPURIOUS 0xff
#define AP_TRAMPOLINE 0x8000
#define STACK_SIZE 0x4000

#define MAX_CPUS 64
#define MAX_ITERATIONS 100000
#define DEFAULT_ITERATIONS 10000
#define WARMUP 100

#define LAPIC_BASE 0xfee00000UL
#define LAPIC_ID 0x20
#define LAPIC_TPR 0x80
#define LAPIC_EOI 0xb0
#define LAPIC_SVR 0xf0
#define LAPIC_ICR_LO 0x300
#define LAPIC_ICR_HI 0x310
#define LAPIC_LINT0 0x350
#define LAPIC_LINT1 0x360

#define LVT_MASKED (1 << 16)

#define ICR_INIT (5 << 8)
#define ICR_STARTUP (6 << 8)
#define ICR_BUSY (1 << 12)
#define ICR_ASSERT (1 << 14)
#define ICR_ALL_BUT_SELF (3 << 18)

#define MSR_APIC_BASE 0x1b
#define APIC_BASE_ENABLE (1 << 11)

#define COM1 0x3f8

/* this machine's \_S5 sleep type */
#define SLP_TYP_S5 5
#define SLP_EN (1 << 13)

#define PM_TIMER_FREQ 3579545

struct hvm_start_info {
    uint32_t magic;
    uint32_t version;
    uint32_t flags;
    uint32_t nr_modules;
    uint64_t modlist_paddr;
    uint64_t cmdline_paddr;
    uint64_t rsdp_paddr;
    uint64_t memmap_paddr;
    uint32_t memmap_entries;
    uint32_t reserved;
};

struct idt_entry {
    uint16_t offset_lo;
    uint16_t selector;
    uint16_t flags;
    uint16_t offset_mid;
    uint32_t offset_hi;
    uint32_t reserved;
};

enum ap_mode {
    AP_SPIN,
    AP_HALT,
};

/* start.S */
extern const char ap_trampoline[], ap_trampoline_end[];
extern void (*const exception_table[22])(void);
extern void ping_handler(void), pong_handler(void), ack_handler(void);
extern void wake_handler(void), spurious_handler(void);

/* shared with start.S */
uint32_t bsp_apic_id;
volatile uint32_t pong_flag;
volatile uint32_t ack_count;
uint64_t ap_stack_top;

static struct idt_entry idt[256];
static uint8_t ap_stacks[MAX_CPUS][STACK_SIZE] __attribute__((aligned(16)));
static uint32_t apic_ids[MAX_CPUS];
static int cpu_count;
static volatile int ap_started;
static volatile enum ap_mode ap_mode;

static uint16_t pm_timer_port, pm1_control_port;
static uint64_t tsc_khz;
static uint64_t samples[MAX_ITERATIONS];
static int iterations = DEFAULT_ITERATIONS;
static bool failed;

static inline void outb(uint16_t port, uint8_t val)
{
    __asm__ volatile("outb %0, %1" : : "a"(val), "Nd"(port));
}

static inline void outw(uint16_t port, uint16_t val)
{
    __asm__ volatile("outw %0, %1" : : "a"(val), "Nd"(port));
}

static inline uint8_t inb(uint16_t port)
{
    uint8_t val;
    __asm__ volatile("inb %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline uint32_t inl(uint16_t port)
{
    uint32_t val;
    __asm__ volatile("inl %1, %0" : "=a"(val) : "Nd"(port));
    return val;
}

static inline uint64_t rdtsc(void)
{
    uint32_t lo, hi;
    __asm__ volatile("lfence; rdtsc" : "=a"(lo), "=d"(hi) : : "memory");
    return ((uint64_t)hi << 32) | lo;
}

static inline uint64_t rdmsr(uint32_t msr)
{
    uint32_t lo, hi;
    __asm__ volatile("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return ((uint64_t)hi << 32) | lo;
}

static inline void wrmsr(uint32_t msr, uint64_t val)
{
    __asm__ volatile("wrmsr" : : "c"(msr), "a"((uint32_t)val),
                     "d"((uint32_t)(val >> 32)));
}

static inline void pause(void)
{
    __builtin_ia32_pause();
}

static inline uint32_t lapic_read(uint32_t reg)
{
    return *(volatile uint32_t *)(LAPIC_BASE + reg);
}

static inline void lapic_write(uint32_t reg, uint32_t val)
{
    *(volatile uint32_t *)(LAPIC_BASE + reg) = val;
}

/* The compiler may call these for copies and clears. */

void *memcpy(void *dst, const void *src, size_t n)
{
    void *d = dst;
    __asm__ volatile("rep movsb" : "+D"(d), "+S"(src), "+c"(n) : : "memory");
    return dst;
}

void *memset(void *dst, int c, size_t n)
{
    void *d = dst;
    __asm__ volatile("rep stosb" : "+D"(d), "+c"(n) : "a"(c) : "memory");
    return dst;
}

/* Output */

static void print_char(char c)
{
    if (c == '\n')
        print_char('\r');
    for (int i = 0; i < 100000 && !(inb(COM1 + 5) & 0x20); i++)
        pause();
    outb(COM1, c);
}

static void print(const char *s)
{
    while (*s)
        print_char(*s++);
}

static void put_uint(uint64_t val, int width)
{
    char buf[21];
    int n = 0;

    do {
        buf[n++] = '0' + val % 10;
        val /= 10;
    } while (val);
    while (width-- > n)
        print_char(' ');
    while (n)
        print_char(buf[--n]);
}

static void put_hex(uint64_t val)
{
    print("0x");
    for (int shift = 60; shift >= 0; shift -= 4) {
        if (shift != 0 && (val >> shift) == 0)
            continue;
        print_char("0123456789abcdef"[(val >> shift) & 15]);
    }
}

/* nanoseconds as microseconds with two decimals, right aligned */
static void put_us(uint64_t ns)
{
    uint64_t hundredths = (ns + 5) / 10;
    put_uint(hundredths / 100, 6);
    print_char('.');
    print_char('0' + hundredths / 10 % 10);
    print_char('0' + hundredths % 10);
}

/* Ending the run */

static void __attribute__((noreturn)) triple_fault(void)
{
    static const struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) empty = {0, 0};

    __asm__ volatile("lidt %0; int3" : : "m"(empty));
    for (;;)
        __asm__ volatile("cli; hlt");
}

static void __attribute__((noreturn)) power_off(void)
{
    outw(pm1_control_port, SLP_TYP_S5 << 10 | SLP_EN);
    for (;;)
        __asm__ volatile("cli; hlt");
}

static void __attribute__((noreturn)) die(const char *msg)
{
    print("ici: ");
    print(msg);
    print("\n");
    triple_fault();
}

/* vector, error code, then the processor's interrupt frame */
void fault(uint64_t *frame)
{
    print("ici: exception ");
    put_uint(frame[0], 0);
    print(" error ");
    put_hex(frame[1]);
    print(" rip ");
    put_hex(frame[2]);
    print(" apic ");
    put_uint(lapic_read(LAPIC_ID) >> 24, 0);
    print("\n");
    triple_fault();
}

/* Time */

static uint32_t pm_timer(void)
{
    return inl(pm_timer_port) & 0xffffff;
}

static void pm_delay_us(uint64_t us)
{
    uint64_t ticks = us * PM_TIMER_FREQ / 1000000;
    uint32_t last = pm_timer();
    uint64_t elapsed = 0;

    while (elapsed < ticks) {
        uint32_t now = pm_timer();
        elapsed += (now - last) & 0xffffff;
        last = now;
        pause();
    }
}

/* TSC ticks over 100 ms of the PM timer, which runs on host time on every
   backend; RDTSC is the hardware's under a hypervisor. */
static void calibrate_tsc(void)
{
    uint64_t start = rdtsc();
    pm_delay_us(100000);
    tsc_khz = (rdtsc() - start) / 100;
    if (tsc_khz == 0)
        die("the TSC does not count");
}

static uint64_t ticks_to_ns(uint64_t ticks)
{
    return ticks * 1000000 / tsc_khz;
}

/* ACPI */

struct acpi_header {
    char signature[4];
    uint32_t length;
    uint8_t revision;
    uint8_t checksum;
    char oem_id[6];
    char oem_table_id[8];
    uint32_t oem_revision;
    uint32_t creator_id;
    uint32_t creator_revision;
} __attribute__((packed));

static bool same4(const char *a, const char *b)
{
    return a[0] == b[0] && a[1] == b[1] && a[2] == b[2] && a[3] == b[3];
}

static const struct acpi_header *find_table(uint64_t rsdp, const char *sig)
{
    const uint8_t *r = (const uint8_t *)rsdp;
    bool xsdt = r[15] >= 2 && *(const uint64_t *)(r + 24) != 0;
    const struct acpi_header *sdt = (const struct acpi_header *)(xsdt ?
        *(const uint64_t *)(r + 24) : *(const uint32_t *)(r + 16));
    int entry = xsdt ? 8 : 4;
    int n = (sdt->length - sizeof(*sdt)) / entry;
    const uint8_t *p = (const uint8_t *)(sdt + 1);

    for (int i = 0; i < n; i++) {
        uint64_t addr = xsdt ? *(const uint64_t *)(p + i * entry) :
            *(const uint32_t *)(p + i * entry);
        const struct acpi_header *h = (const struct acpi_header *)addr;
        if (same4(h->signature, sig))
            return h;
    }
    return NULL;
}

static void read_acpi(uint64_t rsdp)
{
    if (rsdp == 0 || !same4((const char *)rsdp, "RSD "))
        die("no ACPI RSDP");

    const struct acpi_header *fadt = find_table(rsdp, "FACP");
    if (fadt == NULL)
        die("no FADT");
    const uint8_t *f = (const uint8_t *)fadt;
    pm1_control_port = *(const uint32_t *)(f + 64);
    pm_timer_port = *(const uint32_t *)(f + 76);

    const struct acpi_header *madt = find_table(rsdp, "APIC");
    if (madt == NULL)
        die("no MADT; the benchmark needs interrupt_controller: \"apic\"");
    const uint8_t *p = (const uint8_t *)madt + sizeof(*madt) + 8;
    const uint8_t *end = (const uint8_t *)madt + madt->length;
    for (; p + 2 <= end && p[1] >= 2; p += p[1]) {
        /* processor local APIC, enabled */
        if (p[0] == 0 && (p[4] & 1) && cpu_count < MAX_CPUS)
            apic_ids[cpu_count++] = p[3];
    }
}

/* Local APIC and interrupts */

static void set_gate(int vector, void (*handler)(void))
{
    uint64_t addr = (uint64_t)handler;
    idt[vector] = (struct idt_entry){
        .offset_lo = addr,
        .selector = 0x18,
        .flags = 0x8e00, /* present, interrupt gate */
        .offset_mid = addr >> 16,
        .offset_hi = addr >> 32,
    };
}

static void load_idt(void)
{
    struct {
        uint16_t limit;
        uint64_t base;
    } __attribute__((packed)) idtr = {sizeof(idt) - 1, (uint64_t)idt};

    __asm__ volatile("lidt %0" : : "m"(idtr));
}

static void lapic_enable(void)
{
    uint64_t base = rdmsr(MSR_APIC_BASE);
    if ((base & ~0xfffULL) != LAPIC_BASE)
        die("the local APIC is not at 0xfee00000");
    wrmsr(MSR_APIC_BASE, base | APIC_BASE_ENABLE);
    lapic_write(LAPIC_SVR, 0x100 | VECTOR_SPURIOUS);
    lapic_write(LAPIC_LINT0, LVT_MASKED);
    lapic_write(LAPIC_LINT1, LVT_MASKED);
    lapic_write(LAPIC_TPR, 0);
}

static void send_ipi(uint32_t apic_id, uint32_t low)
{
    lapic_write(LAPIC_ICR_HI, apic_id << 24);
    lapic_write(LAPIC_ICR_LO, low);
}

static void wait_icr_idle(void)
{
    while (lapic_read(LAPIC_ICR_LO) & ICR_BUSY)
        pause();
}

/* Starting the others */

void ap_main(void)
{
    load_idt();
    lapic_enable();
    ap_started = 1;
    __asm__ volatile("sti");
    for (;;) {
        if (ap_mode == AP_HALT)
            __asm__ volatile("hlt");
        else
            pause();
    }
}

static bool wait_started(uint64_t us)
{
    for (uint64_t i = 0; i < us / 100; i++) {
        if (ap_started)
            return true;
        pm_delay_us(100);
    }
    return ap_started;
}

static void start_cpu(int index)
{
    uint32_t id = apic_ids[index];

    ap_started = 0;
    ap_stack_top = (uint64_t)ap_stacks[index + 1];
    send_ipi(id, ICR_INIT | ICR_ASSERT);
    wait_icr_idle();
    pm_delay_us(10000);
    for (int i = 0; i < 2; i++) {
        send_ipi(id, ICR_STARTUP | AP_TRAMPOLINE >> 12);
        wait_icr_idle();
        if (wait_started(i == 0 ? 10000 : 1000000))
            return;
    }
    print("ici: CPU with APIC ID ");
    put_uint(id, 0);
    print(" did not start\n");
    triple_fault();
}

/* Statistics */

static void sort(uint64_t *a, int n)
{
    /* heapsort: no recursion, no extra space */
    for (int start = n / 2 - 1, end = n; end > 1;) {
        int root;
        if (start >= 0) {
            root = start--;
        } else {
            uint64_t t = a[0];
            a[0] = a[--end];
            a[end] = t;
            root = 0;
        }
        for (;;) {
            int child = 2 * root + 1;
            if (child >= end)
                break;
            if (child + 1 < end && a[child + 1] > a[child])
                child++;
            if (a[root] >= a[child])
                break;
            uint64_t t = a[root];
            a[root] = a[child];
            a[child] = t;
            root = child;
        }
    }
}

static void report(const char *name, int target)
{
    uint64_t sum = 0;
    int column = 0;

    sort(samples, iterations);
    for (int i = 0; i < iterations; i++)
        sum += samples[i];
    for (; name[column]; column++)
        print_char(name[column]);
    if (target >= 0) {
        print(" cpu");
        put_uint(target, 0);
        column += target < 10 ? 5 : 6;
    }
    for (; column < 20; column++)
        print_char(' ');
    put_us(ticks_to_ns(samples[0]));
    put_us(ticks_to_ns(samples[iterations / 2]));
    put_us(ticks_to_ns(sum / iterations));
    put_us(ticks_to_ns(samples[iterations * 99 / 100]));
    put_us(ticks_to_ns(samples[iterations - 1]));
    print("\n");
}

/* The benchmarks. CPU 0 waits for the answers spinning, interrupts on. */

static bool wait_flag(volatile uint32_t *flag, uint32_t value, uint64_t start)
{
    uint64_t timeout = tsc_khz * 1000;

    while (*flag != value) {
        if (rdtsc() - start > timeout)
            return false;
        pause();
    }
    return true;
}

static void set_mode(enum ap_mode mode)
{
    ap_mode = mode;
    send_ipi(0, ICR_ALL_BUT_SELF | VECTOR_WAKE);
    pm_delay_us(1000);
}

static void ping_pong(const char *name, int target)
{
    uint32_t id = apic_ids[target];

    for (int i = -WARMUP; i < iterations; i++) {
        pong_flag = 0;
        uint64_t start = rdtsc();
        send_ipi(id, VECTOR_PING);
        if (!wait_flag(&pong_flag, 1, start)) {
            print("ici: no answer from cpu");
            put_uint(target, 0);
            print("\n");
            failed = true;
            return;
        }
        if (i >= 0)
            samples[i] = rdtsc() - start;
    }
    report(name, target);
}

static void broadcast(const char *name)
{
    for (int i = -WARMUP; i < iterations; i++) {
        ack_count = 0;
        uint64_t start = rdtsc();
        send_ipi(0, ICR_ALL_BUT_SELF | VECTOR_ACK);
        if (!wait_flag(&ack_count, cpu_count - 1, start)) {
            print("ici: broadcast answered by ");
            put_uint(ack_count, 0);
            print(" of ");
            put_uint(cpu_count - 1, 0);
            print("\n");
            failed = true;
            return;
        }
        if (i >= 0)
            samples[i] = rdtsc() - start;
    }
    report(name, -1);
}

/* "iterations=N" among the space separated words */
static void parse_cmdline(const char *s)
{
    static const char key[] = "iterations=";

    while (s != NULL && *s) {
        int k = 0;
        while (key[k] && s[k] == key[k])
            k++;
        if (!key[k]) {
            int n = 0;
            for (s += k; *s >= '0' && *s <= '9' && n <= MAX_ITERATIONS; s++)
                n = n * 10 + (*s - '0');
            if (n < 1 || n > MAX_ITERATIONS || (*s && *s != ' '))
                die("iterations must be 1 to 100000");
            iterations = n;
        }
        while (*s && *s != ' ')
            s++;
        while (*s == ' ')
            s++;
    }
}

void bsp_main(const struct hvm_start_info *si)
{
    outb(COM1 + 1, 0x00);   /* no interrupts */
    outb(COM1 + 3, 0x03);   /* 8N1 */
    /* CPU 0 may start with LINT0 as the 8259s' virtual wire */
    outb(0x21, 0xff);
    outb(0xa1, 0xff);

    if (si->magic != 0x336ec578)
        die("not started through PVH");
    parse_cmdline((const char *)si->cmdline_paddr);
    read_acpi(si->rsdp_paddr);

    for (int i = 0; i < 22; i++)
        set_gate(i, exception_table[i]);
    set_gate(VECTOR_PING, ping_handler);
    set_gate(VECTOR_PONG, pong_handler);
    set_gate(VECTOR_ACK, ack_handler);
    set_gate(VECTOR_WAKE, wake_handler);
    set_gate(VECTOR_SPURIOUS, spurious_handler);
    load_idt();
    lapic_enable();
    bsp_apic_id = lapic_read(LAPIC_ID) >> 24;
    if (cpu_count == 0 || apic_ids[0] != bsp_apic_id)
        die("the MADT does not list the boot processor first");

    calibrate_tsc();
    print("ici: ");
    put_uint(cpu_count, 0);
    print(" cpus, TSC ");
    put_uint(tsc_khz / 1000, 0);
    print_char('.');
    put_uint(tsc_khz % 1000 / 100, 0);
    print(" MHz, ");
    put_uint(iterations, 0);
    print(" iterations\n");
    if (cpu_count < 2)
        die("needs 2 or more cpus");

    memcpy((void *)AP_TRAMPOLINE, ap_trampoline,
           ap_trampoline_end - ap_trampoline);
    for (int i = 1; i < cpu_count; i++)
        start_cpu(i);

    __asm__ volatile("sti");
    print("round trip, us            min   median     mean      p99      max\n");
    set_mode(AP_SPIN);
    for (int i = 1; i < cpu_count && !failed; i++)
        ping_pong("pingpong spin", i);
    if (!failed)
        broadcast("broadcast spin");
    set_mode(AP_HALT);
    for (int i = 1; i < cpu_count && !failed; i++)
        ping_pong("pingpong halt", i);
    if (!failed)
        broadcast("broadcast halt");

    if (failed)
        triple_fault();
    print("ici: done\n");
    power_off();
}
