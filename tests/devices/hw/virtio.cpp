/*
 * Virtio devices, over MMIO and PCI
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
#include "virtio.h"

#include "heap.h"
#include "mmio.h"
#include "test.h"

#define PCI_VENDOR_VIRTIO 0x1af4
#define PCI_CAP_VENDOR    0x09

#define MSIX_FLAGS  2
#define MSIX_TABLE  4
#define MSIX_ENABLE 0x8000

#define BARRIER() __atomic_thread_fence(__ATOMIC_SEQ_CST)

namespace {

bool mmio_probe(const HwDevice *d, VirtioDev *dev)
{
    const HwRegion *r = hw_region(d, HW_MMIO);
    if (r == nullptr ||
        mmio_read32(r->base + VIRTIO_MMIO_MAGIC) != VIRTIO_MMIO_MAGIC_VALUE ||
        mmio_read32(r->base + VIRTIO_MMIO_VERSION) != 2)
        return false;
    *dev = VirtioDev {};
    dev->hw = d;
    dev->base = r->base;
    dev->device_id = mmio_read32(r->base + VIRTIO_MMIO_DEVICE_ID);
    return true;
}

/* The structures by the capabilities that point at them, the first of
   each type as the specification asks a driver to use. */
bool pci_probe(PciFunc *f, VirtioDev *dev)
{
    if (f->vendor != PCI_VENDOR_VIRTIO || f->device < 0x1040 ||
        f->device > 0x107f)
        return false;
    *dev = VirtioDev {};
    dev->pci = true;
    dev->fn = f;
    dev->device_id = f->device - 0x1040;
    dev->common.bar = dev->notify.bar = dev->isr.bar = dev->device.bar = -1;
    uint32_t cap = pci_read8(f, PCI_CAP_PTR) & ~3u;
    for (int n = 0; cap && n < 48; n++) {
        if (pci_read8(f, cap) == PCI_CAP_VENDOR) {
            uint8_t type = pci_read8(f, cap + 3);
            VirtioRegion r = {pci_read8(f, cap + 4), pci_read32(f, cap + 8),
                              pci_read32(f, cap + 12)};
            VirtioRegion *slot = nullptr;
            switch (type) {
            case VIRTIO_PCI_CAP_COMMON:
                slot = &dev->common;
                break;
            case VIRTIO_PCI_CAP_NOTIFY:
                slot = &dev->notify;
                break;
            case VIRTIO_PCI_CAP_ISR:
                slot = &dev->isr;
                break;
            case VIRTIO_PCI_CAP_DEVICE:
                slot = &dev->device;
                break;
            }
            if (type == VIRTIO_PCI_CAP_PCI && dev->pci_cfg_cap == 0)
                dev->pci_cfg_cap = cap;
            if (slot && slot->bar < 0) {
                *slot = r;
                if (type == VIRTIO_PCI_CAP_NOTIFY)
                    dev->notify_mult = pci_read32(f, cap + 16);
            }
        }
        cap = pci_read8(f, cap + 1) & ~3u;
    }
    return dev->common.bar >= 0 && dev->notify.bar >= 0 &&
           dev->isr.bar >= 0;
}

uint32_t mrd(const VirtioDev *dev, uint32_t off)
{
    return mmio_read32(dev->base + off);
}

void mwr(const VirtioDev *dev, uint32_t off, uint32_t v)
{
    mmio_write32(dev->base + off, v);
}

}


bool virtio_find(uint32_t device_id, int index, VirtioDev *dev)
{
    for (int i = 0; i < registry_count(); i++) {
        const HwDevice *d = registry_at(i);
        if ((hw_is(d, "virtio,mmio") || hw_is(d, "LNRO0005")) &&
            mmio_probe(d, dev) && dev->device_id == device_id &&
            index-- == 0)
            return true;
    }
    pci_enumerate();
    for (int i = 0; i < pci_func_count(); i++) {
        if (pci_probe(pci_func(i), dev) && dev->device_id == device_id &&
            index-- == 0)
            return true;
    }
    return false;
}


void virtio_require(uint32_t device_id, VirtioDev *dev, int index)
{
    if (!virtio_find(device_id, index, dev))
        t_skip("no virtio device %u", device_id);
}


bool virtio_console_present()
{
    VirtioDev dev;
    return virtio_find(VIRTIO_ID_CONSOLE, 0, &dev);
}


const char *virtio_transport(const VirtioDev *dev)
{
    return dev->pci ? "pci" : "mmio";
}


