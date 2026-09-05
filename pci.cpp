/*
 * Simple PCI bus driver
 * 
 * Copyright (c) 2017 Fabrice Bellard
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
#include "pci.h"

//#define DEBUG_CONFIG

typedef struct {
    uint32_t size; /* 0 means no mapping defined */
    uint8_t type;
    uint8_t enabled; /* true if mapping is enabled */
    PCIBarTarget *bar_target;
} PCIIORegion;

struct PCIDevice: public IRQTarget {
    PCIBus *bus;
    uint8_t devfn;
    IRQSignal irq[4];
    uint8_t config[256];
    uint8_t next_cap_offset; /* offset of the next capability */
    char *name; /* for debug only */
    PCIIORegion io_regions[PCI_NUM_REGIONS];

    void SetIRQ(int irq_num, int level) override;
};

struct PCIBus {
    int bus_num;
    PCIDevice *device[256];
    PhysMemoryMap *mem_map;
    PhysMemoryMap *port_map;
    uint32_t irq_state[4][8]; /* one bit per device */
    IRQSignal irq[4];
    PCIMsiTarget *msi_target; /* null if the bridge has no MSI receiver */
};

int pci_bus_map_irq(int devfn, int irq_num)
{
    int slot_addend;
    slot_addend = (devfn >> 3) - 1;
    return (irq_num + slot_addend) & 3;
}

static int bus_map_irq(PCIDevice *d, int irq_num)
{
    return pci_bus_map_irq(d->devfn, irq_num);
}

void PCIDevice::SetIRQ(int irq_num, int level)
{
    PCIDevice *d = this;
    PCIBus *b = d->bus;
    uint32_t mask;
    int i, irq_level;
    
    //    printf("%s: pci_device_seq_irq: %d %d\n", d->name, irq_num, level);
    irq_num = bus_map_irq(d, irq_num);
    mask = 1 << (d->devfn & 0x1f);
    if (level)
        b->irq_state[irq_num][d->devfn >> 5] |= mask;
    else
        b->irq_state[irq_num][d->devfn >> 5] &= ~mask;

    /* compute the IRQ state */
    mask = 0;
    for(i = 0; i < 8; i++)
        mask |= b->irq_state[irq_num][i];
    irq_level = (mask != 0);
    b->irq[irq_num].Set(irq_level);
}

static int devfn_alloc(PCIBus *b)
{
    int devfn;
    for(devfn = 0; devfn < 256; devfn += 8) {
        if (!b->device[devfn])
            return devfn;
    }
    return -1;
}

/* devfn < 0 means to allocate it */
PCIDevice *pci_register_device(PCIBus *b, const char *name, int devfn,
                               uint16_t vendor_id, uint16_t device_id,
                               uint8_t revision, uint16_t class_id)
{
    PCIDevice *d;
    int i;
    
    if (devfn < 0) {
        devfn = devfn_alloc(b);
        if (devfn < 0)
            return NULL;
    }
    if (b->device[devfn])
        return NULL;

    d = new PCIDevice();
    d->bus = b;
    d->name = strdup(name);
    d->devfn = devfn;

    put_le16(d->config + 0x00, vendor_id);
    put_le16(d->config + 0x02, device_id);
    d->config[0x08] = revision;
    put_le16(d->config + 0x0a, class_id);
    d->config[0x0e] = 0x00; /* header type */
    d->next_cap_offset = 0x40;
    
    for(i = 0; i < 4; i++)
        d->irq[i].Init(d, i);
    b->device[devfn] = d;

    return d;
}

IRQSignal *pci_device_get_irq(PCIDevice *d, unsigned int irq_num)
{
    assert(irq_num < 4);
    return &d->irq[irq_num];
}

