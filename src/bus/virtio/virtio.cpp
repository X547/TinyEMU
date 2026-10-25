/*
 * VIRTIO driver
 * 
 * Copyright (c) 2016 Fabrice Bellard
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <stdarg.h>
#include <limits.h>
#include <atomic>

#include "cutils.h"
#include "fdt.h"
#include "machine.h"
#include "virtio.h"

//#define DEBUG_VIRTIO

/* MMIO addresses - from the Linux kernel */
#define VIRTIO_MMIO_MAGIC_VALUE		0x000
#define VIRTIO_MMIO_VERSION		0x004
#define VIRTIO_MMIO_DEVICE_ID		0x008
#define VIRTIO_MMIO_VENDOR_ID		0x00c
#define VIRTIO_MMIO_DEVICE_FEATURES	0x010
#define VIRTIO_MMIO_DEVICE_FEATURES_SEL	0x014
#define VIRTIO_MMIO_DRIVER_FEATURES	0x020
#define VIRTIO_MMIO_DRIVER_FEATURES_SEL	0x024
#define VIRTIO_MMIO_GUEST_PAGE_SIZE	0x028 /* version 1 only */
#define VIRTIO_MMIO_QUEUE_SEL		0x030
#define VIRTIO_MMIO_QUEUE_NUM_MAX	0x034
#define VIRTIO_MMIO_QUEUE_NUM		0x038
#define VIRTIO_MMIO_QUEUE_ALIGN		0x03c /* version 1 only */
#define VIRTIO_MMIO_QUEUE_PFN		0x040 /* version 1 only */
#define VIRTIO_MMIO_QUEUE_READY		0x044
#define VIRTIO_MMIO_QUEUE_NOTIFY	0x050
#define VIRTIO_MMIO_INTERRUPT_STATUS	0x060
#define VIRTIO_MMIO_INTERRUPT_ACK	0x064
#define VIRTIO_MMIO_STATUS		0x070
#define VIRTIO_MMIO_QUEUE_DESC_LOW	0x080
#define VIRTIO_MMIO_QUEUE_DESC_HIGH	0x084
#define VIRTIO_MMIO_QUEUE_AVAIL_LOW	0x090
#define VIRTIO_MMIO_QUEUE_AVAIL_HIGH	0x094
#define VIRTIO_MMIO_QUEUE_USED_LOW	0x0a0
#define VIRTIO_MMIO_QUEUE_USED_HIGH	0x0a4
#define VIRTIO_MMIO_SHM_LEN_LOW		0x0b0
#define VIRTIO_MMIO_SHM_LEN_HIGH	0x0b4
#define VIRTIO_MMIO_CONFIG_GENERATION	0x0fc
#define VIRTIO_MMIO_CONFIG		0x100

/* PCI registers */
#define VIRTIO_PCI_DEVICE_FEATURE_SEL	0x000
#define VIRTIO_PCI_DEVICE_FEATURE	0x004
#define VIRTIO_PCI_GUEST_FEATURE_SEL	0x008
#define VIRTIO_PCI_GUEST_FEATURE	0x00c
#define VIRTIO_PCI_MSIX_CONFIG          0x010
#define VIRTIO_PCI_NUM_QUEUES           0x012
#define VIRTIO_PCI_DEVICE_STATUS        0x014
#define VIRTIO_PCI_CONFIG_GENERATION    0x015
#define VIRTIO_PCI_QUEUE_SEL		0x016
#define VIRTIO_PCI_QUEUE_SIZE	        0x018
#define VIRTIO_PCI_QUEUE_MSIX_VECTOR    0x01a
#define VIRTIO_PCI_QUEUE_ENABLE         0x01c
#define VIRTIO_PCI_QUEUE_NOTIFY_OFF     0x01e
#define VIRTIO_PCI_QUEUE_DESC_LOW	0x020
#define VIRTIO_PCI_QUEUE_DESC_HIGH	0x024
#define VIRTIO_PCI_QUEUE_AVAIL_LOW	0x028
#define VIRTIO_PCI_QUEUE_AVAIL_HIGH	0x02c
#define VIRTIO_PCI_QUEUE_USED_LOW	0x030
#define VIRTIO_PCI_QUEUE_USED_HIGH	0x034

#define VIRTIO_PCI_CFG_OFFSET          0x0000
#define VIRTIO_PCI_ISR_OFFSET          0x1000
#define VIRTIO_PCI_CONFIG_OFFSET       0x2000
#define VIRTIO_PCI_NOTIFY_OFFSET       0x3000
#define VIRTIO_PCI_MSIX_TABLE_OFFSET   0x4000
#define VIRTIO_PCI_MSIX_PBA_OFFSET     0x5000

/* The whole BAR, sized to hold everything above. */
#define VIRTIO_PCI_BAR                 4
#define VIRTIO_PCI_BAR_SIZE            0x8000

/* the structures the capabilities point at */
#define VIRTIO_PCI_CAP_COMMON 1
#define VIRTIO_PCI_CAP_NOTIFY 2
#define VIRTIO_PCI_CAP_ISR    3
#define VIRTIO_PCI_CAP_DEVICE 4
#define VIRTIO_PCI_CAP_PCI    5

#define VIRTIO_PCI_CAP_LEN 16

/* Interrupt causes reported through the ISR register. */
#define VIRTIO_INT_USED_RING 1
#define VIRTIO_INT_CONFIG    2


#define VRING_DESC_F_NEXT	1
#define VRING_DESC_F_WRITE	2
#define VRING_DESC_F_INDIRECT	4

typedef struct {
    uint64_t addr;
    uint32_t len;
    uint16_t flags; /* VRING_DESC_F_x */
    uint16_t next;
} VIRTIODesc;


/* What the device offers: its own features, and those of the transport
   and the rings. */
