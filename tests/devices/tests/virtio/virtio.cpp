/*
 * Virtio transport and split virtqueue tests
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
#include "heap.h"
#include "mmio.h"
#include "test.h"
#include "virtio.h"

/* What the virtio specification (1.2) asks of every device: the device
   status and feature negotiation (2.1, 2.2, 3.1), the split virtqueue
   (2.7), and the MMIO (4.2) and PCI (4.1) transports. The console is the
   device they are tried on: its transmit queue, 1, takes buffers as soon
   as they come, and hands them back unwritten. */

#define CONSOLE_RX 0
#define CONSOLE_TX 1
#define QUEUE_SIZE 8

namespace {

VirtioDev sDev;

void cleanup()
{
    virtio_reset(&sDev);
    if (sDev.pci)
        pci_restore_all();
}

void require()
{
    virtio_require(VIRTIO_ID_CONSOLE, &sDev);
    t_on_cleanup(cleanup);
}

/* Negotiated, with the transmit queue set up and DRIVER_OK. */
void start(Virtq *vq, uint64_t features = VIRTIO_F_VERSION_1)
{
    ASSERT_TRUE(virtio_negotiate(&sDev, features));
    ASSERT_TRUE(virtq_setup(&sDev, CONSOLE_TX, QUEUE_SIZE, vq));
    virtio_set_status(&sDev, virtio_status(&sDev) | VIRTIO_STATUS_DRIVER_OK);
}

void *buffer(size_t size)
{
    void *p = heap_alloc(size, 16);
    if (p == nullptr)
        t_skip("no memory");
    memset(p, 'v', size);
    return p;
}

void set_desc(VirtqDesc *d, const void *buf, uint32_t len, uint16_t flags,
              uint16_t next = 0)
{
    d->addr = (uintptr_t)buf;
    d->len = len;
    d->flags = flags;
    d->next = next;
}

/* One readable buffer in descriptor 'head', made available and
   notified. */
void send_one(Virtq *vq, uint16_t head, uint32_t len = 8)
{
    set_desc(&vq->desc[head], buffer(len), len, 0);
    virtq_publish(vq, head);
    virtq_kick(vq);
}

/* The interrupt causes, taken: PCI's ISR clears as it is read, MMIO's
   InterruptStatus when acknowledged. */
uint32_t take_isr()
{
    uint32_t isr = virtio_isr(&sDev);
    virtio_ack(&sDev, isr);
    return isr;
}

uint32_t queue_count()
{
    if (sDev.pci)
        return virtio_common_read(&sDev, VIRTIO_PCI_NUM_QUEUES, 1);
    uint32_t n = 0;
    for (; n < 64; n++) {
        virtio_queue_select(&sDev, n);
        if (virtio_queue_max(&sDev) == 0)
            break;
    }
    return n;
}

volatile int sIrqCount;
volatile uint32_t sIrqIsr;

void irq_handler(void *)
{
    sIrqIsr = sIrqIsr | take_isr();
    sIrqCount = sIrqCount + 1;
}

volatile int sMsiCount;

void msi_handler(void *)
{
    sMsiCount = sMsiCount + 1;
}

void unroute()
{
    virtio_unroute_irq(&sDev);
}

/* An address that is not RAM: the test-control device's registers. */
uint64_t not_ram()
{
    const HwDevice *tc = test_control_device();
    const HwRegion *r = tc ? hw_region(tc, HW_MMIO) : nullptr;
    if (r == nullptr)
        t_skip("needs the test-control device");
    return r->base;
}

/* Let an interrupt that is coming arrive. */
void settle()
{
    arch_irq_enable();
    t_delay_us(2000);
    arch_irq_disable();
}

}


/* The transport's identity: the MMIO magic value and version 2, or the
   PCI IDs of a non-transitional device, revision 1 and a subsystem ID of
   0x40 or more (4.1.2.1). The capabilities point at the structures. */
