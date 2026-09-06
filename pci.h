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
/* Set by a driver that intends to poll rather than take INTx. */
#define PCI_COMMAND_INTX_DISABLE (1 << 10)
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

/* MSI-X capability, as it sits in configuration space. */
#define PCI_CAP_ID_MSIX          0x11
#define PCI_MSIX_FLAGS           0x02 /* 16 bits */
#define  PCI_MSIX_FLAGS_ENABLE   0x8000
#define  PCI_MSIX_FLAGS_MASKALL  0x4000
#define PCI_MSIX_TABLE           0x04
#define PCI_MSIX_PBA             0x08
#define PCI_MSIX_CAP_LEN         12

/* Per vector mask, in an MSI-X table entry's control word. */
#define PCI_MSIX_ENTRY_CTRL_MASKBIT 1

/* What a driver writes to a vector register to say "no interrupt", and what
   it reads back if the device could not honour its choice. */
#define PCI_MSIX_NO_VECTOR 0xffff


/* One MSI-X table entry, laid out as the guest sees it. */
struct PCIMsixEntry {
    uint32_t addr_lo;
    uint32_t addr_hi;
    uint32_t data;
    uint32_t vector_ctrl; /* bit 0 masks the vector */
};


/* A device's MSI-X capability, together with the vector table and the pending
   bit array that go with it. Both of those live inside one of the device's
   BARs, so the device forwards the accesses that land in their windows to
   TableRead/TableWrite/PbaRead; everything else about the mechanism -- the two
   levels of masking, the pending bits, delivery -- is handled here.

   Init() declines on a bus whose bridge has no MSI receiver, because a driver
   that chose MSI-X there would have nothing to collect the message. The object
   then reports itself absent, which is how the owning device knows to stay on
   the INTx path. */
class PCIMsixState {
private:
    PCIDevice *fDev = nullptr;
    int fCapOffset = -1;
    int fVectorCount = 0;
    PCIMsixEntry *fTable = nullptr;
    uint32_t *fPba = nullptr; /* one bit per vector */

    bool MaskedAll() const;
    uint32_t *TableSlot(uint32_t offset);

public:
    ~PCIMsixState();

    /* Add the capability and allocate the table. 'table_offset' and
       'pba_offset' are byte offsets within BAR 'bar_num', which the device
       must map itself. Returns whether the capability was offered. */
    bool Init(PCIDevice *dev, int bar_num, int vector_count,
              uint32_t table_offset, uint32_t pba_offset);

    bool Present() const {return fCapOffset >= 0;}
    int VectorCount() const {return fVectorCount;}

    /* True once the guest has turned the capability on. While it is on, the
       device's INTx line must stay low. */
    bool Enabled() const;

    /* Accept a vector a driver assigned to one of the device's interrupt
       sources. One that cannot be honoured reads back as PCI_MSIX_NO_VECTOR,
       which is how the driver is told the request was refused. */
    uint16_t AcceptVector(uint32_t vector) const;

    /* Post one vector, or record it pending if it is masked. A masked vector
       is delivered when the mask is lifted. */
    void Send(int vector);

    /* The table and the pending bit array are read and written a word at a
       time like any other register. Offsets are relative to each window. */
    uint32_t TableRead(uint32_t offset, int size_log2);
    void TableWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t PbaRead(uint32_t offset, int size_log2);
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