static uint64_t virtio_offered_features(VIRTIODevice *s)
{
    return s->device_features | VIRTIO_F_INDIRECT_DESC | VIRTIO_F_EVENT_IDX |
           VIRTIO_F_VERSION_1;
}


/* The half of the offered features that the select register names. */
static uint32_t virtio_read_device_features(VIRTIODevice *s)
{
    if (s->device_features_sel >= 2)
        return 0;
    return virtio_offered_features(s) >> (32 * s->device_features_sel);
}


/* The half of the accepted features that the select register names. A
   device reads them to tell a driver that knows virtio 1.0 from one that
   does not. They are settled once the driver sets FEATURES_OK. */
static void virtio_write_driver_features(VIRTIODevice *s, uint32_t val)
{
    if (s->status & VIRTIO_STATUS_FEATURES_OK)
        return;
    if (s->driver_features_sel == 0) {
        s->driver_features = (s->driver_features & ~0xffffffffULL) | val;
    } else if (s->driver_features_sel == 1) {
        s->driver_features = (s->driver_features & 0xffffffffULL) |
                             ((uint64_t)val << 32);
    }
}


static void virtio_reset(VIRTIODevice *s)
{
    int i;

    s->status = 0;
    s->queue_sel = 0;
    s->device_features_sel = 0;
    s->driver_features_sel = 0;
    s->driver_features = 0;
    s->int_status = 0;
    s->config_msix_vector = PCI_MSIX_NO_VECTOR;
    for(i = 0; i < MAX_QUEUE; i++) {
        QueueState *qs = &s->queue[i];
        qs->ready = 0;
        qs->num = MAX_QUEUE_NUM;
        qs->desc_addr = 0;
        qs->avail_addr = 0;
        qs->used_addr = 0;
        qs->last_avail_idx = 0;
        qs->msix_vector = PCI_MSIX_NO_VECTOR;
    }
    /* The MSI-X table itself survives a device reset: it belongs to the PCI
       function, not to the virtio protocol running on top of it. */
}


static void queue_notify_any(VIRTIODevice *s, int queue_idx);

/* Writing 0 resets the device. FEATURES_OK holds only when the driver
   accepted nothing that was not offered, and the queues start with
   DRIVER_OK: buffers a driver added before are taken then. */
static void virtio_write_status(VIRTIODevice *s, uint32_t val)
{
    uint32_t old = s->status;

    if (val == 0) {
        s->status = 0;
        s->irq->Set(0);
        virtio_reset(s);
        s->Reset();
        return;
    }
    if ((val & VIRTIO_STATUS_FEATURES_OK) &&
        !(old & VIRTIO_STATUS_FEATURES_OK) &&
        (s->driver_features & ~virtio_offered_features(s)))
        val &= ~VIRTIO_STATUS_FEATURES_OK;
    s->status = val;
    if ((val & VIRTIO_STATUS_DRIVER_OK) && !(old & VIRTIO_STATUS_DRIVER_OK)) {
        for (uint32_t i = 0; i < s->num_queues; i++)
            queue_notify_any(s, i);
    }
}


//#pragma mark - MSI-X

/* Raise an interrupt for one cause, over whichever mechanism is in use. While
   MSI-X is on, the ISR register plays no part and the INTx line stays low. */
static void virtio_raise_irq(VIRTIODevice *s, uint32_t int_type,
                             uint16_t vector)
{
    if (s->msix.Enabled()) {
        if (vector != PCI_MSIX_NO_VECTOR)
            s->msix.Send(vector);
        return;
    }
    s->int_status |= int_type;
    s->irq->Set(1);
}

class VIRTIOPCITransport final: public VIRTIOTransport {
public:
    VIRTIOPCITransport(VIRTIODevice &dev): VIRTIOTransport(dev) {}

    uint8_t *GetRamPtr(virtio_phys_addr_t paddr, bool is_rw) override
    {
        return pci_device_get_dma_ptr(fDev.pci_dev, paddr, is_rw);
    }

    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;
};


class VIRTIOMMIOTransport final: public VIRTIOTransport {
public:
    VIRTIOMMIOTransport(VIRTIODevice &dev): VIRTIOTransport(dev) {}

    uint8_t *GetRamPtr(virtio_phys_addr_t paddr, bool is_rw) override
    {
        return fDev.mem_map->GetRamPtr(paddr, is_rw);
    }

    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;
};


uint8_t *VIRTIODevice::GetRamPtr(virtio_phys_addr_t paddr, bool is_rw)
{
    return transport->GetRamPtr(paddr, is_rw);
}


void VIRTIODevice::SetBar(int bar_num, uint64_t addr, bool enabled)
{
    (void)bar_num;
    mem_range->SetAddr(addr, enabled);
}

/* The notification capability adds the offset multiplier, and the
   configuration access capability its data window. */
static int virtio_add_pci_capability(VIRTIODevice *s, int cfg_type,
                                     int bar, uint32_t offset, uint32_t len,
                                     uint32_t mult)
{
    uint8_t cap[20];
    int cap_len;
    if (cfg_type == VIRTIO_PCI_CAP_NOTIFY || cfg_type == VIRTIO_PCI_CAP_PCI)
        cap_len = 20;
    else
        cap_len = 16;
    memset(cap, 0, cap_len);
    cap[0] = 0x09; /* vendor specific */
    cap[2] = cap_len; /* set by pci_add_capability() */
    cap[3] = cfg_type;
    cap[4] = bar;
    put_le32(cap + 8, offset);
    put_le32(cap + 12, len);
    if (cfg_type == VIRTIO_PCI_CAP_NOTIFY)
        put_le32(cap + 16, mult);
    return pci_add_capability(s->pci_dev, cap, cap_len);
}


/* The configuration access capability: a read of its data window reads
   'length' bytes of the BAR it names at 'offset', and a write writes
   them. */
