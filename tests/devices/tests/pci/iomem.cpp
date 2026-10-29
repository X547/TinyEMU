/*
 * Memory map tests: how accesses are fitted to the widths a range takes
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
#include "fixture.h"
#include "mmio.h"

/* No hardware specification: this is temu's own contract between the memory
   map and a device, in iomem.h. The probe windows of the "pci-test" fixture
   log each access just as the device is handed it. */

namespace {

#if defined(__i386__) || defined(__x86_64__)
#define IOMEM_X86 1
/* x86 loads and stores at any address with one instruction */
#define ANY_ALIGN __attribute__((aligned(1)))
#else
#define ANY_ALIGN
#endif
#if UINTPTR_MAX > 0xffffffffu
#define IOMEM_64 1
#endif

typedef uint16_t ANY_ALIGN u16_any;
typedef uint32_t ANY_ALIGN u32_any;
typedef uint64_t ANY_ALIGN u64_any;

#define R false
#define W true

struct Access {
    uint32_t offset;
    int size_log2;
    bool write;
    uint64_t val;
};

/* One load or store of the width. */
uint64_t load(uint64_t a, int size_log2)
{
    MMIO_BARRIER();
    switch (size_log2) {
    case 0: return *(volatile uint8_t *)(uintptr_t)a;
    case 1: return *(volatile u16_any *)(uintptr_t)a;
    case 2: return *(volatile u32_any *)(uintptr_t)a;
    default: return *(volatile u64_any *)(uintptr_t)a;
    }
}

void store(uint64_t a, uint64_t v, int size_log2)
{
    switch (size_log2) {
    case 0: *(volatile uint8_t *)(uintptr_t)a = v; break;
    case 1: *(volatile u16_any *)(uintptr_t)a = v; break;
    case 2: *(volatile u32_any *)(uintptr_t)a = v; break;
    default: *(volatile u64_any *)(uintptr_t)a = v; break;
    }
    MMIO_BARRIER();
}

/* What the window holds at 'offset' before anything is written. */
uint64_t pattern(uint32_t offset, int size_log2)
{
    uint64_t v = 0;
    for (int i = 0; i < (1 << size_log2); i++)
        v |= (uint64_t)TEST_PCI_PROBE_BYTE(offset + i) << (8 * i);
    return v;
}

struct Probe {
    const PciFunc *f;
    uint64_t base;

    uint64_t Load(uint32_t offset, int size_log2) const
        {return load(base + offset, size_log2);}
    void Store(uint32_t offset, uint64_t v, int size_log2) const
        {store(base + offset, v, size_log2);}
    /* Clears the log and refills the window. */
    void Reset() const {pt_write(f, TEST_PCI_PROBE_COUNT, 0);}
    uint32_t Count() const {return pt_read(f, TEST_PCI_PROBE_COUNT);}

    /* The log holds just 'want', in order; then it is reset. */
    template <int N>
    void Expect(const char *what, const Access (&want)[N]) const
    {
        uint32_t count = Count();
        if (count != (uint32_t)N)
            t_log("%s: %u accesses", what, count);
        EXPECT_EQ(count, (uint32_t)N);
        for (int i = 0; i < N && i < (int)count; i++) {
            pt_write(f, TEST_PCI_PROBE_SELECT, i);
            uint32_t access = pt_read(f, TEST_PCI_PROBE_ACCESS);
            uint64_t val = (uint64_t)pt_read(f, TEST_PCI_PROBE_VAL_HI) << 32 |
                pt_read(f, TEST_PCI_PROBE_VAL_LO);
            uint32_t expect = want[i].offset |
                want[i].size_log2 << TEST_PCI_PROBE_SIZE_SHIFT |
                (want[i].write ? TEST_PCI_PROBE_WRITE : 0);
            if (access != expect || val != want[i].val)
                t_log("%s: access %d is %#x %#llx", what, i, access,
                      (unsigned long long)val);
            EXPECT_EQ(access, expect);
            EXPECT_EQ(val, want[i].val);
        }
        Reset();
    }