TEST(virtio, identification)
{
    require();
    SNAP("transport", "%s", virtio_transport(&sDev));
    if (!sDev.pci) {
        EXPECT_EQ(mmio_read32(sDev.base + VIRTIO_MMIO_MAGIC),
                  (uint32_t)VIRTIO_MMIO_MAGIC_VALUE);
        EXPECT_EQ(mmio_read32(sDev.base + VIRTIO_MMIO_VERSION), 2u);
        EXPECT_EQ(mmio_read32(sDev.base + VIRTIO_MMIO_DEVICE_ID),
                  (uint32_t)VIRTIO_ID_CONSOLE);
        SNAP("vendor", "%#x",
             mmio_read32(sDev.base + VIRTIO_MMIO_VENDOR_ID));
        return;
    }
    const PciFunc *f = sDev.fn;
    EXPECT_EQ(f->vendor, 0x1af4);
    EXPECT_EQ(f->device, 0x1040 + VIRTIO_ID_CONSOLE);
    EXPECT_GE(f->class_rev & 0xff, 1u);
    EXPECT_GE(pci_read16(f, 0x2e), 0x40);
    SNAP("subsystem", "%04x:%04x", pci_read16(f, 0x2c), pci_read16(f, 0x2e));
    SNAP("class", "%06x", f->class_rev >> 8);
    SNAP("common", "bar %d off %#x len %#x", sDev.common.bar,
         sDev.common.offset, sDev.common.length);
    SNAP("notify", "bar %d off %#x len %#x mult %u", sDev.notify.bar,
         sDev.notify.offset, sDev.notify.length, sDev.notify_mult);
    SNAP("isr", "bar %d off %#x len %#x", sDev.isr.bar, sDev.isr.offset,
         sDev.isr.length);
    SNAP("device", "bar %d off %#x len %#x", sDev.device.bar,
         sDev.device.offset, sDev.device.length);
    /* the common configuration is 0x38 bytes in virtio 1.2 */
    EXPECT_GE(sDev.common.length, 0x38u);
    EXPECT_GE(sDev.isr.length, 1u);
}

namespace {

uint32_t pci_cfg_read(uint8_t bar, uint32_t offset, uint32_t len)
{
    uint32_t cap = sDev.pci_cfg_cap;
    pci_write8(sDev.fn, cap + 4, bar);
    pci_write32(sDev.fn, cap + 8, offset);
    pci_write32(sDev.fn, cap + 12, len);
    return pci_read32(sDev.fn, cap + 16);
}

void pci_cfg_write(uint8_t bar, uint32_t offset, uint32_t len, uint32_t v)
{
    uint32_t cap = sDev.pci_cfg_cap;
    pci_write8(sDev.fn, cap + 4, bar);
    pci_write32(sDev.fn, cap + 8, offset);
    pci_write32(sDev.fn, cap + 12, len);
    pci_write32(sDev.fn, cap + 16, v);
}

}

/* The PCI configuration access capability, which a device must have: its
   data window reads and writes 'length' bytes of the BAR it names, at
   'offset'. */
TEST(virtio, pci_cfg_access)
{
    require();
    if (!sDev.pci)
        t_skip("a PCI capability");
    ASSERT_NE(sDev.pci_cfg_cap, 0u);
    uint8_t bar = sDev.common.bar;
    uint32_t base = sDev.common.offset;
    EXPECT_EQ(pci_cfg_read(bar, base + VIRTIO_PCI_NUM_QUEUES, 2) & 0xffff, 2u);
    pci_cfg_write(bar, base + VIRTIO_PCI_QUEUE_SELECT, 2, 1);
    EXPECT_EQ(virtio_common_read(&sDev, VIRTIO_PCI_QUEUE_SELECT, 1), 1u);
    virtio_queue_select(&sDev, 0);
    EXPECT_EQ(pci_cfg_read(bar, base + VIRTIO_PCI_QUEUE_SELECT, 1) & 0xff, 0u);
    EXPECT_EQ(pci_cfg_read(bar, base + VIRTIO_PCI_DEVICE_FEATURE, 4),
              virtio_device_features(&sDev, 0));
    /* lengths and BARs a driver may not use reach nothing */
    pci_write32(sDev.fn, sDev.pci_cfg_cap + 12, 0);
    pci_write32(sDev.fn, sDev.pci_cfg_cap + 16, 0x5a5a5a5a);
    SNAP("length_3", "%#x", pci_cfg_read(bar, base, 3));
    SNAP("other_bar", "%#x", pci_cfg_read(bar ^ 1, base, 4));
}

/* What reset leaves: status 0, every queue disabled (4.2.2.1, 4.1.4.3.1),
   no MSI-X vector chosen. */
FRESH_TEST(virtio, reset_state)
{
    require();
    EXPECT_EQ(virtio_status(&sDev), 0);
    SNAP("features0", "%#x", virtio_device_features(&sDev, 0));
    SNAP("features1", "%#x", virtio_device_features(&sDev, 1));
    SNAP("generation", "%u", virtio_config_generation(&sDev));
    for (uint32_t q = 0; q < 2; q++) {
        virtio_queue_select(&sDev, q);
        EXPECT_EQ(virtio_queue_ready(&sDev), 0u);
        char key[16];
        snprintf(key, sizeof(key), "q%u_size", q);
        SNAP(key, "%u", virtio_queue_size(&sDev));
    }
    if (sDev.pci) {
        EXPECT_EQ(virtio_common_read(&sDev, VIRTIO_PCI_MSIX_CONFIG, 1),
                  (uint32_t)VIRTIO_MSI_NO_VECTOR);
        virtio_queue_select(&sDev, 0);
        EXPECT_EQ(virtio_common_read(&sDev, VIRTIO_PCI_QUEUE_MSIX_VECTOR, 1),
                  (uint32_t)VIRTIO_MSI_NO_VECTOR);
    } else {
        EXPECT_EQ(virtio_isr(&sDev), 0u);
    }
}