static uint32_t pci_device_config_read(PCIDevice *d, uint32_t addr,
                                       int size_log2)
{
    uint32_t val;
    switch(size_log2) {
    case 0:
        val = *(uint8_t *)(d->config + addr);
        break;
    case 1:
        /* Note: may be unaligned */
        if (addr <= 0xfe)
            val = get_le16(d->config + addr);
        else
            val = *(uint8_t *)(d->config + addr);
        break;
    case 2:
        /* always aligned */
        val = get_le32(d->config + addr);
        break;
    default:
        abort();
    }
#ifdef DEBUG_CONFIG
    printf("pci_config_read: dev=%s addr=0x%02x val=0x%x s=%d\n",
           d->name, addr, val, 1 << size_log2);
#endif
    return val;
}

PhysMemoryMap *pci_device_get_mem_map(PCIDevice *d)
{
    return d->bus->mem_map;
}

PhysMemoryMap *pci_device_get_port_map(PCIDevice *d)
{
    return d->bus->port_map;
}

void pci_register_bar(PCIDevice *d, unsigned int bar_num,
                      uint32_t size, int type, PCIBarTarget *bar_target)
{
    PCIIORegion *r;
    uint32_t val, config_addr;
    
    assert(bar_num < PCI_NUM_REGIONS);
    assert((size & (size - 1)) == 0); /* power of two */
    assert(size >= 4);
    r = &d->io_regions[bar_num];
    assert(r->size == 0);
    r->size = size;
    r->type = type;
    r->enabled = false;
    r->bar_target = bar_target;
    /* set the config value */
    val = 0;
    if (bar_num == PCI_ROM_SLOT) {
        config_addr = 0x30;
    } else {
        val |= r->type;
        config_addr = 0x10 + 4 * bar_num;
    }
    put_le32(&d->config[config_addr], val);
}

static void pci_update_mappings(PCIDevice *d)
{
    int cmd, i, offset;
    uint32_t new_addr;
    bool new_enabled;
    PCIIORegion *r;
    
    cmd = get_le16(&d->config[PCI_COMMAND]);

    for(i = 0; i < PCI_NUM_REGIONS; i++) {
        r = &d->io_regions[i];
        if (i == PCI_ROM_SLOT) {
            offset = 0x30;
        } else {
            offset = 0x10 + i * 4;
        }
        new_addr = get_le32(&d->config[offset]);
        new_enabled = false;
        if (r->size != 0) {
            if ((r->type & PCI_ADDRESS_SPACE_IO) &&
                (cmd & PCI_COMMAND_IO)) {
                new_enabled = true;
            } else {
                if (cmd & PCI_COMMAND_MEMORY) {
                    if (i == PCI_ROM_SLOT) {
                        new_enabled = (new_addr & 1);
                    } else {
                        new_enabled = true;
                    }
                }
            }
        }
        if (new_enabled) {
            /* new address */
            new_addr = get_le32(&d->config[offset]) & ~(r->size - 1);
            r->bar_target->SetBar(i, new_addr, true);
            r->enabled = true;
        } else if (r->enabled) {
            r->bar_target->SetBar(i, 0, false);
            r->enabled = false;
        }
    }
}

/* return != 0 if write is not handled */
static int pci_write_bar(PCIDevice *d, uint32_t addr,
                          uint32_t val)
{
    PCIIORegion *r;
    int reg;
    
    if (addr == 0x30)
        reg = PCI_ROM_SLOT;
    else
        reg = (addr - 0x10) >> 2;
    //    printf("%s: write bar addr=%x data=%x\n", d->name, addr, val);
    r = &d->io_regions[reg];
    if (r->size == 0)
        return -1;
    if (reg == PCI_ROM_SLOT) {
        val = val & ((~(r->size - 1)) | 1);
    } else {
        val = (val & ~(r->size - 1)) | r->type;
    }
    put_le32(d->config + addr, val);
    pci_update_mappings(d);
    return 0;
}

