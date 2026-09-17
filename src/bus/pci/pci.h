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

#include <memory>

#include "bits.h"
#include "iomem.h"

typedef struct PCIBus PCIBus;
typedef struct PCIDevice PCIDevice;

/* A bus owns the functions registered on it, and a bridge function the bus
   behind it, so only a root bus needs an owner of its own. */
struct PCIBusDeleter {
    void operator()(PCIBus *b) const;
};
typedef std::unique_ptr<PCIBus, PCIBusDeleter> PCIBusPtr;

/* bar type */
#define PCI_ADDRESS_SPACE_MEM		0x00
#define PCI_ADDRESS_SPACE_IO		0x01
/* A memory BAR that carries a 64 bit address, and so occupies the slot it is
   registered in and the one after it. */
#define PCI_ADDRESS_SPACE_MEM_TYPE_64	0x04
#define PCI_ADDRESS_SPACE_MEM_PREFETCH	0x08

#define PCI_ROM_SLOT 6
#define PCI_NUM_REGIONS 7

/* Configuration space as conventional PCI defines it, and as PCI Express
   extends it. Every device carries the larger one; on a bus whose host bridge
   only reaches the first 256 bytes the rest is simply never addressed. */
#define PCI_CONFIG_SIZE     0x100
#define PCI_EXT_CONFIG_SIZE 0x1000

/* Configuration addresses as this bus takes them, in the layout ECAM uses:
   bus, then devfn, then a 12 bit register number. */
#define PCI_CONFIG_ADDR(bus, devfn, reg) \
    (((uint32_t)(bus) << 20) | ((uint32_t)(devfn) << 12) | (uint32_t)(reg))

/* PCI config addresses */
#define PCI_VENDOR_ID		0x00	/* 16 bits */
#define PCI_DEVICE_ID		0x02	/* 16 bits */
#define PCI_COMMAND		0x04	/* 16 bits */
#define PCI_COMMAND_IO		(1 << 0)
#define PCI_COMMAND_MEMORY	(1 << 1)
/* Set by a driver that intends to poll rather than take INTx. */
#define PCI_COMMAND_INTX_DISABLE bit_at(10)
#define PCI_STATUS		0x06	/* 16 bits */
#define  PCI_STATUS_CAP_LIST	(1 << 4)
#define PCI_CLASS_PROG		0x09
#define PCI_CLASS_DEVICE	0x0a	/* 16 bits */
#define PCI_HEADER_TYPE		0x0e	/* 8 bits */
#define  PCI_HEADER_TYPE_NORMAL	0x00
#define  PCI_HEADER_TYPE_BRIDGE	0x01
#define  PCI_HEADER_TYPE_MULTI	0x80	/* more than one function */
#define PCI_BASE_ADDRESS_0	0x10
#define PCI_SUBSYSTEM_VENDOR_ID	0x2c    /* 16 bits */
#define PCI_SUBSYSTEM_ID	0x2e    /* 16 bits */
#define PCI_ROM_ADDRESS		0x30	/* type 0 header */
#define PCI_CAPABILITY_LIST	0x34    /* 8 bits */
#define PCI_INTERRUPT_LINE	0x3c    /* 8 bits */
#define PCI_INTERRUPT_PIN	0x3d    /* 8 bits */

/* Type 1 (PCI to PCI bridge) header. The window registers are here so that a
   guest can program them and read them back; the address decoding this
   emulator performs is flat, so nothing is gated on them. */
#define PCI_PRIMARY_BUS		0x18	/* 8 bits */
#define PCI_SECONDARY_BUS	0x19	/* 8 bits */
#define PCI_SUBORDINATE_BUS	0x1a	/* 8 bits */
#define PCI_SEC_LATENCY_TIMER	0x1b	/* 8 bits */
#define PCI_IO_BASE		0x1c	/* 8 bits, 4 KB units */
#define  PCI_IO_RANGE_TYPE_32	0x01	/* in the low nibble of both */
#define PCI_IO_LIMIT		0x1d	/* 8 bits */
#define PCI_SEC_STATUS		0x1e	/* 16 bits */
#define PCI_MEMORY_BASE		0x20	/* 16 bits, 1 MB units */
#define PCI_MEMORY_LIMIT	0x22	/* 16 bits */
#define PCI_PREF_MEMORY_BASE	0x24	/* 16 bits */
#define PCI_PREF_MEMORY_LIMIT	0x26	/* 16 bits */
#define  PCI_PREF_RANGE_TYPE_64	0x01	/* in the low nibble of both */
#define PCI_PREF_BASE_UPPER32	0x28	/* 32 bits */
#define PCI_PREF_LIMIT_UPPER32	0x2c	/* 32 bits */
#define PCI_IO_BASE_UPPER16	0x30	/* 16 bits */
#define PCI_IO_LIMIT_UPPER16	0x32	/* 16 bits */
#define PCI_ROM_ADDRESS1	0x38	/* type 1 header */
#define PCI_BRIDGE_CONTROL	0x3e	/* 16 bits */

