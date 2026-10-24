/*
 * PCI tests: configuration space, BARs, bridges and INTx
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
#include "format.h"
#include "heap.h"

/* By the PCI Local Bus specification 3.0, the PCI-to-PCI Bridge
   Architecture specification 1.2 and the PCI Express Base specification;
   the test functions are temu's "pci-test" fixture. */

namespace {

void need_hosts()
{
    if (pci_host_count() == 0)
        t_skip("no PCI host bridge");
}

/* The first bridge, or skip. */
PciFunc *need_bridge()
{
    for (int i = 0; PciFunc *f = pci_func(i); i++) {
        if (f->bridge)
            return f;
    }
    t_skip("no PCI bridge");
}

/* A test function behind a bridge, or skip. */
PciFunc *need_bridged(int index = 0)
{
    for (int i = 0; PciFunc *f = pt_get(i); i++) {
        if (f->parent && index-- == 0) {
            t_on_cleanup(pt_cleanup);
            return f;
        }
    }
    t_skip("no test function behind a bridge");
}

void key(char *buf, size_t size, const char *what, const PciFunc *f)
{
    char name[24];
    pci_name(f, name, sizeof(name));
    snprintf(buf, size, "%s%s", what, name);
}

/* Whether the test function's registers answer at a PCI address. */
bool alive_at(const PciFunc *f, bool io, uint64_t addr)
{
    uint32_t v;
    return pci_space_probe(f->host, io, addr + TEST_PCI_ID, 2, &v) &&
           v == TEST_PCI_MAGIC;
}

bool alive(const PciFunc *f, int bar)
{
    return alive_at(f, f->bars[bar].io, f->bars[bar].addr);
}

/* What a read finds where nothing may answer: a value, or "fault". */
void snap_peek(const char *k, const PciHost *h, bool io, uint64_t addr)
{
    uint32_t v;
    if (pci_space_probe(h, io, addr, 2, &v))
        SNAP(k, "%08x", v);
    else
        SNAP(k, "fault");
}

volatile int sIrqs;
const PciFunc *sIrqFunc;

void intx_handler(void *ctx)
{
    const PciFunc *f = (const PciFunc *)ctx;
    sIrqs = sIrqs + 1;
    pt_write(f, TEST_PCI_INTX, 0);
}

}


/* What the root bus functions read before anything programmed them. */
FRESH_TEST(pci, reset_state)
{
    need_hosts();
    for (int i = 0; i < pci_host_count(); i++) {
        const PciHost *h = pci_host(i);
        for (uint32_t devfn = 0; devfn < 256; devfn++) {
            if (h->Read(h->bus_first, devfn, PCI_VENDOR_ID, 1) == 0xffff)
                continue;
            char k[32];
            uint32_t r[16];
            for (int w = 0; w < 16; w++)
                r[w] = h->Read(h->bus_first, devfn, 4 * w, 2);
            snprintf(k, sizeof(k), "%d:%02x.%x", i, PCI_SLOT(devfn),
                     PCI_FN(devfn));
            SNAP(k, "%08x %08x %08x %08x %08x %08x %08x %08x "
                 "%08x %08x %08x %08x %08x %08x %08x %08x",
                 r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9],
                 r[10], r[11], r[12], r[13], r[14], r[15]);
        }
    }
}

TEST(pci, hosts)
{
    need_hosts();
    for (int i = 0; i < pci_host_count(); i++) {
        const PciHost *h = pci_host(i);
        char k[16];
        snprintf(k, sizeof(k), "host%d", i);
        SNAP(k, "%s buses %u-%u io %s mem %s mem64 %s%s msi %s", h->Kind(),
             h->bus_first, h->bus_last, h->io.size ? "yes" : "no",
             h->mem.size ? "yes" : "no", h->mem64.size ? "yes" : "no",
             h->mem64.prefetch ? " (prefetchable)" : "",
             h->msi ? h->msi->Kind() : "none");
    }
}

