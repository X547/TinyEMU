/*
 * PCI tests: MSI and MSI-X
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

/* The capabilities by the PCI Local Bus specification 3.0, 6.8; messages
   go to whatever collects them on the host: the IMSIC, the local APIC, or
   the receiver in the host bridge. */

#define MSI_FLAGS      2
#define MSI_ADDR_LO    4
#define MSI_ADDR_HI    8
#define MSI_DATA       12
#define MSI_ENABLE     0x0001
#define MSI_QSIZE(n)   ((n) << 4)

#define MSIX_FLAGS     2
#define MSIX_TABLE     4
#define MSIX_PBA       8
#define MSIX_ENABLE    0x8000
#define MSIX_MASKALL   0x4000

namespace {

volatile uint32_t sGot;   /* one bit per message number */
volatile int sCount;

void handler(void *ctx)
{
    sGot = sGot | 1u << (uintptr_t)ctx;
    sCount = sCount + 1;
}

struct Msi {
    PciFunc *f;
    uint32_t cap;
    uint64_t addr;
    uint32_t data;
};

/* A test function whose host collects messages, with its capability. */
Msi need_msi(uint8_t id, int index = 0)
{
    Msi m {};
    for (int i = 0; PciFunc *f = pt_get(i); i++) {
        if (f->host->msi == nullptr || !pci_find_cap(f, id) || index-- > 0)
            continue;
        m.f = f;
        m.cap = pci_find_cap(f, id);
        t_on_cleanup(pt_cleanup);
        return m;
    }
    t_skip("no test function with that capability and a receiver");
}

/* Messages 0 to count-1 routed, the address and first data returned. */
void route(Msi &m, int count)
{
    for (int n = 0; n < count; n++) {
        uint64_t addr;
        uint32_t data;
        ASSERT_TRUE(m.f->host->msi->Message(n, handler, (void *)(uintptr_t)n,
                                            &addr, &data));
        if (n == 0) {
            m.addr = addr;
            m.data = data;
        }
    }
    sGot = 0;
    sCount = 0;
}

void send(const PciFunc *f, uint32_t vector)
{
    pt_write(f, TEST_PCI_MSI_SEND, vector);
}

/* Let whatever was sent arrive. */
void settle()
{
    arch_irq_enable();
    t_delay_us(2000);
    arch_irq_disable();
}

uint32_t msix_entry(const PciFunc *f, int n, int word)
{
    return pci_bar_read(f, TEST_PCI_BAR_MSIX,
                        TEST_PCI_MSIX_TABLE + 16 * n + 4 * word, 2);
}

void set_msix_entry(const PciFunc *f, int n, int word, uint32_t v)
{
    pci_bar_write(f, TEST_PCI_BAR_MSIX, TEST_PCI_MSIX_TABLE + 16 * n + 4 * word,
                  v, 2);
}

uint32_t pba(const PciFunc *f)
{
    return pci_bar_read(f, TEST_PCI_BAR_MSIX, TEST_PCI_MSIX_PBA, 2);
}

/* The enable and mask bits, keeping the table size: temu lets a write
   change that too. */
void msix_control(const Msi &m, uint16_t bits)
{
    uint16_t ctrl = pci_read16(m.f, m.cap + MSIX_FLAGS);
    pci_write16(m.f, m.cap + MSIX_FLAGS, (ctrl & 0x7ff) | bits);
}

}


/* What the functions offer, and on a host with no receiver, nothing. */
TEST(pci_msi, offered)
{
    pt_require();
    for (int i = 0; PciFunc *f = pt_get(i); i++) {
        char k[32], name[24];
        pci_name(f, name, sizeof(name));
        snprintf(k, sizeof(k), "fn%s", name);
        uint32_t msi = pci_find_cap(f, PCI_CAP_MSI);
        uint32_t msix = pci_find_cap(f, PCI_CAP_MSIX);
        SNAP(k, "info %08x msi %04x msix %04x %08x %08x",
             pt_read(f, TEST_PCI_MSI_INFO),
             msi ? pci_read16(f, msi + MSI_FLAGS) : 0xffff,
             msix ? pci_read16(f, msix + MSIX_FLAGS) : 0xffff,
             msix ? pci_read32(f, msix + MSIX_TABLE) : 0,
             msix ? pci_read32(f, msix + MSIX_PBA) : 0);
    }
}

/* One vector, then as many as the function offers, each message carrying
   its number in the low bits of the data. */
