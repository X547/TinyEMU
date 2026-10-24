/*
 * PCI host bridges, enumeration and resource assignment
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

#include "intc.h"

/* configuration space, by the PCI Local Bus and PCI Express specifications */
#define PCI_VENDOR_ID        0x00
#define PCI_DEVICE_ID        0x02
#define PCI_COMMAND          0x04
#define PCI_STATUS           0x06
#define PCI_REVISION         0x08
#define PCI_CLASS_PROG       0x09
#define PCI_CLASS_DEVICE     0x0a
#define PCI_HEADER_TYPE      0x0e
#define PCI_BAR(n)           (0x10 + 4 * (n))
#define PCI_ROM0             0x30
#define PCI_CAP_PTR          0x34
#define PCI_ROM1             0x38
#define PCI_INTERRUPT_LINE   0x3c
#define PCI_INTERRUPT_PIN    0x3d
#define PCI_PRIMARY_BUS      0x18
#define PCI_SECONDARY_BUS    0x19
#define PCI_SUBORDINATE_BUS  0x1a
#define PCI_IO_BASE          0x1c
#define PCI_IO_LIMIT         0x1d
#define PCI_SEC_STATUS       0x1e
#define PCI_MEM_BASE         0x20
#define PCI_MEM_LIMIT        0x22
#define PCI_PREF_BASE        0x24
#define PCI_PREF_LIMIT       0x26
#define PCI_PREF_BASE_HI     0x28
#define PCI_PREF_LIMIT_HI    0x2c
#define PCI_IO_BASE_HI       0x30
#define PCI_IO_LIMIT_HI      0x32
#define PCI_BRIDGE_CONTROL   0x3e

#define PCI_CMD_IO           0x0001
#define PCI_CMD_MEMORY       0x0002
#define PCI_CMD_MASTER       0x0004
#define PCI_CMD_INTX_DISABLE 0x0400
#define PCI_STATUS_INTX      0x0008
#define PCI_STATUS_CAPS      0x0010

#define PCI_BAR_IO           0x1
#define PCI_BAR_MEM64        0x4
#define PCI_BAR_PREFETCH     0x8
#define PCI_ROM_ENABLE       0x1

#define PCI_CAP_MSI          0x05
#define PCI_CAP_EXP          0x10
#define PCI_CAP_MSIX         0x11
#define PCI_EXT_CAP_START    0x100

#define PCI_DEVFN(slot, fn)  ((slot) << 3 | (fn))
#define PCI_SLOT(devfn)      ((devfn) >> 3)
#define PCI_FN(devfn)        ((devfn) & 7)

/* A window of the host bridge: PCI addresses from 'pci', which the
   processor reaches from 'cpu'. */
struct PciAperture {
    uint64_t pci = 0;
    uint64_t cpu = 0;
    uint64_t size = 0;
    bool prefetch = false;
};

enum PciHostKind {
    PCI_HOST_ECAM,   /* pci-host-ecam-generic */
    PCI_HOST_DW,     /* snps,dw-pcie */
    PCI_HOST_PLDA,   /* starfive,jh7110-pcie */
    PCI_HOST_CF8,    /* PNP0A03 through ports 0xcf8 and 0xcfc */
};

class PciMsiSink;

/* A host bridge as the tables describe it: how configuration space is
   reached, its windows, and where its interrupts go. */
struct PciHost {
    PciHostKind kind;
    const HwDevice *dev;
    int index;            /* among the hosts */
    uint32_t bus_first;
    uint32_t bus_last;
    bool ext_config;      /* configuration space past 256 bytes */
    PciAperture io;       /* size 0 when there is none */
    PciAperture mem;
    PciAperture mem64;
    bool io_ports;        /* x86: ports by IN and OUT, not a memory window */
    uint64_t cfg;         /* ECAM, or DW's config window */
    uint64_t dbi;         /* DW */
    uint64_t apb;         /* PLDA */
    PciMsiSink *msi;      /* nullptr without a receiver the suite drives */

    const char *Kind() const;
    /* Configuration cycles; a function nothing answers for reads all
       ones. 'size_log2' is 0, 1 or 2. */
    uint32_t Read(uint32_t bus, uint32_t devfn, uint32_t reg,
                  int size_log2) const;
    void Write(uint32_t bus, uint32_t devfn, uint32_t reg, uint32_t value,
               int size_log2) const;
};

/* Every host bridge in the tables, found without touching any of them. */
int pci_host_count();
PciHost *pci_host(int index);

struct PciBar {
    uint64_t addr;  /* the PCI address, 0 when unassigned */
    uint64_t size;  /* 0 when not implemented */
    bool io;
    bool is64;      /* takes the next slot as its upper half */
    bool prefetch;
};