/* What enumeration found, depth first. */
TEST(pci, topology)
{
    need_hosts();
    for (int i = 0; PciFunc *f = pci_func(i); i++) {
        char k[32], extra[48] = "";
        key(k, sizeof(k), "fn", f);
        if (f->bridge)
            snprintf(extra, sizeof(extra), " buses %u-%u", f->secondary,
                     f->subordinate);
        SNAP(k, "%04x:%04x class %06x rev %02x header %02x pin %u%s",
             f->vendor, f->device, f->class_rev >> 8, f->class_rev & 0xff,
             f->header, f->pin, extra);
    }
}

/* The capability lists, and the PCI Express port types. */
TEST(pci, capabilities)
{
    need_hosts();
    for (int i = 0; PciFunc *f = pci_func(i); i++) {
        char k[32], list[96] = "", ext[64] = "";
        int n = 0;
        uint32_t off = pci_read8(f, PCI_CAP_PTR);
        for (int guard = 0; off && guard < 16; guard++) {
            n += snprintf(list + n, sizeof(list) - n, " %02x@%02x",
                          pci_read8(f, off), off);
            off = pci_read8(f, off + 1);
        }
        n = 0;
        off = PCI_EXT_CAP_START;
        for (int guard = 0; f->host->ext_config && off && guard < 8; guard++) {
            uint32_t hdr = pci_read32(f, off);
            if (hdr == 0 || hdr == ~0u)
                break;
            n += snprintf(ext + n, sizeof(ext) - n, " %04x.v%u@%03x",
                          hdr & 0xffff, (hdr >> 16) & 0xf, off);
            off = hdr >> 20;
        }
        key(k, sizeof(k), "caps", f);
        uint32_t exp = pci_find_cap(f, PCI_CAP_EXP);
        SNAP(k, "status %04x list%s ext%s type %d", pci_read16(f, PCI_STATUS),
             list, ext,
             exp ? (pci_read16(f, exp + 2) >> 4) & 0xf : -1);
    }
}

/* Every BAR inside a window of its host, behind every bridge above it, and
   answering there. */
TEST(pci, bars_decode)
{
    need_hosts();
    pt_require();
    for (int i = 0; PciFunc *f = pci_func(i); i++) {
        for (int b = 0; b < 6; b++) {
            const PciBar &bar = f->bars[b];
            if (bar.size == 0)
                continue;
            EXPECT_NE(bar.addr, 0u);
            EXPECT_EQ(bar.addr & (bar.size - 1), 0u);
            for (const PciFunc *p = f->parent; p; p = p->parent) {
                const PciAperture &w = bar.io ? p->win_io
                    : bar.is64 && bar.prefetch && p->win_pref.size &&
                      bar.addr >= p->win_pref.pci ? p->win_pref : p->win_mem;
                EXPECT_GE(bar.addr, w.pci);
                EXPECT_LE(bar.addr + bar.size, w.pci + w.size);
            }
        }
    }
    for (int i = 0; PciFunc *f = pt_get(i); i++) {
        char k[32];
        key(k, sizeof(k), "", f);
        bool io = f->bars[TEST_PCI_BAR_IO].size != 0;
        bool high = !pci_reachable(f->host, f->bars[TEST_PCI_BAR_MEM64].addr,
                                   4, false);
        EXPECT_TRUE(alive(f, TEST_PCI_BAR_REGS));
        if (io) {
            EXPECT_TRUE(alive(f, TEST_PCI_BAR_IO));
            /* written through the I/O BAR, read through memory */
            pci_bar_write(f, TEST_PCI_BAR_IO, TEST_PCI_SCRATCH, 0x1000 + i, 2);
            EXPECT_EQ(pt_read(f, TEST_PCI_SCRATCH), 0x1000u + i);
        }
        if (!high)
            EXPECT_TRUE(alive(f, TEST_PCI_BAR_MEM64));
        /* the 64 bit BAR repeats the registers every 4 KB */
        if (!high)
            EXPECT_EQ(pci_bar_read(f, TEST_PCI_BAR_MEM64, 0x3000 + TEST_PCI_ID,
                                   2), (uint32_t)TEST_PCI_MAGIC);
        SNAP(k, "io %s mem64 %s", io ? "yes" : "no",
             high ? "out of reach" : f->bars[TEST_PCI_BAR_MEM64].addr >=
             0x100000000ull ? "above 4G" : "below 4G");
    }
}

