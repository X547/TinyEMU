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
#pragma once

#include "pci.h"
#include "registry.h"

/* By the Virtual I/O Device (VIRTIO) Version 1.2 specification, "the
   virtio specification" in the tests: the device status (2.1), the
   reserved feature bits (6), the split virtqueue (2.7) and the MMIO (4.2)
   and PCI (4.1) transports. Only the virtio 1 interfaces. */

#define VIRTIO_ID_NET     1
#define VIRTIO_ID_BLOCK   2
#define VIRTIO_ID_CONSOLE 3
#define VIRTIO_ID_9P      9
#define VIRTIO_ID_SCSI    8
#define VIRTIO_ID_GPU     16
#define VIRTIO_ID_INPUT   18

#define VIRTIO_STATUS_ACKNOWLEDGE 0x01
#define VIRTIO_STATUS_DRIVER      0x02
#define VIRTIO_STATUS_DRIVER_OK   0x04
#define VIRTIO_STATUS_FEATURES_OK 0x08
#define VIRTIO_STATUS_NEEDS_RESET 0x40
#define VIRTIO_STATUS_FAILED      0x80

#define VIRTIO_F_INDIRECT_DESC (1ull << 28)
#define VIRTIO_F_EVENT_IDX     (1ull << 29)
#define VIRTIO_F_VERSION_1     (1ull << 32)

#define VIRTQ_DESC_F_NEXT     1
#define VIRTQ_DESC_F_WRITE    2
#define VIRTQ_DESC_F_INDIRECT 4

#define VIRTQ_AVAIL_F_NO_INTERRUPT 1
#define VIRTQ_USED_F_NO_NOTIFY     1

/* the ISR status and MMIO InterruptStatus bits */
#define VIRTIO_ISR_QUEUE  0x1
#define VIRTIO_ISR_CONFIG 0x2

#define VIRTIO_MSI_NO_VECTOR 0xffff

/* MMIO registers (4.2.2) */
#define VIRTIO_MMIO_MAGIC            0x000
#define VIRTIO_MMIO_VERSION          0x004
#define VIRTIO_MMIO_DEVICE_ID        0x008
#define VIRTIO_MMIO_VENDOR_ID        0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES  0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL 0x014
#define VIRTIO_MMIO_DRIVER_FEATURES  0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL 0x024
#define VIRTIO_MMIO_QUEUE_SEL        0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX    0x034
#define VIRTIO_MMIO_QUEUE_NUM        0x038
#define VIRTIO_MMIO_QUEUE_READY      0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY     0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS 0x060
#define VIRTIO_MMIO_INTERRUPT_ACK    0x064
#define VIRTIO_MMIO_STATUS           0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW   0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH  0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW  0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH 0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW   0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH  0x0a4
#define VIRTIO_MMIO_SHM_SEL          0x0ac
#define VIRTIO_MMIO_QUEUE_RESET      0x0c0
#define VIRTIO_MMIO_CONFIG_GENERATION 0x0fc
#define VIRTIO_MMIO_CONFIG           0x100
#define VIRTIO_MMIO_MAGIC_VALUE      0x74726976 /* "virt" */

/* PCI common configuration (4.1.4.3) */
#define VIRTIO_PCI_DEVICE_FEATURE_SEL 0x00
#define VIRTIO_PCI_DEVICE_FEATURE     0x04
#define VIRTIO_PCI_DRIVER_FEATURE_SEL 0x08
#define VIRTIO_PCI_DRIVER_FEATURE     0x0c
#define VIRTIO_PCI_MSIX_CONFIG        0x10
#define VIRTIO_PCI_NUM_QUEUES         0x12
#define VIRTIO_PCI_DEVICE_STATUS      0x14
#define VIRTIO_PCI_CONFIG_GENERATION  0x15
#define VIRTIO_PCI_QUEUE_SELECT       0x16
#define VIRTIO_PCI_QUEUE_SIZE         0x18
#define VIRTIO_PCI_QUEUE_MSIX_VECTOR  0x1a
#define VIRTIO_PCI_QUEUE_ENABLE       0x1c
#define VIRTIO_PCI_QUEUE_NOTIFY_OFF   0x1e
#define VIRTIO_PCI_QUEUE_DESC         0x20
#define VIRTIO_PCI_QUEUE_DRIVER       0x28
#define VIRTIO_PCI_QUEUE_DEVICE       0x30

/* PCI capability types (4.1.4) */
#define VIRTIO_PCI_CAP_COMMON 1
#define VIRTIO_PCI_CAP_NOTIFY 2
#define VIRTIO_PCI_CAP_ISR    3
#define VIRTIO_PCI_CAP_DEVICE 4
#define VIRTIO_PCI_CAP_PCI    5

struct VirtioRegion {
    int bar;
    uint32_t offset;
    uint32_t length;
};

struct VirtioDev {
    bool pci;
    uint32_t device_id;
    /* MMIO */
    const HwDevice *hw;
    uint64_t base;
    /* PCI: the structures the capabilities point at */
    PciFunc *fn;
    VirtioRegion common;
    VirtioRegion notify;
    VirtioRegion isr;
    VirtioRegion device;
    uint32_t notify_mult;
    /* the configuration access capability, 0 when there is none */
    uint32_t pci_cfg_cap;
};