bool VIRTIODevice::PciCfgAccess(int *size_log2, uint32_t *offset)
{
    uint32_t cap = pci_cfg_cap;
    uint32_t bar = pci_device_get_config(pci_dev, cap + 4, 0);
    uint32_t len = pci_device_get_config(pci_dev, cap + 12, 2);

    *offset = pci_device_get_config(pci_dev, cap + 8, 2);
    if (bar != VIRTIO_PCI_BAR || (len != 1 && len != 2 && len != 4) ||
        *offset % len != 0 || *offset >= VIRTIO_PCI_BAR_SIZE)
        return false;
    *size_log2 = len == 1 ? 0 : len == 2 ? 1 : 2;
    return true;
}

void VIRTIODevice::ConfigRead(uint32_t addr, int size_log2)
{
    int len_log2;
    uint32_t offset;

    (void)addr;
    (void)size_log2;
    if (!PciCfgAccess(&len_log2, &offset))
        return;
    uint32_t val = transport->DeviceRead(offset, len_log2);
    for (int i = 0; i < (1 << len_log2); i++)
        pci_device_set_config8(pci_dev, pci_cfg_cap + 16 + i,
                               get_bits(val, 8 * i, 8));
}

void VIRTIODevice::ConfigWritten(uint32_t addr, int size_log2)
{
    int len_log2;
    uint32_t offset;

    (void)addr;
    (void)size_log2;
    if (!PciCfgAccess(&len_log2, &offset))
        return;
    uint32_t val = pci_device_get_config(pci_dev, pci_cfg_cap + 16, 2);
    transport->DeviceWrite(offset, get_bits(val, 0, 8 << len_log2),
                           len_log2);}

VIRTIODevice::VIRTIODevice(const char *name, uint32_t device_id,
                           uint16_t pci_class, int config_space_size,
                           int num_queues):
    Device(name),
    num_queues(num_queues),
    device_id(device_id),
    vendor_id(0xffff),
    config_space_size(config_space_size),
    fPciClass(pci_class)
{
    assert(num_queues <= MAX_QUEUE);
    assert(config_space_size <= MAX_CONFIG_SPACE_SIZE);
    virtio_reset(this);
}

bool VIRTIODevice::Prepare()
{
    if (ParentBus()->AsPCIBus() != nullptr)
        return true;
    fMmio = AddResource(RES_MMIO, VIRTIO_PAGE_SIZE, VIRTIO_PAGE_SIZE);
    fIrq = AddResource(RES_IRQ, 1);
    return fMmio != nullptr && fIrq != nullptr;
}

void VIRTIODevice::RealizePCI(PCIBus *bus)
{
    VIRTIODevice *s = this;
    uint16_t pci_device_id;
    char name[32];
    int bar_num;

    /* Only the virtio 1.0 layout is implemented, so the identifier and
       revision have to be non-transitional ones: a guest picks the layout
       from them, and a transitional device promises a port window that is
       not here. */
    pci_device_id = 0x1040 + device_id;
    snprintf(name, sizeof(name), "virtio_%04x", pci_device_id);
    s->pci_dev = pci_register_device(bus, name, -1,
                                     0x1af4, pci_device_id, 0x01,
                                     fPciClass);
    pci_device_set_config16(s->pci_dev, 0x2c, 0x1af4);
    /* The type is in the identifier. A non-transitional device should
       have a subsystem of 0x40 or above, and some drivers insist. */
    pci_device_set_config16(s->pci_dev, 0x2e, 0x1100);
    pci_device_set_config8(s->pci_dev, PCI_INTERRUPT_PIN, 1);

    bar_num = VIRTIO_PCI_BAR;
    virtio_add_pci_capability(s, VIRTIO_PCI_CAP_COMMON, bar_num,
                              VIRTIO_PCI_CFG_OFFSET, 0x1000, 0);
    virtio_add_pci_capability(s, VIRTIO_PCI_CAP_ISR, bar_num,
                              VIRTIO_PCI_ISR_OFFSET, 0x1000, 0);
    virtio_add_pci_capability(s, VIRTIO_PCI_CAP_DEVICE, bar_num,
                              VIRTIO_PCI_CONFIG_OFFSET, 0x1000, 0);
    virtio_add_pci_capability(s, VIRTIO_PCI_CAP_NOTIFY, bar_num,
                              VIRTIO_PCI_NOTIFY_OFFSET, 0x1000, 0);
    /* the driver chooses the BAR, offset and length, and moves data */
    s->pci_cfg_cap = virtio_add_pci_capability(s, VIRTIO_PCI_CAP_PCI,
                                               0, 0, 0, 0);
    pci_device_set_wmask(s->pci_dev, s->pci_cfg_cap + 4, 0xff, 1);
    pci_device_set_wmask(s->pci_dev, s->pci_cfg_cap + 8, 0xffffffff, 4);
    pci_device_set_wmask(s->pci_dev, s->pci_cfg_cap + 12, 0xffffffff, 4);
    pci_device_set_wmask(s->pci_dev, s->pci_cfg_cap + 16, 0xffffffff, 4);
    pci_device_set_config_target(s->pci_dev, s->pci_cfg_cap + 16, 4, s);

    /* Declines by itself on a bus whose bridge has no MSI receiver. */
    s->msix.Init(s->pci_dev, bar_num, VIRTIO_MSIX_VECTOR_COUNT,
                 VIRTIO_PCI_MSIX_TABLE_OFFSET, VIRTIO_PCI_MSIX_PBA_OFFSET);

    s->transport = std::make_unique<VIRTIOPCITransport>(*s);
    s->irq = pci_device_get_irq(s->pci_dev, 0);
    s->mem_map = pci_device_get_mem_map(s->pci_dev);
    s->mem_range = s->mem_map->RegisterDevice(0, VIRTIO_PCI_BAR_SIZE,
                                              s->transport.get(),
                                              DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32 | DEVIO_DISABLED);
    pci_register_bar(s->pci_dev, bar_num, VIRTIO_PCI_BAR_SIZE,
                     PCI_ADDRESS_SPACE_MEM, s);
}