/* VERSION_1 is offered, which a device must (6.2); feature words past the
   second read 0. */
TEST(virtio, features_offered)
{
    require();
    uint64_t f = virtio_device_features(&sDev, 0) |
                 (uint64_t)virtio_device_features(&sDev, 1) << 32;
    EXPECT_TRUE(f & VIRTIO_F_VERSION_1);
    SNAP("features", "%#llx", T_FMT_VAL(f));
    EXPECT_EQ(virtio_device_features(&sDev, 2), 0u);
    EXPECT_EQ(virtio_device_features(&sDev, 0x80000000), 0u);
}

/* A device that cannot work with what the driver accepted does not keep
   FEATURES_OK (2.2.2); a feature it did not offer is such a choice. Once
   FEATURES_OK holds, the accepted features no longer change. */
TEST(virtio, features_ok)
{
    require();
    uint64_t offered = virtio_device_features(&sDev, 0) |
                       (uint64_t)virtio_device_features(&sDev, 1) << 32;
    uint64_t unoffered = 0;
    for (int b = 0; b < 64 && !unoffered; b++) {
        if (!(offered >> b & 1))
            unoffered = 1ull << b;
    }
    ASSERT_NE(unoffered, 0u);

    virtio_reset(&sDev);
    virtio_set_status(&sDev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    virtio_set_driver_features(&sDev, 0, (uint32_t)(offered | unoffered));
    virtio_set_driver_features(&sDev, 1, (offered | unoffered) >> 32);
    virtio_set_status(&sDev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                             VIRTIO_STATUS_FEATURES_OK);
    EXPECT_EQ(virtio_status(&sDev) & VIRTIO_STATUS_FEATURES_OK, 0);

    uint64_t accepted;
    EXPECT_TRUE(virtio_negotiate(&sDev, VIRTIO_F_VERSION_1, &accepted));
    EXPECT_EQ(accepted, VIRTIO_F_VERSION_1);
    if (sDev.pci) {
        virtio_set_driver_features(&sDev, 1, 0);
        virtio_common_write(&sDev, VIRTIO_PCI_DRIVER_FEATURE_SEL, 1, 2);
        EXPECT_EQ(virtio_common_read(&sDev, VIRTIO_PCI_DRIVER_FEATURE, 2), 1u);
    }
}

/* The console has two queues, receive and transmit, without
   VIRTIO_CONSOLE_F_MULTIPORT (5.3.2). A queue the device does not have
   reads a size of 0, on MMIO in QueueNumMax (4.2.2), on PCI in
   queue_size (4.1.4.3), and ignores what is written to it. */
TEST(virtio, queue_count)
{
    require();
    EXPECT_EQ(queue_count(), 2u);
    for (uint32_t q = 0; q < 2; q++) {
        virtio_queue_select(&sDev, q);
        uint32_t max = virtio_queue_max(&sDev);
        EXPECT_GT(max, 0u);
        EXPECT_EQ(max & (max - 1), 0u);
    }
    static const uint32_t absent[] = {2, 7, 8, 0xffff};
    for (uint32_t q : absent) {
        virtio_queue_select(&sDev, q);
        EXPECT_EQ(virtio_queue_max(&sDev), 0u);
        virtio_set_queue_size(&sDev, 4);
        virtio_set_queue_addrs(&sDev, 0x1000, 0x2000, 0x3000);
        virtio_set_queue_ready(&sDev, 1);
        EXPECT_EQ(virtio_queue_size(&sDev), 0u);
        EXPECT_EQ(virtio_queue_ready(&sDev), 0u);
    }
}

/* The size a driver chooses: a power of 2 up to the maximum (2.7). */
TEST(virtio, queue_size)
{
    require();
    virtio_queue_select(&sDev, CONSOLE_TX);
    uint32_t max = virtio_queue_max(&sDev);
    virtio_set_queue_size(&sDev, 4);
    EXPECT_EQ(virtio_queue_size(&sDev), 4u);
    virtio_set_queue_size(&sDev, 6);
    SNAP("not_power_of_2", "%u", virtio_queue_size(&sDev));
    virtio_set_queue_size(&sDev, max * 2);
    SNAP("above_max", "%u", virtio_queue_size(&sDev));
    virtio_set_queue_size(&sDev, 0);
    SNAP("zero", "%u", virtio_queue_size(&sDev));
}

/* The ring addresses are 64 bits, in halves, and read back. */
TEST(virtio, queue_addresses)
{
    require();
    virtio_queue_select(&sDev, CONSOLE_TX);
    virtio_set_queue_addrs(&sDev, 0x123456789abcd000ull, 0x2000, 0x3000);
    if (sDev.pci) {
        EXPECT_EQ(virtio_common_read(&sDev, VIRTIO_PCI_QUEUE_DESC, 2),
                  0x9abcd000u);
        EXPECT_EQ(virtio_common_read(&sDev, VIRTIO_PCI_QUEUE_DESC + 4, 2),
                  0x12345678u);
        EXPECT_EQ(virtio_common_read(&sDev, VIRTIO_PCI_QUEUE_DRIVER, 2),
                  0x2000u);
        EXPECT_EQ(virtio_common_read(&sDev, VIRTIO_PCI_QUEUE_DEVICE, 2),
                  0x3000u);
        EXPECT_EQ(virtio_common_read(&sDev, VIRTIO_PCI_QUEUE_DRIVER + 4, 2),
                  0u);
        EXPECT_EQ(virtio_common_read(&sDev, VIRTIO_PCI_QUEUE_DEVICE + 4, 2),
                  0u);
    } else {
        EXPECT_EQ(mmio_read32(sDev.base + VIRTIO_MMIO_QUEUE_DESC_LOW),
                  0x9abcd000u);
        EXPECT_EQ(mmio_read32(sDev.base + VIRTIO_MMIO_QUEUE_DESC_HIGH),
                  0x12345678u);
        EXPECT_EQ(mmio_read32(sDev.base + VIRTIO_MMIO_QUEUE_AVAIL_LOW),
                  0x2000u);
        EXPECT_EQ(mmio_read32(sDev.base + VIRTIO_MMIO_QUEUE_AVAIL_HIGH), 0u);
        EXPECT_EQ(mmio_read32(sDev.base + VIRTIO_MMIO_QUEUE_USED_LOW),
                  0x3000u);
        EXPECT_EQ(mmio_read32(sDev.base + VIRTIO_MMIO_QUEUE_USED_HIGH), 0u);
    }
    virtio_set_queue_addrs(&sDev, 0, 0, 0);
}

/* PCI's feature selectors read back (4.1.4.3). MMIO's are write only
   (4.2.2), and so are some other registers; what they read, and what an
   access other than 32 bits finds, is temu's. */
TEST(virtio, selectors)
{
    require();
    virtio_device_features(&sDev, 1);
    virtio_set_driver_features(&sDev, 1, 0);
    virtio_queue_select(&sDev, 1);
    if (sDev.pci) {
        EXPECT_EQ(virtio_common_read(&sDev, VIRTIO_PCI_DEVICE_FEATURE_SEL, 2),
                  1u);
        EXPECT_EQ(virtio_common_read(&sDev, VIRTIO_PCI_DRIVER_FEATURE_SEL, 2),
                  1u);
        EXPECT_EQ(virtio_common_read(&sDev, VIRTIO_PCI_QUEUE_SELECT, 1), 1u);
        return;
    }
    SNAP("write_only", "%#x %#x %#x",
         mmio_read32(sDev.base + VIRTIO_MMIO_DEVICE_FEATURES_SEL),
         mmio_read32(sDev.base + VIRTIO_MMIO_QUEUE_SEL),
         mmio_read32(sDev.base + VIRTIO_MMIO_QUEUE_NOTIFY));
    SNAP("unknown", "%#x", mmio_read32(sDev.base + 0x0c8));
    SNAP("narrow", "%#x %#x", mmio_read8(sDev.base + VIRTIO_MMIO_MAGIC),
         mmio_read16(sDev.base + VIRTIO_MMIO_VERSION));
    mmio_write8(sDev.base + VIRTIO_MMIO_STATUS, VIRTIO_STATUS_ACKNOWLEDGE);
    SNAP("narrow_write", "%#x", virtio_status(&sDev));
}

/* Writing 0 to the status resets the device: the queues are disabled and
   the interrupt causes cleared (4.2.2.1, 4.1.4.3.1). */
TEST(virtio, reset)
{
    require();
    Virtq vq;
    start(&vq);
    send_one(&vq, 0);
    EXPECT_NE(virtio_isr(&sDev), 0u);
    virtio_set_status(&sDev, 0);
    EXPECT_EQ(virtio_status(&sDev), 0);
    virtio_queue_select(&sDev, CONSOLE_TX);
    EXPECT_EQ(virtio_queue_ready(&sDev), 0u);
    EXPECT_EQ(virtio_isr(&sDev), 0u);
    SNAP("size", "%u", virtio_queue_size(&sDev));
}

/* A readable buffer on the transmit queue comes back with nothing written
   (the used ring's len is what the device wrote), its head's ID, and the
   used buffer interrupt cause. */
TEST(virtio, used_ring)
{
    require();
    Virtq vq;
    start(&vq);
    send_one(&vq, 3);
    VirtqUsedElem e;
    ASSERT_TRUE(virtq_get_used(&vq, &e));
    EXPECT_EQ(e.id, 3u);
    EXPECT_EQ(e.len, 0u);
    EXPECT_EQ(virtq_used_idx(&vq), 1);
    EXPECT_EQ(take_isr() & VIRTIO_ISR_QUEUE, (uint32_t)VIRTIO_ISR_QUEUE);
    EXPECT_EQ(virtio_isr(&sDev), 0u);
}

/* Chains of several descriptors, made available out of order, come back
   in the order they were made available. The ring wraps past its size. */
TEST(virtio, chains)
{
    require();
    Virtq vq;
    start(&vq);
    set_desc(&vq.desc[5], buffer(4), 4, VIRTQ_DESC_F_NEXT, 1);
    set_desc(&vq.desc[1], buffer(4), 4, VIRTQ_DESC_F_NEXT, 7);
    set_desc(&vq.desc[7], buffer(4), 4, 0);
    set_desc(&vq.desc[2], buffer(4), 4, 0);
    virtq_publish(&vq, 5);
    virtq_publish(&vq, 2);
    virtq_kick(&vq);
    VirtqUsedElem e;
    ASSERT_TRUE(virtq_get_used(&vq, &e));
    EXPECT_EQ(e.id, 5u);
    ASSERT_TRUE(virtq_get_used(&vq, &e));
    EXPECT_EQ(e.id, 2u);
    for (int i = 0; i < 3 * QUEUE_SIZE; i++) {
        send_one(&vq, i % QUEUE_SIZE);
        ASSERT_TRUE(virtq_get_used(&vq, &e));
        EXPECT_EQ(e.id, (uint32_t)(i % QUEUE_SIZE));
    }
}

/* Nothing is taken before DRIVER_OK (2.1.2), nor from a queue that is not
   enabled; buffers waiting when DRIVER_OK is set are taken then. */
TEST(virtio, driver_ok)
{
    require();
    Virtq vq;
    ASSERT_TRUE(virtio_negotiate(&sDev, VIRTIO_F_VERSION_1));
    ASSERT_TRUE(virtq_setup(&sDev, CONSOLE_TX, QUEUE_SIZE, &vq));
    send_one(&vq, 0);
    t_delay_us(1000);
    EXPECT_EQ(virtq_used_idx(&vq), 0);
    EXPECT_EQ(virtio_isr(&sDev), 0u);
    virtio_set_status(&sDev, virtio_status(&sDev) | VIRTIO_STATUS_DRIVER_OK);
    VirtqUsedElem e;
    EXPECT_TRUE(virtq_get_used(&vq, &e));

    virtio_queue_select(&sDev, CONSOLE_TX);
    virtio_set_queue_ready(&sDev, 0);
    send_one(&vq, 1);
    t_delay_us(1000);
    SNAP("disabled_queue_used", "%u", virtq_used_idx(&vq));
}

/* The used buffer interrupt arrives on the line the tables give, or by
   INTx on PCI, with its cause in the ISR. */
TEST(virtio, interrupt_line)
{
    require();
    Virtq vq;
    start(&vq);
    sIrqCount = 0;
    sIrqIsr = 0;
    if (!virtio_route_irq(&sDev, irq_handler, nullptr))
        t_skip("the interrupt reaches no controller the suite drives");
    t_on_cleanup(unroute);
    arch_irq_enable();
    send_one(&vq, 0);
    EXPECT_TRUE(t_wait_until([] { return sIrqCount > 0; }));
    arch_irq_disable();
    EXPECT_EQ(sIrqIsr & VIRTIO_ISR_QUEUE, (uint32_t)VIRTIO_ISR_QUEUE);
    /* quiet once taken */
    int before = sIrqCount;
    settle();
    EXPECT_EQ(sIrqCount, before);
}

/* With MSI-X on, a queue whose vector is set signals by message, and the
   ISR is not used (PCI's used buffer notifications). A vector the table
   does not have reads back as NO_VECTOR (MSI-X vector configuration). */
TEST(virtio, msix)
{
    require();
    if (!sDev.pci)
        t_skip("MSI-X is PCI's");
    Virtq vq;
    start(&vq);
    sMsiCount = 0;
    if (!virtio_msix_route(&sDev, 0, msi_handler, nullptr))
        t_skip("no MSI-X, or nothing to receive it");
    virtio_queue_select(&sDev, CONSOLE_TX);
    virtio_common_write(&sDev, VIRTIO_PCI_QUEUE_MSIX_VECTOR, 0, 1);
    EXPECT_EQ(virtio_common_read(&sDev, VIRTIO_PCI_QUEUE_MSIX_VECTOR, 1), 0u);
    virtio_common_write(&sDev, VIRTIO_PCI_MSIX_CONFIG, 0x7ff, 1);
    EXPECT_EQ(virtio_common_read(&sDev, VIRTIO_PCI_MSIX_CONFIG, 1),
              (uint32_t)VIRTIO_MSI_NO_VECTOR);
    send_one(&vq, 0);
    settle();
    EXPECT_EQ(sMsiCount, 1);
    EXPECT_EQ(virtio_isr(&sDev), 0u);
    /* the table and the PBA are in the BAR the capability names */
    uint32_t cap = pci_find_cap(sDev.fn, PCI_CAP_MSIX);
    uint32_t table = pci_read32(sDev.fn, cap + 4);
    uint32_t pba = pci_read32(sDev.fn, cap + 8);
    EXPECT_EQ(pci_bar_read(sDev.fn, table & 7, (table & ~7u) + 12, 2), 0u);
    EXPECT_EQ(pci_bar_read(sDev.fn, pba & 7, pba & ~7u, 2), 0u);
    /* NO_VECTOR: no message at all */
    virtio_common_write(&sDev, VIRTIO_PCI_QUEUE_MSIX_VECTOR,
                        VIRTIO_MSI_NO_VECTOR, 1);
    send_one(&vq, 1);
    settle();
    EXPECT_EQ(sMsiCount, 1);
}

/* Without the event index, VIRTQ_AVAIL_F_NO_INTERRUPT asks the device not
   to interrupt (Used Buffer Notification Suppression); the buffers still
   come back. */
TEST(virtio, no_interrupt_flag)
{
    require();
    Virtq vq;
    start(&vq);
    virtq_set_avail_flags(&vq, VIRTQ_AVAIL_F_NO_INTERRUPT);
    send_one(&vq, 0);
    VirtqUsedElem e;
    EXPECT_TRUE(virtq_get_used(&vq, &e));
    EXPECT_EQ(virtio_isr(&sDev), 0u);
    virtq_set_avail_flags(&vq, 0);
    send_one(&vq, 1);
    EXPECT_TRUE(virtq_get_used(&vq, &e));
    EXPECT_EQ(take_isr() & VIRTIO_ISR_QUEUE, (uint32_t)VIRTIO_ISR_QUEUE);
}

/* With VIRTIO_F_EVENT_IDX the device interrupts when the used index
   passes used_event, and keeps avail_event at the available index it has
   seen (Used and Available Buffer Notification Suppression). */
TEST(virtio, event_index)
{
    require();
    Virtq vq;
    start(&vq, VIRTIO_F_VERSION_1 | VIRTIO_F_EVENT_IDX);
    virtq_set_used_event(&vq, 2);
    /* the flag is ignored with the event index */
    virtq_set_avail_flags(&vq, VIRTQ_AVAIL_F_NO_INTERRUPT);
    VirtqUsedElem e;
    for (int i = 0; i < 2; i++) {
        send_one(&vq, i);
        ASSERT_TRUE(virtq_get_used(&vq, &e));
        EXPECT_EQ(virtio_isr(&sDev), 0u);
        EXPECT_EQ(virtq_avail_event(&vq), vq.next_avail);
    }
    send_one(&vq, 2);
    ASSERT_TRUE(virtq_get_used(&vq, &e));
    EXPECT_EQ(take_isr() & VIRTIO_ISR_QUEUE, (uint32_t)VIRTIO_ISR_QUEUE);
    EXPECT_EQ(virtq_avail_event(&vq), 3);
    send_one(&vq, 3);
    ASSERT_TRUE(virtq_get_used(&vq, &e));
    EXPECT_EQ(virtio_isr(&sDev), 0u);
}

/* With VIRTIO_F_INDIRECT_DESC a descriptor may point at a table that
   holds the rest of the chain, after descriptors of the ring's own
   (Indirect Descriptors). Its write flag means nothing. */
TEST(virtio, indirect)
{
    require();
    uint64_t f = virtio_device_features(&sDev, 0);
    if (!(f & VIRTIO_F_INDIRECT_DESC))
        t_skip("indirect descriptors are not offered");
    Virtq vq;
    start(&vq, VIRTIO_F_VERSION_1 | VIRTIO_F_INDIRECT_DESC);
    VirtqDesc *table = (VirtqDesc *)heap_alloc(4 * sizeof(VirtqDesc), 16);
    ASSERT_TRUE(table != nullptr);
    set_desc(&table[0], buffer(3), 3, VIRTQ_DESC_F_NEXT, 2);
    set_desc(&table[2], buffer(3), 3, VIRTQ_DESC_F_NEXT, 1);
    set_desc(&table[1], buffer(3), 3, 0);
    set_desc(&vq.desc[4], table, 4 * sizeof(VirtqDesc),
             VIRTQ_DESC_F_INDIRECT | VIRTQ_DESC_F_WRITE);
    virtq_publish(&vq, 4);
    virtq_kick(&vq);
    VirtqUsedElem e;
    ASSERT_TRUE(virtq_get_used(&vq, &e));
    EXPECT_EQ(e.id, 4u);
    EXPECT_EQ(e.len, 0u);

    /* a descriptor of the ring first */
    set_desc(&vq.desc[0], buffer(2), 2, VIRTQ_DESC_F_NEXT, 6);
    set_desc(&vq.desc[6], table, 2 * sizeof(VirtqDesc), VIRTQ_DESC_F_INDIRECT);
    set_desc(&table[0], buffer(3), 3, VIRTQ_DESC_F_NEXT, 1);
    virtq_publish(&vq, 0);
    virtq_kick(&vq);
    ASSERT_TRUE(virtq_get_used(&vq, &e));
    EXPECT_EQ(e.id, 0u);
    EXPECT_EQ(virtio_status(&sDev) & VIRTIO_STATUS_NEEDS_RESET, 0);
}

namespace {

/* Publishes 'head' and expects the device to refuse the chain: it sets
   DEVICE_NEEDS_RESET with a configuration change, and gives nothing back
   (2.1.2). */
void expect_refused(Virtq *vq, uint16_t head)
{
    take_isr();
    virtq_publish(vq, head);
    virtq_kick(vq);
    t_delay_us(1000);
    EXPECT_EQ(virtq_used_idx(vq), vq->last_used);
    EXPECT_EQ(virtio_status(&sDev) & VIRTIO_STATUS_NEEDS_RESET,
              VIRTIO_STATUS_NEEDS_RESET);
    EXPECT_EQ(take_isr() & VIRTIO_ISR_CONFIG, (uint32_t)VIRTIO_ISR_CONFIG);
    /* and it stays still until reset */
    send_one(vq, 0);
    t_delay_us(1000);
    EXPECT_EQ(virtq_used_idx(vq), vq->last_used);
}

}

/* A chain that loops, leaves its table, or has a readable descriptor
   after a writable one is not one a driver may make (the descriptor
   table's driver requirements), and the device asks to be reset. Reset,
   it works again. */
TEST(virtio, malformed_chains)
{
    require();
    Virtq vq;

    start(&vq);
    set_desc(&vq.desc[1], buffer(4), 4, VIRTQ_DESC_F_NEXT, 2);
    set_desc(&vq.desc[2], buffer(4), 4, VIRTQ_DESC_F_NEXT, 1);
    expect_refused(&vq, 1);

    start(&vq);
    set_desc(&vq.desc[1], buffer(4), 4, VIRTQ_DESC_F_NEXT, QUEUE_SIZE);
    expect_refused(&vq, 1);

    start(&vq);
    expect_refused(&vq, QUEUE_SIZE + 3);

    start(&vq);
    set_desc(&vq.desc[1], buffer(4), 4,
             VIRTQ_DESC_F_WRITE | VIRTQ_DESC_F_NEXT, 2);
    set_desc(&vq.desc[2], buffer(4), 4, 0);
    expect_refused(&vq, 1);

    /* an indirect table without the feature negotiated */
    start(&vq);
    VirtqDesc *table = (VirtqDesc *)heap_alloc(sizeof(VirtqDesc), 16);
    ASSERT_TRUE(table != nullptr);
    set_desc(&table[0], buffer(4), 4, 0);
    set_desc(&vq.desc[1], table, sizeof(VirtqDesc), VIRTQ_DESC_F_INDIRECT);
    expect_refused(&vq, 1);

    /* a descriptor table where there is no RAM */
    uint64_t nowhere = not_ram();
    start(&vq);
    virtio_queue_select(&sDev, CONSOLE_TX);
    virtio_set_queue_addrs(&sDev, nowhere, (uintptr_t)vq.avail,
                           (uintptr_t)vq.used);
    expect_refused(&vq, 1);

    start(&vq);
    send_one(&vq, 5);
    VirtqUsedElem e;
    ASSERT_TRUE(virtq_get_used(&vq, &e));
    EXPECT_EQ(e.id, 5u);
}

/* A buffer where there is no RAM is handed back, with nothing taken from
   it. */
TEST(virtio, buffer_outside_ram)
{
    require();
    Virtq vq;
    start(&vq);
    set_desc(&vq.desc[0], (void *)(uintptr_t)not_ram(), 16, 0);
    virtq_publish(&vq, 0);
    virtq_kick(&vq);
    VirtqUsedElem e;
    ASSERT_TRUE(virtq_get_used(&vq, &e));
    EXPECT_EQ(e.id, 0u);
    EXPECT_EQ(e.len, 0u);
    EXPECT_EQ(virtio_status(&sDev) & VIRTIO_STATUS_NEEDS_RESET, 0);
}

/* The rings must be aligned (2.7); what a device does with ones that are
   not is its own. */
TEST(virtio, misaligned_rings)
{
    require();
    Virtq vq;
    start(&vq);
    set_desc(&vq.desc[0], buffer(4), 4, 0);
    virtq_publish(&vq, 0);
    virtio_queue_select(&sDev, CONSOLE_TX);
    virtio_set_queue_addrs(&sDev, (uintptr_t)vq.desc, (uintptr_t)vq.avail + 1,
                           (uintptr_t)vq.used);
    virtq_kick(&vq);
    t_delay_us(1000);
    SNAP("avail_odd", "%u %#x", vq.used[1], virtio_status(&sDev));
    virtio_set_queue_addrs(&sDev, (uintptr_t)vq.desc, (uintptr_t)vq.avail,
                           (uintptr_t)vq.used + 1);
    virtq_kick(&vq);
    t_delay_us(1000);
    const volatile uint8_t *u = (const volatile uint8_t *)vq.used;
    SNAP("used_odd", "%02x%02x%02x%02x%02x%02x %#x", u[1], u[2], u[3], u[4],
         u[5], u[6], virtio_status(&sDev));
}

/* Indirect tables that break the driver requirements of Indirect
   Descriptors: nested, continued by NEXT, empty, or not a whole number of
   descriptors. */
TEST(virtio, malformed_indirect)
{
    require();
    if (!(virtio_device_features(&sDev, 0) & VIRTIO_F_INDIRECT_DESC))
        t_skip("indirect descriptors are not offered");
    Virtq vq;
    uint64_t f = VIRTIO_F_VERSION_1 | VIRTIO_F_INDIRECT_DESC;
    VirtqDesc *outer = (VirtqDesc *)heap_alloc(2 * sizeof(VirtqDesc), 16);
    VirtqDesc *inner = (VirtqDesc *)heap_alloc(sizeof(VirtqDesc), 16);
    ASSERT_TRUE(outer && inner);
    set_desc(&inner[0], buffer(4), 4, 0);

    start(&vq, f);
    set_desc(&outer[0], inner, sizeof(VirtqDesc), VIRTQ_DESC_F_INDIRECT);
    set_desc(&vq.desc[1], outer, sizeof(VirtqDesc), VIRTQ_DESC_F_INDIRECT);
    expect_refused(&vq, 1);

    start(&vq, f);
    set_desc(&vq.desc[1], inner, sizeof(VirtqDesc),
             VIRTQ_DESC_F_INDIRECT | VIRTQ_DESC_F_NEXT, 2);
    set_desc(&vq.desc[2], buffer(4), 4, 0);
    expect_refused(&vq, 1);

    start(&vq, f);
    set_desc(&vq.desc[1], inner, 0, VIRTQ_DESC_F_INDIRECT);
    expect_refused(&vq, 1);

    start(&vq, f);
    set_desc(&vq.desc[1], inner, sizeof(VirtqDesc) + 4, VIRTQ_DESC_F_INDIRECT);
    expect_refused(&vq, 1);
}

/* Configuration space: reads past its end find nothing, and the
   generation stays put while nothing changes (2.5.1, 4.2.2). MMIO has no
   shared memory regions, whose length then reads -1 (4.2.2). */
TEST(virtio, config_space)
{
    require();
    uint32_t gen = virtio_config_generation(&sDev);
    uint32_t cols = virtio_config_read(&sDev, 0, 1);
    uint32_t size = virtio_config_read(&sDev, 0, 2);
    EXPECT_EQ(virtio_config_generation(&sDev), gen);
    EXPECT_EQ(virtio_config_read(&sDev, 0, 1), cols);
    EXPECT_EQ(virtio_config_read(&sDev, 1, 0), cols >> 8);
    EXPECT_EQ(size & 0xffff, cols);
    /* the console's fields are read only; a driver may not write them,
       and writing them as they are changes nothing */
    virtio_config_write(&sDev, 0, cols & 0xff, 0);
    virtio_config_write(&sDev, 0, cols, 1);
    virtio_config_write(&sDev, 0, size, 2);
    virtio_config_write(&sDev, 0x40, 0x12345678, 2);
    EXPECT_EQ(virtio_config_read(&sDev, 0, 2), size);
    SNAP("past_end", "%#x %#x %#x", virtio_config_read(&sDev, 0x40, 2),
         virtio_config_read(&sDev, 0xfe, 1), virtio_config_read(&sDev, 0xff, 0));
    if (!sDev.pci) {
        mmio_write32(sDev.base + VIRTIO_MMIO_SHM_SEL, 0);
        EXPECT_EQ(mmio_read32(sDev.base + 0xb0), 0xffffffffu);
        EXPECT_EQ(mmio_read32(sDev.base + 0xb4), 0xffffffffu);
    }
}