/* The 'index'th virtio device with this ID, MMIO ones first; false when
   there is none. */
bool virtio_find(uint32_t device_id, int index, VirtioDev *dev);
/* As virtio_find(), or the test is skipped. */
void virtio_require(uint32_t device_id, VirtioDev *dev, int index = 0);
/* Whether a virtio console takes temu's console input. */
bool virtio_console_present();
const char *virtio_transport(const VirtioDev *dev);

/* The transport's registers. */
uint8_t virtio_status(const VirtioDev *dev);
void virtio_set_status(const VirtioDev *dev, uint8_t status);
/* Writes 0 and waits for it to read back, as the driver must. */
void virtio_reset(const VirtioDev *dev);
uint32_t virtio_device_features(const VirtioDev *dev, uint32_t sel);
void virtio_set_driver_features(const VirtioDev *dev, uint32_t sel,
                                uint32_t value);
void virtio_queue_select(const VirtioDev *dev, uint32_t q);
uint32_t virtio_queue_max(const VirtioDev *dev);   /* MMIO QueueNumMax */
uint32_t virtio_queue_size(const VirtioDev *dev);
void virtio_set_queue_size(const VirtioDev *dev, uint32_t size);
uint32_t virtio_queue_ready(const VirtioDev *dev);
void virtio_set_queue_ready(const VirtioDev *dev, uint32_t ready);
void virtio_set_queue_addrs(const VirtioDev *dev, uint64_t desc,
                            uint64_t avail, uint64_t used);
void virtio_notify(const VirtioDev *dev, uint32_t q);
/* MMIO InterruptStatus, or PCI ISR status, which reading clears. */
uint32_t virtio_isr(const VirtioDev *dev);
/* MMIO only. */
void virtio_ack(const VirtioDev *dev, uint32_t bits);
uint32_t virtio_config_generation(const VirtioDev *dev);
uint32_t virtio_config_read(const VirtioDev *dev, uint32_t offset,
                            int size_log2);
void virtio_config_write(const VirtioDev *dev, uint32_t offset,
                         uint32_t value, int size_log2);

/* The PCI common configuration, raw. */
uint32_t virtio_common_read(const VirtioDev *dev, uint32_t offset,
                            int size_log2);
void virtio_common_write(const VirtioDev *dev, uint32_t offset,
                         uint32_t value, int size_log2);

/* Reset, ACKNOWLEDGE, DRIVER, the features offered and 'wanted' in
   common accepted, FEATURES_OK. False when the device did not keep
   FEATURES_OK; the accepted features in 'accepted'. */
bool virtio_negotiate(const VirtioDev *dev, uint64_t wanted,
                      uint64_t *accepted = nullptr);

/* The device's interrupt, by INTx on PCI. */
bool virtio_route_irq(const VirtioDev *dev, IrqHandler fn, void *ctx);
void virtio_unroute_irq(const VirtioDev *dev);
/* PCI: MSI-X on, with table entry 'n' delivering to 'fn'. False without
   MSI-X or a receiver for it. */
bool virtio_msix_route(const VirtioDev *dev, int n, IrqHandler fn, void *ctx);

struct VirtqDesc {
    uint64_t addr;
    uint32_t len;
    uint16_t flags;
    uint16_t next;
};

struct VirtqUsedElem {
    uint32_t id;
    uint32_t len;
};

/* A split virtqueue in memory of its own. */
struct Virtq {
    const VirtioDev *dev;
    uint32_t index;
    uint16_t num;
    VirtqDesc *desc;
    volatile uint16_t *avail;  /* flags, idx, ring[num], used_event */
    volatile uint16_t *used;   /* flags, idx, then the elements */
    uint16_t next_avail;
    uint16_t last_used;
};

/* Allocate a queue of 'num' entries for queue 'index', give the device
   its addresses and enable it. False when the device has no such queue
   or it is smaller. */
bool virtq_setup(const VirtioDev *dev, uint32_t index, uint16_t num,
                 Virtq *vq);
/* Make descriptor 'head' available, without notifying. */
void virtq_publish(Virtq *vq, uint16_t head);
void virtq_kick(Virtq *vq);
uint16_t virtq_used_idx(const Virtq *vq);
VirtqUsedElem virtq_used_elem(const Virtq *vq, uint16_t i);
/* The next used element, waiting up to 'timeout_us' for one; false when
   none came. */
bool virtq_get_used(Virtq *vq, VirtqUsedElem *elem,
                    uint64_t timeout_us = 1000000);
void virtq_set_avail_flags(Virtq *vq, uint16_t flags);
uint16_t virtq_used_flags(const Virtq *vq);
/* The event index fields: used_event in the avail ring, avail_event in
   the used ring. */
void virtq_set_used_event(Virtq *vq, uint16_t idx);
uint16_t virtq_avail_event(const Virtq *vq);