/* The capability headers and the bits of their control words that report
   rather than configure, written as a careless driver might. */
TEST(pci, capability_writes)
{
    PciFunc *f = pt_require();
    static const uint8_t ids[] = {PCI_CAP_EXP, PCI_CAP_MSIX, PCI_CAP_MSI};
    for (uint8_t id : ids) {
        uint32_t cap = pci_find_cap(f, id);
        if (cap == 0)
            continue;
        char k[16];
        uint32_t before = pci_read32(f, cap);
        pci_write16(f, cap + 2, 0);
        uint32_t zeros = pci_read32(f, cap);
        pci_write32(f, cap, ~0u);
        uint32_t ones = pci_read32(f, cap);
        pci_write32(f, cap, before);
        snprintf(k, sizeof(k), "cap%02x", id);
        SNAP(k, "%08x zeros %08x ones %08x", before, zeros, ones);
    }
}

/* Each BAR's size and type, by writing all ones. */
TEST(pci, bar_sizing)
{
    PciFunc *f = pt_require();
    pci_write16(f, PCI_COMMAND, 0);
    for (int b = 0; b < 6; b++) {
        char k[8];
        pci_write32(f, PCI_BAR(b), ~0u);
        snprintf(k, sizeof(k), "bar%d", b);
        SNAP(k, "%08x", pci_read32(f, PCI_BAR(b)));
        pci_write32(f, PCI_BAR(b), 0);
        EXPECT_EQ(pci_read32(f, PCI_BAR(b)) & ~0xfu, 0u);
    }
    pci_write32(f, PCI_ROM0, ~0u);
    SNAP("rom", "%08x", pci_read32(f, PCI_ROM0));
    pci_write32(f, PCI_ROM0, 0);
}

/* The bits of the header a driver can change, by writing all ones and then
   zeros to each register, decoding off. */
TEST(pci, write_masks)
{
    PciFunc *ep = pt_require();
    const PciFunc *funcs[2] = {ep, need_bridge()};
    for (const PciFunc *f : funcs) {
        uint32_t cmd = pci_read16(f, PCI_COMMAND);
        pci_write16(f, PCI_COMMAND, 0);
        for (uint32_t reg = 0; reg < 0x40; reg += 4) {
            if (reg == PCI_COMMAND)
                continue;
            char k[32];
            uint32_t orig = pci_read32(f, reg);
            pci_write32(f, reg, ~0u);
            uint32_t ones = pci_read32(f, reg);
            pci_write32(f, reg, 0);
            uint32_t zeros = pci_read32(f, reg);
            pci_write32(f, reg, orig);
            snprintf(k, sizeof(k), "%s%02x", f->bridge ? "bridge" : "ep",
                     reg);
            SNAP(k, "%08x %08x", ones, zeros);
        }
        /* the command register at last, with every BAR where it was */
        pci_write16(f, PCI_COMMAND, 0xffff);
        SNAP(f->bridge ? "bridge_command" : "ep_command", "%04x",
             pci_read16(f, PCI_COMMAND));
        pci_write16(f, PCI_COMMAND, cmd);
    }
}

/* The error bits of the status registers clear on a written one; the rest
   stays, even under a read, or and write back of the command dword. */
TEST(pci, status_write_one_to_clear)
{
    PciFunc *f = pt_require();
    uint32_t before = pci_read16(f, PCI_STATUS);
    pci_write16(f, PCI_STATUS, 0xffff);
    EXPECT_EQ(pci_read16(f, PCI_STATUS), before);
    pci_write32(f, PCI_COMMAND, pci_read32(f, PCI_COMMAND));
    EXPECT_EQ(pci_read16(f, PCI_STATUS), before);
    SNAP("status", "%04x", before);
    const PciFunc *br = need_bridge();
    uint32_t sec = pci_read16(br, PCI_SEC_STATUS);
    pci_write16(br, PCI_SEC_STATUS, 0xffff);
    SNAP("secondary_status", "%04x %04x", sec,
         pci_read16(br, PCI_SEC_STATUS));
}

