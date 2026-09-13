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

#include "cutils.h"
#include "devices.h"
#include "fdt.h"
#include "list.h"
#include "virtio.h"
#include "virtio_priv.h"

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
#define VIRTIO_PCI_BAR_SIZE            0x8000

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


static void virtio_reset(VIRTIODevice *s)
{
    int i;

    s->status = 0;
    s->queue_sel = 0;
    s->device_features_sel = 0;
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

static void virtio_add_pci_capability(VIRTIODevice *s, int cfg_type,
                                      int bar, uint32_t offset, uint32_t len,
                                      uint32_t mult)
{
    uint8_t cap[20];
    int cap_len;
    if (cfg_type == 2)
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
    if (cfg_type == 2)
        put_le32(cap + 16, mult);
    pci_add_capability(s->pci_dev, cap, cap_len);
}

void virtio_init(VIRTIODevice *s, VIRTIOBusDef *bus,
                        uint32_t device_id, int config_space_size)
{
    if (bus->pci_bus) {
        uint16_t pci_device_id, class_id;
        char name[32];
        int bar_num;
        
        switch(device_id) {
        case 1:
            class_id = 0x0200; /* net */
            break;
        case 2:
            class_id = 0x0100; /* block. XXX: check it */
            break;
        case 3:
            class_id = 0x0780; /* console */
            break;
        case 9:
            class_id = 0x2;
            break;
        case 18:
            class_id = 0x0980;
            break;
        default:
            abort();
        }
        /* Only the virtio 1.0 layout is implemented, so the identifier and
           revision have to be non-transitional ones: a guest picks the layout
           from them, and a transitional device promises a port window that is
           not here. */
        pci_device_id = 0x1040 + device_id;
        snprintf(name, sizeof(name), "virtio_%04x", pci_device_id);
        s->pci_dev = pci_register_device(bus->pci_bus, name, -1,
                                         0x1af4, pci_device_id, 0x01,
                                         class_id);
        pci_device_set_config16(s->pci_dev, 0x2c, 0x1af4);
        pci_device_set_config16(s->pci_dev, 0x2e, device_id);
        pci_device_set_config8(s->pci_dev, PCI_INTERRUPT_PIN, 1);

        bar_num = 4;
        virtio_add_pci_capability(s, 1, bar_num,
                              VIRTIO_PCI_CFG_OFFSET, 0x1000, 0); /* common */
        virtio_add_pci_capability(s, 3, bar_num,
                              VIRTIO_PCI_ISR_OFFSET, 0x1000, 0); /* isr */
        virtio_add_pci_capability(s, 4, bar_num,
                              VIRTIO_PCI_CONFIG_OFFSET, 0x1000, 0); /* config */
        virtio_add_pci_capability(s, 2, bar_num,
                              VIRTIO_PCI_NOTIFY_OFFSET, 0x1000, 0); /* notify */

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
    } else {
        /* MMIO case */
        s->mem_map = bus->mem_map;
        s->irq = bus->irq;
        s->transport = std::make_unique<VIRTIOMMIOTransport>(*s);
        s->mem_range = s->mem_map->RegisterDevice(bus->addr, VIRTIO_PAGE_SIZE,
                                                  s->transport.get(),
                                                  DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32);
    }

    s->device_id = device_id;
    s->vendor_id = 0xffff;
    s->config_space_size = config_space_size;
    virtio_reset(s);
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

static int get_desc(VIRTIODevice *s, VIRTIODesc *desc,  
                    int queue_idx, int desc_idx)
{
    QueueState *qs = &s->queue[queue_idx];
    return virtio_memcpy_from_ram(s, reinterpret_cast<uint8_t *>(desc), qs->desc_addr +
                                  desc_idx * sizeof(VIRTIODesc),
                                  sizeof(VIRTIODesc));
}

static int memcpy_to_from_queue(VIRTIODevice *s, uint8_t *buf,
                                int queue_idx, int desc_idx,
                                int offset, int count, bool to_queue)
{
    VIRTIODesc desc;
    int l, f_write_flag;

    if (count == 0)
        return 0;

    get_desc(s, &desc, queue_idx, desc_idx);

    if (to_queue) {
        f_write_flag = VRING_DESC_F_WRITE;
        /* find the first write descriptor */
        for(;;) {
            if ((desc.flags & VRING_DESC_F_WRITE) == f_write_flag)
                break;
            if (!(desc.flags & VRING_DESC_F_NEXT))
                return -1;
            desc_idx = desc.next;
            get_desc(s, &desc, queue_idx, desc_idx);
        }
    } else {
        f_write_flag = 0;
    }

    /* find the descriptor at offset */
    for(;;) {
        if ((desc.flags & VRING_DESC_F_WRITE) != f_write_flag)
            return -1;
        if (offset < desc.len)
            break;
        if (!(desc.flags & VRING_DESC_F_NEXT))
            return -1;
        desc_idx = desc.next;
        offset -= desc.len;
        get_desc(s, &desc, queue_idx, desc_idx);
    }

    for(;;) {
        l = min_int(count, desc.len - offset);
        if (to_queue)
            virtio_memcpy_to_ram(s, desc.addr + offset, buf, l);
        else
            virtio_memcpy_from_ram(s, buf, desc.addr + offset, l);
        count -= l;
        if (count == 0)
            break;
        offset += l;
        buf += l;
        if (offset == desc.len) {
            if (!(desc.flags & VRING_DESC_F_NEXT))
                return -1;
            desc_idx = desc.next;
            get_desc(s, &desc, queue_idx, desc_idx);
            if ((desc.flags & VRING_DESC_F_WRITE) != f_write_flag)
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

/* signal that the descriptor has been consumed */
void virtio_consume_desc(VIRTIODevice *s,
                                int queue_idx, int desc_idx, int desc_len)
{
    QueueState *qs = &s->queue[queue_idx];
    virtio_phys_addr_t addr;
    uint32_t index;

    addr = qs->used_addr + 2;
    index = virtio_read16(s, addr);
    virtio_write16(s, addr, index + 1);

    addr = qs->used_addr + 4 + (index & (qs->num - 1)) * 8;
    virtio_write32(s, addr, desc_idx);
    virtio_write32(s, addr + 4, desc_len);

    virtio_raise_irq(s, VIRTIO_INT_USED_RING, qs->msix_vector);
}

int get_desc_rw_size(VIRTIODevice *s,
                             int *pread_size, int *pwrite_size,
                             int queue_idx, int desc_idx)
{
    VIRTIODesc desc;
    int read_size, write_size;

    read_size = 0;
    write_size = 0;
    get_desc(s, &desc, queue_idx, desc_idx);

    for(;;) {
        if (desc.flags & VRING_DESC_F_WRITE)
            break;
        read_size += desc.len;
        if (!(desc.flags & VRING_DESC_F_NEXT))
            goto done;
        desc_idx = desc.next;
        get_desc(s, &desc, queue_idx, desc_idx);
    }
    
    for(;;) {
        if (!(desc.flags & VRING_DESC_F_WRITE))
            return -1;
        write_size += desc.len;
        if (!(desc.flags & VRING_DESC_F_NEXT))
            break;
        desc_idx = desc.next;
        get_desc(s, &desc, queue_idx, desc_idx);
    }

 done:
    *pread_size = read_size;
    *pwrite_size = write_size;
    return 0;
}

/* XXX: test if the queue is ready ? */
void queue_notify(VIRTIODevice *s, int queue_idx)
{
    QueueState *qs = &s->queue[queue_idx];
    uint16_t avail_idx;
    int desc_idx, read_size, write_size;

    if (qs->manual_recv)
        return;

    avail_idx = virtio_read16(s, qs->avail_addr + 2);
    while (qs->last_avail_idx != avail_idx) {
        desc_idx = virtio_read16(s, qs->avail_addr + 4 + 
                                 (qs->last_avail_idx & (qs->num - 1)) * 2);
        if (!get_desc_rw_size(s, &read_size, &write_size, queue_idx, desc_idx)) {
#ifdef DEBUG_VIRTIO
            if (s->debug & VIRTIO_DEBUG_IO) {
                printf("queue_notify: idx=%d read_size=%d write_size=%d\n",
                       queue_idx, read_size, write_size);
            }
#endif
            if (s->RecvRequest(queue_idx, desc_idx,
                               read_size, write_size) < 0)
                break;
        }
        qs->last_avail_idx++;
    }
}

static uint32_t virtio_config_read(VIRTIODevice *s, uint32_t offset,
                                   int size_log2)
{
    uint32_t val;
    switch(size_log2) {
    case 0:
        if (offset < s->config_space_size) {
            val = s->config_space[offset];
        } else {
            val = 0;
        }
        break;
    case 1:
        if (offset < (s->config_space_size - 1)) {
            val = get_le16(&s->config_space[offset]);
        } else {
            val = 0;
        }
        break;
    case 2:
        if (offset < (s->config_space_size - 3)) {
            val = get_le32(s->config_space + offset);
        } else {
            val = 0;
        }
        break;
    default:
        abort();
    }
    return val;
}

static void virtio_config_write(VIRTIODevice *s, uint32_t offset,
                                uint32_t val, int size_log2)
{
    switch(size_log2) {
    case 0:
        if (offset < s->config_space_size) {
            s->config_space[offset] = val;
            s->ConfigWrite();
        }
        break;
    case 1:
        if (offset < s->config_space_size - 1) {
            put_le16(s->config_space + offset, val);
            s->ConfigWrite();
        }
        break;
    case 2:
        if (offset < s->config_space_size - 3) {
            put_le32(s->config_space + offset, val);
            s->ConfigWrite();
        }
        break;
    }
}

uint32_t VIRTIOMMIOTransport::DeviceRead(uint32_t offset, int size_log2)
{
    VIRTIODevice *s = &fDev;
    uint32_t val;

    if (offset >= VIRTIO_MMIO_CONFIG) {
        return virtio_config_read(s, offset - VIRTIO_MMIO_CONFIG, size_log2);
    }

    if (size_log2 == 2) {
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
            switch(s->device_features_sel) {
            case 0:
                val = s->device_features;
                break;
            case 1:
                val = 1; /* version 1 */
                break;
            default:
                val = 0;
                break;
            }
            break;
        case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
            val = s->device_features_sel;
            break;
        case VIRTIO_MMIO_QUEUE_SEL:
            val = s->queue_sel;
            break;
        case VIRTIO_MMIO_QUEUE_NUM_MAX:
            val = MAX_QUEUE_NUM;
            break;
        case VIRTIO_MMIO_QUEUE_NUM:
            val = s->queue[s->queue_sel].num;
            break;
        case VIRTIO_MMIO_QUEUE_DESC_LOW:
            val = s->queue[s->queue_sel].desc_addr;
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
            val = s->queue[s->queue_sel].avail_addr;
            break;
        case VIRTIO_MMIO_QUEUE_USED_LOW:
            val = s->queue[s->queue_sel].used_addr;
            break;
#if VIRTIO_ADDR_BITS == 64
        case VIRTIO_MMIO_QUEUE_DESC_HIGH:
            val = s->queue[s->queue_sel].desc_addr >> 32;
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
            val = s->queue[s->queue_sel].avail_addr >> 32;
            break;
        case VIRTIO_MMIO_QUEUE_USED_HIGH:
            val = s->queue[s->queue_sel].used_addr >> 32;
            break;
#endif
        case VIRTIO_MMIO_QUEUE_READY:
            val = s->queue[s->queue_sel].ready;
            break;
        case VIRTIO_MMIO_INTERRUPT_STATUS:
            val = s->int_status;
            break;
        case VIRTIO_MMIO_STATUS:
            val = s->status;
            break;
        case VIRTIO_MMIO_CONFIG_GENERATION:
            val = 0;
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
    *paddr = (*paddr & ~(virtio_phys_addr_t)0xffffffff) | val;
}

static void set_high32(virtio_phys_addr_t *paddr, uint32_t val)
{
    *paddr = (*paddr & 0xffffffff) | ((virtio_phys_addr_t)val << 32);
}
#else
static void set_low32(virtio_phys_addr_t *paddr, uint32_t val)
{
    *paddr = val;
}
#endif

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
        switch(offset) {
        case VIRTIO_MMIO_DEVICE_FEATURES_SEL:
            s->device_features_sel = val;
            break;
        case VIRTIO_MMIO_QUEUE_SEL:
            if (val < MAX_QUEUE)
                s->queue_sel = val;
            break;
        case VIRTIO_MMIO_QUEUE_NUM:
            if ((val & (val - 1)) == 0 && val > 0) {
                s->queue[s->queue_sel].num = val;
            }
            break;
        case VIRTIO_MMIO_QUEUE_DESC_LOW:
            set_low32(&s->queue[s->queue_sel].desc_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_LOW:
            set_low32(&s->queue[s->queue_sel].avail_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_USED_LOW:
            set_low32(&s->queue[s->queue_sel].used_addr, val);
            break;
#if VIRTIO_ADDR_BITS == 64
        case VIRTIO_MMIO_QUEUE_DESC_HIGH:
            set_high32(&s->queue[s->queue_sel].desc_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_AVAIL_HIGH:
            set_high32(&s->queue[s->queue_sel].avail_addr, val);
            break;
        case VIRTIO_MMIO_QUEUE_USED_HIGH:
            set_high32(&s->queue[s->queue_sel].used_addr, val);
            break;
#endif
        case VIRTIO_MMIO_STATUS:
            s->status = val;
            if (val == 0) {
                /* reset */
                s->irq->Set(0);
                virtio_reset(s);
            }
            break;
        case VIRTIO_MMIO_QUEUE_READY:
            s->queue[s->queue_sel].ready = val & 1;
            break;
        case VIRTIO_MMIO_QUEUE_NOTIFY:
            if (val < MAX_QUEUE)
                queue_notify(s, val);
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

    offset = offset1 & 0xfff;
    switch(offset1 >> 12) {
    case VIRTIO_PCI_CFG_OFFSET >> 12:
        if (size_log2 == 2) {
            switch(offset) {
            case VIRTIO_PCI_DEVICE_FEATURE:
                switch(s->device_features_sel) {
                case 0:
                    val = s->device_features;
                    break;
                case 1:
                    val = 1; /* version 1 */
                    break;
                default:
                    val = 0;
                    break;
                }
                break;
            case VIRTIO_PCI_DEVICE_FEATURE_SEL:
                val = s->device_features_sel;
                break;
            case VIRTIO_PCI_QUEUE_DESC_LOW:
                val = s->queue[s->queue_sel].desc_addr;
                break;
            case VIRTIO_PCI_QUEUE_AVAIL_LOW:
                val = s->queue[s->queue_sel].avail_addr;
                break;
            case VIRTIO_PCI_QUEUE_USED_LOW:
                val = s->queue[s->queue_sel].used_addr;
                break;
#if VIRTIO_ADDR_BITS == 64
            case VIRTIO_PCI_QUEUE_DESC_HIGH:
                val = s->queue[s->queue_sel].desc_addr >> 32;
                break;
            case VIRTIO_PCI_QUEUE_AVAIL_HIGH:
                val = s->queue[s->queue_sel].avail_addr >> 32;
                break;
            case VIRTIO_PCI_QUEUE_USED_HIGH:
                val = s->queue[s->queue_sel].used_addr >> 32;
                break;
#endif
            }
        } else if (size_log2 == 1) {
            switch(offset) {
            case VIRTIO_PCI_NUM_QUEUES:
                val = MAX_QUEUE_NUM;
                break;
            case VIRTIO_PCI_QUEUE_SEL:
                val = s->queue_sel;
                break;
            case VIRTIO_PCI_QUEUE_SIZE:
                val = s->queue[s->queue_sel].num;
                break;
            case VIRTIO_PCI_QUEUE_ENABLE:
                val = s->queue[s->queue_sel].ready;
                break;
            case VIRTIO_PCI_QUEUE_NOTIFY_OFF:
                val = 0;
                break;
            case VIRTIO_PCI_MSIX_CONFIG:
                val = s->config_msix_vector;
                break;
            case VIRTIO_PCI_QUEUE_MSIX_VECTOR:
                val = s->queue[s->queue_sel].msix_vector;
                break;
            }
        } else if (size_log2 == 0) {
            switch(offset) {
            case VIRTIO_PCI_DEVICE_STATUS:
                val = s->status;
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
    offset = offset1 & 0xfff;
    switch(offset1 >> 12) {
    case VIRTIO_PCI_CFG_OFFSET >> 12:
        if (size_log2 == 2) {
            switch(offset) {
            case VIRTIO_PCI_DEVICE_FEATURE_SEL:
                s->device_features_sel = val;
                break;
            case VIRTIO_PCI_QUEUE_DESC_LOW:
                set_low32(&s->queue[s->queue_sel].desc_addr, val);
                break;
            case VIRTIO_PCI_QUEUE_AVAIL_LOW:
                set_low32(&s->queue[s->queue_sel].avail_addr, val);
                break;
            case VIRTIO_PCI_QUEUE_USED_LOW:
                set_low32(&s->queue[s->queue_sel].used_addr, val);
                break;
#if VIRTIO_ADDR_BITS == 64
            case VIRTIO_PCI_QUEUE_DESC_HIGH:
                set_high32(&s->queue[s->queue_sel].desc_addr, val);
                break;
            case VIRTIO_PCI_QUEUE_AVAIL_HIGH:
                set_high32(&s->queue[s->queue_sel].avail_addr, val);
                break;
            case VIRTIO_PCI_QUEUE_USED_HIGH:
                set_high32(&s->queue[s->queue_sel].used_addr, val);
                break;
#endif
            }
        } else if (size_log2 == 1) {
            switch(offset) {
            case VIRTIO_PCI_QUEUE_SEL:
                if (val < MAX_QUEUE)
                    s->queue_sel = val;
                break;
            case VIRTIO_PCI_QUEUE_SIZE:
                if ((val & (val - 1)) == 0 && val > 0) {
                    s->queue[s->queue_sel].num = val;
                }
                break;
            case VIRTIO_PCI_QUEUE_ENABLE:
                s->queue[s->queue_sel].ready = val & 1;
                break;
            case VIRTIO_PCI_MSIX_CONFIG:
                s->config_msix_vector = s->msix.AcceptVector(val);
                break;
            case VIRTIO_PCI_QUEUE_MSIX_VECTOR:
                s->queue[s->queue_sel].msix_vector = s->msix.AcceptVector(val);
                break;
            }
        } else if (size_log2 == 0) {
            switch(offset) {
            case VIRTIO_PCI_DEVICE_STATUS:
                s->status = val;
                if (val == 0) {
                    /* reset */
                    s->irq->Set(0);
                    virtio_reset(s);
                }
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
        if (val < MAX_QUEUE)
            queue_notify(s, val);
        break;
    }
}

void virtio_set_debug(VIRTIODevice *s, int debug)
{
    s->debug = debug;
}

void virtio_config_change_notify(VIRTIODevice *s)
{
    virtio_raise_irq(s, VIRTIO_INT_CONFIG, s->config_msix_vector);
}


//#pragma mark - VirtioDevice

/* A virtio input device as a machine input target. */
class VirtioInputTarget final: public InputEventTarget {
private:
    VIRTIODevice *fDev;
    bool fAbsolute;

public:
    VirtioInputTarget(VIRTIODevice *dev, bool absolute):
        fDev(dev), fAbsolute(absolute) {}

    void SendKeyEvent(bool is_down, uint16_t key_code) override
        {virtio_input_send_key_event(fDev, is_down, key_code);}

    void SendMouseEvent(int dx, int dy, int dz,
                        unsigned int buttons) override
        {virtio_input_send_mouse_event(fDev, dx, dy, dz, buttons);}

    bool MouseIsAbsolute() override {return fAbsolute;}
};


typedef enum {
    VIRTIO_KIND_BLOCK,
    VIRTIO_KIND_NET,
    VIRTIO_KIND_CONSOLE,
    VIRTIO_KIND_9P,
    VIRTIO_KIND_INPUT,
} VirtioKindEnum;


/* One wrapper for every virtio device, on either transport. Which resources
   it needs is decided by the bus it was attached to: on MMIO it takes a
   register page and an interrupt line, on PCI it takes neither because the
   guest places the BARs and the bridge routes INTx. */
class VirtioDevice final: public Device {
private:
    VirtioKindEnum fKind;
    DeviceContext *fCtx;
    VMDeviceNode *fNode;
    VirtioInputTypeEnum fInputType = VIRTIO_INPUT_TYPE_KEYBOARD;
    const char *fTag = nullptr;
    Resource *fMmio = nullptr;
    Resource *fIrq = nullptr;
    /* taken over from the node; declared first, so that fDev goes before
       the back end it uses */
    std::unique_ptr<BlockDevice> fBlockDev;
    std::unique_ptr<FSDevice> fFsDev;
    std::unique_ptr<EthernetDevice> fNet;
    std::unique_ptr<VIRTIODevice> fDev;
    std::unique_ptr<VirtioInputTarget> fInputTarget;

public:
    VirtioDevice(const char *name, VirtioKindEnum kind, DeviceContext *ctx,
                 VMDeviceNode *node):
        Device(name), fKind(kind), fCtx(ctx), fNode(node) {}

    void SetInputType(VirtioInputTypeEnum type) {fInputType = type;}
    void SetTag(const char *tag) {fTag = tag;}

    bool Prepare() override
    {
        if (ParentBus()->AsPCIBus() != nullptr) {
            return true;
        }
        fMmio = AddResource(RES_MMIO, VIRTIO_PAGE_SIZE, VIRTIO_PAGE_SIZE);
        fIrq = AddResource(RES_IRQ, 1);
        return fMmio != nullptr && fIrq != nullptr;
    }

    bool Realize() override
    {
        VIRTIOBusDef vbus = {};
        PCIBus *pci_bus = ParentBus()->AsPCIBus();

        if (pci_bus != nullptr) {
            vbus.pci_bus = pci_bus;
        } else {
            SystemBus *sys = static_cast<SystemBus *>(ParentBus());
            vbus.mem_map = sys->MemMap();
            vbus.addr = fMmio->base;
            vbus.irq = sys->IrqSignalFor(fIrq->base);
            if (vbus.irq == nullptr) {
                vm_error("%s: bad interrupt line %d\n", Name(),
                         (int)fIrq->base);
                return false;
            }
        }

        switch (fKind) {
        case VIRTIO_KIND_BLOCK:
            if (fNode->block_dev == nullptr) {
                vm_error("%s: no block back end\n", Name());
                return false;
            }
            fBlockDev = std::move(fNode->block_dev);
            fDev = virtio_block_init(&vbus, fBlockDev.get());
            break;
        case VIRTIO_KIND_NET:
            if (fNode->net == nullptr) {
                vm_error("%s: no network back end\n", Name());
                return false;
            }
            /* The emulator polls a single back end from its main loop, so a
               second network device would simply never receive anything. */
            if (fCtx->net != nullptr) {
                vm_error("%s: only one network device is supported\n", Name());
                return false;
            }
            fNet = std::move(fNode->net);
            fDev = virtio_net_init(&vbus, fNet.get());
            fCtx->net = fNet.get();
            break;
        case VIRTIO_KIND_CONSOLE:
            if (fCtx->console == nullptr) {
                vm_error("%s: no console back end\n", Name());
                return false;
            }
            fDev = virtio_console_init(&vbus, fCtx->console);
            fCtx->console_dev = fDev.get();
            break;
        case VIRTIO_KIND_9P:
            if (fNode->fs_dev == nullptr) {
                vm_error("%s: no filesystem back end\n", Name());
                return false;
            }
            fFsDev = std::move(fNode->fs_dev);
            fDev = virtio_9p_init(&vbus, fFsDev.get(), fTag);
            break;
        case VIRTIO_KIND_INPUT:
            fDev = virtio_input_init(&vbus, fInputType);
            if (fDev == nullptr) {
                break;
            }
            fInputTarget = std::make_unique<VirtioInputTarget>(
                fDev.get(), fInputType == VIRTIO_INPUT_TYPE_TABLET);
            if (fInputType == VIRTIO_INPUT_TYPE_KEYBOARD) {
                fCtx->keyboard = fInputTarget.get();
            } else {
                fCtx->mouse = fInputTarget.get();
            }
            break;
        }
        return fDev != nullptr;
    }

    void BuildFDT(FDTContext &ctx) override
    {
        /* On PCI the guest finds the device by enumerating configuration
           space, so it must not also appear as a node. */
        if (fMmio == nullptr) {
            return;
        }
        ctx.fdt->BeginNodeNum("virtio", fMmio->base);
        ctx.fdt->PropStr("compatible", "virtio,mmio");
        ctx.fdt->PropU64Range("reg", fMmio->base, fMmio->size);
        fdt_prop_irq(ctx, fIrq->base);
        ctx.fdt->EndNode();
    }
};


//#pragma mark - factory

Device *virtio_block_node_create(DeviceContext *ctx, VMDeviceNode *node)
{
    return new VirtioDevice("virtio-block", VIRTIO_KIND_BLOCK, ctx, node);
}


Device *virtio_net_node_create(DeviceContext *ctx, VMDeviceNode *node)
{
    return new VirtioDevice("virtio-net", VIRTIO_KIND_NET, ctx, node);
}


Device *virtio_console_node_create(DeviceContext *ctx, VMDeviceNode *node)
{
    return new VirtioDevice("virtio-console", VIRTIO_KIND_CONSOLE, ctx, node);
}


Device *virtio_9p_node_create(DeviceContext *ctx, VMDeviceNode *node,
                              const char *mount_tag)
{
    VirtioDevice *dev = new VirtioDevice("virtio-9p", VIRTIO_KIND_9P, ctx,
                                         node);
    dev->SetTag(mount_tag);
    return dev;
}


Device *virtio_input_node_create(DeviceContext *ctx, VMDeviceNode *node,
                                 VirtioInputTypeEnum type)
{
    VirtioDevice *dev = new VirtioDevice("virtio-input", VIRTIO_KIND_INPUT,
                                         ctx, node);
    dev->SetInputType(type);
    return dev;
}
