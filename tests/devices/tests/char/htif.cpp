/*
 * HTIF tests
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
#include "mmio.h"
#include "test.h"

/* The Berkeley host-target interface has no written specification; the
   reference is the front end of riscv-isa-sim (fesvr/htif.cc and
   device.cc): a command in tohost is device << 56 | command << 48 |
   payload, taken by the host clearing tohost, and answered, if at all, in
   fromhost. The "ucb,htif0" node lists fromhost, then tohost. Device 0
   command 0 with bit 0 set is the exit of riscv-tests, with the status in
   the bits above; device 1 is the console, command 1 a character written.
   Device 2, the calendar time, is temu's own. */

#define HTIF_CMD(dev, cmd, payload) \
    ((uint64_t)(dev) << 56 | (uint64_t)(cmd) << 48 | (payload))

namespace {

uint64_t sToHost;
uint64_t sFromHost;

void require_htif()
{
    const HwDevice *d = t_require_device("ucb,htif0");
    if (d == console_device())
        t_skip("HTIF carries the report");
    ASSERT_GE(d->nregions, 2);
    sFromHost = d->regions[0].base;
    sToHost = d->regions[1].base;
}

uint64_t rd64(uint64_t a)
{
    uint32_t lo = mmio_read32(a);
    return (uint64_t)mmio_read32(a + 4) << 32 | lo;
}

/* The low half first: writing the high half issues the command. */
void wr64(uint64_t a, uint64_t v)
{
    mmio_write32(a, (uint32_t)v);
    mmio_write32(a + 4, v >> 32);
}

void clear_fromhost()
{
    wr64(sFromHost, 0);
}

}


FRESH_TEST(htif, reset_state)
{
    require_htif();
    EXPECT_EQ(rd64(sToHost), 0u);
    EXPECT_EQ(rd64(sFromHost), 0u);
}

/* A character for the console is taken, and answered with the device
   and command and no payload, as the console device of the front end
   does. */
TEST(htif, console_write)
{
    require_htif();
    t_on_cleanup(clear_fromhost);
    wr64(sToHost, HTIF_CMD(1, 1, '.'));
    EXPECT_EQ(rd64(sToHost), 0u);
    EXPECT_EQ(rd64(sFromHost), HTIF_CMD(1, 1, 0));
}

/* A console read is taken; its answer would come with a key, which
   nothing types here. */
TEST(htif, console_read)
{
    require_htif();
    clear_fromhost();
    wr64(sToHost, HTIF_CMD(1, 0, 0));
    EXPECT_EQ(rd64(sToHost), 0u);
    EXPECT_EQ(rd64(sFromHost), 0u);
}

/* temu's own device 2 answers with the time in microseconds since 1970:
   at least the start of 2026. */
TEST(htif, calendar_time)
{
    require_htif();
    t_on_cleanup(clear_fromhost);
    wr64(sToHost, HTIF_CMD(2, 0, 0));
    EXPECT_EQ(rd64(sToHost), 0u);
    EXPECT_GE(rd64(sFromHost), 1767225600ull * 1000000);
}

/* The front end takes a command for a device it does not have, and
   leaves fromhost alone. */
TEST(htif, unknown_device)
{
    require_htif();
    clear_fromhost();
    wr64(sToHost, HTIF_CMD(9, 3, 0x1234));
    EXPECT_EQ(rd64(sToHost), 0u);
    EXPECT_EQ(rd64(sFromHost), 0u);
    /* and device 0 without bit 0, a system call, which there is no proxy
       for */
    wr64(sToHost, HTIF_CMD(0, 0, 0x1000));
    EXPECT_EQ(rd64(sToHost), 0u);
    EXPECT_EQ(rd64(sFromHost), 0u);
}

/* The low half alone issues nothing: it reads back until the high half
   is written. fromhost is the guest's to clear. */
TEST(htif, register_halves)
{
    require_htif();
    t_on_cleanup(clear_fromhost);
    mmio_write32(sToHost, 0x12345678);
    EXPECT_EQ(mmio_read32(sToHost), 0x12345678u);
    EXPECT_EQ(mmio_read32(sToHost + 4), 0u);
    mmio_write32(sToHost + 4, 0x09000000);
    EXPECT_EQ(rd64(sToHost), 0u);
    wr64(sFromHost, 0x0123456789abcdefull);
    EXPECT_EQ(rd64(sFromHost), 0x0123456789abcdefull);
}

#if defined(__riscv) && __riscv_xlen == 64
/* A 64 bit store issues the command as the two halves would, and a 64
   bit load reads both. */
TEST(htif, access_64bit)
{
    require_htif();
    t_on_cleanup(clear_fromhost);
    *(volatile uint64_t *)(uintptr_t)sToHost = HTIF_CMD(1, 1, '.');
    EXPECT_EQ(*(volatile uint64_t *)(uintptr_t)sToHost, 0u);
    EXPECT_EQ(*(volatile uint64_t *)(uintptr_t)sFromHost, HTIF_CMD(1, 1, 0));
}
#endif

/* The exit of riscv-tests: status << 1 | 1, so 1 alone passes. */
TEST(htif, exit_pass)
{
    require_htif();
    t_expect_exit(0);
    wr64(sToHost, HTIF_CMD(0, 0, 1));
    arch_halt();
}

TEST(htif, exit_status)
{
    require_htif();
    t_expect_exit(5);
    wr64(sToHost, HTIF_CMD(0, 0, 5 << 1 | 1));
    arch_halt();
}

/* A status that does not fit the host's 8 bits is a failure, never 0. */
TEST(htif, exit_status_too_large)
{
    require_htif();
    t_expect_exit(255);
    wr64(sToHost, HTIF_CMD(0, 0, 256 << 1 | 1));
    arch_halt();
}