/* Narrower reads see the same bytes as a wide one; what nothing answers
   reads all ones. */
TEST(pci, access_sizes)
{
    PciFunc *f = pt_require();
    uint32_t id = pci_read32(f, PCI_VENDOR_ID);
    EXPECT_EQ(pci_read16(f, PCI_VENDOR_ID), id & 0xffff);
    EXPECT_EQ(pci_read16(f, PCI_DEVICE_ID), id >> 16);
    for (int i = 0; i < 4; i++)
        EXPECT_EQ(pci_read8(f, i), (id >> (8 * i)) & 0xff);

    pci_write8(f, PCI_INTERRUPT_LINE, 0x5a);
    EXPECT_EQ(pci_read8(f, PCI_INTERRUPT_LINE), 0x5a);
    pci_write16(f, PCI_INTERRUPT_LINE, 0x01a5);
    SNAP("line_pin", "%04x", pci_read16(f, PCI_INTERRUPT_LINE));

    const PciHost *h = f->host;
    uint32_t empty = 0;
    for (uint32_t devfn = 0; devfn < 256; devfn += 8) {
        if (h->Read(f->bus, devfn, PCI_VENDOR_ID, 1) == 0xffff) {
            empty = devfn;
            break;
        }
    }
    h->Write(f->bus, empty, PCI_INTERRUPT_LINE, 0x12, 0);
    SNAP("absent", "%08x %04x %02x", h->Read(f->bus, empty, 0, 2),
         h->Read(f->bus, empty, 0, 1), h->Read(f->bus, empty, 0, 0));
    /* a bus no bridge claims */
    SNAP("unclaimed_bus", "%08x", h->Read(h->bus_last, 0, 0, 2));
    if (h->ext_config) {
        uint32_t dsn = pci_find_ext_cap(f, 0x0003);
        SNAP("dsn", "%s", dsn ? "found" : "none");
        /* read only up there */
        if (dsn) {
            uint32_t v = pci_read32(f, dsn + 4);
            pci_write32(f, dsn + 4, ~v);
            EXPECT_EQ(pci_read32(f, dsn + 4), v);
        }
        SNAP("last_word", "%08x", pci_read32(f, 0xffc));
    }
}

/* The expansion ROM decodes only with its own enable bit as well. */
TEST(pci, expansion_rom)
{
    PciFunc *f = pt_require();
    const PciBar &rom = f->bars[6];
    ASSERT_NE(rom.size, 0u);
    ASSERT_NE(rom.addr, 0u);
    snap_peek("disabled", f->host, false, rom.addr);
    pci_write32(f, PCI_ROM0, (uint32_t)rom.addr | PCI_ROM_ENABLE);
    EXPECT_EQ(pci_read32(f, PCI_ROM0), (uint32_t)rom.addr | PCI_ROM_ENABLE);
    EXPECT_EQ(pci_space_read(f->host, false, rom.addr, 2), 0x0302aa55u);
    EXPECT_EQ(pci_space_read(f->host, false, rom.addr + 0x7fe, 1), 0xfffeu);
    EXPECT_EQ(pci_space_read(f->host, false, rom.addr + 0x10, 0), 0x10u);
    /* written bytes go nowhere */
    pci_space_write(f->host, false, rom.addr + 0x10, 0, 2);
    EXPECT_EQ(pci_space_read(f->host, false, rom.addr + 0x10, 2), 0x13121110u);
    /* the memory enable is needed as well */
    pci_write16(f, PCI_COMMAND, PCI_CMD_IO);
    snap_peek("memory_off", f->host, false, rom.addr);
}

