/*
 * 16550A UART tests
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
#include "intc.h"
#include "mmio.h"
#include "test.h"
#include "virtio.h"

/* The registers and their bits as the PC16550D data sheet (National
   Semiconductor, June 1995) gives them, "the data sheet" below. Loopback
   mode is what lets the receiver be tested without a line. */

#define RBR 0 /* read, DLAB 0 */
#define THR 0 /* write, DLAB 0 */
#define DLL 0 /* DLAB 1 */
#define IER 1 /* DLAB 0 */
#define DLM 1 /* DLAB 1 */
#define IIR 2 /* read */
#define FCR 2 /* write */
#define LCR 3
#define MCR 4
#define LSR 5
#define MSR 6
#define SCR 7

#define IER_RDA  0x01
#define IER_THRE 0x02
#define IER_RLS  0x04
#define IER_MS   0x08

#define IIR_NONE 0x01
#define IIR_MS   0x00
#define IIR_THRE 0x02
#define IIR_RDA  0x04
#define IIR_RLS  0x06
#define IIR_CTI  0x0c
#define IIR_FIFO 0xc0

#define FCR_ENABLE 0x01
#define FCR_RXRST  0x02
#define FCR_TXRST  0x04
#define FCR_TRIG(n) ((n) << 6) /* 1, 4, 8 or 14 characters */

#define LCR_8N1  0x03
#define LCR_SBC  0x40
#define LCR_DLAB 0x80

#define MCR_DTR  0x01
#define MCR_RTS  0x02
#define MCR_OUT1 0x04
#define MCR_OUT2 0x08
#define MCR_LOOP 0x10

#define LSR_DR   0x01
#define LSR_OE   0x02
#define LSR_BI   0x10
#define LSR_THRE 0x20
#define LSR_TEMT 0x40
#define LSR_FIFOE 0x80

#define MSR_DCTS 0x01
#define MSR_DDSR 0x02
#define MSR_TERI 0x04
#define MSR_DDCD 0x08
#define MSR_CTS  0x10
#define MSR_DSR  0x20
#define MSR_RI   0x40
#define MSR_DCD  0x80

/* The PC's crystal, which ACPI does not give. */
#define PC_CLOCK_HZ 1843200