uint32_t virtio_common_read(const VirtioDev *dev, uint32_t offset,
                            int size_log2)
{
    return pci_bar_read(dev->fn, dev->common.bar, dev->common.offset + offset,
                        size_log2);
}


void virtio_common_write(const VirtioDev *dev, uint32_t offset,
                         uint32_t value, int size_log2)
{
    pci_bar_write(dev->fn, dev->common.bar, dev->common.offset + offset, value,
                  size_log2);
}


uint8_t virtio_status(const VirtioDev *dev)
{
    if (dev->pci)
        return virtio_common_read(dev, VIRTIO_PCI_DEVICE_STATUS, 0);
    return mrd(dev, VIRTIO_MMIO_STATUS);
}


void virtio_set_status(const VirtioDev *dev, uint8_t status)
{
    if (dev->pci)
        virtio_common_write(dev, VIRTIO_PCI_DEVICE_STATUS, status, 0);
    else
        mwr(dev, VIRTIO_MMIO_STATUS, status);
}


void virtio_reset(const VirtioDev *dev)
{
    virtio_set_status(dev, 0);
    for (int i = 0; i < 1000 && virtio_status(dev) != 0; i++)
        arch_cpu_relax();
}


uint32_t virtio_device_features(const VirtioDev *dev, uint32_t sel)
{
    if (dev->pci) {
        virtio_common_write(dev, VIRTIO_PCI_DEVICE_FEATURE_SEL, sel, 2);
        return virtio_common_read(dev, VIRTIO_PCI_DEVICE_FEATURE, 2);
    }
    mwr(dev, VIRTIO_MMIO_DEVICE_FEATURES_SEL, sel);
    return mrd(dev, VIRTIO_MMIO_DEVICE_FEATURES);
}


void virtio_set_driver_features(const VirtioDev *dev, uint32_t sel,
                                uint32_t value)
{
    if (dev->pci) {
        virtio_common_write(dev, VIRTIO_PCI_DRIVER_FEATURE_SEL, sel, 2);
        virtio_common_write(dev, VIRTIO_PCI_DRIVER_FEATURE, value, 2);
        return;
    }
    mwr(dev, VIRTIO_MMIO_DRIVER_FEATURES_SEL, sel);
    mwr(dev, VIRTIO_MMIO_DRIVER_FEATURES, value);
}


void virtio_queue_select(const VirtioDev *dev, uint32_t q)
{
    if (dev->pci)
        virtio_common_write(dev, VIRTIO_PCI_QUEUE_SELECT, q, 1);
    else
        mwr(dev, VIRTIO_MMIO_QUEUE_SEL, q);
}


uint32_t virtio_queue_max(const VirtioDev *dev)
{
    if (dev->pci)
        return virtio_common_read(dev, VIRTIO_PCI_QUEUE_SIZE, 1);
    return mrd(dev, VIRTIO_MMIO_QUEUE_NUM_MAX);
}


uint32_t virtio_queue_size(const VirtioDev *dev)
{
    if (dev->pci)
        return virtio_common_read(dev, VIRTIO_PCI_QUEUE_SIZE, 1);
    return mrd(dev, VIRTIO_MMIO_QUEUE_NUM);
}


void virtio_set_queue_size(const VirtioDev *dev, uint32_t size)
{
    if (dev->pci)
        virtio_common_write(dev, VIRTIO_PCI_QUEUE_SIZE, size, 1);
    else
        mwr(dev, VIRTIO_MMIO_QUEUE_NUM, size);
}


uint32_t virtio_queue_ready(const VirtioDev *dev)
{
    if (dev->pci)
        return virtio_common_read(dev, VIRTIO_PCI_QUEUE_ENABLE, 1);
    return mrd(dev, VIRTIO_MMIO_QUEUE_READY);
}


void virtio_set_queue_ready(const VirtioDev *dev, uint32_t ready)
{
    if (dev->pci)
        virtio_common_write(dev, VIRTIO_PCI_QUEUE_ENABLE, ready, 1);
    else
        mwr(dev, VIRTIO_MMIO_QUEUE_READY, ready);
}


