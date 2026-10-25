/*
 * CFI flash tests
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
#include "format.h"
#include "mmio.h"
#include "test.h"
#include "test_control.h"
#include "testctl.h"

/* NOR flash with the Intel command set, as the Common Flash Interface
   (JESD68.01) describes its query and the Intel StrataFlash 3 V datasheet
   its commands and status register: two 16 bit chips on a 32 bit bus,
   each seeing its own half of every word. The array lasts across tests,
   so each test keeps to blocks of its own. */

#define CMD_PROGRAM      0x40
#define CMD_ERASE        0x20
#define CMD_CLEAR_STATUS 0x50
#define CMD_LOCK_SETUP   0x60
#define CMD_READ_STATUS  0x70
#define CMD_READ_ID      0x90
#define CMD_READ_QUERY   0x98
#define CMD_CONFIRM      0xd0
#define CMD_BUFFERED     0xe8
#define CMD_READ_ARRAY   0xff
#define CMD_LOCK         0x01

#define SR_READY         0x80
#define SR_ERASE_ERROR   0x20
#define SR_PROGRAM_ERROR 0x10
#define SR_LOCKED        0x02

namespace {

struct Flash {
    uint64_t base;
    uint64_t size;
    uint32_t block;  /* the erase block on the bus, both chips together */

    void Cmd(uint64_t off, uint8_t cmd) const
        {mmio_write32(base + off, cmd | (uint32_t)cmd << 16);}
    void W(uint64_t off, uint32_t v) const {mmio_write32(base + off, v);}
    uint32_t R(uint64_t off) const {return mmio_read32(base + off);}
    /* the status, both chips' copies */
    uint32_t Status() const
    {
        Cmd(0, CMD_READ_STATUS);
        return R(0);
    }
    uint8_t Query(int index) const {return R(index * 4) & 0xff;}
};

uint32_t both(uint8_t v) {return v | (uint32_t)v << 16;}

void cleanup()
{
    for (int i = 0;; i++) {
        const HwDevice *d = registry_find("cfi-flash", i);
        if (d == nullptr || hw_region(d, HW_MMIO) == nullptr)
            break;
        uint64_t base = hw_region(d, HW_MMIO)->base;
        mmio_write32(base, both(CMD_CLEAR_STATUS));
        mmio_write32(base, both(CMD_READ_ARRAY));
    }
}

/* The 'index'th flash, its geometry from its query; false without one. */
bool find(int index, Flash *f)
{
    const HwDevice *d = registry_find("cfi-flash", index);
    if (d == nullptr || hw_region(d, HW_MMIO) == nullptr)
        return false;
    t_on_cleanup(cleanup);
    f->base = hw_region(d, HW_MMIO)->base;
    f->size = hw_region(d, HW_MMIO)->size;
    f->Cmd(0, CMD_READ_QUERY);
    f->block = (f->Query(0x2f) << 8 | f->Query(0x30) << 16) * 2;
    f->Cmd(0, CMD_READ_ARRAY);
    return true;
}

/* The flash that is writable or the one that is not, or skip. */
Flash need(bool writable)
{
    Flash f;
    for (int i = 0; find(i, &f); i++) {
        f.Cmd(0, CMD_READ_ID);
        bool locked = f.R(2 * 4) & 1;
        f.Cmd(0, CMD_READ_ARRAY);
        /* a write protected flash reports every block locked */
        if (locked != writable)
            return f;
    }
    t_skip(writable ? "no writable flash" : "no write protected flash");
}

bool is_pattern(const Flash &f, uint64_t sector)
{
    for (uint32_t i = 0; i < 512; i += 4) {
        uint32_t v = f.R(sector * 512 + i);
        for (int k = 0; k < 4; k++) {
            if ((uint8_t)(v >> 8 * k) != TEST_BLOCK_PATTERN(sector, i + k))
                return false;
        }
    }
    return true;
}

}