bool VIRTIODevice::Realize()
{
    PCIBus *pci_bus = ParentBus()->AsPCIBus();

    if (pci_bus != nullptr) {
        RealizePCI(pci_bus);
        return true;
    }
    SystemBus *sys = static_cast<SystemBus *>(ParentBus());
    irq = sys->IrqSignalFor(fIrq->base);
    if (irq == nullptr) {
        vm_error("%s: bad interrupt line %d\n", Name(), (int)fIrq->base);
        return false;
    }
    mem_map = sys->MemMap();
    transport = std::make_unique<VIRTIOMMIOTransport>(*this);
    mem_range = mem_map->RegisterDevice(fMmio->base, VIRTIO_PAGE_SIZE,
                                        transport.get(),
                                        DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32);
    return true;
}

void VIRTIODevice::BuildFDT(FDTContext &ctx)
{
    /* On PCI the guest finds the device by enumerating configuration
       space, so it must not also appear as a node. */
    if (fMmio == nullptr)
        return;
    ctx.fdt->BeginNodeNum("virtio", fMmio->base);
    ctx.fdt->PropStr("compatible", "virtio,mmio");
    ctx.fdt->PropU64Range("reg", fMmio->base, fMmio->size);
    fdt_prop_irq(ctx, fIrq->base);
    ctx.fdt->EndNode();
}

uint16_t virtio_read16(VIRTIODevice *s, virtio_phys_addr_t addr)
{
    uint8_t *ptr;
    if (addr & 1)
        return 0; /* unaligned access are not supported */
    ptr = s->GetRamPtr(addr, false);
    if (!ptr)
        return 0;
    return *(uint16_t *)ptr;
}

/* The ring entries below the index are read only after it. */
static uint16_t virtio_avail_idx(VIRTIODevice *s, int queue_idx)
{
    uint16_t idx = virtio_read16(s, s->queue[queue_idx].avail_addr + 2);
    std::atomic_thread_fence(std::memory_order_acquire);
    return idx;
}

static void virtio_write16(VIRTIODevice *s, virtio_phys_addr_t addr,
                           uint16_t val)
{
    uint8_t *ptr;
    if (addr & 1)
        return; /* unaligned access are not supported */
    ptr = s->GetRamPtr(addr, true);
    if (!ptr)
        return;
    *(uint16_t *)ptr = val;
}

static void virtio_write32(VIRTIODevice *s, virtio_phys_addr_t addr,
                           uint32_t val)
{
    uint8_t *ptr;
    if (addr & 3)
        return; /* unaligned access are not supported */
    ptr = s->GetRamPtr(addr, true);
    if (!ptr)
        return;
    *(uint32_t *)ptr = val;
}

static int virtio_memcpy_from_ram(VIRTIODevice *s, uint8_t *buf,
                                  virtio_phys_addr_t addr, int count)
{
    uint8_t *ptr;
    int l;

    while (count > 0) {
        l = min_int(count, VIRTIO_PAGE_SIZE - (addr & (VIRTIO_PAGE_SIZE - 1)));
        ptr = s->GetRamPtr(addr, false);
        if (!ptr)
            return -1;
        memcpy(buf, ptr, l);
        addr += l;
        buf += l;
        count -= l;
    }
    return 0;
}

static int virtio_memcpy_to_ram(VIRTIODevice *s, virtio_phys_addr_t addr, 
                                const uint8_t *buf, int count)
{
    uint8_t *ptr;
    int l;

    while (count > 0) {
        l = min_int(count, VIRTIO_PAGE_SIZE - (addr & (VIRTIO_PAGE_SIZE - 1)));
        ptr = s->GetRamPtr(addr, true);
        if (!ptr)
            return -1;
        memcpy(ptr, buf, l);
        addr += l;
        buf += l;
        count -= l;
    }
    return 0;
}

/* A walk along one descriptor chain, which may end in an indirect table.
   A chain that leaves its table, or visits more descriptors than the
   table holds and so loops, is malformed. */
struct DescWalk {
    virtio_phys_addr_t table;
    uint32_t table_size;
    uint32_t steps;
    bool indirect;
    VIRTIODesc desc;
};

static int desc_walk_read(VIRTIODevice *s, DescWalk *w, uint32_t idx)
{
    if (idx >= w->table_size || ++w->steps > w->table_size)
        return -1;
    if (virtio_memcpy_from_ram(s, reinterpret_cast<uint8_t *>(&w->desc),
                               w->table + idx * sizeof(VIRTIODesc),
                               sizeof(VIRTIODesc)) < 0)
        return -1;
    if (!(w->desc.flags & VRING_DESC_F_INDIRECT))
        return 0;
    /* The rest of the chain is in a table of its own, which neither nests
       nor continues past the descriptor naming it, whose write flag means
       nothing. */
    if (w->indirect || (w->desc.flags & VRING_DESC_F_NEXT) ||
        !(s->driver_features & VIRTIO_F_INDIRECT_DESC) ||
        w->desc.len == 0 || w->desc.len % sizeof(VIRTIODesc) != 0 ||
        w->desc.len / sizeof(VIRTIODesc) > VIRTIO_MAX_INDIRECT)
        return -1;
    w->indirect = true;
    w->table = w->desc.addr;
    w->table_size = w->desc.len / sizeof(VIRTIODesc);
    w->steps = 0;
    return desc_walk_read(s, w, 0);
}

/* The head of the chain, or -1 when it is malformed. */
static int desc_walk_start(VIRTIODevice *s, DescWalk *w, int queue_idx,
                           int desc_idx)
{
    QueueState *qs = &s->queue[queue_idx];

    w->table = qs->desc_addr;
    w->table_size = qs->num;
    w->steps = 0;
    w->indirect = false;
    return desc_walk_read(s, w, desc_idx);
}