TEST(pci_msi, msi_delivery)
{
    Msi m = need_msi(PCI_CAP_MSI);
    uint32_t flags = pci_read16(m.f, m.cap + MSI_FLAGS);
    int log2 = (flags >> 1) & 7;
    int count = 1 << log2;
    route(m, count);
    pci_write32(m.f, m.cap + MSI_ADDR_LO, (uint32_t)m.addr);
    pci_write32(m.f, m.cap + MSI_ADDR_HI, m.addr >> 32);
    pci_write16(m.f, m.cap + MSI_DATA, m.data);

    /* nothing before it is enabled */
    send(m.f, 0);
    settle();
    EXPECT_EQ(sGot, 0u);

    pci_write16(m.f, m.cap + MSI_FLAGS, flags | MSI_ENABLE);
    EXPECT_EQ(pt_read(m.f, TEST_PCI_MSI_INFO) >> TEST_PCI_MSI_COUNT_SHIFT, 1u);
    send(m.f, 0);
    send(m.f, 1); /* beyond the one vector it was given */
    settle();
    EXPECT_EQ(sGot, 1u);

    pci_write16(m.f, m.cap + MSI_FLAGS,
                flags | MSI_ENABLE | MSI_QSIZE(log2));
    sGot = 0;
    for (int n = 0; n < count + 1; n++)
        send(m.f, n);
    settle();
    EXPECT_EQ(sGot, count == 32 ? ~0u : (1u << count) - 1);
    SNAP("vectors", "%d info %08x", count, pt_read(m.f, TEST_PCI_MSI_INFO));

    /* asking for more than is offered gets what is offered */
    pci_write16(m.f, m.cap + MSI_FLAGS, flags | MSI_ENABLE | MSI_QSIZE(5));
    SNAP("oversized", "%08x", pt_read(m.f, TEST_PCI_MSI_INFO));
}

/* MSI-X: each vector its own address and data; masked vectors are left
   pending and sent when unmasked. */
TEST(pci_msi, msix_delivery)
{
    Msi m = need_msi(PCI_CAP_MSIX);
    int count = (pci_read16(m.f, m.cap + MSIX_FLAGS) & 0x7ff) + 1;
    int use = min(count, 4);
    for (int n = 0; n < use; n++) {
        uint64_t addr;
        uint32_t data;
        ASSERT_TRUE(m.f->host->msi->Message(n, handler, (void *)(uintptr_t)n,
                                            &addr, &data));
        set_msix_entry(m.f, n, 0, (uint32_t)addr);
        set_msix_entry(m.f, n, 1, addr >> 32);
        set_msix_entry(m.f, n, 2, data);
        set_msix_entry(m.f, n, 3, 0);
    }
    EXPECT_EQ(msix_entry(m.f, 1, 2), msix_entry(m.f, 0, 2) + 1);
    msix_control(m, MSIX_ENABLE);
    sGot = 0;
    for (int n = 0; n < use; n++)
        send(m.f, n);
    send(m.f, count); /* no such vector */
    settle();
    EXPECT_EQ(sGot, (1u << use) - 1);

    /* a masked vector waits in the pending bits */
    sGot = 0;
    set_msix_entry(m.f, 0, 3, 1);
    send(m.f, 0);
    settle();
    EXPECT_EQ(sGot, 0u);
    EXPECT_EQ(pba(m.f) & 1, 1u);
    set_msix_entry(m.f, 0, 3, 0);
    settle();
    EXPECT_EQ(sGot, 1u);
    EXPECT_EQ(pba(m.f) & 1, 0u);

    /* the function mask holds all of them */
    sGot = 0;
    msix_control(m, MSIX_ENABLE | MSIX_MASKALL);
    send(m.f, 1);
    settle();
    EXPECT_EQ(sGot, 0u);
    EXPECT_EQ(pba(m.f), 2u);
    /* lifting it sends what is pending, by the PCI specification */
    msix_control(m, MSIX_ENABLE);
    settle();
    EXPECT_EQ(sGot, 2u);
    EXPECT_EQ(pba(m.f), 0u);
    /* and nothing is left for the vector's own unmask */
    set_msix_entry(m.f, 1, 3, 1);
    set_msix_entry(m.f, 1, 3, 0);
    settle();
    EXPECT_EQ(sGot, 2u);

    /* MSI-X wins when a driver turns on both */
    uint32_t msi = pci_find_cap(m.f, PCI_CAP_MSI);
    if (msi) {
        pci_write32(m.f, msi + MSI_ADDR_LO, 0);
        pci_write16(m.f, msi + MSI_FLAGS,
                    pci_read16(m.f, msi + MSI_FLAGS) | MSI_ENABLE);
        sGot = 0;
        send(m.f, 2);
        settle();
        EXPECT_EQ(sGot, 4u);
        SNAP("both_enabled", "%08x", pt_read(m.f, TEST_PCI_MSI_INFO));
        msix_control(m, 0);
        SNAP("msi_alone", "%08x", pt_read(m.f, TEST_PCI_MSI_INFO));
    }
}