#define PCI_CLASS_BRIDGE_PCI	0x0604

/* PCI Express capability, and the device/port types a function may report in
   it. Which one a function claims is what tells a guest whether it is looking
   at an endpoint, the port of a root complex, or the port of a switch. */
#define PCI_CAP_ID_EXP		0x10
#define PCI_EXP_CAP_LEN		0x3c	/* capability version 2 */
#define PCI_EXP_TYPE_ENDPOINT	0x0
#define PCI_EXP_TYPE_ROOT_PORT	0x4
#define PCI_EXP_TYPE_UPSTREAM	0x5
#define PCI_EXP_TYPE_DOWNSTREAM	0x6

/* Extended capabilities live above the conventional 256 bytes and are a
   forward chain starting at this offset. A zero header there is how a guest
   is told there are none. */
#define PCI_EXT_CAP_START	0x100
#define PCI_EXT_CAP_ID_DSN	0x0003	/* device serial number */

/* Implemented by a device to learn where the guest mapped one of its BARs.
   The address is 64 bits wide because a BAR may be. */
class PCIBarTarget {
public:
    virtual ~PCIBarTarget() = default;

    virtual void SetBar(int bar_num, uint64_t addr, bool enabled) = 0;
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
    std::unique_ptr<PCIMsixEntry[]> fTable;
    std::unique_ptr<uint32_t[]> fPba; /* one bit per vector */