static void pci_device_config_write8(PCIDevice *d, uint32_t addr,
                                     uint32_t data)
{
    int can_write;

    if (addr == PCI_STATUS || addr == (PCI_STATUS + 1)) {
        /* write 1 reset bits */
        d->config[addr] &= ~data;
        return;
    }
    
    switch(d->config[0x0e]) {
    case 0x00:
    case 0x80:
        switch(addr) {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x08:
        case 0x09:
        case 0x0a:
        case 0x0b:
        case 0x0e:
        case 0x10 ... 0x27: /* base */
        case 0x30 ... 0x33: /* rom */
        case 0x3d:
            can_write = 0;
            break;
        default:
            can_write = 1;
            break;
        }
        break;
    default:
    case 0x01:
        switch(addr) {
        case 0x00:
        case 0x01:
        case 0x02:
        case 0x03:
        case 0x08:
        case 0x09:
        case 0x0a:
        case 0x0b:
        case 0x0e:
        case 0x38 ... 0x3b: /* rom */
        case 0x3d:
            can_write = 0;
            break;
        default:
            can_write = 1;
            break;
        }
        break;
    }
    if (can_write)
        d->config[addr] = data;
}
                                  

static void pci_device_config_write(PCIDevice *d, uint32_t addr,
                                    uint32_t data, int size_log2)
{
    int size, i;
    uint32_t addr1;
    
#ifdef DEBUG_CONFIG
    printf("pci_config_write: dev=%s addr=0x%02x val=0x%x s=%d\n",
           d->name, addr, data, 1 << size_log2);
#endif
    if (size_log2 == 2 &&
        ((addr >= 0x10 && addr < 0x10 + 4 * 6) ||
         addr == 0x30)) {
        if (pci_write_bar(d, addr, data) == 0)
            return;
    }
    size = 1 << size_log2;
    for(i = 0; i < size; i++) {
        addr1 = addr + i;
        if (addr1 <= 0xff) {
            pci_device_config_write8(d, addr1, (data >> (i * 8)) & 0xff);
        }
    }
    if (PCI_COMMAND >= addr && PCI_COMMAND < addr + size) {
        pci_update_mappings(d);
    }
}


static void pci_data_write(PCIBus *s, uint32_t addr,
                           uint32_t data, int size_log2)
{
    PCIDevice *d;
    int bus_num, devfn, config_addr;
    
    bus_num = (addr >> 16) & 0xff;
    if (bus_num != s->bus_num)
        return;
    devfn = (addr >> 8) & 0xff;
    d = s->device[devfn];
    if (!d)
        return;
    config_addr = addr & 0xff;
    pci_device_config_write(d, config_addr, data, size_log2);
}

static const uint32_t val_ones[3] = { 0xff, 0xffff, 0xffffffff };

static uint32_t pci_data_read(PCIBus *s, uint32_t addr, int size_log2)
{
    PCIDevice *d;
    int bus_num, devfn, config_addr;
    
    bus_num = (addr >> 16) & 0xff;
    if (bus_num != s->bus_num)
        return val_ones[size_log2];
    devfn = (addr >> 8) & 0xff;
    d = s->device[devfn];
    if (!d)
        return val_ones[size_log2];
    config_addr = addr & 0xff;
    return pci_device_config_read(d, config_addr, size_log2);
}

PCIBus *pci_bus_init(PhysMemoryMap *mem_map, PhysMemoryMap *port_map)
{
    PCIBus *b = new PCIBus();
    b->bus_num = 0;
    b->mem_map = mem_map;
    b->port_map = port_map;
    b->msi_target = NULL;
    return b;
}

void pci_bus_set_irq(PCIBus *b, int pin, const IRQSignal *sig)
{
    assert(pin >= 0 && pin < 4);
    b->irq[pin] = *sig;
}

void pci_bus_set_bus_num(PCIBus *b, int bus_num)
{
    assert(bus_num >= 0 && bus_num < 256);
    b->bus_num = bus_num;
}

void pci_bus_set_msi_target(PCIBus *b, PCIMsiTarget *target)
{
    b->msi_target = target;
}

bool pci_bus_has_msi(PCIBus *b)
{
    return b->msi_target != NULL;
}