/* The table and pending bits as registers. */
TEST(pci_msi, msix_table)
{
    Msi m = need_msi(PCI_CAP_MSIX);
    int count = (pci_read16(m.f, m.cap + MSIX_FLAGS) & 0x7ff) + 1;
    set_msix_entry(m.f, 0, 0, 0x11223344);
    set_msix_entry(m.f, 0, 1, 0x55667788);
    set_msix_entry(m.f, 0, 2, 0x99aabbcc);
    set_msix_entry(m.f, 0, 3, 0xffffffff);
    SNAP("entry0", "%08x %08x %08x %08x", msix_entry(m.f, 0, 0),
         msix_entry(m.f, 0, 1), msix_entry(m.f, 0, 2), msix_entry(m.f, 0, 3));
    /* narrow accesses to one word of it */
    pci_bar_write(m.f, TEST_PCI_BAR_MSIX, 1, 0xab, 0);
    pci_bar_write(m.f, TEST_PCI_BAR_MSIX, 6, 0xcdef, 1);
    SNAP("narrow", "%08x %08x %02x %04x", msix_entry(m.f, 0, 0),
         msix_entry(m.f, 0, 1),
         pci_bar_read(m.f, TEST_PCI_BAR_MSIX, 1, 0),
         pci_bar_read(m.f, TEST_PCI_BAR_MSIX, 6, 1));
    /* past the last entry and the last pending word */
    set_msix_entry(m.f, count, 2, 0x1234);
    SNAP("past_end", "%08x %08x", msix_entry(m.f, count, 2),
         pci_bar_read(m.f, TEST_PCI_BAR_MSIX, TEST_PCI_MSIX_PBA + 4, 2));
    /* the pending bits are read only */
    pci_bar_write(m.f, TEST_PCI_BAR_MSIX, TEST_PCI_MSIX_PBA, ~0u, 2);
    EXPECT_EQ(pba(m.f), 0u);
    for (int n = 0; n < count; n++)
        set_msix_entry(m.f, n, 3, 1);
}

/* A message not aimed at a host bridge's own receiver is a plain write to
   memory, on each host that has one. */
TEST(pci_msi, msix_to_memory)
{
    pt_require();
    volatile uint32_t *buf = (volatile uint32_t *)heap_alloc(
        16, 16, ARCH_ADDRESS_LIMIT);
    ASSERT_TRUE(buf != nullptr);
    uint64_t a = (uint64_t)(uintptr_t)buf;
    int tried = 0;
    for (int h = 0; h < pci_host_count(); h++) {
        PciHostKind kind = pci_host(h)->kind;
        if (kind != PCI_HOST_DW && kind != PCI_HOST_PLDA)
            continue;
        Msi m {};
        for (int i = 0; PciFunc *f = pt_get(i); i++) {
            if (f->host == pci_host(h) && pci_find_cap(f, PCI_CAP_MSIX)) {
                m.f = f;
                m.cap = pci_find_cap(f, PCI_CAP_MSIX);
                break;
            }
        }
        if (m.f == nullptr)
            continue;
        buf[0] = 0;
        set_msix_entry(m.f, 0, 0, (uint32_t)a);
        set_msix_entry(m.f, 0, 1, a >> 32);
        set_msix_entry(m.f, 0, 2, 0x5eed);
        set_msix_entry(m.f, 0, 3, 0);
        msix_control(m, MSIX_ENABLE);
        send(m.f, 0);
        EXPECT_EQ(buf[0], 0x5eedu);
        /* and to a device: the test function's own scratch register */
        uint64_t scratch = m.f->bars[TEST_PCI_BAR_REGS].addr +
            TEST_PCI_SCRATCH;
        set_msix_entry(m.f, 0, 0, (uint32_t)scratch);
        set_msix_entry(m.f, 0, 1, scratch >> 32);
        send(m.f, 0);
        EXPECT_EQ(pt_read(m.f, TEST_PCI_SCRATCH), 0x5eedu);
        tried++;
    }
    if (tried == 0)
        t_skip("no host bridge with a receiver of its own");
}