/* A BAR answers at the address last written, while decoding is on. */
TEST(pci, bar_moves)
{
    PciFunc *f = pt_require();
    uint64_t regs = f->bars[TEST_PCI_BAR_REGS].addr;
    uint64_t msix = f->bars[TEST_PCI_BAR_MSIX].addr;
    ASSERT_TRUE(regs != 0 && msix != 0);
    pci_write32(f, PCI_BAR(TEST_PCI_BAR_REGS), (uint32_t)msix);
    pci_write32(f, PCI_BAR(TEST_PCI_BAR_MSIX), (uint32_t)regs);
    EXPECT_TRUE(alive_at(f, false, msix));
    EXPECT_FALSE(alive_at(f, false, regs));
    pci_write16(f, PCI_COMMAND, PCI_CMD_IO);
    snap_peek("memory_off", f->host, false, msix + TEST_PCI_ID);
    pci_write16(f, PCI_COMMAND, PCI_CMD_MEMORY);
    EXPECT_TRUE(alive_at(f, false, msix));
    if (f->bars[TEST_PCI_BAR_IO].size) {
        snap_peek("io_off", f->host, true,
                  f->bars[TEST_PCI_BAR_IO].addr + TEST_PCI_ID);
        pci_write16(f, PCI_COMMAND, PCI_CMD_IO);
        EXPECT_TRUE(alive(f, TEST_PCI_BAR_IO));
    }
}

/* The upper half of a 64 bit BAR places it above 4 GB. */
TEST(pci, bar_above_4g)
{
    PciFunc *f = pt_require();
    const PciHost *h = f->host;
    if (h->mem64.size == 0)
        t_skip("no 64 bit window");
    const PciBar &b = f->bars[TEST_PCI_BAR_MEM64];
    uint64_t addr = align_up(h->mem64.pci + h->mem64.size / 2, b.size);
    uint32_t reg = PCI_BAR(TEST_PCI_BAR_MEM64);
    pci_write32(f, reg, (uint32_t)addr);
    pci_write32(f, reg + 4, addr >> 32);
    EXPECT_EQ(pci_read32(f, reg + 4), (uint32_t)(addr >> 32));
    EXPECT_EQ(pci_read32(f, reg) & ~0xfu, (uint32_t)addr);
    if (!pci_reachable(h, addr, 4, false))
        return;
    /* behind a bridge only its prefetchable window carries it, which
       enumeration opened only where it put something */
    bool up = alive_at(f, false, addr);
    if (f->parent)
        SNAP("decodes", "%d", up);
    else
        EXPECT_TRUE(up);
}

/* A bridge forwards what falls in its windows and nothing else. */
TEST(pci, bridge_windows)
{
    PciFunc *f = need_bridged();
    const PciFunc *br = f->parent;
    ASSERT_TRUE(alive(f, TEST_PCI_BAR_REGS));
    uint16_t mem_base = pci_read16(br, PCI_MEM_BASE);
    uint16_t mem_limit = pci_read16(br, PCI_MEM_LIMIT);
    pci_write16(br, PCI_MEM_BASE, 0xfff0);
    pci_write16(br, PCI_MEM_LIMIT, 0);
    bool closed_mem = alive(f, TEST_PCI_BAR_REGS);
    /* the prefetchable window carries it as well */
    const PciAperture &m = br->win_mem;
    pci_write16(br, PCI_PREF_BASE, (m.pci >> 16) & 0xfff0);
    pci_write16(br, PCI_PREF_LIMIT, ((m.pci + m.size - 1) >> 16) & 0xfff0);
    pci_write32(br, PCI_PREF_BASE_HI, m.pci >> 32);
    pci_write32(br, PCI_PREF_LIMIT_HI, (m.pci + m.size - 1) >> 32);
    bool by_pref = alive(f, TEST_PCI_BAR_REGS);
    pci_write16(br, PCI_MEM_BASE, mem_base);
    pci_write16(br, PCI_MEM_LIMIT, mem_limit);
    EXPECT_TRUE(alive(f, TEST_PCI_BAR_REGS));
    EXPECT_FALSE(closed_mem);
    EXPECT_TRUE(by_pref);
    SNAP("window_regs", "%04x %04x %04x %04x", mem_base, mem_limit,
         pci_read16(br, PCI_PREF_BASE), pci_read16(br, PCI_PREF_LIMIT));

    if (f->bars[TEST_PCI_BAR_IO].size) {
        EXPECT_TRUE(alive(f, TEST_PCI_BAR_IO));
        uint8_t io_base = pci_read8(br, PCI_IO_BASE);
        pci_write8(br, PCI_IO_BASE, 0xf0);
        EXPECT_FALSE(alive(f, TEST_PCI_BAR_IO));
        /* the upper 16 bits move it out of the way as well */
        pci_write8(br, PCI_IO_BASE, io_base);
        pci_write16(br, PCI_IO_BASE_HI, 1);
        pci_write16(br, PCI_IO_LIMIT_HI, 1);
        EXPECT_FALSE(alive(f, TEST_PCI_BAR_IO));
        pci_write16(br, PCI_IO_BASE_HI, 0);
        pci_write16(br, PCI_IO_LIMIT_HI, 0);
        EXPECT_TRUE(alive(f, TEST_PCI_BAR_IO));
        SNAP("io_regs", "%02x %02x", io_base, pci_read8(br, PCI_IO_LIMIT));
    }
}