    /* Nothing reached the device. */
    void ExpectNone(const char *what) const
    {
        uint32_t count = Count();
        if (count != 0)
            t_log("%s: %u accesses", what, count);
        EXPECT_EQ(count, 0u);
        Reset();
    }
};

/* The test function whose window is registered as 'info' says, or skip. */
Probe need_probe(uint32_t info)
{
    for (int i = 0; PciFunc *f = pt_get(i); i++) {
        if (pt_read(f, TEST_PCI_PROBE_INFO) != info)
            continue;
        uint64_t base = pci_bar_cpu(f, TEST_PCI_BAR_PROBE);
        if (base == 0)
            continue;
        t_on_cleanup(pt_cleanup);
        Probe p = {f, base};
        p.Reset();
        return p;
    }
    t_skip("no probe window registered as %#x", info);
}

#define WIDTHS_1    0x1
#define WIDTHS_2    0x2
#define WIDTHS_4    0x4
#define WIDTHS_8    0x8
#define WIDTHS_ALL  0xf

#ifdef IOMEM_64
const int kMaxSize = 3;
#else
const int kMaxSize = 2;
#endif

}


/* An access of a width the range takes reaches the device as it is. */
TEST(iomem, passes_on)
{
    Probe p = need_probe(WIDTHS_ALL);
    const uint64_t v = 0x8877665544332211ull;
    for (int s = 0; s <= kMaxSize; s++) {
        uint32_t at = 0x40;
        uint64_t mask = s == 3 ? ~0ull : (1ull << (8 << s)) - 1;
        EXPECT_EQ(p.Load(at, s), pattern(at, s));
        p.Expect("read", {Access{at, s, R, pattern(at, s)}});
        p.Store(at, v, s);
        p.Expect("write", {Access{at, s, W, v & mask}});
    }
}


/* A narrower read is taken from the register holding it. */
TEST(iomem, narrow_read)
{
    Probe p = need_probe(WIDTHS_4);
    EXPECT_EQ(p.Load(0x43, 0), pattern(0x43, 0));
    p.Expect("read8", {Access{0x40, 2, R, pattern(0x40, 2)}});
    EXPECT_EQ(p.Load(0x42, 1), pattern(0x42, 1));
    p.Expect("read16", {Access{0x40, 2, R, pattern(0x40, 2)}});
}


TEST(iomem, narrow_read_64)
{
    Probe p = need_probe(WIDTHS_8 | TEST_PCI_PROBE_MERGE);
    EXPECT_EQ(p.Load(0x44, 2), pattern(0x44, 2));
    p.Expect("read32", {Access{0x40, 3, R, pattern(0x40, 3)}});
    EXPECT_EQ(p.Load(0x47, 0), pattern(0x47, 0));
    p.Expect("read8", {Access{0x40, 3, R, pattern(0x40, 3)}});
}


/* A narrower write is dropped, unless the range merges it. */
TEST(iomem, narrow_write_dropped)
{
    Probe p = need_probe(WIDTHS_4);
    p.Store(0x41, 0xab, 0);
    p.ExpectNone("write8");
    p.Store(0x42, 0xabcd, 1);
    p.ExpectNone("write16");
}


TEST(iomem, narrow_write_merged)
{
    Probe p = need_probe(WIDTHS_4 | TEST_PCI_PROBE_MERGE);
    uint64_t old = pattern(0x40, 2);
    p.Store(0x41, 0xab, 0);
    p.Expect("write8", {Access{0x40, 2, R, old},
                        Access{0x40, 2, W, (old & ~0xff00ull) | 0xab00}});
    p.Store(0x42, 0xabcd, 1);
    p.Expect("write16", {Access{0x40, 2, R, old},
                         Access{0x40, 2, W, (old & 0xffff) | 0xabcd0000}});
}