/* 1 with the next descriptor of the chain, 0 at its end, -1 when it is
   malformed. */
static int desc_walk_next(VIRTIODevice *s, DescWalk *w)
{
    if (!(w->desc.flags & VRING_DESC_F_NEXT))
        return 0;
    return desc_walk_read(s, w, w->desc.next) < 0 ? -1 : 1;
}

static int memcpy_to_from_queue(VIRTIODevice *s, uint8_t *buf,
                                int queue_idx, int desc_idx,
                                int offset, int count, bool to_queue)
{
    DescWalk w;
    int l, f_write_flag, ret;

    if (count == 0)
        return 0;

    if (desc_walk_start(s, &w, queue_idx, desc_idx) < 0)
        return -1;

    if (to_queue) {
        f_write_flag = VRING_DESC_F_WRITE;
        /* find the first write descriptor */
        while ((w.desc.flags & VRING_DESC_F_WRITE) != f_write_flag) {
            if (desc_walk_next(s, &w) <= 0)
                return -1;
        }
    } else {
        f_write_flag = 0;
    }

    /* find the descriptor at offset */
    for(;;) {
        if ((w.desc.flags & VRING_DESC_F_WRITE) != f_write_flag)
            return -1;
        if ((uint32_t)offset < w.desc.len)
            break;
        offset -= w.desc.len;
        if (desc_walk_next(s, &w) <= 0)
            return -1;
    }

    for(;;) {
        l = min_int(count, w.desc.len - offset);
        if (to_queue)
            ret = virtio_memcpy_to_ram(s, w.desc.addr + offset, buf, l);
        else
            ret = virtio_memcpy_from_ram(s, buf, w.desc.addr + offset, l);
        if (ret < 0)
            return -1;
        count -= l;
        if (count == 0)
            break;
        offset += l;
        buf += l;
        if ((uint32_t)offset == w.desc.len) {
            if (desc_walk_next(s, &w) <= 0)
                return -1;
            if ((w.desc.flags & VRING_DESC_F_WRITE) != f_write_flag)
                return -1;
            offset = 0;
        }
    }
    return 0;
}

int memcpy_from_queue(VIRTIODevice *s, void *buf,
                             int queue_idx, int desc_idx,
                             int offset, int count)
{
    return memcpy_to_from_queue(s, static_cast<uint8_t *>(buf),
                                queue_idx, desc_idx, offset, count,
                                false);
}

int memcpy_to_queue(VIRTIODevice *s,
                           int queue_idx, int desc_idx,
                           int offset, const void *buf, int count)
{
    return memcpy_to_from_queue(s, static_cast<uint8_t *>(const_cast<void *>(buf)),
                                queue_idx, desc_idx, offset,
                                count, true);
}

/* After the used index moved to 'new_idx': with the event index, whether
   it passed the driver's used_event; without, whether the driver left the
   interrupt on. */
static bool virtio_used_notification_wanted(VIRTIODevice *s, QueueState *qs,
                                            uint16_t new_idx)
{
    if (!(s->driver_features & VIRTIO_F_EVENT_IDX))
        return !(virtio_read16(s, qs->avail_addr) & VRING_AVAIL_F_NO_INTERRUPT);
    /* the index is out before used_event is read */
    std::atomic_thread_fence(std::memory_order_seq_cst);
    uint16_t used_event = virtio_read16(s, qs->avail_addr + 4 + qs->num * 2);
    return (uint16_t)(new_idx - used_event - 1) < 1;
}

/* signal that the descriptor has been consumed */
void virtio_consume_desc(VIRTIODevice *s,
                                int queue_idx, int desc_idx, int desc_len)
{
    QueueState *qs = &s->queue[queue_idx];
    virtio_phys_addr_t addr;
    uint32_t index;

    index = virtio_read16(s, qs->used_addr + 2);

    addr = qs->used_addr + 4 + (index & (qs->num - 1)) * 8;
    virtio_write32(s, addr, desc_idx);
    virtio_write32(s, addr + 4, desc_len);

    /* Another processor may be reading the ring right now: the element has
       to be there before the index that hands it over. */
    std::atomic_thread_fence(std::memory_order_release);
    virtio_write16(s, qs->used_addr + 2, index + 1);

    if (virtio_used_notification_wanted(s, qs, index + 1))
        virtio_raise_irq(s, VIRTIO_INT_USED_RING, qs->msix_vector);
}

/* The readable descriptors come first, then the writable ones, and each
   part fits in an int. */
int get_desc_rw_size(VIRTIODevice *s,
                             int *pread_size, int *pwrite_size,
                             int queue_idx, int desc_idx)
{
    DescWalk w;
    uint64_t size[2] = {0, 0};
    bool writing = false;
    int ret;

    if (desc_walk_start(s, &w, queue_idx, desc_idx) < 0)
        return -1;
    do {
        if (w.desc.flags & VRING_DESC_F_WRITE)
            writing = true;
        else if (writing)
            return -1;
        size[writing] += w.desc.len;
        if (size[writing] > INT_MAX)
            return -1;
    } while ((ret = desc_walk_next(s, &w)) > 0);
    if (ret < 0)
        return -1;
    *pread_size = size[0];
    *pwrite_size = size[1];
    return 0;
}

void virtio_set_needs_reset(VIRTIODevice *s)
{
    if (s->status & VIRTIO_STATUS_NEEDS_RESET)
        return;
    s->status |= VIRTIO_STATUS_NEEDS_RESET;
    virtio_config_change_notify(s);
}

/* The driver notified the queue, or set DRIVER_OK with buffers in it.
   Nothing is taken from a queue before both it and the driver are
   ready, or once the device needs a reset. */