void virtio_set_queue_addrs(const VirtioDev *dev, uint64_t desc,
                            uint64_t avail, uint64_t used)
{
    if (dev->pci) {
        virtio_common_write(dev, VIRTIO_PCI_QUEUE_DESC, (uint32_t)desc, 2);
        virtio_common_write(dev, VIRTIO_PCI_QUEUE_DESC + 4, desc >> 32, 2);
        virtio_common_write(dev, VIRTIO_PCI_QUEUE_DRIVER, (uint32_t)avail, 2);
        virtio_common_write(dev, VIRTIO_PCI_QUEUE_DRIVER + 4, avail >> 32, 2);
        virtio_common_write(dev, VIRTIO_PCI_QUEUE_DEVICE, (uint32_t)used, 2);
        virtio_common_write(dev, VIRTIO_PCI_QUEUE_DEVICE + 4, used >> 32, 2);
        return;
    }
    mwr(dev, VIRTIO_MMIO_QUEUE_DESC_LOW, (uint32_t)desc);
    mwr(dev, VIRTIO_MMIO_QUEUE_DESC_HIGH, desc >> 32);
    mwr(dev, VIRTIO_MMIO_QUEUE_AVAIL_LOW, (uint32_t)avail);
    mwr(dev, VIRTIO_MMIO_QUEUE_AVAIL_HIGH, avail >> 32);
    mwr(dev, VIRTIO_MMIO_QUEUE_USED_LOW, (uint32_t)used);
    mwr(dev, VIRTIO_MMIO_QUEUE_USED_HIGH, used >> 32);
}


/* PCI: at the queue's notify offset times the multiplier, the queue's
   index as the value (4.1.4.4). */
void virtio_notify(const VirtioDev *dev, uint32_t q)
{
    BARRIER();
    if (!dev->pci) {
        mwr(dev, VIRTIO_MMIO_QUEUE_NOTIFY, q);
        return;
    }
    virtio_queue_select(dev, q);
    uint32_t off = virtio_common_read(dev, VIRTIO_PCI_QUEUE_NOTIFY_OFF, 1);
    pci_bar_write(dev->fn, dev->notify.bar,
                  dev->notify.offset + off * dev->notify_mult, q, 1);
}


uint32_t virtio_isr(const VirtioDev *dev)
{
    if (dev->pci)
        return pci_bar_read(dev->fn, dev->isr.bar, dev->isr.offset, 0);
    return mrd(dev, VIRTIO_MMIO_INTERRUPT_STATUS);
}


void virtio_ack(const VirtioDev *dev, uint32_t bits)
{
    if (!dev->pci)
        mwr(dev, VIRTIO_MMIO_INTERRUPT_ACK, bits);
}


uint32_t virtio_config_generation(const VirtioDev *dev)
{
    if (dev->pci)
        return virtio_common_read(dev, VIRTIO_PCI_CONFIG_GENERATION, 0);
    return mrd(dev, VIRTIO_MMIO_CONFIG_GENERATION);
}


uint32_t virtio_config_read(const VirtioDev *dev, uint32_t offset,
                            int size_log2)
{
    if (dev->pci)
        return pci_bar_read(dev->fn, dev->device.bar,
                            dev->device.offset + offset, size_log2);
    uint64_t a = dev->base + VIRTIO_MMIO_CONFIG + offset;
    switch (size_log2) {
    case 0:
        return mmio_read8(a);
    case 1:
        return mmio_read16(a);
    default:
        return mmio_read32(a);
    }
}


void virtio_config_write(const VirtioDev *dev, uint32_t offset,
                         uint32_t value, int size_log2)
{
    if (dev->pci) {
        pci_bar_write(dev->fn, dev->device.bar, dev->device.offset + offset,
                      value, size_log2);
        return;
    }
    uint64_t a = dev->base + VIRTIO_MMIO_CONFIG + offset;
    switch (size_log2) {
    case 0:
        mmio_write8(a, value);
        break;
    case 1:
        mmio_write16(a, value);
        break;
    default:
        mmio_write32(a, value);
        break;
    }
}