/* Every bridge on the way must forward, not just the nearest. */
TEST(pci, nested_bridges)
{
    PciFunc *f = need_bridged();
    const PciFunc *top = f->parent;
    while (top->parent)
        top = top->parent;
    if (top == f->parent)
        t_skip("only one bridge above");
    pci_write16(top, PCI_MEM_BASE, 0xfff0);
    pci_write16(top, PCI_MEM_LIMIT, 0);
    EXPECT_FALSE(alive(f, TEST_PCI_BAR_REGS));
    pci_restore_all();
    EXPECT_TRUE(alive(f, TEST_PCI_BAR_REGS));
}

/* Configuration cycles reach the buses a bridge's numbers claim. */
TEST(pci, bus_numbers)
{
    need_hosts();
    t_on_cleanup(pt_cleanup);
    /* a function right behind a bridge on the root bus */
    PciFunc *f = nullptr;
    for (int i = 0; (f = pci_func(i)); i++) {
        if (f->parent && f->parent->parent == nullptr)
            break;
    }
    if (f == nullptr)
        t_skip("no bridge with anything behind it");
    PciFunc *br = f->parent;
    const PciHost *h = f->host;
    uint32_t id = pci_read32(f, PCI_VENDOR_ID);
    uint32_t moved = h->bus_last;
    ASSERT_GT(moved, br->subordinate);
    /* a subordinate below the secondary claims nothing */
    pci_write8(br, PCI_SUBORDINATE_BUS, f->bus - 1);
    SNAP("empty_range", "%08x", h->Read(f->bus, f->devfn, PCI_VENDOR_ID, 2));
    pci_write8(br, PCI_SECONDARY_BUS, moved);
    pci_write8(br, PCI_SUBORDINATE_BUS, moved);
    EXPECT_EQ(h->Read(f->bus, f->devfn, PCI_VENDOR_ID, 2), 0xffffffffu);
    EXPECT_EQ(h->Read(moved, f->devfn, PCI_VENDOR_ID, 2), id);
    pci_restore_all();
    EXPECT_EQ(h->Read(f->bus, f->devfn, PCI_VENDOR_ID, 2), id);
}

/* Each test function's INTx, through the controller and input the tables
   give for its pin as swizzled up to the root bus. */
TEST(pci, intx)
{
    need_hosts();
    pt_require();
    int routed = 0;
    for (int i = 0; PciFunc *f = pt_get(i); i++) {
        char k[32];
        key(k, sizeof(k), "", f);
        IntController *c;
        uint32_t line;
        if (!pci_intx_resolve(f, &c, &line)) {
            SNAP(k, "pin %u unresolved", f->pin);
            continue;
        }
        sIrqs = 0;
        ASSERT_TRUE(pci_intx_route(f, intx_handler, f));
        arch_irq_enable();
        pt_write(f, TEST_PCI_INTX, 1);
        bool got = t_wait_until([] {return sIrqs > 0;});
        arch_irq_disable();
        pci_intx_unroute(f);
        EXPECT_TRUE(got);
        EXPECT_FALSE(pci_read16(f, PCI_STATUS) & PCI_STATUS_INTX);
        SNAP(k, "pin %u %s %u", f->pin, c->Kind(), line);
        routed++;
    }
    if (routed == 0)
        t_skip("no INTx the suite can take");
}