namespace {

const HwDevice *sDev;
uint64_t sBase;
bool sPortIo;
uint32_t sShift;
uint32_t sWidth;
uint32_t sClockHz;

uint8_t rd(int reg)
{
    uint64_t a = sBase + ((uint64_t)reg << sShift);
#if defined(__i386__) || defined(__x86_64__)
    if (sPortIo)
        return inb(a);
#endif
    return sWidth == 4 ? mmio_read32(a) : mmio_read8(a);
}

void wr(int reg, uint8_t v)
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

/* Empties the receiver, however many characters it holds. */
void drain()
{
    for (int i = 0; i < 64 && (rd(LSR) & LSR_DR); i++)
        rd(RBR);
}

void set_divisor(uint16_t div)
{
    uint8_t lcr = rd(LCR);
    wr(LCR, lcr | LCR_DLAB);
    wr(DLL, div & 0xff);
    wr(DLM, div >> 8);
    wr(LCR, lcr);
}

/* What reset left, as far as the tests change it. */
void restore()
{
    wr(IER, 0);
    wr(LCR, 0);
    wr(MCR, MCR_LOOP);
    wr(FCR, 0);
    drain();
    wr(MCR, 0);
    rd(LSR);
    rd(MSR);
    rd(IIR);
}

/* The first 16550 the tables give that does not carry the report. */
void require_uart()
{
    const HwDevice *d = registry_find("ns16550a");
    if (d == nullptr)
        d = registry_find("PNP0501");
    if (d == nullptr)
        t_skip("no 16550");
    if (d == console_device())
        t_skip("the 16550 carries the report");
    sDev = d;
    const HwRegion *r = hw_region(d, HW_MMIO);
    sPortIo = r == nullptr;
    if (r == nullptr)
        r = hw_region(d, HW_IO);
    ASSERT_TRUE(r != nullptr);
    sBase = r->base;
    sShift = 0;
    sWidth = 1;
    sClockHz = PC_CLOCK_HZ;
    if (d->fdt) {
        fdt_prop_u32(d->fdt, "reg-shift", &sShift);
        fdt_prop_u32(d->fdt, "reg-io-width", &sWidth);
        fdt_prop_u32(d->fdt, "clock-frequency", &sClockHz);
    }
    t_on_cleanup(restore);
}

/* Loopback with the FIFOs at a trigger level, 8 data bits, and slow
   enough that no character times out while a test looks. */
void loopback_fifo(int trigger)
{
    wr(LCR, LCR_8N1);
    set_divisor(0xffff);
    wr(MCR, MCR_LOOP);
    wr(FCR, FCR_ENABLE | FCR_RXRST | FCR_TXRST | FCR_TRIG(trigger));
    drain();
    rd(LSR);
    rd(MSR);
}

void send(const char *s)
{
    while (*s)
        wr(THR, *s++);
}

int sIrqCount;
uint8_t sIrqIir[16];

/* Records what IIR reported and quiets the source: the data sheet's
   "interrupt reset control" for each identification. */
void uart_irq(void *)
{
    uint8_t iir = rd(IIR) & 0x0f;
    if (sIrqCount < (int)ARRAY_SIZE(sIrqIir))
        sIrqIir[sIrqCount] = iir;
    sIrqCount++;
    switch (iir) {
    case IIR_RLS:
        rd(LSR);
        break;
    case IIR_RDA:
    case IIR_CTI:
        drain();
        break;
    case IIR_MS:
        rd(MSR);
        break;
    default:
        /* THRE was taken back by reading IIR */
        break;
    }
}

void route_irq()
{
    sIrqCount = 0;
    if (!irq_route(sDev, 0, uart_irq, nullptr))
        t_skip("the 16550's interrupt reaches no controller the suite drives");
}

void unroute()
{
    irq_unroute(sDev, 0);
}

}


/* The data sheet's reset table: what master reset leaves in each
   register. MSR's
   upper half reads the modem inputs, which nothing drives here; the
   divisor and the scratch register are not reset. */
FRESH_TEST(uart, reset_state)
{
    require_uart();
    EXPECT_EQ(rd(IER), 0);
    EXPECT_EQ(rd(IIR), IIR_NONE);
    EXPECT_EQ(rd(LCR), 0);
    EXPECT_EQ(rd(MCR), 0);
    EXPECT_EQ(rd(LSR), LSR_THRE | LSR_TEMT);
    EXPECT_EQ(rd(MSR) & 0x0f, 0);
    SNAP("msr", "%#x", rd(MSR));
    SNAP("scr", "%#x", rd(SCR));
    wr(LCR, LCR_DLAB);
    SNAP("dll", "%#x", rd(DLL));
    SNAP("dlm", "%#x", rd(DLM));
    wr(LCR, 0);
}

/* The scratch register holds whatever is written to it. */
TEST(uart, scratch_register)
{
    require_uart();
    static const uint8_t values[] = {0x00, 0xff, 0x55, 0xaa, 0x01, 0x80};
    for (uint8_t v : values) {
        wr(SCR, v);
        EXPECT_EQ(rd(SCR), v);
    }
}

/* With DLAB set offsets 0 and 1 are the 16 bit divisor latch; with it
   clear they are the data and interrupt enable registers again. */