    bool MaskedAll() const;
    uint32_t *TableSlot(uint32_t offset);

public:
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

/* The CPU side of a host bridge's I/O aperture, on a machine whose processor
   has no port instructions. Such a machine reaches port space through a
   memory window instead, so the bridge maps one of these over the window it
   was given and it forwards what lands there to the port numbers the aperture
   was assigned. The device tree says the same thing in its "ranges": an I/O
   range whose parent address is the window and whose child address is the
   first port. */
class PCIIOWindow final: public DeviceIO {
private:
    PhysMemoryMap *fPortMap = nullptr;
    uint64_t fPortBase = 0;
    uint64_t fPortSize = 0;

public:
    void Init(PhysMemoryMap *port_map, uint64_t port_base, uint64_t port_size);

    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;
};

/* A bare PCI bus, with no host bridge attached yet. 'port_map' may be null on
   machines without a port I/O space. The caller wires the four INTx lines with
   pci_bus_set_irq(). */
PCIBusPtr pci_bus_init(PhysMemoryMap *mem_map, PhysMemoryMap *port_map);
void pci_bus_set_irq(PCIBus *b, int pin, const IRQSignal *sig);

/* The bus number this bus answers configuration cycles for. Defaults to 0.
   Only meaningful for a root bus: the number of a bus behind a bridge is the
   one the guest programmed into that bridge's secondary bus register, and is
   read from there on every access. */
void pci_bus_set_bus_num(PCIBus *b, int bus_num);
int pci_bus_get_bus_num(PCIBus *b);

/* True for a bus a host bridge owns directly, as opposed to one behind a
   PCI to PCI bridge. */
bool pci_bus_is_root(PCIBus *b);

/* Whether this is a PCI Express hierarchy. Devices registered on such a bus
   are given a PCI Express capability, which is what makes their extended
   configuration space meaningful. Inherited by the buses behind bridges. */
void pci_bus_set_pcie(PCIBus *b, bool is_pcie);
bool pci_bus_is_pcie(PCIBus *b);

/* Install the bus's MSI receiver. pci_bus_has_msi() lets a device decide
   whether to advertise MSI-X at all: offering it on a bus where nothing would
   ever collect the message would leave the guest with no interrupts. A bus
   behind a bridge uses the receiver of the hierarchy it hangs from. */
void pci_bus_set_msi_target(PCIBus *b, PCIMsiTarget *target);
bool pci_bus_has_msi(PCIBus *b);

/* The INTx swizzle a root bus applies, exposed so that a host bridge can
   derive its FDT "interrupt-map" from the very function that routes the
   interrupt at run time. 'irq_num' and the result are 0-based (INTA = 0). */
int pci_bus_map_irq(int devfn, int irq_num);

/* Configuration space access by an arbitrary host bridge. 'addr' is
   PCI_CONFIG_ADDR(bus, devfn, register), and the register is 12 bits wide so
   that extended configuration space is reachable. The bus routes the cycle
   down through any bridge whose programmed bus range contains it. */
uint32_t pci_bus_config_read(PCIBus *b, uint32_t addr, int size_log2);
void pci_bus_config_write(PCIBus *b, uint32_t addr, uint32_t data,
                          int size_log2);

/* Where a device's capability list starts. The first byte above the header is
   the usual place, and is where it goes unless the device asks otherwise. */
#define PCI_FIRST_CAP_OFFSET 0x40

/* 'first_cap_offset' moves the capability list up, for a device whose binding
   puts a register of its own in the way: the PCI SD Host Controller
   specification defines a slot information register at 0x40, which is exactly
   where the list would otherwise begin. */
PCIDevice *pci_register_device(PCIBus *b, const char *name, int devfn,
                               uint16_t vendor_id, uint16_t device_id,
                               uint8_t revision, uint16_t class_id,
                               int first_cap_offset = PCI_FIRST_CAP_OFFSET);

/* The function at 'devfn' of this bus, or null if there is none. Lets a host
   bridge walk the devices it owns without reaching into the bus itself. */
PCIDevice *pci_bus_get_device(PCIBus *b, int devfn);

/* Add a type 1 function at 'devfn' of 'parent' and return the secondary bus
   it owns. Configuration cycles reach that bus once the guest has programmed
   the bridge's bus numbers, and its four INTx lines land on the bridge's own
   pins, so each tier applies the swizzle hardware applies. 'port_type' is the
   PCI Express port type to report, and is ignored on a bus that is not PCI
   Express. */
PCIBus *pci_bridge_init(PCIBus *parent, int devfn, const char *name,
                        uint16_t vendor_id, uint16_t device_id, int port_type,
                        PCIDevice **pdev);

/* The port type a bridge added to this bus should report. A bus a host bridge
   owns takes root ports; below one of those the tiers alternate, because a
   switch is an upstream port, an internal bus, and the downstream ports on
   it. Reporting anything else makes a guest correct the type itself. */
int pci_bus_bridge_port_type(PCIBus *b);

PhysMemoryMap *pci_device_get_mem_map(PCIDevice *d);
PhysMemoryMap *pci_device_get_port_map(PCIDevice *d);

/* Declare one base address register. A memory BAR asking for
   PCI_ADDRESS_SPACE_MEM_TYPE_64 is programmed by the guest as a pair of
   registers and consumes 'bar_num' and 'bar_num' + 1; only the first of the
   two is ever reported to the PCIBarTarget. */
void pci_register_bar(PCIDevice *d, unsigned int bar_num,
                      uint64_t size, int type, PCIBarTarget *bar_target);
IRQSignal *pci_device_get_irq(PCIDevice *d, unsigned int irq_num);
uint8_t *pci_device_get_dma_ptr(PCIDevice *d, uint64_t addr, bool is_rw);

/* Post one MSI. Goes to the bus's PCIMsiTarget if it has one, and otherwise is
   performed as the plain memory write it is defined to be. */
void pci_device_send_msi(PCIDevice *d, uint64_t addr, uint32_t data);

void pci_device_set_config8(PCIDevice *d, uint16_t addr, uint8_t val);
void pci_device_set_config16(PCIDevice *d, uint16_t addr, uint16_t val);
/* Read back a device's own configuration space. A device whose behaviour
   depends on a register the guest writes through the generic config path (the
   MSI-X control word, say) reads it here rather than shadowing the write. */
uint32_t pci_device_get_config(PCIDevice *d, uint16_t addr, int size_log2);
int pci_device_get_devfn(PCIDevice *d);

/* Append to the conventional capability list, below 256 bytes. */
int pci_add_capability(PCIDevice *d, const uint8_t *buf, int size);

/* Append to the extended capability chain, above 256 bytes. 'body' is what
   follows the four byte capability header. Extended configuration space is
   read only here, so a capability added this way is one a guest inspects
   rather than programs. */
int pci_add_ext_capability(PCIDevice *d, uint16_t cap_id, int version,
                           const uint8_t *body, int body_size);

/* Add a PCI Express capability reporting 'port_type'. Called for you when a
   device is registered on a bus marked PCI Express, so a device only needs
   this to correct the type it was given. */
int pci_add_pcie_capability(PCIDevice *d, int port_type);

#endif /* PCI_H */