/* The initialization sequence of 3.1.1, up to FEATURES_OK. */
bool virtio_negotiate(const VirtioDev *dev, uint64_t wanted,
                      uint64_t *accepted)
{
    virtio_reset(dev);
    virtio_set_status(dev, VIRTIO_STATUS_ACKNOWLEDGE);
    virtio_set_status(dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    uint64_t offered = virtio_device_features(dev, 0) |
                       (uint64_t)virtio_device_features(dev, 1) << 32;
    uint64_t take = offered & wanted;
    virtio_set_driver_features(dev, 0, (uint32_t)take);
    virtio_set_driver_features(dev, 1, take >> 32);
    uint8_t s = VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER |
                VIRTIO_STATUS_FEATURES_OK;
    virtio_set_status(dev, s);
    if (accepted)
        *accepted = take;
    return (virtio_status(dev) & VIRTIO_STATUS_FEATURES_OK) != 0;
}


bool virtio_route_irq(const VirtioDev *dev, IrqHandler fn, void *ctx)
{
    if (dev->pci)
        return pci_intx_route(dev->fn, fn, ctx);
    return irq_route(dev->hw, 0, fn, ctx);
}


void virtio_unroute_irq(const VirtioDev *dev)
{
    if (dev->pci)
        pci_intx_unroute(dev->fn);
    else
        irq_unroute(dev->hw, 0);
}


bool virtio_msix_route(const VirtioDev *dev, int n, IrqHandler fn, void *ctx)
{
    if (!dev->pci || dev->fn->host->msi == nullptr)
        return false;
    uint32_t cap = pci_find_cap(dev->fn, PCI_CAP_MSIX);
    if (cap == 0)
        return false;
    uint64_t addr;
    uint32_t data;
    if (!dev->fn->host->msi->Message(n, fn, ctx, &addr, &data))
        return false;
    uint32_t table = pci_read32(dev->fn, cap + MSIX_TABLE);
    int bar = table & 7;
    uint32_t entry = (table & ~7u) + 16 * n;
    pci_bar_write(dev->fn, bar, entry, (uint32_t)addr, 2);
    pci_bar_write(dev->fn, bar, entry + 4, addr >> 32, 2);
    pci_bar_write(dev->fn, bar, entry + 8, data, 2);
    pci_bar_write(dev->fn, bar, entry + 12, 0, 2);
    uint16_t ctrl = pci_read16(dev->fn, cap + MSIX_FLAGS);
    pci_write16(dev->fn, cap + MSIX_FLAGS, (ctrl & 0x7ff) | MSIX_ENABLE);
    return true;
}


//#pragma mark - virtqueues

bool virtq_setup(const VirtioDev *dev, uint32_t index, uint16_t num,
                 Virtq *vq)
{
    virtio_queue_select(dev, index);
    uint32_t max = virtio_queue_max(dev);
    if (max == 0 || max < num)
        return false;
    *vq = Virtq {};
    vq->dev = dev;
    vq->index = index;
    vq->num = num;
    vq->desc = (VirtqDesc *)heap_alloc(4096, 4096);
    vq->avail = (volatile uint16_t *)heap_alloc(4096, 4096);
    vq->used = (volatile uint16_t *)heap_alloc(4096, 4096);
    if (!vq->desc || !vq->avail || !vq->used)
        return false;
    memset(vq->desc, 0, 4096);
    memset((void *)vq->avail, 0, 4096);
    memset((void *)vq->used, 0, 4096);
    virtio_set_queue_size(dev, num);
    virtio_set_queue_addrs(dev, (uintptr_t)vq->desc, (uintptr_t)vq->avail,
                           (uintptr_t)vq->used);
    virtio_set_queue_ready(dev, 1);
    return true;
}


void virtq_publish(Virtq *vq, uint16_t head)
{
    vq->avail[2 + vq->next_avail % vq->num] = head;
    BARRIER();
    vq->next_avail++;
    vq->avail[1] = vq->next_avail;
    BARRIER();
}


void virtq_kick(Virtq *vq)
{
    virtio_notify(vq->dev, vq->index);
}


uint16_t virtq_used_idx(const Virtq *vq)
{
    BARRIER();
    return vq->used[1];
}


VirtqUsedElem virtq_used_elem(const Virtq *vq, uint16_t i)
{
    BARRIER();
    const volatile uint32_t *ring = (const volatile uint32_t *)(vq->used + 2);
    uint16_t slot = i % vq->num;
    return VirtqUsedElem {ring[2 * slot], ring[2 * slot + 1]};
}


bool virtq_get_used(Virtq *vq, VirtqUsedElem *elem, uint64_t timeout_us)
{
    if (!t_wait_until([vq] { return virtq_used_idx(vq) != vq->last_used; },
                      timeout_us))
        return false;
    *elem = virtq_used_elem(vq, vq->last_used);
    vq->last_used++;
    return true;
}


void virtq_set_avail_flags(Virtq *vq, uint16_t flags)
{
    vq->avail[0] = flags;
    BARRIER();
}


uint16_t virtq_used_flags(const Virtq *vq)
{
    BARRIER();
    return vq->used[0];
}


void virtq_set_used_event(Virtq *vq, uint16_t idx)
{
    vq->avail[2 + vq->num] = idx;
    BARRIER();
}


uint16_t virtq_avail_event(const Virtq *vq)
{
    BARRIER();
    /* after the 4 byte header and num 8 byte elements */
    return vq->used[2 + 4 * vq->num];
}