TEST(uart, divisor_latch)
{
    require_uart();
    wr(IER, IER_MS);
    wr(LCR, LCR_DLAB);
    wr(DLL, 0x34);
    wr(DLM, 0x12);
    EXPECT_EQ(rd(DLL), 0x34);
    EXPECT_EQ(rd(DLM), 0x12);
    wr(LCR, LCR_8N1);
    EXPECT_EQ(rd(IER), IER_MS);
    wr(LCR, LCR_DLAB | LCR_8N1);
    EXPECT_EQ(rd(DLL), 0x34);
    EXPECT_EQ(rd(DLM), 0x12);
    wr(DLL, 0x01);
    wr(DLM, 0x00);
    wr(LCR, 0);
}

/* Every bit of LCR reads back. IER has four bits and MCR five; the rest
   read 0 (data sheet, Interrupt Enable and Modem Control Registers). */
TEST(uart, register_bits)
{
    require_uart();
    wr(LCR, 0x7f);
    EXPECT_EQ(rd(LCR), 0x7f);
    wr(LCR, 0);
    wr(IER, 0xff);
    EXPECT_EQ(rd(IER), 0x0f);
    wr(IER, 0);
    wr(MCR, 0xff);
    EXPECT_EQ(rd(MCR), 0x1f);
    wr(MCR, 0);
    /* LSR and MSR are for reading; the data sheet calls a write to LSR a
       factory test, which a 16550 need not take */
    rd(MSR);
    uint8_t msr = rd(MSR);
    wr(LSR, 0x1e);
    wr(MSR, 0x0f);
    SNAP("lsr_after_write", "%#x", rd(LSR));
    EXPECT_EQ(rd(MSR), msr);
}

/* Outside loopback mode a character goes out on the line, here temu's
   console, and the transmitter is ready for the next. */
TEST(uart, transmit)
{
    require_uart();
    wr(LCR, LCR_8N1);
    send("uart transmit\n");
    EXPECT_EQ(rd(LSR) & (LSR_THRE | LSR_TEMT | LSR_DR), LSR_THRE | LSR_TEMT);
}

/* IIR bits 6 and 7 are set while FCR bit 0 has the FIFOs on (data sheet,
   Interrupt Identification Register), which is how a driver tells a
   16550A from a 16450. */
TEST(uart, fifo_identification)
{
    require_uart();
    wr(FCR, FCR_ENABLE);
    EXPECT_EQ(rd(IIR) & IIR_FIFO, IIR_FIFO);
    wr(FCR, 0);
    EXPECT_EQ(rd(IIR) & IIR_FIFO, 0);
    /* the other bits are only taken together with bit 0 */
    wr(FCR, FCR_TRIG(3));
    EXPECT_EQ(rd(IIR) & IIR_FIFO, 0);
}

/* In loopback mode the transmitter's output goes to the receiver: a
   character written reaches RBR, with DR set until it is read. The
   transmitter is never left busy here. */
TEST(uart, loopback_echo)
{
    require_uart();
    wr(LCR, LCR_8N1);
    wr(MCR, MCR_LOOP);
    drain();
    wr(THR, 'A');
    EXPECT_EQ(rd(LSR) & (LSR_DR | LSR_THRE | LSR_TEMT),
              LSR_DR | LSR_THRE | LSR_TEMT);
    EXPECT_EQ(rd(RBR), 'A');
    EXPECT_EQ(rd(LSR) & LSR_DR, 0);
    wr(THR, 0x00);
    wr(THR, 0xff);
    /* no FIFO: the second overwrote the first */
    EXPECT_EQ(rd(LSR) & (LSR_DR | LSR_OE), LSR_DR | LSR_OE);
    EXPECT_EQ(rd(RBR), 0xff);
    /* with nothing received, what RBR reads is not the data sheet's */
    SNAP("rbr_empty", "%#x", rd(RBR));
}

/* In loopback mode DTR, RTS, OUT1 and OUT2 drive DSR, CTS, RI and DCD
   (data sheet, Modem Control Register). A change sets the delta bit, RI
   only on its
   trailing edge, and reading MSR clears them. */