static void queue_notify_any(VIRTIODevice *s, int queue_idx)
{
    QueueState *qs = &s->queue[queue_idx];

    if (!(s->status & VIRTIO_STATUS_DRIVER_OK) ||
        (s->status & VIRTIO_STATUS_NEEDS_RESET) || !qs->ready)
        return;
    if (qs->manual_recv)
        s->ManualQueueNotify(queue_idx);
    else
        queue_notify(s, queue_idx);
}

bool virtio_queue_peek(VIRTIODevice *s, int queue_idx, int *desc_idx)
{
    QueueState *qs = &s->queue[queue_idx];
    uint16_t avail_idx;

    if (!(s->status & VIRTIO_STATUS_DRIVER_OK) ||
        (s->status & VIRTIO_STATUS_NEEDS_RESET) || !qs->ready)
        return false;
    avail_idx = virtio_avail_idx(s, queue_idx);
    /* with the event index, the driver notifies once it adds past what
       the device has seen */
    if (s->driver_features & VIRTIO_F_EVENT_IDX) {
        virtio_write16(s, qs->used_addr + 4 + qs->num * 8, avail_idx);
        std::atomic_thread_fence(std::memory_order_seq_cst);
        avail_idx = virtio_avail_idx(s, queue_idx);
    }
    if (qs->last_avail_idx == avail_idx)
        return false;
    *desc_idx = virtio_read16(s, qs->avail_addr + 4 +
                              (qs->last_avail_idx & (qs->num - 1)) * 2);
    return true;
}

void virtio_queue_pop(VIRTIODevice *s, int queue_idx)
{
    s->queue[queue_idx].last_avail_idx++;
}

void queue_notify(VIRTIODevice *s, int queue_idx)
{
    int desc_idx, read_size, write_size;

    while (virtio_queue_peek(s, queue_idx, &desc_idx)) {
        if (get_desc_rw_size(s, &read_size, &write_size, queue_idx,
                             desc_idx) < 0) {
            virtio_set_needs_reset(s);
            break;
        }
#ifdef DEBUG_VIRTIO
        if (s->debug & VIRTIO_DEBUG_IO) {
            printf("queue_notify: idx=%d read_size=%d write_size=%d\n",
                   queue_idx, read_size, write_size);
        }
#endif
        if (s->RecvRequest(queue_idx, desc_idx,
                           read_size, write_size) < 0)
            break;
        virtio_queue_pop(s, queue_idx);
    }
}

/* Past the end of the device's configuration space reads 0 and writes
   are dropped. */
static uint32_t virtio_config_read(VIRTIODevice *s, uint32_t offset,
                                   int size_log2)
{
    if (offset + (1u << size_log2) > s->config_space_size)
        return 0;
    switch(size_log2) {
    case 0:
        return s->config_space[offset];
    case 1:
        return get_le16(s->config_space + offset);
    default:
        return get_le32(s->config_space + offset);
    }
}

static void virtio_config_write(VIRTIODevice *s, uint32_t offset,
                                uint32_t val, int size_log2)
{
    if (offset + (1u << size_log2) > s->config_space_size)
        return;
    switch(size_log2) {
    case 0:
        s->config_space[offset] = val;
        break;
    case 1:
        put_le16(s->config_space + offset, val);
        break;
    default:
        put_le32(s->config_space + offset, val);
        break;
    }
    s->ConfigWrite();
}

/* The queue the driver selected, or nullptr when the device has no such
   queue: its registers then read 0 and ignore writes. */
static QueueState *virtio_selected_queue(VIRTIODevice *s)
{
    return s->queue_sel < s->num_queues ? &s->queue[s->queue_sel] : nullptr;
}

uint32_t VIRTIOMMIOTransport::DeviceRead(uint32_t offset, int size_log2)
{
    VIRTIODevice *s = &fDev;
    uint32_t val;

    if (offset >= VIRTIO_MMIO_CONFIG) {
        return virtio_config_read(s, offset - VIRTIO_MMIO_CONFIG, size_log2);
    }

    if (size_log2 == 2) {
        QueueState *qs = virtio_selected_queue(s);
        switch(offset) {
        case VIRTIO_MMIO_MAGIC_VALUE:
            val = 0x74726976;
            break;
        case VIRTIO_MMIO_VERSION:
            val = 2;
            break;
        case VIRTIO_MMIO_DEVICE_ID:
            val = s->device_id;
            break;
        case VIRTIO_MMIO_VENDOR_ID:
            val = s->vendor_id;
            break;
        case VIRTIO_MMIO_DEVICE_FEATURES:
            val = virtio_read_device_features(s);
            break;
        case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
            val = s->device_features_sel;
            break;
        case VIRTIO_MMIO_QUEUE_SEL:
            val = s->queue_sel;
            break;
        case VIRTIO_MMIO_QUEUE_NUM_MAX:
            val = qs ? MAX_QUEUE_NUM : 0;
            break;
        case VIRTIO_MMIO_QUEUE_NUM:
            val = qs ? qs->num : 0;
            break;
        case VIRTIO_MMIO_QUEUE_DESC_LOW:
            val = qs ? qs->desc_addr : 0;
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
            val = qs ? qs->avail_addr : 0;
            break;
        case VIRTIO_MMIO_QUEUE_USED_LOW:
            val = qs ? qs->used_addr : 0;
            break;
#if VIRTIO_ADDR_BITS == 64
        case VIRTIO_MMIO_QUEUE_DESC_HIGH:
            val = qs ? qs->desc_addr >> 32 : 0;
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
            val = qs ? qs->avail_addr >> 32 : 0;
            break;
        case VIRTIO_MMIO_QUEUE_USED_HIGH:
            val = qs ? qs->used_addr >> 32 : 0;
            break;
#endif
        case VIRTIO_MMIO_QUEUE_READY:
            val = qs ? qs->ready : 0;
            break;
        case VIRTIO_MMIO_INTERRUPT_STATUS:
            val = s->int_status;
            break;
        case VIRTIO_MMIO_STATUS:
            val = s->status;
            break;
        case VIRTIO_MMIO_SHM_LEN_LOW:
        case VIRTIO_MMIO_SHM_LEN_HIGH:
            /* there are no shared memory regions */
            val = 0xffffffff;
            break;
        case VIRTIO_MMIO_CONFIG_GENERATION:
            val = s->config_generation;
            break;
        default:
            val = 0;
            break;
        }
    } else {
        val = 0;
    }
#ifdef DEBUG_VIRTIO
    if (s->debug & VIRTIO_DEBUG_IO) {
        printf("virto_mmio_read: offset=0x%x val=0x%x size=%d\n", 
               offset, val, 1 << size_log2);
    }
#endif
    return val;
}