uint32_t pci_bus_config_read(PCIBus *b, uint32_t addr, int size_log2)
{
    return pci_data_read(b, addr, size_log2);
}

void pci_bus_config_write(PCIBus *b, uint32_t addr, uint32_t data,
                          int size_log2)
{
    pci_data_write(b, addr, data, size_log2);
}

/* warning: only valid for one DEVIO page. Return NULL if no memory at
   the given address */
uint8_t *pci_device_get_dma_ptr(PCIDevice *d, uint64_t addr, bool is_rw)
{
    return d->bus->mem_map->GetRamPtr(addr, is_rw);
}

void pci_device_send_msi(PCIDevice *d, uint64_t addr, uint32_t data)
{
    PCIBus *b = d->bus;

    if (b->msi_target) {
        b->msi_target->SendMsi(addr, data);
        return;
    }

    /* No receiver on this bus: an MSI is architecturally a posted memory
       write, so perform it. On a machine with no MSI controller the write
       lands in RAM and nothing observes it, which is what the hardware would
       do too. */
    uint8_t *ptr = b->mem_map->GetRamPtr(addr, true);
    if (ptr)
        put_le32(ptr, data);
}

void pci_device_set_config8(PCIDevice *d, uint8_t addr, uint8_t val)
{
    d->config[addr] = val;
}

void pci_device_set_config16(PCIDevice *d, uint8_t addr, uint16_t val)
{
    put_le16(&d->config[addr], val);
}

uint32_t pci_device_get_config(PCIDevice *d, uint8_t addr, int size_log2)
{
    return pci_device_config_read(d, addr, size_log2);
}

int pci_device_get_devfn(PCIDevice *d)
{
    return d->devfn;
}

/* return the offset of the capability or < 0 if error. */
int pci_add_capability(PCIDevice *d, const uint8_t *buf, int size)
{
    int offset;
    
    offset = d->next_cap_offset;
    if ((offset + size) > 256)
        return -1;
    d->next_cap_offset += size;
    d->config[PCI_STATUS] |= PCI_STATUS_CAP_LIST;
    memcpy(d->config + offset, buf, size);
    d->config[offset + 1] = d->config[PCI_CAPABILITY_LIST];
    d->config[PCI_CAPABILITY_LIST] = offset;
    return offset;
}

/* i440FX host bridge */

struct I440FXState: public IRQTarget {
    PCIBus *pci_bus;
    PCIDevice *pci_dev;
    PCIDevice *piix3_dev;
    uint32_t config_reg;
    uint8_t pic_irq_state[16];
    IRQSignal *pic_irqs; /* 16 irqs */

    uint32_t ReadAddr(uint32_t offset, int size_log2);
    void WriteAddr(uint32_t offset, uint32_t data, int size_log2);
    uint32_t ReadData(uint32_t offset, int size_log2);
    void WriteData(uint32_t offset, uint32_t data, int size_log2);

    DeviceIOAdapter<I440FXState, &I440FXState::ReadAddr,
                    &I440FXState::WriteAddr> fAddrIo {*this};
    DeviceIOAdapter<I440FXState, &I440FXState::ReadData,
                    &I440FXState::WriteData> fDataIo {*this};

    void SetIRQ(int irq_num, int level) override;
};

void I440FXState::WriteAddr(uint32_t offset, uint32_t data, int size_log2)
{
    I440FXState *s = this;
    s->config_reg = data;
}

uint32_t I440FXState::ReadAddr(uint32_t offset, int size_log2)
{
    I440FXState *s = this;
    return s->config_reg;
}

void I440FXState::WriteData(uint32_t offset, uint32_t data, int size_log2)
{
    I440FXState *s = this;
    if (s->config_reg & 0x80000000) {
        if (size_log2 == 2) {
            /* it is simpler to assume 32 bit config accesses are
               always aligned */
            pci_data_write(s->pci_bus, s->config_reg & ~3, data, size_log2);
        } else {
            pci_data_write(s->pci_bus, s->config_reg | offset, data, size_log2);
        }
    }
}