TEST(uart, loopback_modem_lines)
{
    require_uart();
    wr(MCR, MCR_LOOP);
    rd(MSR);
    wr(MCR, MCR_LOOP | MCR_DTR);
    EXPECT_EQ(rd(MSR), MSR_DSR | MSR_DDSR);
    EXPECT_EQ(rd(MSR), MSR_DSR);
    wr(MCR, MCR_LOOP | MCR_DTR | MCR_RTS);
    EXPECT_EQ(rd(MSR), MSR_DSR | MSR_CTS | MSR_DCTS);
    wr(MCR, MCR_LOOP | MCR_OUT2);
    EXPECT_EQ(rd(MSR), MSR_DCD | MSR_DDCD | MSR_DDSR | MSR_DCTS);
    wr(MCR, MCR_LOOP | MCR_OUT1);
    /* RI rising sets no delta */
    EXPECT_EQ(rd(MSR), MSR_RI | MSR_DDCD);
    wr(MCR, MCR_LOOP);
    EXPECT_EQ(rd(MSR), MSR_TERI);
    EXPECT_EQ(rd(MSR), 0);
    /* out of loopback the lines are the inputs again */
    wr(MCR, MCR_LOOP | MCR_DTR | MCR_RTS | MCR_OUT1 | MCR_OUT2);
    rd(MSR);
    wr(MCR, 0);
    SNAP("msr_after_loopback", "%#x", rd(MSR));
}

/* The receive FIFO holds 16 characters, read back in order. */
TEST(uart, fifo_holds_16)
{
    require_uart();
    loopback_fifo(3);
    send("0123456789abcdef");
    EXPECT_EQ(rd(LSR) & (LSR_DR | LSR_OE), LSR_DR);
    for (int i = 0; i < 16; i++)
        EXPECT_EQ(rd(RBR), "0123456789abcdef"[i]);
    EXPECT_EQ(rd(LSR) & LSR_DR, 0);
}

/* A 17th character finds the FIFO full: OE is set and the character is
   lost, the FIFO keeps what it had (data sheet, Line Status Register,
   bit 1). Reading LSR
   clears OE. */
TEST(uart, fifo_overrun)
{
    require_uart();
    loopback_fifo(3);
    send("0123456789abcdefX");
    EXPECT_EQ(rd(LSR) & (LSR_DR | LSR_OE), LSR_DR | LSR_OE);
    EXPECT_EQ(rd(LSR) & LSR_OE, 0);
    int n = 0;
    while ((rd(LSR) & LSR_DR) && n < 20) {
        EXPECT_EQ(rd(RBR), "0123456789abcdef"[n]);
        n++;
    }
    EXPECT_EQ(n, 16);
}

/* FCR bit 1 empties the receive FIFO; turning the FIFOs off or on
   empties it too. */
TEST(uart, fifo_reset)
{
    require_uart();
    loopback_fifo(0);
    send("abcde");
    wr(FCR, FCR_ENABLE | FCR_RXRST);
    EXPECT_EQ(rd(LSR) & LSR_DR, 0);
    send("abc");
    wr(FCR, FCR_ENABLE | FCR_TXRST);
    EXPECT_EQ(rd(LSR) & LSR_DR, LSR_DR);
    wr(FCR, 0);
    EXPECT_EQ(rd(LSR) & LSR_DR, 0);
    wr(THR, 'z');
    wr(FCR, FCR_ENABLE);
    EXPECT_EQ(rd(LSR) & LSR_DR, 0);
}

/* With the FIFOs on, the received data interrupt is reported once the
   FIFO holds the trigger level, and goes away when it drops below (data
   sheet, Interrupt Identification and FIFO Control Registers). */