/* INTx Disable keeps the pin low; Interrupt Status still says it would
   be up. */
TEST(pci, intx_disable)
{
    PciFunc *f = pt_require();
    if (!pci_intx_route(f, intx_handler, f))
        t_skip("its INTx goes nowhere the suite drives");
    sIrqs = 0;
    pci_write16(f, PCI_COMMAND, f->command | PCI_CMD_INTX_DISABLE);
    arch_irq_enable();
    pt_write(f, TEST_PCI_INTX, 1);
    t_delay_us(5000);
    EXPECT_EQ(sIrqs, 0);
    EXPECT_TRUE(pci_read16(f, PCI_STATUS) & PCI_STATUS_INTX);
    pci_write16(f, PCI_COMMAND, f->command);
    EXPECT_TRUE(t_wait_until([] {return sIrqs > 0;}));
    arch_irq_disable();
    pci_intx_unroute(f);
}

/* Two functions on one input: it stays up while either drives it. */
TEST(pci, intx_shared)
{
    need_hosts();
    pt_require();
    PciFunc *a = nullptr, *b = nullptr;
    for (int i = 0; b == nullptr && (a = pt_get(i)); i++) {
        IntController *ca, *cb;
        uint32_t la, lb;
        if (!pci_intx_resolve(a, &ca, &la))
            continue;
        for (int j = i + 1; PciFunc *f = pt_get(j); j++) {
            if (pci_intx_resolve(f, &cb, &lb) && cb == ca && lb == la) {
                b = f;
                break;
            }
        }
    }
    if (b == nullptr)
        t_skip("no two test functions share an input");
    sIrqs = 0;
    /* the handler quiets only the first, so the second keeps it up */
    sIrqFunc = a;
    ASSERT_TRUE(pci_intx_route(a, [](void *) {
        sIrqs = sIrqs + 1;
        pt_write(sIrqFunc, TEST_PCI_INTX, 0);
        if (sIrqs > 3)
            for (int i = 0; PciFunc *f = pt_get(i); i++)
                pt_write(f, TEST_PCI_INTX, 0);
    }, nullptr));
    pt_write(b, TEST_PCI_INTX, 1);
    pt_write(a, TEST_PCI_INTX, 1);
    arch_irq_enable();
    t_wait_until([] {return sIrqs > 3;}, 20000);
    arch_irq_disable();
    pci_intx_unroute(a);
    EXPECT_GE(sIrqs, 1);
    /* whether the controller takes the input again while it stays up */
    SNAP("redelivered", "%d", sIrqs > 3);
}

/* The test function writing and reading RAM. */
TEST(pci, dma)
{
    PciFunc *f = pt_require();
    volatile uint32_t *buf = (volatile uint32_t *)heap_alloc(
        16, 16, ARCH_ADDRESS_LIMIT);
    ASSERT_TRUE(buf != nullptr);
    uint64_t a = (uint64_t)(uintptr_t)buf;
    pt_write(f, TEST_PCI_DMA_LO, (uint32_t)a);
    pt_write(f, TEST_PCI_DMA_HI, a >> 32);
    pt_write(f, TEST_PCI_DMA_DATA, 0x12345678);
    EXPECT_EQ(buf[0], 0x12345678u);
    EXPECT_EQ(pt_read(f, TEST_PCI_DMA_STATUS), 0u);
    buf[0] = 0xcafef00d;
    EXPECT_EQ(pt_read(f, TEST_PCI_DMA_DATA), 0xcafef00du);
    /* where there is no RAM */
    uint64_t hole = f->bars[TEST_PCI_BAR_REGS].addr;
    pt_write(f, TEST_PCI_DMA_LO, (uint32_t)hole);
    pt_write(f, TEST_PCI_DMA_HI, hole >> 32);
    pt_write(f, TEST_PCI_DMA_DATA, 1);
    SNAP("no_ram", "%u %08x", pt_read(f, TEST_PCI_DMA_STATUS),
         pt_read(f, TEST_PCI_DMA_DATA));
}