TEST(cfi_flash, query)
{
    /* the query structure, at word 10h on of each chip (JESD68.01, 4.3) */
    Flash f;
    if (!find(0, &f))
        t_skip("no CFI flash");
    for (int i = 0; find(i, &f); i++) {
        f.Cmd(0x55 * 4, CMD_READ_QUERY);
        EXPECT_EQ(f.R(0x10 * 4), both('Q'));
        EXPECT_EQ(f.R(0x11 * 4), both('R'));
        EXPECT_EQ(f.R(0x12 * 4), both('Y'));
        /* the Intel command set, with its extended table at 31h */
        EXPECT_EQ(f.Query(0x13) | f.Query(0x14) << 8, 0x0001);
        EXPECT_EQ(f.Query(0x15) | f.Query(0x16) << 8, 0x0031);
        EXPECT_EQ(f.R(0x31 * 4), both('P'));
        EXPECT_EQ(f.R(0x32 * 4), both('R'));
        EXPECT_EQ(f.R(0x33 * 4), both('I'));
        /* the geometry adds up to the window the tables give */
        uint32_t chip_size = 1u << f.Query(0x27);
        EXPECT_EQ(2ull * chip_size, f.size);
        EXPECT_EQ(f.Query(0x2c), 1);
        uint32_t blocks = (f.Query(0x2d) | f.Query(0x2e) << 8) + 1;
        EXPECT_EQ((uint64_t)blocks * f.block, f.size);
        EXPECT_EQ(f.size % f.block, 0u);
        char line[0x50 * 3 + 1];
        for (int q = 0; q < 0x50; q++)
            snprintf(line + q * 3, 4, "%02x ", f.Query(q));
        SNAP("query", "%s", line);
        /* the upper chip on the upper half, by 16 bit reads */
        EXPECT_EQ(mmio_read16(f.base + 0x10 * 4 + 2), 'Q');
        EXPECT_EQ(mmio_read8(f.base + 0x10 * 4 + 2), 'Q');
        /* the status register in every location */
        f.Cmd(0, CMD_READ_STATUS);
        EXPECT_EQ(f.R(0), both(SR_READY));
        EXPECT_EQ(f.R(0x1234 * 4), both(SR_READY));
        f.Cmd(0, CMD_READ_ARRAY);
        EXPECT_TRUE(is_pattern(f, 0));
    }
}


TEST(cfi_flash, read_id)
{
    /* manufacturer, device, and each block's lock state at its third
       word */
    Flash f = need(true);
    f.Cmd(0, CMD_READ_ID);
    EXPECT_EQ(f.R(0), both(0x89));
    SNAP("device", "%08x", f.R(4));
    for (uint64_t b = 0; b < f.size; b += f.block)
        EXPECT_EQ(f.R(b + 8), 0u);
    f.Cmd(0, CMD_READ_ARRAY);
}


TEST(cfi_flash, read_array)
{
    /* the image, read in place; past its end the array starts out erased
       (the write protected flash, which no test changes) */
    Flash f = need(false);
    uint64_t sectors = 0;
    while (sectors * 512 < f.size && is_pattern(f, sectors))
        sectors++;
    ASSERT_GE(tc_block_find(sectors), 0);
    EXPECT_LT(sectors * 512, f.size);
    bool erased = true;
    for (uint64_t a = sectors * 512; a < f.size; a += 4)
        erased = erased && f.R(a) == 0xffffffff;
    EXPECT_TRUE(erased);
    EXPECT_EQ(mmio_read8(f.base + 5), TEST_BLOCK_PATTERN(0, 5));
    EXPECT_EQ(mmio_read16(f.base + 6),
              TEST_BLOCK_PATTERN(0, 6) | TEST_BLOCK_PATTERN(0, 7) << 8);
}


