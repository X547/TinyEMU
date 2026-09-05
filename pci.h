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
#ifndef PCI_H
#define PCI_H

#include "iomem.h"

typedef struct PCIBus PCIBus;
typedef struct PCIDevice PCIDevice;

/* bar type */
#define PCI_ADDRESS_SPACE_MEM		0x00
#define PCI_ADDRESS_SPACE_IO		0x01
#define PCI_ADDRESS_SPACE_MEM_PREFETCH	0x08

#define PCI_ROM_SLOT 6
#define PCI_NUM_REGIONS 7

/* PCI config addresses */
#define PCI_VENDOR_ID		0x00	/* 16 bits */
#define PCI_DEVICE_ID		0x02	/* 16 bits */
#define PCI_COMMAND		0x04	/* 16 bits */
#define PCI_COMMAND_IO		(1 << 0)
#define PCI_COMMAND_MEMORY	(1 << 1)
#define PCI_STATUS		0x06	/* 16 bits */
#define  PCI_STATUS_CAP_LIST	(1 << 4)
#define PCI_CLASS_PROG		0x09
#define PCI_SUBSYSTEM_VENDOR_ID	0x2c    /* 16 bits */
#define PCI_SUBSYSTEM_ID	0x2e    /* 16 bits */
#define PCI_CAPABILITY_LIST	0x34    /* 8 bits */
#define PCI_INTERRUPT_LINE	0x3c    /* 8 bits */
#define PCI_INTERRUPT_PIN	0x3d    /* 8 bits */

/* Implemented by a device to learn where the guest mapped one of its BARs. */
class PCIBarTarget {
public:
    virtual ~PCIBarTarget() = default;

    virtual void SetBar(int bar_num, uint32_t addr, bool enabled) = 0;
};

/* Implemented by a host bridge that contains an MSI receiver. A message
   signalled interrupt really is just a memory write by the device, so a bus
   without one of these delivers it as exactly that. */
class PCIMsiTarget {
public:
    virtual ~PCIMsiTarget() = default;

    virtual void SendMsi(uint64_t addr, uint32_t data) = 0;
};

/* A bare PCI bus, with no host bridge attached yet. 'port_map' may be null on
   machines without a port I/O space. The caller wires the four INTx lines with
   pci_bus_set_irq(). */
PCIBus *pci_bus_init(PhysMemoryMap *mem_map, PhysMemoryMap *port_map);
void pci_bus_set_irq(PCIBus *b, int pin, const IRQSignal *sig);

/* The bus number this bus answers configuration cycles for. Defaults to 0; a
   bridge that puts its devices on a secondary bus sets it here. */
void pci_bus_set_bus_num(PCIBus *b, int bus_num);

/* Install the bus's MSI receiver. pci_bus_has_msi() lets a device decide
   whether to advertise MSI-X at all: offering it on a bus where nothing would
   ever collect the message would leave the guest with no interrupts. */
void pci_bus_set_msi_target(PCIBus *b, PCIMsiTarget *target);
bool pci_bus_has_msi(PCIBus *b);

/* The INTx swizzle this bus applies, exposed so that a host bridge can derive
   its FDT "interrupt-map" from the very function that routes the interrupt at
   run time. 'irq_num' and the result are 0-based (INTA = 0). */
int pci_bus_map_irq(int devfn, int irq_num);

/* Configuration space access by an arbitrary host bridge. 'addr' is
   (bus << 16) | (devfn << 8) | register. */
uint32_t pci_bus_config_read(PCIBus *b, uint32_t addr, int size_log2);
void pci_bus_config_write(PCIBus *b, uint32_t addr, uint32_t data,
                          int size_log2);

PCIDevice *pci_register_device(PCIBus *b, const char *name, int devfn,
                               uint16_t vendor_id, uint16_t device_id,
                               uint8_t revision, uint16_t class_id);
PhysMemoryMap *pci_device_get_mem_map(PCIDevice *d);
PhysMemoryMap *pci_device_get_port_map(PCIDevice *d);
void pci_register_bar(PCIDevice *d, unsigned int bar_num,
                      uint32_t size, int type, PCIBarTarget *bar_target);
IRQSignal *pci_device_get_irq(PCIDevice *d, unsigned int irq_num);
uint8_t *pci_device_get_dma_ptr(PCIDevice *d, uint64_t addr, bool is_rw);

/* Post one MSI. Goes to the bus's PCIMsiTarget if it has one, and otherwise is
   performed as the plain memory write it is defined to be. */
void pci_device_send_msi(PCIDevice *d, uint64_t addr, uint32_t data);

void pci_device_set_config8(PCIDevice *d, uint8_t addr, uint8_t val);
void pci_device_set_config16(PCIDevice *d, uint8_t addr, uint16_t val);
/* Read back a device's own configuration space. A device whose behaviour
   depends on a register the guest writes through the generic config path (the
   MSI-X control word, say) reads it here rather than shadowing the write. */
uint32_t pci_device_get_config(PCIDevice *d, uint8_t addr, int size_log2);
int pci_device_get_devfn(PCIDevice *d);
int pci_add_capability(PCIDevice *d, const uint8_t *buf, int size);

typedef struct I440FXState I440FXState;

I440FXState *i440fx_init(PCIBus **pbus, int *ppiix3_devfn,
                         PhysMemoryMap *mem_map, PhysMemoryMap *port_map,
                         IRQSignal *pic_irqs);
void i440fx_map_interrupts(I440FXState *s, uint8_t *elcr,
                           const uint8_t *pci_irqs);

#endif /* PCI_H */