TEST(uart, trigger_levels)
{
    require_uart();
    static const int levels[4] = {1, 4, 8, 14};
    for (int t = 0; t < 4; t++) {
        loopback_fifo(t);
        wr(IER, IER_RDA);
        for (int i = 0; i < levels[t] - 1; i++)
            wr(THR, 'a' + i);
        EXPECT_EQ(rd(IIR), IIR_FIFO | IIR_NONE);
        wr(THR, '!');
        EXPECT_EQ(rd(IIR), IIR_FIFO | IIR_RDA);
        rd(RBR);
        EXPECT_EQ(rd(IIR), IIR_FIFO | IIR_NONE);
        wr(IER, 0);
        drain();
    }
}

/* Characters below the trigger level are reported by the timeout
   indication once four character times pass with nothing received or
   read (data sheet, FIFO Interrupt Mode Operation). Reading a character
   takes it
   back. */
TEST(uart, character_timeout)
{
    require_uart();
    loopback_fifo(1);
    /* about 50 ms a character of 5 bits, 1.5 stop bits and no parity */
    wr(LCR, 0x04);
    uint32_t div = sClockHz / 16 / 150;
    set_divisor(div > 0xffff ? 0xffff : div);
    wr(IER, IER_RDA);
    uint64_t start = t_now_us();
    wr(THR, 0x15);
    uint8_t first = rd(IIR);
    if (t_now_us() - start < 50000)
        EXPECT_EQ(first, IIR_FIFO | IIR_NONE);
    EXPECT_TRUE(t_wait_until([] { return rd(IIR) == (IIR_FIFO | IIR_CTI); },
                             5000000));
    EXPECT_EQ(rd(RBR), 0x15);
    EXPECT_EQ(rd(IIR), IIR_FIFO | IIR_NONE);
    /* and the trigger level outranks it */
    wr(THR, 0x01);
    wr(THR, 0x02);
    EXPECT_TRUE(t_wait_until([] { return rd(IIR) == (IIR_FIFO | IIR_CTI); },
                             5000000));
    wr(THR, 0x03);
    wr(THR, 0x04);
    EXPECT_EQ(rd(IIR), IIR_FIFO | IIR_RDA);
    wr(IER, 0);
}

/* A break starting in loopback mode reaches the receiver as a zero
   character with BI. In FIFO mode BI is reported when that character is
   at the top of the FIFO, and LSR bit 7 while any character with an error
   is in it (data sheet, Line Status Register). */
TEST(uart, loopback_break)
{
    require_uart();
    loopback_fifo(0);
    wr(THR, 'a');
    wr(LCR, LCR_8N1 | LCR_SBC);
    wr(LCR, LCR_8N1);
    EXPECT_EQ(rd(LSR) & (LSR_DR | LSR_BI | LSR_FIFOE), LSR_DR | LSR_FIFOE);
    EXPECT_EQ(rd(RBR), 'a');
    EXPECT_EQ(rd(LSR) & (LSR_DR | LSR_BI | LSR_FIFOE),
              LSR_DR | LSR_BI | LSR_FIFOE);
    /* read once, BI is gone; bit 7 stays while the character is there */
    EXPECT_EQ(rd(LSR) & (LSR_BI | LSR_FIFOE), LSR_FIFOE);
    EXPECT_EQ(rd(RBR), 0);
    EXPECT_EQ(rd(LSR) & (LSR_DR | LSR_BI | LSR_FIFOE), 0);
}

/* The interrupt identifications in priority order (data sheet,
   Interrupt Identification Register): line status, received data,
   transmitter empty, modem status. Each
   goes when its source is quieted, uncovering the next. */
TEST(uart, interrupt_priority)
{
    require_uart();
    loopback_fifo(0);
    send("0123456789abcdefX");          /* data and an overrun */
    wr(MCR, MCR_LOOP | MCR_DTR);        /* a modem status change */
    wr(IER, IER_RDA | IER_THRE | IER_RLS | IER_MS);
    EXPECT_EQ(rd(IIR), IIR_FIFO | IIR_RLS);
    rd(LSR);
    EXPECT_EQ(rd(IIR), IIR_FIFO | IIR_RDA);
    drain();
    EXPECT_EQ(rd(IIR), IIR_FIFO | IIR_THRE);
    /* reading IIR while it reports THRE took that back */
    EXPECT_EQ(rd(IIR), IIR_FIFO | IIR_MS);
    rd(MSR);
    EXPECT_EQ(rd(IIR), IIR_FIFO | IIR_NONE);
    wr(IER, 0);
}