struct PciFunc {
    PciHost *host;
    PciFunc *parent;   /* the bridge above, nullptr on the root bus */
    uint32_t bus;
    uint32_t devfn;
    uint16_t vendor;
    uint16_t device;
    uint32_t class_rev; /* class, subclass, interface, revision */
    uint8_t header;     /* with the multi-function bit */
    uint8_t pin;        /* 1 to 4, 0 for none */
    bool bridge;
    uint32_t secondary; /* bridges */
    uint32_t subordinate;
    PciBar bars[7];     /* 6 is the expansion ROM */
    /* bridges: the windows as assigned, PCI addresses, size 0 when closed */
    PciAperture win_io;
    PciAperture win_mem;
    PciAperture win_pref;
    uint16_t command;   /* as enumeration left it */
    /* the rest of the first 256 bytes then: the capabilities, which temu
       lets a driver overwrite */
    uint32_t dev_config[48];
};

/* Number the buses of every host, place every BAR and bridge window in the
   host's apertures, and turn on decoding and bus mastering, as firmware
   would. Done once, on first use; the functions are then listed in the
   order they were found, depth first. */
void pci_enumerate();
int pci_func_count();
PciFunc *pci_func(int index);
/* The 'index'th function with these IDs, or nullptr. */
PciFunc *pci_find(uint16_t vendor, uint16_t device, int index = 0);
/* As pci_find(), or the test is skipped. */
PciFunc *pci_require(uint16_t vendor, uint16_t device, int index = 0);
/* "0:03.0" or "1:00.0@2" for a function of the second host. */
void pci_name(const PciFunc *f, char *buf, size_t size);

/* Put back what pci_enumerate() programmed: bus numbers, BARs, windows and
   command registers, with MSI and MSI-X turned off. For t_on_cleanup(). */
void pci_restore_all();

uint32_t pci_read(const PciFunc *f, uint32_t reg, int size_log2);
void pci_write(const PciFunc *f, uint32_t reg, uint32_t value, int size_log2);
inline uint8_t pci_read8(const PciFunc *f, uint32_t reg)
    {return pci_read(f, reg, 0);}
inline uint16_t pci_read16(const PciFunc *f, uint32_t reg)
    {return pci_read(f, reg, 1);}
inline uint32_t pci_read32(const PciFunc *f, uint32_t reg)
    {return pci_read(f, reg, 2);}
inline void pci_write8(const PciFunc *f, uint32_t reg, uint8_t v)
    {pci_write(f, reg, v, 0);}
inline void pci_write16(const PciFunc *f, uint32_t reg, uint16_t v)
    {pci_write(f, reg, v, 1);}
inline void pci_write32(const PciFunc *f, uint32_t reg, uint32_t v)
    {pci_write(f, reg, v, 2);}

/* The offset of the first capability with this ID, or 0. */
uint32_t pci_find_cap(const PciFunc *f, uint8_t id);
uint32_t pci_find_ext_cap(const PciFunc *f, uint16_t id);

/* Whether the processor can reach PCI address 'addr' of this kind. */
bool pci_reachable(const PciHost *h, uint64_t addr, uint64_t size, bool io);
/* Register access in a BAR, through the host's windows. */
uint32_t pci_bar_read(const PciFunc *f, int bar, uint32_t offset,
                      int size_log2);
void pci_bar_write(const PciFunc *f, int bar, uint32_t offset, uint32_t value,
                   int size_log2);
/* The same at a PCI address of the host, memory or I/O. */
uint32_t pci_space_read(const PciHost *h, bool io, uint64_t addr,
                        int size_log2);
void pci_space_write(const PciHost *h, bool io, uint64_t addr, uint32_t value,
                     int size_log2);
/* A read where nothing may answer: false when it faulted, as an unclaimed
   physical address does on RISC-V. */
bool pci_space_probe(const PciHost *h, bool io, uint64_t addr, int size_log2,
                     uint32_t *value);

/* The pin as it arrives at the root bus, swizzled at every bridge, and the
   root bus function it arrives through. */
uint8_t pci_root_pin(const PciFunc *f, const PciFunc **root);
/* The controller and input the function's INTx reaches, as the tables
   say. False when the function has no pin or the tables lead nowhere the
   suite drives. */
bool pci_intx_resolve(const PciFunc *f, IntController **ctrl,
                      uint32_t *line);
bool pci_intx_route(const PciFunc *f, IrqHandler fn, void *ctx);
void pci_intx_unroute(const PciFunc *f);

/* Where a host's message signalled interrupts are collected: the IMSIC of
   the boot hart, its local APIC, or a receiver in the host bridge. */
class PciMsiSink : public IntController {
public:
    /* The address and data that deliver message 'n' (0 to 31) to 'fn'.
       Routes it; false when 'n' is out of range. */
    virtual bool Message(int n, IrqHandler fn, void *ctx, uint64_t *addr,
                         uint32_t *data) = 0;
};