#if VIRTIO_ADDR_BITS == 64
static void set_low32(virtio_phys_addr_t *paddr, uint32_t val)
{
    *paddr = set_bits(*paddr, 0, 32, val);
}

static void set_high32(virtio_phys_addr_t *paddr, uint32_t val)
{
    *paddr = set_bits(*paddr, 32, 32, val);
}
#else
static void set_low32(virtio_phys_addr_t *paddr, uint32_t val)
{
    *paddr = val;
}
#endif

/* A split virtqueue's size is a power of 2, at most what the device
   offers. */
static void virtio_set_queue_num(QueueState *qs, uint32_t val)
{
    if (qs != nullptr && val > 0 && val <= MAX_QUEUE_NUM &&
        (val & (val - 1)) == 0)
        qs->num = val;
}

void VIRTIOMMIOTransport::DeviceWrite(uint32_t offset, uint32_t val, int size_log2)
{
    VIRTIODevice *s = &fDev;
    
#ifdef DEBUG_VIRTIO
    if (s->debug & VIRTIO_DEBUG_IO) {
        printf("virto_mmio_write: offset=0x%x val=0x%x size=%d\n",
               offset, val, 1 << size_log2);
    }
#endif

    if (offset >= VIRTIO_MMIO_CONFIG) {
        virtio_config_write(s, offset - VIRTIO_MMIO_CONFIG, val, size_log2);
        return;
    }

    if (size_log2 == 2) {
        QueueState *qs = virtio_selected_queue(s);
        switch(offset) {
        case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
            s->device_features_sel = val;
            break;
        case VIRTIO_MMIO_DRIVER_FEATURES_SEL:
            s->driver_features_sel = val;
            break;
        case VIRTIO_MMIO_DRIVER_FEATURES:
            virtio_write_driver_features(s, val);
            break;
        case VIRTIO_MMIO_QUEUE_SEL:
            s->queue_sel = val;
            break;
        case VIRTIO_MMIO_QUEUE_NUM:
            virtio_set_queue_num(qs, val);
            break;
        case VIRTIO_MMIO_QUEUE_DESC_LOW:
            if (qs)
                set_low32(&qs->desc_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
            if (qs)
                set_low32(&qs->avail_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_USED_LOW:
            if (qs)
                set_low32(&qs->used_addr, val);
            break;
#if VIRTIO_ADDR_BITS == 64
        case VIRTIO_MMIO_QUEUE_DESC_HIGH:
            if (qs)
                set_high32(&qs->desc_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
            if (qs)
                set_high32(&qs->avail_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_USED_HIGH:
            if (qs)
                set_high32(&qs->used_addr, val);
            break;
#endif
        case VIRTIO_MMIO_STATUS:
            virtio_write_status(s, val);
            break;
        case VIRTIO_MMIO_QUEUE_READY:
            if (qs)
                qs->ready = val & 1;
            break;
        case VIRTIO_MMIO_QUEUE_NOTIFY:
            if (val < s->num_queues)
                queue_notify_any(s, val);
            break;
        case VIRTIO_MMIO_INTERRUPT_ACK:
            s->int_status &= ~val;
            if (s->int_status == 0) {
                s->irq->Set(0);
            }
            break;
        }
    }
}

uint32_t VIRTIOPCITransport::DeviceRead(uint32_t offset1, int size_log2)
{
    VIRTIODevice *s = &fDev;
    uint32_t offset;
    uint32_t val = 0;

    offset = get_bits(offset1, 0, 12);
    QueueState *qs = virtio_selected_queue(s);
    switch(offset1 >> 12) {
    case VIRTIO_PCI_CFG_OFFSET >> 12:
        if (size_log2 == 2) {
            switch(offset) {
            case VIRTIO_PCI_DEVICE_FEATURE:
                val = virtio_read_device_features(s);
                break;
            case VIRTIO_PCI_DEVICE_FEATURE_SEL:
                val = s->device_features_sel;
                break;
            case VIRTIO_PCI_GUEST_FEATURE_SEL:
                val = s->driver_features_sel;
                break;
            case VIRTIO_PCI_GUEST_FEATURE:
                val = s->driver_features_sel < 2 ?
                    (uint32_t)(s->driver_features >>
                               (32 * s->driver_features_sel)) : 0;
                break;
            case VIRTIO_PCI_QUEUE_DESC_LOW:
                val = qs ? qs->desc_addr : 0;
                break;
            case VIRTIO_PCI_QUEUE_AVAIL_LOW:
                val = qs ? qs->avail_addr : 0;
                break;
            case VIRTIO_PCI_QUEUE_USED_LOW:
                val = qs ? qs->used_addr : 0;
                break;
#if VIRTIO_ADDR_BITS == 64
            case VIRTIO_PCI_QUEUE_DESC_HIGH:
                val = qs ? qs->desc_addr >> 32 : 0;
                break;
            case VIRTIO_PCI_QUEUE_AVAIL_HIGH:
                val = qs ? qs->avail_addr >> 32 : 0;
                break;
            case VIRTIO_PCI_QUEUE_USED_HIGH:
                val = qs ? qs->used_addr >> 32 : 0;
                break;
#endif
            }
        } else if (size_log2 == 1) {
            switch(offset) {
            case VIRTIO_PCI_NUM_QUEUES:
                val = s->num_queues;
                break;
            case VIRTIO_PCI_QUEUE_SEL:
                val = s->queue_sel;
                break;
            case VIRTIO_PCI_QUEUE_SIZE:
                /* 0 for a queue the device does not have */
                val = qs ? qs->num : 0;
                break;
            case VIRTIO_PCI_QUEUE_ENABLE:
                val = qs ? qs->ready : 0;
                break;
            case VIRTIO_PCI_QUEUE_NOTIFY_OFF:
                val = 0;
                break;
            case VIRTIO_PCI_MSIX_CONFIG:
                val = s->config_msix_vector;
                break;
            case VIRTIO_PCI_QUEUE_MSIX_VECTOR:
                val = qs ? qs->msix_vector : PCI_MSIX_NO_VECTOR;
                break;
            }
        } else if (size_log2 == 0) {
            switch(offset) {
            case VIRTIO_PCI_DEVICE_STATUS:
                val = s->status;
                break;
            case VIRTIO_PCI_CONFIG_GENERATION:
                val = s->config_generation & 0xff;
                break;
            }
        }
        break;
    case VIRTIO_PCI_ISR_OFFSET >> 12:
        if (offset == 0 && size_log2 == 0) {
            val = s->int_status;
            s->int_status = 0;
            s->irq->Set(0);
        }
        break;
    case VIRTIO_PCI_CONFIG_OFFSET >> 12:
        val = virtio_config_read(s, offset, size_log2);
        break;
    case VIRTIO_PCI_MSIX_TABLE_OFFSET >> 12:
        val = s->msix.TableRead(offset, size_log2);
        break;
    case VIRTIO_PCI_MSIX_PBA_OFFSET >> 12:
        val = s->msix.PbaRead(offset, size_log2);
        break;
    }
#ifdef DEBUG_VIRTIO
    if (s->debug & VIRTIO_DEBUG_IO) {
        printf("virto_pci_read: offset=0x%x val=0x%x size=%d\n", 
               offset1, val, 1 << size_log2);
    }
#endif
    return val;
}

void VIRTIOPCITransport::DeviceWrite(uint32_t offset1, uint32_t val, int size_log2)
{
    VIRTIODevice *s = &fDev;
    uint32_t offset;
    
#ifdef DEBUG_VIRTIO
    if (s->debug & VIRTIO_DEBUG_IO) {
        printf("virto_pci_write: offset=0x%x val=0x%x size=%d\n",
               offset1, val, 1 << size_log2);
    }
#endif
    offset = get_bits(offset1, 0, 12);
    QueueState *qs = virtio_selected_queue(s);
    switch(offset1 >> 12) {
    case VIRTIO_PCI_CFG_OFFSET >> 12:
        if (size_log2 == 2) {
            switch(offset) {
            case VIRTIO_PCI_DEVICE_FEATURE_SEL:
                s->device_features_sel = val;
                break;
            case VIRTIO_PCI_GUEST_FEATURE_SEL:
                s->driver_features_sel = val;
                break;
            case VIRTIO_PCI_GUEST_FEATURE:
                virtio_write_driver_features(s, val);
                break;
            case VIRTIO_PCI_QUEUE_DESC_LOW:
                if (qs)
                    set_low32(&qs->desc_addr, val);
                break;
            case VIRTIO_PCI_QUEUE_AVAIL_LOW:
                if (qs)
                    set_low32(&qs->avail_addr, val);
                break;
            case VIRTIO_PCI_QUEUE_USED_LOW:
                if (qs)
                    set_low32(&qs->used_addr, val);
                break;
#if VIRTIO_ADDR_BITS == 64
            case VIRTIO_PCI_QUEUE_DESC_HIGH:
                if (qs)
                    set_high32(&qs->desc_addr, val);
                break;
            case VIRTIO_PCI_QUEUE_AVAIL_HIGH:
                if (qs)
                    set_high32(&qs->avail_addr, val);
                break;
            case VIRTIO_PCI_QUEUE_USED_HIGH:
                if (qs)
                    set_high32(&qs->used_addr, val);
                break;
#endif
            }
        } else if (size_log2 == 1) {
            switch(offset) {
            case VIRTIO_PCI_QUEUE_SEL:
                s->queue_sel = val;
                break;
            case VIRTIO_PCI_QUEUE_SIZE:
                virtio_set_queue_num(qs, val);
                break;
            case VIRTIO_PCI_QUEUE_ENABLE:
                if (qs)
                    qs->ready = val & 1;
                break;
            case VIRTIO_PCI_MSIX_CONFIG:
                s->config_msix_vector = s->msix.AcceptVector(val);
                break;
            case VIRTIO_PCI_QUEUE_MSIX_VECTOR:
                if (qs)
                    qs->msix_vector = s->msix.AcceptVector(val);
                break;
            }
        } else if (size_log2 == 0) {
            switch(offset) {
            case VIRTIO_PCI_DEVICE_STATUS:
                virtio_write_status(s, val);
                break;
            }
        }
        break;
    case VIRTIO_PCI_CONFIG_OFFSET >> 12:
        virtio_config_write(s, offset, val, size_log2);
        break;
    case VIRTIO_PCI_MSIX_TABLE_OFFSET >> 12:
        s->msix.TableWrite(offset, val, size_log2);
        break;
    case VIRTIO_PCI_NOTIFY_OFFSET >> 12:
        if (val < s->num_queues)
            queue_notify_any(s, val);
        break;
    }
}

void virtio_set_debug(VIRTIODevice *s, int debug)
{
    s->debug = debug;
}

void virtio_config_change_notify(VIRTIODevice *s)
{
    s->config_generation++;
    if (s->status & VIRTIO_STATUS_DRIVER_OK)
        virtio_raise_irq(s, VIRTIO_INT_CONFIG, s->config_msix_vector);
}
