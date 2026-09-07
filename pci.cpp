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
    uint64_t size; /* 0 means no mapping defined */
    uint8_t type;
    bool enabled;   /* true if mapping is enabled */
    bool is64;      /* the region spans this slot and the next */
    bool high_half; /* this slot is the upper half of the preceding region */
    PCIBarTarget *bar_target;
} PCIIORegion;

struct PCIDevice: public IRQTarget {
    PCIBus *bus;
    uint8_t devfn;
    IRQSignal irq[4];
    uint8_t config[PCI_EXT_CONFIG_SIZE];
    uint16_t next_cap_offset; /* offset of the next capability */
    uint16_t next_ext_cap_offset; /* offset of the next extended capability */
    char *name; /* for debug only */
    PCIIORegion io_regions[PCI_NUM_REGIONS];
    /* Non-null on a type 1 function: the bus this bridge forwards to. */
    PCIBus *secondary_bus;
    int pcie_type; /* -1 without a PCI Express capability */

    void SetIRQ(int irq_num, int level) override;
};

struct PCIBus {
    int bus_num; /* only meaningful on a root bus */
    /* The bridge this bus hangs from, or null on a root bus. It supplies the
       bus number, the MSI receiver and the INTx path. */
    PCIDevice *parent_bridge;
    PCIDevice *device[256];
    PhysMemoryMap *mem_map;
    PhysMemoryMap *port_map;
    uint32_t irq_state[4][8]; /* one bit per device */
    IRQSignal irq[4];
    PCIMsiTarget *msi_target; /* null if the bridge has no MSI receiver */
    bool is_pcie;
};

static bool pci_is_bridge(const PCIDevice *d)
{
    return (d->config[PCI_HEADER_TYPE] & 0x7f) == PCI_HEADER_TYPE_BRIDGE;
}

int pci_bus_map_irq(int devfn, int irq_num)
{
    int slot_addend;
    slot_addend = (devfn >> 3) - 1;
    return (irq_num + slot_addend) & 3;
}

/* The swizzle a PCI to PCI bridge applies to the pin of a device on its
   secondary bus. It differs from the root bus one above by the constant the
   host bridge folded into its FDT "interrupt-map": a guest walking up the
   tree applies exactly this at every bridge and consults that table once, at
   the top, so the two must be spelled differently to agree. */
static int pci_bridge_map_irq(int devfn, int irq_num)
{
    return (irq_num + (devfn >> 3)) & 3;
}

static int bus_map_irq(PCIDevice *d, int irq_num)
{
    if (d->bus->parent_bridge != NULL)
        return pci_bridge_map_irq(d->devfn, irq_num);
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
static PCIDevice *pci_register_device_type(PCIBus *b, const char *name,
                                           int devfn, uint16_t vendor_id,
                                           uint16_t device_id,
                                           uint8_t revision, uint16_t class_id,
                                           uint8_t header_type, int port_type)
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

    put_le16(d->config + PCI_VENDOR_ID, vendor_id);
    put_le16(d->config + PCI_DEVICE_ID, device_id);
    d->config[0x08] = revision;
    put_le16(d->config + PCI_CLASS_DEVICE, class_id);
    d->config[PCI_HEADER_TYPE] = header_type;
    d->next_cap_offset = 0x40;
    d->next_ext_cap_offset = PCI_EXT_CAP_START;
    d->secondary_bus = NULL;
    d->pcie_type = -1;

    for(i = 0; i < 4; i++)
        d->irq[i].Init(d, i);
    b->device[devfn] = d;

    /* A function on a PCI Express hierarchy must say so, because that is what
       tells a guest its configuration space runs past 256 bytes. */
    if (pci_bus_is_pcie(b))
        pci_add_pcie_capability(d, port_type);

    return d;
}

PCIDevice *pci_register_device(PCIBus *b, const char *name, int devfn,
                               uint16_t vendor_id, uint16_t device_id,
                               uint8_t revision, uint16_t class_id)
{
    return pci_register_device_type(b, name, devfn, vendor_id, device_id,
                                    revision, class_id, PCI_HEADER_TYPE_NORMAL,
                                    PCI_EXP_TYPE_ENDPOINT);
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
        if (addr <= PCI_EXT_CONFIG_SIZE - 2)
            val = get_le16(d->config + addr);
        else
            val = *(uint8_t *)(d->config + addr);
        break;
    case 2:
        /* Aligned by construction, but a guest is free to name an address in
           the last few bytes anyway, and reading a whole word there would be
           reading past the space. */
        if (addr > PCI_EXT_CONFIG_SIZE - 4)
            return 0xffffffff;
        val = get_le32(d->config + addr);
        break;
    default:
        abort();
    }