uint32_t I440FXState::ReadData(uint32_t offset, int size_log2)
{
    I440FXState *s = this;
    if (!(s->config_reg & 0x80000000))
        return val_ones[size_log2];
    if (size_log2 == 2) {
        /* it is simpler to assume 32 bit config accesses are
           always aligned */
        return pci_data_read(s->pci_bus, s->config_reg & ~3, size_log2);
    } else {
        return pci_data_read(s->pci_bus, s->config_reg | offset, size_log2);
    }
}

void I440FXState::SetIRQ(int irq_num, int irq_level)
{
    I440FXState *s = this;
    PCIDevice *hd = s->piix3_dev;
    int pic_irq;
    
    /* map to the PIC irq (different IRQs can be mapped to the same
       PIC irq) */
    hd->config[0x60 + irq_num] &= ~0x80;
    pic_irq = hd->config[0x60 + irq_num];
    if (pic_irq < 16) {
        if (irq_level)
            s->pic_irq_state[pic_irq] |= 1 << irq_num;
        else
            s->pic_irq_state[pic_irq] &= ~(1 << irq_num);
        s->pic_irqs[pic_irq].Set((s->pic_irq_state[pic_irq] != 0));
    }
}

I440FXState *i440fx_init(PCIBus **pbus, int *ppiix3_devfn,
                         PhysMemoryMap *mem_map, PhysMemoryMap *port_map,
                         IRQSignal *pic_irqs)
{
    I440FXState *s;
    PCIBus *b;
    PCIDevice *d;
    int i;
    
    s = new I440FXState();

    b = pci_bus_init(mem_map, port_map);

    s->pic_irqs = pic_irqs;
    for(i = 0; i < 4; i++) {
        b->irq[i].Init(s, i);
    }
    
    port_map->RegisterDevice(0xcf8, 1, &s->fAddrIo, DEVIO_SIZE32);
    port_map->RegisterDevice(0xcfc, 4, &s->fDataIo,
                             DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32);
    d = pci_register_device(b, "i440FX", 0, 0x8086, 0x1237, 0x02, 0x0600);
    put_le16(&d->config[PCI_SUBSYSTEM_VENDOR_ID], 0x1af4); /* Red Hat, Inc. */
    put_le16(&d->config[PCI_SUBSYSTEM_ID], 0x1100); /* QEMU virtual machine */
    
    s->pci_dev = d;
    s->pci_bus = b;

    s->piix3_dev = pci_register_device(b, "PIIX3", 8, 0x8086, 0x7000,
                                       0x00, 0x0601);
    pci_device_set_config8(s->piix3_dev, 0x0e, 0x80); /* header type */

    *pbus = b;
    *ppiix3_devfn = s->piix3_dev->devfn;
    return s;
}

/* in case no BIOS is used, map the interrupts. */
void i440fx_map_interrupts(I440FXState *s, uint8_t *elcr,
                           const uint8_t *pci_irqs)
{
    PCIBus *b = s->pci_bus;
    PCIDevice *d, *hd;
    int irq_num, pic_irq, devfn, i;
    
    /* set a default PCI IRQ mapping to PIC IRQs */
    hd = s->piix3_dev;

    elcr[0] = 0;
    elcr[1] = 0;
    for(i = 0; i < 4; i++) {
        irq_num = pci_irqs[i];
        hd->config[0x60 + i] = irq_num;
        elcr[irq_num >> 3] |= (1 << (irq_num & 7));
    }

    for(devfn = 0; devfn < 256; devfn++) {
        d = b->device[devfn];
        if (!d)
            continue;
        if (d->config[PCI_INTERRUPT_PIN]) {
            irq_num = 0;
            irq_num = bus_map_irq(d, irq_num);
            pic_irq = hd->config[0x60 + irq_num];
            if (pic_irq < 16) {
                d->config[PCI_INTERRUPT_LINE] = pic_irq;
            }
        }
    }
}
