/*
 * Where the report goes
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
#include "console.h"

#include "format.h"
#include "mmio.h"
#include "test_control.h"

#ifdef __riscv
#include "riscv.h"
#endif

#define UART_THR 0
#define UART_LSR 5
#define UART_LSR_THRE 0x20

namespace {

enum Kind {
    OUT_NONE,
    OUT_TEST_CONTROL,
    OUT_UART,
    OUT_HTIF,
    OUT_SBI,
};

Kind sKind;
const HwDevice *sDevice;
uint64_t sBase;
bool sPortIo;
uint32_t sShift;
uint32_t sWidth = 1;

void uart_write_reg(int reg, uint8_t v)
{
    uint64_t a = sBase + ((uint64_t)reg << sShift);
#if defined(__i386__) || defined(__x86_64__)
    if (sPortIo) {
        outb(a, v);
        return;
    }
#endif
    if (sWidth == 4)
        mmio_write32(a, v);
    else
        mmio_write8(a, v);
}

uint8_t uart_read_reg(int reg)
{
    uint64_t a = sBase + ((uint64_t)reg << sShift);
#if defined(__i386__) || defined(__x86_64__)
    if (sPortIo)
        return inb(a);
#endif
    return sWidth == 4 ? mmio_read32(a) : mmio_read8(a);
}

void putc_raw(char c)
{
    switch (sKind) {
    case OUT_TEST_CONTROL:
        mmio_write32(sBase + TEST_CONTROL_OUTPUT, (uint8_t)c);
        break;
    case OUT_UART:
        /* bounded, so a UART a test left broken cannot hang the report */
        for (int i = 0; i < 100000; i++) {
            if (uart_read_reg(UART_LSR) & UART_LSR_THRE)
                break;
        }
        uart_write_reg(UART_THR, c);
        break;
    case OUT_HTIF: {
        /* tohost is the second register of the node, fromhost the first;
           device 1 command 1 writes a character */
        uint64_t tohost = sDevice->regions[1].base;
        uint64_t fromhost = sDevice->regions[0].base;
        mmio_write32(tohost, (uint8_t)c);
        mmio_write32(tohost + 4, 0x01010000);
        while (mmio_read32(tohost) || mmio_read32(tohost + 4))
            ;
        mmio_write32(fromhost, 0);
        mmio_write32(fromhost + 4, 0);
        break;
    }
    case OUT_SBI:
#ifdef __riscv
        sbi_call(SBI_EXT_DBCN, 2, (uint8_t)c);
#endif
        break;
    case OUT_NONE:
        break;
    }
}

bool pick_uart(const HwDevice *d)
{
    if (d == nullptr)
        return false;
    const HwRegion *r = hw_region(d, HW_MMIO);
    if (r) {
        sBase = r->base;
        sPortIo = false;
    } else if ((r = hw_region(d, HW_IO)) != nullptr) {
        sBase = r->base;
        sPortIo = true;
    } else {
        return false;
    }
    sShift = 0;
    sWidth = 1;
    if (d->fdt) {
        fdt_prop_u32(d->fdt, "reg-shift", &sShift);
        fdt_prop_u32(d->fdt, "reg-io-width", &sWidth);
    }
    sKind = OUT_UART;
    sDevice = d;
    return true;
}

const HwDevice *chosen_uart()
{
    const FdtNode *chosen = fdt_root() ? fdt_find_path("/chosen") : nullptr;
    const char *path = chosen ? fdt_prop_str(chosen, "stdout-path") : nullptr;
    if (path == nullptr)
        return nullptr;
    /* the path ends at the options */
    char buf[128];
    size_t n = 0;
    while (path[n] && path[n] != ':' && n + 1 < sizeof(buf)) {
        buf[n] = path[n];
        n++;
    }
    buf[n] = '\0';
    const FdtNode *node = fdt_find_path(buf);
    for (int i = 0; node && i < registry_count(); i++) {
        if (registry_at(i)->fdt == node)
            return registry_at(i);
    }
    return nullptr;
}

void console_sink(void *, char c)
{
    putc_raw(c);
}

}


const HwDevice *test_control_device()
{
    const HwDevice *d = registry_find(TEST_CONTROL_COMPATIBLE);
    return d ? d : registry_find(TEST_CONTROL_ACPI_HID);
}


void console_init()
{
    const HwDevice *tc = test_control_device();
    const HwRegion *r = tc ? hw_region(tc, HW_MMIO) : nullptr;
    if (r && mmio_read32(r->base + TEST_CONTROL_ID) == TEST_CONTROL_MAGIC) {
        sKind = OUT_TEST_CONTROL;
        sDevice = tc;
        sBase = r->base;
        return;
    }
    if (pick_uart(chosen_uart()) || pick_uart(registry_find("ns16550a")) ||
        pick_uart(registry_find("PNP0501")))
        return;
    const HwDevice *htif = registry_find("ucb,htif0");
    if (htif && htif->nregions >= 2) {
        sKind = OUT_HTIF;
        sDevice = htif;
        return;
    }
#ifdef __riscv
    if (!riscv_machine_mode() && sbi_probe(SBI_EXT_DBCN)) {
        sKind = OUT_SBI;
        return;
    }
#endif
}


bool console_ready()
{
    return sKind != OUT_NONE;
}


const char *console_kind()
{
    switch (sKind) {
    case OUT_TEST_CONTROL:
        return "test-control";
    case OUT_UART:
        return "uart";
    case OUT_HTIF:
        return "htif";
    case OUT_SBI:
        return "sbi";
    default:
        return "none";
    }
}


const HwDevice *console_device()
{
    return sDevice;
}


void console_write(const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++)
        putc_raw(s[i]);
}


void vreport(const char *fmt, va_list ap)
{
    putc_raw('@');
    putc_raw('@');
    putc_raw('T');
    putc_raw(' ');
    vformat(console_sink, nullptr, fmt, ap);
    putc_raw('\n');
}


void report(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vreport(fmt, ap);
    va_end(ap);
}