#ifdef DEBUG_CONFIG
    printf("pci_config_read: dev=%s addr=0x%03x val=0x%x s=%d\n",
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

/* How many base address registers this header type has, and where its
   expansion ROM register sits. A type 1 function spends the space a type 0
   one gives to BARs 2 to 5 on its bus numbers and forwarding windows. */
static int pci_bar_count(const PCIDevice *d)
{
    return pci_is_bridge(d) ? 2 : 6;
}

static uint32_t pci_bar_offset(const PCIDevice *d, int bar_num)
{
    if (bar_num == PCI_ROM_SLOT)
        return pci_is_bridge(d) ? PCI_ROM_ADDRESS1 : PCI_ROM_ADDRESS;
    return PCI_BASE_ADDRESS_0 + 4 * bar_num;
}

/* The region a 32 bit configuration write at 'addr' lands in, or -1. */
static int pci_bar_reg(const PCIDevice *d, uint32_t addr)
{
    if (addr == pci_bar_offset(d, PCI_ROM_SLOT))
        return PCI_ROM_SLOT;
    if (addr >= PCI_BASE_ADDRESS_0 &&
        addr < PCI_BASE_ADDRESS_0 + 4u * pci_bar_count(d))
        return (addr - PCI_BASE_ADDRESS_0) >> 2;
    return -1;
}

void pci_register_bar(PCIDevice *d, unsigned int bar_num,
                      uint64_t size, int type, PCIBarTarget *bar_target)
{
    PCIIORegion *r;
    bool is64;

    assert(bar_num < PCI_NUM_REGIONS);
    assert((size & (size - 1)) == 0); /* power of two */
    assert(size >= 4);

    is64 = (type & PCI_ADDRESS_SPACE_MEM_TYPE_64) != 0 &&
        (type & PCI_ADDRESS_SPACE_IO) == 0;
    if (!is64)
        assert(size <= 0x100000000ull);

    r = &d->io_regions[bar_num];
    assert(r->size == 0 && !r->high_half);
    r->size = size;
    r->type = type;
    r->enabled = false;
    r->is64 = is64;
    r->bar_target = bar_target;

    if (is64) {
        /* The upper half is a register of its own, so it needs a slot of its
           own; the expansion ROM register has no room for one. */
        PCIIORegion *hi = r + 1;
        assert(bar_num != PCI_ROM_SLOT);
        assert((int)bar_num + 1 < pci_bar_count(d));
        assert(hi->size == 0 && !hi->high_half);
        hi->high_half = true;
        put_le32(&d->config[pci_bar_offset(d, bar_num + 1)], 0);
    }

    /* set the config value */
    put_le32(&d->config[pci_bar_offset(d, bar_num)],
             bar_num == PCI_ROM_SLOT ? 0 : (uint32_t)r->type);
}

static void pci_update_mappings(PCIDevice *d)
{
    int cmd, i;
    uint32_t offset;
    uint64_t new_addr;
    bool new_enabled;
    PCIIORegion *r;

    cmd = get_le16(&d->config[PCI_COMMAND]);

    for(i = 0; i < PCI_NUM_REGIONS; i++) {
        r = &d->io_regions[i];
        if (r->size == 0 || r->high_half)
            continue;
        offset = pci_bar_offset(d, i);
        new_addr = get_le32(&d->config[offset]);
        if (r->is64)
            new_addr |= (uint64_t)get_le32(&d->config[offset + 4]) << 32;

        new_enabled = false;
        if (r->type & PCI_ADDRESS_SPACE_IO) {
            new_enabled = (cmd & PCI_COMMAND_IO) != 0;
        } else if (cmd & PCI_COMMAND_MEMORY) {
            /* The expansion ROM has an enable bit of its own. */
            new_enabled = i != PCI_ROM_SLOT || (new_addr & 1) != 0;
        }
        if (new_enabled) {
            r->bar_target->SetBar(i, new_addr & ~(r->size - 1), true);
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

    reg = pci_bar_reg(d, addr);
    if (reg < 0)
        return -1;
    //    printf("%s: write bar addr=%x data=%x\n", d->name, addr, val);
    r = &d->io_regions[reg];
    if (r->high_half) {
        /* The upper half of the 64 bit region in the slot before this one.
           Sizing works the same way it does below: the bits the size leaves
           fixed read back as zero. */
        PCIIORegion *lo = r - 1;
        if (lo->size == 0)
            return -1;
        val &= (uint32_t)(~(lo->size - 1) >> 32);
    } else if (r->size == 0) {
        return -1;
    } else if (reg == PCI_ROM_SLOT) {
        val = val & ((uint32_t)~(r->size - 1) | 1);
    } else {
        val = (val & (uint32_t)~(r->size - 1)) | r->type;
    }
    put_le32(d->config + addr, val);
    pci_update_mappings(d);
    return 0;
}

/* The bits a guest may change at 'addr'. Anything past the header is fully
   writable, which is what lets a capability be programmed without having to
   describe itself here; extended configuration space is read only, because
   the only extended capability modelled is. */
static uint8_t pci_config_wmask(const PCIDevice *d, uint32_t addr)
{
    if (addr >= PCI_CONFIG_SIZE)
        return 0x00;
    if (addr >= 0x40)
        return 0xff;

    switch(addr) {
    case 0x00: case 0x01: /* vendor id */
    case 0x02: case 0x03: /* device id */
    case 0x06: case 0x07: /* status; its error bits clear on a written one */
    case 0x08:            /* revision */
    case 0x09: case 0x0a: case 0x0b: /* class code */
    case 0x0e:            /* header type */
    case 0x34:            /* capability list pointer */
    case 0x3d:            /* interrupt pin */
        return 0x00;
    }

    if (pci_is_bridge(d)) {
        switch(addr) {
        /* base addresses, written via pci_write_bar() */
        case PCI_BASE_ADDRESS_0 ... PCI_BASE_ADDRESS_0 + 7:
        /* secondary status, as the primary one above */
        case PCI_SEC_STATUS: case PCI_SEC_STATUS + 1:
        /* the I/O window is 16 bit, so it has no upper halves */
        case PCI_IO_BASE_UPPER16 ... PCI_IO_LIMIT_UPPER16 + 1:
        /* expansion rom */
        case PCI_ROM_ADDRESS1 ... PCI_ROM_ADDRESS1 + 3:
            return 0x00;
        /* The low nibble of a window register reports what the bridge can
           decode rather than where it decodes, so it is read only. */
        case PCI_IO_BASE:
        case PCI_IO_LIMIT:
        case PCI_MEMORY_BASE:
        case PCI_MEMORY_LIMIT:
        case PCI_PREF_MEMORY_BASE:
        case PCI_PREF_MEMORY_LIMIT:
            return 0xf0;
        }
        return 0xff;
    }

    switch(addr) {
    /* base addresses, written via pci_write_bar() */
    case PCI_BASE_ADDRESS_0 ... PCI_BASE_ADDRESS_0 + 23:
    /* subsystem ids */
    case PCI_SUBSYSTEM_VENDOR_ID ... PCI_SUBSYSTEM_ID + 1:
    /* expansion rom */
    case PCI_ROM_ADDRESS ... PCI_ROM_ADDRESS + 3:
        return 0x00;
    }
    return 0xff;
}

/* The bits at 'addr' a written one clears rather than sets. Only the error
   bits in the top half of a status register behave that way; treating the
   whole register as one would let a driver reading, or'ing and writing back
   the command dword clear the capability list bit next to it. */
static uint8_t pci_config_w1c_mask(const PCIDevice *d, uint32_t addr)
{
    if (addr == PCI_STATUS + 1 ||
        (pci_is_bridge(d) && addr == PCI_SEC_STATUS + 1)) {
        /* Everything but the two bits reporting the device select timing. */
        return 0xf9;
    }
    return 0x00;
}

static void pci_device_config_write8(PCIDevice *d, uint32_t addr,
                                     uint32_t data)
{
    uint8_t mask = pci_config_wmask(d, addr);
    uint8_t w1c = pci_config_w1c_mask(d, addr);

    d->config[addr] &= ~(data & w1c);
    d->config[addr] = (d->config[addr] & ~mask) | (data & mask);
}


static void pci_device_config_write(PCIDevice *d, uint32_t addr,
                                    uint32_t data, int size_log2)
{
    int size, i;
    uint32_t addr1;

#ifdef DEBUG_CONFIG
    printf("pci_config_write: dev=%s addr=0x%03x val=0x%x s=%d\n",
           d->name, addr, data, 1 << size_log2);
#endif
    if (size_log2 == 2 && pci_bar_reg(d, addr) >= 0) {
        if (pci_write_bar(d, addr, data) == 0)
            return;
    }
    size = 1 << size_log2;
    for(i = 0; i < size; i++) {
        addr1 = addr + i;
        if (addr1 < PCI_EXT_CONFIG_SIZE) {
            pci_device_config_write8(d, addr1, (data >> (i * 8)) & 0xff);
        }
    }
    if (PCI_COMMAND >= addr && PCI_COMMAND < addr + size) {
        pci_update_mappings(d);
    }
}


/* Route a configuration cycle the way hardware does: a bus answers for its own
   number, and hands anything else to the one bridge on it whose programmed
   secondary to subordinate range contains the target. Before firmware has
   numbered the bridges nothing but the root bus is reachable, which is what
   makes enumeration work at all. */
static PCIDevice *pci_find_device(PCIBus *b, int bus_num, int devfn)
{
    int i;

    if (bus_num == pci_bus_get_bus_num(b))
        return b->device[devfn];

    for(i = 0; i < 256; i++) {
        PCIDevice *br = b->device[i];
        if (br == NULL || br->secondary_bus == NULL)
            continue;
        if (bus_num >= br->config[PCI_SECONDARY_BUS] &&
            bus_num <= br->config[PCI_SUBORDINATE_BUS])
            return pci_find_device(br->secondary_bus, bus_num, devfn);
    }
    return NULL;
}

static void pci_data_write(PCIBus *s, uint32_t addr,
                           uint32_t data, int size_log2)
{
    PCIDevice *d;

    d = pci_find_device(s, (addr >> 20) & 0xff, (addr >> 12) & 0xff);
    if (!d)
        return;
    pci_device_config_write(d, addr & (PCI_EXT_CONFIG_SIZE - 1), data,
                            size_log2);
}

static const uint32_t val_ones[3] = { 0xff, 0xffff, 0xffffffff };

static uint32_t pci_data_read(PCIBus *s, uint32_t addr, int size_log2)
{
    PCIDevice *d;

    d = pci_find_device(s, (addr >> 20) & 0xff, (addr >> 12) & 0xff);
    if (!d)
        return val_ones[size_log2];
    return pci_device_config_read(d, addr & (PCI_EXT_CONFIG_SIZE - 1),
                                  size_log2);
}

PCIBus *pci_bus_init(PhysMemoryMap *mem_map, PhysMemoryMap *port_map)
{
    PCIBus *b = new PCIBus();
    b->bus_num = 0;
    b->parent_bridge = NULL;
    b->mem_map = mem_map;
    b->port_map = port_map;
    b->msi_target = NULL;
    b->is_pcie = false;
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
    assert(b->parent_bridge == NULL);
    b->bus_num = bus_num;
}

int pci_bus_get_bus_num(PCIBus *b)
{
    /* Behind a bridge the number is whatever the guest wrote into it, so it
       is read from there rather than cached: the two can never drift. */
    if (b->parent_bridge != NULL)
        return b->parent_bridge->config[PCI_SECONDARY_BUS];
    return b->bus_num;
}

bool pci_bus_is_root(PCIBus *b)
{
    return b->parent_bridge == NULL;
}

void pci_bus_set_pcie(PCIBus *b, bool is_pcie)
{
    b->is_pcie = is_pcie;
}

bool pci_bus_is_pcie(PCIBus *b)
{
    while (!b->is_pcie && b->parent_bridge != NULL)
        b = b->parent_bridge->bus;
    return b->is_pcie;
}

void pci_bus_set_msi_target(PCIBus *b, PCIMsiTarget *target)
{
    b->msi_target = target;
}

/* The receiver a message from this bus would reach. A bridge has none of its
   own: the message travels up as the posted write it is. */
static PCIMsiTarget *pci_bus_msi_target(PCIBus *b)
{
    while (b->msi_target == NULL && b->parent_bridge != NULL)
        b = b->parent_bridge->bus;
    return b->msi_target;
}

bool pci_bus_has_msi(PCIBus *b)
{
    return pci_bus_msi_target(b) != NULL;
}

PCIBus *pci_bridge_init(PCIBus *parent, int devfn, const char *name,
                        uint16_t vendor_id, uint16_t device_id, int port_type,
                        PCIDevice **pdev)
{
    PCIDevice *d;
    PCIBus *b;
    int i;

    d = pci_register_device_type(parent, name, devfn, vendor_id, device_id,
                                 0x00, PCI_CLASS_BRIDGE_PCI,
                                 PCI_HEADER_TYPE_BRIDGE, port_type);
    if (d == NULL)
        return NULL;

    /* The prefetchable window carries 64 bit addresses, so that a 64 bit BAR
       behind this bridge can be placed above 4 GB. The I/O window is 16 bit,
       which the zero left in the low nibble of PCI_IO_BASE reports. */
    d->config[PCI_PREF_MEMORY_BASE] = PCI_PREF_RANGE_TYPE_64;
    d->config[PCI_PREF_MEMORY_LIMIT] = PCI_PREF_RANGE_TYPE_64;

    b = pci_bus_init(parent->mem_map, parent->port_map);
    b->parent_bridge = d;
    d->secondary_bus = b;

    /* The four INTx lines of the new bus land on the bridge's own pins, so
       every tier applies the swizzle its hardware counterpart applies. */
    for(i = 0; i < 4; i++)
        b->irq[i].Init(d, i);

    if (pdev != NULL)
        *pdev = d;
    return b;
}

int pci_bus_bridge_port_type(PCIBus *b)
{
    if (b->parent_bridge == NULL)
        return PCI_EXP_TYPE_ROOT_PORT;

    /* A switch is an upstream port, the bus inside it, and the downstream
       ports on that bus, so the tiers alternate. Anything hanging off a
       downstream port or a root port is the upstream port of the next
       switch. */
    if (b->parent_bridge->pcie_type == PCI_EXP_TYPE_UPSTREAM)
        return PCI_EXP_TYPE_DOWNSTREAM;
    return PCI_EXP_TYPE_UPSTREAM;
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
    PCIMsiTarget *target = pci_bus_msi_target(b);

    if (target) {
        target->SendMsi(addr, data);
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

void pci_device_set_config8(PCIDevice *d, uint16_t addr, uint8_t val)
{
    assert(addr < PCI_EXT_CONFIG_SIZE);
    d->config[addr] = val;
}

void pci_device_set_config16(PCIDevice *d, uint16_t addr, uint16_t val)
{
    assert(addr + 1 < PCI_EXT_CONFIG_SIZE);
    put_le16(&d->config[addr], val);
}

uint32_t pci_device_get_config(PCIDevice *d, uint16_t addr, int size_log2)
{
    assert(addr + (1 << size_log2) <= PCI_EXT_CONFIG_SIZE);
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
    if ((offset + size) > PCI_CONFIG_SIZE)
        return -1;
    d->next_cap_offset += size;
    d->config[PCI_STATUS] |= PCI_STATUS_CAP_LIST;
    memcpy(d->config + offset, buf, size);
    d->config[offset + 1] = d->config[PCI_CAPABILITY_LIST];
    d->config[PCI_CAPABILITY_LIST] = offset;
    return offset;
}

int pci_add_ext_capability(PCIDevice *d, uint16_t cap_id, int version,
                           const uint8_t *body, int body_size)
{
    int offset, size, prev;

    size = (4 + body_size + 3) & ~3;
    offset = d->next_ext_cap_offset;
    if (offset + size > PCI_EXT_CONFIG_SIZE)
        return -1;

    put_le32(&d->config[offset], cap_id | ((uint32_t)version << 16));
    if (body_size > 0)
        memcpy(&d->config[offset + 4], body, body_size);

    /* Unlike the conventional list, the extended chain is walked forward from
       a fixed head, so a new capability is linked on at the tail. */
    if (offset > PCI_EXT_CAP_START) {
        uint32_t hdr;
        for(prev = PCI_EXT_CAP_START;;) {
            int next = (get_le32(&d->config[prev]) >> 20) & 0xffc;
            if (next == 0)
                break;
            prev = next;
        }
        hdr = get_le32(&d->config[prev]);
        put_le32(&d->config[prev],
                 (hdr & 0x000fffff) | ((uint32_t)offset << 20));
    }

    d->next_ext_cap_offset = offset + size;
    return offset;
}

int pci_add_pcie_capability(PCIDevice *d, int port_type)
{
    uint8_t cap[PCI_EXP_CAP_LEN];
    uint8_t dsn[8];
    int offset;
    /* Serial numbers must differ between functions, and nothing here needs
       them to mean anything more than that. */
    static uint32_t next_serial = 1;

    memset(cap, 0, sizeof(cap));
    cap[0] = PCI_CAP_ID_EXP;
    /* capability version 2, and the device/port type this function reports */
    put_le16(cap + 2, (2 << 0) | (port_type << 4));
    /* Role based error reporting, which is what tells a guest this is not a
       function from before the 1.1 revision of the specification. */
    put_le32(cap + 0x04, 1 << 15);
    /* link capabilities and status: one lane at 2.5 GT/s, link up */
    put_le32(cap + 0x0c, (1 << 0) | (1 << 4));
    put_le16(cap + 0x12, (1 << 0) | (1 << 4));
    offset = pci_add_capability(d, cap, sizeof(cap));
    if (offset < 0)
        return -1;
    d->pcie_type = port_type;

    /* One extended capability, so that a guest walking the chain above 256
       bytes finds a well formed one rather than having to trust that the
       space is there. */
    put_le32(dsn, next_serial++);
    put_le32(dsn + 4, 0x0000ffff); /* a locally administered OUI */
    pci_add_ext_capability(d, PCI_EXT_CAP_ID_DSN, 1, dsn, sizeof(dsn));
    return offset;
}

//#pragma mark - PCIMsixState

/* All the bits an access of this size covers. */
static uint32_t size_mask(int size_log2)
{
    if (size_log2 >= 2)
        return 0xffffffff;
    return (1u << (8 << size_log2)) - 1;
}

PCIMsixState::~PCIMsixState()
{
    delete[] fTable;
    delete[] fPba;
}

bool PCIMsixState::Init(PCIDevice *dev, int bar_num, int vector_count,
                        uint32_t table_offset, uint32_t pba_offset)
{
    uint8_t cap[PCI_MSIX_CAP_LEN];

    assert(vector_count > 0 && vector_count <= 2048);

    /* Offering the capability on a bus where nothing would ever collect the
       message would leave the guest with no interrupts at all. */
    if (!pci_bus_has_msi(dev->bus))
        return false;

    memset(cap, 0, sizeof(cap));
    cap[0] = PCI_CAP_ID_MSIX;
    put_le16(cap + PCI_MSIX_FLAGS, vector_count - 1);
    put_le32(cap + PCI_MSIX_TABLE, table_offset | bar_num);
    put_le32(cap + PCI_MSIX_PBA, pba_offset | bar_num);

    int offset = pci_add_capability(dev, cap, sizeof(cap));
    if (offset < 0)
        return false;

    fDev = dev;
    fCapOffset = offset;
    fVectorCount = vector_count;
    fTable = new PCIMsixEntry[vector_count] {};
    fPba = new uint32_t[(vector_count + 31) / 32] {};
    return true;
}

bool PCIMsixState::Enabled() const
{
    if (fCapOffset < 0)
        return false;
    uint32_t ctrl = pci_device_get_config(fDev, fCapOffset + PCI_MSIX_FLAGS, 1);
    return (ctrl & PCI_MSIX_FLAGS_ENABLE) != 0;
}

bool PCIMsixState::MaskedAll() const
{
    uint32_t ctrl = pci_device_get_config(fDev, fCapOffset + PCI_MSIX_FLAGS, 1);
    return (ctrl & PCI_MSIX_FLAGS_MASKALL) != 0;
}

uint16_t PCIMsixState::AcceptVector(uint32_t vector) const
{
    if (vector < (uint32_t)fVectorCount)
        return vector;
    return PCI_MSIX_NO_VECTOR;
}

void PCIMsixState::Send(int vector)
{
    if (vector < 0 || vector >= fVectorCount)
        return;

    PCIMsixEntry *e = &fTable[vector];
    if (MaskedAll() || (e->vector_ctrl & PCI_MSIX_ENTRY_CTRL_MASKBIT) != 0) {
        fPba[vector >> 5] |= 1u << (vector & 31);
        return;
    }
    fPba[vector >> 5] &= ~(1u << (vector & 31));
    pci_device_send_msi(fDev, ((uint64_t)e->addr_hi << 32) | e->addr_lo,
                        e->data);
}

uint32_t *PCIMsixState::TableSlot(uint32_t offset)
{
    /* The window is part of the BAR whether or not the capability naming it
       was ever offered, so a guest can reach here on a bus that has no MSI
       receiver. Without the capability there is no table to address. */
    if (fCapOffset < 0)
        return nullptr;

    uint32_t index = offset / sizeof(PCIMsixEntry);
    if (index >= (uint32_t)fVectorCount)
        return nullptr;

    PCIMsixEntry *e = &fTable[index];
    switch ((offset / 4) % 4) {
    case 0: return &e->addr_lo;
    case 1: return &e->addr_hi;
    case 2: return &e->data;
    default: return &e->vector_ctrl;
    }
}

uint32_t PCIMsixState::TableRead(uint32_t offset, int size_log2)
{
    const uint32_t *slot = TableSlot(offset);
    if (slot == nullptr)
        return 0;
    return (*slot >> ((offset & 3) * 8)) & size_mask(size_log2);
}

void PCIMsixState::TableWrite(uint32_t offset, uint32_t val, int size_log2)
{
    uint32_t *slot = TableSlot(offset);
    if (slot == nullptr)
        return;

    int shift = (offset & 3) * 8;
    uint32_t mask = size_mask(size_log2) << shift;
    uint32_t old = *slot;
    *slot = (old & ~mask) | ((val << shift) & mask);

    /* Lifting a vector's mask delivers whatever arrived while it was set. */
    uint32_t index = offset / sizeof(PCIMsixEntry);
    if (slot == &fTable[index].vector_ctrl &&
        (old & PCI_MSIX_ENTRY_CTRL_MASKBIT) != 0 &&
        (*slot & PCI_MSIX_ENTRY_CTRL_MASKBIT) == 0 &&
        (fPba[index >> 5] & (1u << (index & 31))) != 0) {
        Send(index);
    }
}

uint32_t PCIMsixState::PbaRead(uint32_t offset, int size_log2)
{
    uint32_t index = offset / 4;
    if (fCapOffset < 0 || index >= (uint32_t)((fVectorCount + 31) / 32))
        return 0;
    return (fPba[index] >> ((offset & 3) * 8)) & size_mask(size_log2);
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

/* Turn a CF8 address into the one the bus takes. The two differ now that the
   register number is 12 bits wide; the eight a CF8 cycle carries are all
   there is, so extended configuration space is simply out of reach here, as
   it is on the hardware this models. */
static uint32_t i440fx_config_addr(uint32_t config_reg, uint32_t offset)
{
    return PCI_CONFIG_ADDR((config_reg >> 16) & 0xff,
                           (config_reg >> 8) & 0xff,
                           (config_reg & 0xfc) | (offset & 3));
}

void I440FXState::WriteData(uint32_t offset, uint32_t data, int size_log2)
{
    I440FXState *s = this;
    if (s->config_reg & 0x80000000) {
        if (size_log2 == 2) {
            /* it is simpler to assume 32 bit config accesses are
               always aligned */
            offset = 0;
        }
        pci_data_write(s->pci_bus, i440fx_config_addr(s->config_reg, offset),
                       data, size_log2);
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
        offset = 0;
    }
    return pci_data_read(s->pci_bus, i440fx_config_addr(s->config_reg, offset),
                         size_log2);
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
    pci_device_set_config8(s->piix3_dev, PCI_HEADER_TYPE,
                           PCI_HEADER_TYPE_NORMAL | PCI_HEADER_TYPE_MULTI);

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