/* The transmitter empty interrupt comes when it is enabled with the
   register empty and after each write, and goes on reading IIR or
   writing THR. */
TEST(uart, thre_interrupt)
{
    require_uart();
    wr(MCR, MCR_LOOP);
    wr(IER, IER_THRE);
    EXPECT_EQ(rd(IIR), IIR_THRE);
    EXPECT_EQ(rd(IIR), IIR_NONE);
    wr(THR, 'q');
    EXPECT_EQ(rd(IIR), IIR_THRE);
    EXPECT_EQ(rd(IIR), IIR_NONE);
    /* enabled again while empty: once more */
    wr(IER, 0);
    wr(IER, IER_THRE);
    EXPECT_EQ(rd(IIR), IIR_THRE);
    wr(IER, 0);
    drain();
}

/* The interrupt line follows IIR: received data, an overrun and a modem
   status change each raise it through the controller the tables name. */
TEST(uart, interrupt_line)
{
    require_uart();
    loopback_fifo(0);
    route_irq();
    t_on_cleanup(unroute);
    arch_irq_enable();
    wr(IER, IER_RDA | IER_RLS | IER_MS);

    wr(THR, 'i');
    EXPECT_TRUE(t_wait_until([] { return sIrqCount >= 1; }));
    EXPECT_EQ(sIrqIir[0], IIR_RDA);

    int before = sIrqCount;
    wr(MCR, MCR_LOOP | MCR_RTS);
    EXPECT_TRUE(t_wait_until([before] { return sIrqCount > before; }));
    EXPECT_EQ(sIrqIir[before], IIR_MS);

    arch_irq_disable();
    wr(IER, IER_RLS);
    before = sIrqCount;
    send("0123456789abcdefX");
    arch_irq_enable();
    EXPECT_TRUE(t_wait_until([before] { return sIrqCount > before; }));
    EXPECT_EQ(sIrqIir[before], IIR_RLS);
    arch_irq_disable();
    wr(IER, 0);
    /* and nothing more once every source is quiet */
    before = sIrqCount;
    arch_irq_enable();
    t_delay_us(10000);
    arch_irq_disable();
    EXPECT_EQ(sIrqCount, before);
}

/* Characters typed on temu's console arrive in the FIFO, and in the
   holding register one at a time while the FIFOs are off. Only on a
   machine where the 16550 takes the console's input. */
TEST(uart, host_input)
{
    require_uart();
    if (virtio_console_present())
        t_skip("a virtio console takes the console's input");
    wr(LCR, LCR_8N1);
    wr(FCR, FCR_ENABLE | FCR_RXRST | FCR_TRIG(3));
    drain();
    t_host_input("hello", 5);
    char got[8] = {};
    int n = 0;
    t_wait_until([&] {
        while (n < 5 && (rd(LSR) & LSR_DR))
            got[n++] = rd(RBR);
        return n == 5;
    }, 5000000);
    EXPECT_EQ(n, 5);
    EXPECT_EQ(memcmp(got, "hello", 5), 0);

    wr(FCR, 0);
    t_host_input("xyz", 3);
    n = 0;
    t_wait_until([&] {
        while (n < 3 && (rd(LSR) & LSR_DR)) {
            EXPECT_EQ(rd(LSR) & LSR_OE, 0);
            got[n++] = rd(RBR);
        }
        return n == 3;
    }, 5000000);
    EXPECT_EQ(n, 3);
    EXPECT_EQ(memcmp(got, "xyz", 3), 0);
}