TEST(iomem, narrow_write_merged_64)
{
    Probe p = need_probe(WIDTHS_8 | TEST_PCI_PROBE_MERGE);
    uint64_t old = pattern(0x40, 3);
    p.Store(0x44, 0xdeadbeef, 2);
    p.Expect("write32",
             {Access{0x40, 3, R, old},
              Access{0x40, 3, W, (old & 0xffffffffull) |
                                 0xdeadbeefull << 32}});
    p.Store(0x40, 0x12345678, 2);
    p.Expect("write32 low",
             {Access{0x40, 3, R, old},
              Access{0x40, 3, W, (old & ~0xffffffffull) | 0x12345678}});
}


/* A wider access is split into halves, the lower address first. */
TEST(iomem, wide_split)
{
    Probe p = need_probe(WIDTHS_1);
    EXPECT_EQ(p.Load(0x40, 2), pattern(0x40, 2));
    p.Expect("read32", {Access{0x40, 0, R, pattern(0x40, 0)},
                        Access{0x41, 0, R, pattern(0x41, 0)},
                        Access{0x42, 0, R, pattern(0x42, 0)},
                        Access{0x43, 0, R, pattern(0x43, 0)}});
    p.Store(0x42, 0xabcd, 1);
    p.Expect("write16", {Access{0x42, 0, W, 0xcd}, Access{0x43, 0, W, 0xab}});
}


#ifdef IOMEM_64
TEST(iomem, wide_split_64)
{
    Probe p = need_probe(WIDTHS_4);
    EXPECT_EQ(p.Load(0x40, 3), pattern(0x40, 3));
    p.Expect("read64", {Access{0x40, 2, R, pattern(0x40, 2)},
                        Access{0x44, 2, R, pattern(0x44, 2)}});
    p.Store(0x40, 0x8877665544332211ull, 3);
    p.Expect("write64", {Access{0x40, 2, W, 0x44332211},
                         Access{0x44, 2, W, 0x88776655}});
}
#endif


#ifdef IOMEM_X86
/* An unaligned access is split into halves, which are then fitted in
   turn. RISC-V splits misaligned accesses into bytes itself. */
TEST(iomem, unaligned)
{
    Probe p = need_probe(WIDTHS_ALL);
    EXPECT_EQ(p.Load(0x42, 2), pattern(0x42, 2));
    p.Expect("read32 at 2", {Access{0x42, 1, R, pattern(0x42, 1)},
                             Access{0x44, 1, R, pattern(0x44, 1)}});
    EXPECT_EQ(p.Load(0x41, 1), pattern(0x41, 1));
    p.Expect("read16 at 1", {Access{0x41, 0, R, pattern(0x41, 0)},
                             Access{0x42, 0, R, pattern(0x42, 0)}});
    p.Store(0x43, 0x44332211, 2);
    p.Expect("write32 at 3", {Access{0x43, 0, W, 0x11},
                              Access{0x44, 0, W, 0x22},
                              Access{0x45, 0, W, 0x33},
                              Access{0x46, 0, W, 0x44}});
}


TEST(iomem, unaligned_narrow)
{
    Probe p = need_probe(WIDTHS_4);
    EXPECT_EQ(p.Load(0x42, 2), pattern(0x42, 2));
    p.Expect("read32 at 2", {Access{0x40, 2, R, pattern(0x40, 2)},
                             Access{0x44, 2, R, pattern(0x44, 2)}});
    p.Store(0x42, 0x44332211, 2);
    p.ExpectNone("write32 at 2");
}


/* With DEVIO_UNALIGNED a width taken passes on at any offset. */
TEST(iomem, unaligned_taken)
{
    Probe p = need_probe(WIDTHS_2 | TEST_PCI_PROBE_UNALIGNED);
    EXPECT_EQ(p.Load(0x41, 1), pattern(0x41, 1));
    p.Expect("read16 at 1", {Access{0x41, 1, R, pattern(0x41, 1)}});
    p.Store(0x43, 0xabcd, 1);
    p.Expect("write16 at 3", {Access{0x43, 1, W, 0xabcd}});
}
#endif