TEST(cfi_flash, program)
{
    /* a word program only clears bits, and the device is left reading
       its status (block 1) */
    Flash f = need(true);
    ASSERT_GE(tc_block_find(f.size / 512), 0);
    uint64_t a = f.block + 0x100;
    uint32_t old = f.R(a);
    uint32_t writes = tc_block_read(TEST_CONTROL_BLOCK_WRITES);
    f.Cmd(a, CMD_PROGRAM);
    EXPECT_EQ(f.R(a), both(SR_READY));
    f.W(a, 0x0f0f00ff);
    EXPECT_EQ(f.R(a), both(SR_READY));
    EXPECT_EQ(f.R(0), both(SR_READY));
    f.Cmd(0, CMD_READ_ARRAY);
    EXPECT_EQ(f.R(a), old & 0x0f0f00ff);
    /* setting bits again takes an erase */
    f.Cmd(a, CMD_PROGRAM);
    f.W(a, 0xffffffff);
    f.Cmd(0, CMD_READ_ARRAY);
    EXPECT_EQ(f.R(a), old & 0x0f0f00ff);
    /* a byte program */
    f.Cmd(a, CMD_PROGRAM);
    mmio_write8(f.base + a + 4, 0);
    f.Cmd(0, CMD_READ_ARRAY);
    EXPECT_EQ(mmio_read8(f.base + a + 4), 0);
    /* what changed went back to the image */
    EXPECT_TRUE(t_wait_until([&] {
        return tc_block_read(TEST_CONTROL_BLOCK_WRITES) > writes;
    }));
}


TEST(cfi_flash, erase)
{
    /* a block erase sets the whole block to ones and nothing else; a
       wrong confirm is a command sequence error, until Clear Status
       (block 2) */
    Flash f = need(true);
    uint64_t b = 2 * (uint64_t)f.block;
    uint32_t before = f.R(b - 4), after = f.R(b + f.block);
    f.Cmd(b + 0x40, CMD_ERASE);
    f.Cmd(b + 0x40, CMD_CONFIRM);
    EXPECT_EQ(f.Status(), both(SR_READY));
    f.Cmd(0, CMD_READ_ARRAY);
    bool erased = true;
    for (uint64_t a = b; a < b + f.block; a += 4)
        erased = erased && f.R(a) == 0xffffffff;
    EXPECT_TRUE(erased);
    EXPECT_EQ(f.R(b - 4), before);
    EXPECT_EQ(f.R(b + f.block), after);

    f.Cmd(b, CMD_ERASE);
    f.Cmd(b, CMD_READ_ARRAY);
    EXPECT_EQ(f.R(b), both(SR_READY | SR_ERASE_ERROR | SR_PROGRAM_ERROR));
    /* the error stays through other commands */
    f.Cmd(0, CMD_READ_ARRAY);
    EXPECT_EQ(f.Status(), both(SR_READY | SR_ERASE_ERROR |
                               SR_PROGRAM_ERROR));
    f.Cmd(0, CMD_CLEAR_STATUS);
    EXPECT_EQ(f.R(0), both(SR_READY));
    f.Cmd(0, CMD_READ_ARRAY);
}


TEST(cfi_flash, buffered_program)
{
    /* Write to Buffer: the count is words per chip less one, then the
       data, then the confirm (block 3) */
    Flash f = need(true);
    uint64_t b = 3 * (uint64_t)f.block;
    f.Cmd(b, CMD_ERASE);
    f.Cmd(b, CMD_CONFIRM);
    f.Cmd(b, CMD_BUFFERED);
    EXPECT_EQ(f.R(b), both(SR_READY)); /* the buffer is free */
    f.W(b, both(7));
    for (int i = 0; i < 8; i++)
        f.W(b + i * 4, 0x01010101u * (i + 1));
    f.Cmd(b, CMD_CONFIRM);
    EXPECT_EQ(f.Status(), both(SR_READY));
    f.Cmd(0, CMD_READ_ARRAY);
    for (int i = 0; i < 8; i++)
        EXPECT_EQ(f.R(b + i * 4), 0x01010101u * (i + 1));
    EXPECT_EQ(f.R(b + 32), 0xffffffffu);

    /* more than the buffer holds */
    f.Cmd(b + 0x100, CMD_BUFFERED);
    f.W(b + 0x100, both(32));
    EXPECT_EQ(f.Status(), both(SR_READY | SR_ERASE_ERROR |
                               SR_PROGRAM_ERROR));
    f.Cmd(0, CMD_CLEAR_STATUS);
    /* data outside the buffer's window */
    f.Cmd(b + 0x100, CMD_BUFFERED);
    f.W(b + 0x100, both(0));
    f.W(b + 0x400, 0);
    f.Cmd(b + 0x100, CMD_CONFIRM);
    EXPECT_EQ(f.Status(), both(SR_READY | SR_ERASE_ERROR |
                               SR_PROGRAM_ERROR));
    f.Cmd(0, CMD_CLEAR_STATUS);
    f.Cmd(0, CMD_READ_ARRAY);
    EXPECT_EQ(f.R(b + 0x400), 0xffffffffu);
}


TEST(cfi_flash, lock)
{
    /* a locked block refuses program and erase with the lock bit in the
       status, and its identifier word says so (block 1) */
    Flash f = need(true);
    uint64_t b = f.block;
    f.Cmd(b, CMD_LOCK_SETUP);
    f.Cmd(b, CMD_LOCK);
    f.Cmd(0, CMD_READ_ID);
    EXPECT_EQ(f.R(b + 8), both(1));
    EXPECT_EQ(f.R(8), 0u);
    f.Cmd(0, CMD_READ_ARRAY);
    uint32_t old = f.R(b + 0x200);
    f.Cmd(b + 0x200, CMD_PROGRAM);
    f.W(b + 0x200, 0);
    EXPECT_EQ(f.R(0), both(SR_READY | SR_PROGRAM_ERROR | SR_LOCKED));
    f.Cmd(0, CMD_CLEAR_STATUS);
    f.Cmd(b, CMD_ERASE);
    f.Cmd(b, CMD_CONFIRM);
    EXPECT_EQ(f.R(0), both(SR_READY | SR_ERASE_ERROR | SR_LOCKED));
    f.Cmd(0, CMD_CLEAR_STATUS);
    f.Cmd(0, CMD_READ_ARRAY);
    EXPECT_EQ(f.R(b + 0x200), old);

    f.Cmd(b, CMD_LOCK_SETUP);
    f.Cmd(b, CMD_CONFIRM);
    f.Cmd(0, CMD_READ_ID);
    EXPECT_EQ(f.R(b + 8), 0u);
    f.Cmd(b + 0x200, CMD_PROGRAM);
    f.W(b + 0x200, 0);
    EXPECT_EQ(f.Status(), both(SR_READY));
    f.Cmd(0, CMD_READ_ARRAY);
    EXPECT_EQ(f.R(b + 0x200), 0u);
}


TEST(cfi_flash, write_protected)
{
    /* every block reads locked and refuses program and erase */
    Flash f = need(false);
    f.Cmd(0, CMD_READ_ID);
    for (uint64_t b = 0; b < f.size; b += f.block)
        EXPECT_EQ(f.R(b + 8), both(1));
    f.Cmd(0, CMD_READ_ARRAY);
    uint32_t old = f.R(0x10);
    f.Cmd(0x10, CMD_PROGRAM);
    f.W(0x10, 0);
    EXPECT_EQ(f.R(0), both(SR_READY | SR_PROGRAM_ERROR | SR_LOCKED));
    f.Cmd(0, CMD_CLEAR_STATUS);
    f.Cmd(0, CMD_ERASE);
    f.Cmd(0, CMD_CONFIRM);
    EXPECT_EQ(f.R(0), both(SR_READY | SR_ERASE_ERROR | SR_LOCKED));
    f.Cmd(0, CMD_CLEAR_STATUS);
    f.Cmd(0, CMD_READ_ARRAY);
    EXPECT_EQ(f.R(0x10), old);
    EXPECT_TRUE(is_pattern(f, 0));
}
