/*
 * Synopsys DesignWare PCI Express host controller
 *
 * Copyright (c) 2016-2018 Fabrice Bellard
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

#include "device.h"
#include "pci.h"
#include "pci_bridge.h"
#include "pci_host_ecam.h"

/* The DBI window carries three things at once: the root complex's own
   configuration space at the bottom, the port logic registers above it, and
   the "unrolled" address translation unit at 0x300000. It must therefore be
   large enough to cover the translation unit. */
#define PCIE_DW_DBI_SIZE 0x400000

/* Configuration cycles for the secondary bus go through a window that the
   driver retargets, one function at a time, by reprogramming an outbound
   translation region. Only the first 4 KB is ever used; the rest of the
   window exists because the size is what bounds the translation region. */
#define PCIE_DW_CONFIG_SIZE 0x10000

#define PCIE_DW_DEFAULT_MMIO_SIZE 0x10000000 /* 256 MB */

/* Advertised by default, as with the ECAM bridge. */
#define PCIE_DW_DEFAULT_MMIO64_SIZE 0x100000000ull /* 4 GB */

/* Bus 0 holds the root port and the buses behind it hold everything else, so
   the default leaves room for a few tiers of bridges. */
#define PCIE_DW_DEFAULT_BUS_COUNT 16

/* What a Haiku or Linux driver matches on. The default names the SiFive FU740
   because that is the variant Haiku's DesignWare driver probes for. */
#define PCIE_DW_DEFAULT_COMPATIBLE "sifive,fu740-pcie"

/* Address translation unit: 8 regions per direction, each direction a 0x100
   block, each region a 0x200 stride. */
#define PCIE_DW_ATU_OFFSET 0x300000
#define PCIE_DW_ATU_REGION_COUNT 8

/* Message signalled interrupts are collected in groups of 32. */
#define PCIE_DW_MSI_GROUP_COUNT 8
#define PCIE_DW_MSI_COUNT (PCIE_DW_MSI_GROUP_COUNT * 32)


/* One outbound or inbound translation region. Only the outbound regions whose
   type is a configuration cycle are acted on; see the note in
   PCIHostDWDevice. */
struct PCIeDWAtuRegion {
    uint32_t ctrl1;    /* TLP type */
    uint32_t ctrl2;    /* enable */
    uint32_t base_lo;
    uint32_t base_hi;
    uint32_t limit;    /* low 32 bits only; the high half is never written */
    uint32_t target_lo;
    uint32_t target_hi;
};


/* One group of 32 message signalled interrupts. A message sets its bit in
   'status' regardless of the mask; the mask only gates whether the group
   drives the controller's interrupt line. */
struct PCIeDWMsiGroup {
    uint32_t enable;
    uint32_t mask;
    uint32_t status;
};


/* A DesignWare root complex. Unlike the ECAM bridge, a device cannot simply
   sit on bus 0: bus 0 holds the root port and nothing else, and the driver
   reaches everything behind it by pointing an outbound translation region at
   one function's configuration space at a time. So there are two buses here,
   and the devices go on the second one. */
class PCIHostDWDevice final: public Device, public PCIMsiTarget {
private:
    PCIBus *fRootBus = nullptr; /* bus 0: the root port alone */
    PCIBus *fDevBus = nullptr;  /* the secondary bus, where devices live */
    PCIDevice *fRootPort = nullptr;
    Bus *fChildBus = nullptr;

    Resource *fDbiRes = nullptr;
    Resource *fConfigRes = nullptr;
    Resource *fMmioRes = nullptr;
    Resource *fMmio64Res = nullptr;
    Resource *fMsiIrqRes = nullptr;
    Resource *fIrqRes[4] {};

    const char *fCompatible;
    uint64_t fMmioSize;
    uint64_t fMmio64Size;
    int fBusCount;

    /* Port logic registers the guest may write and read back. Nothing here
       changes how the model behaves; they exist so that a driver's
       write-then-verify sequences see what they wrote. */
    uint32_t fPortLinkCtrl = 0;
    uint32_t fGen2Ctrl = 0;
    uint32_t fMiscControl1 = 0;

    uint32_t fMsiAddrLo = 0;
    uint32_t fMsiAddrHi = 0;
    PCIeDWMsiGroup fMsi[PCIE_DW_MSI_GROUP_COUNT] {};
    IRQSignal *fMsiIrq = nullptr;
    bool fMsiLevel = false;

    PCIeDWAtuRegion fAtuOut[PCIE_DW_ATU_REGION_COUNT] {};
    PCIeDWAtuRegion fAtuIn[PCIE_DW_ATU_REGION_COUNT] {};

    void MsiUpdate();
    PCIeDWAtuRegion *AtuAt(uint32_t offset, uint32_t *reg_out);
    bool ConfigTarget(uint32_t offset, uint32_t *addr_out);
    bool ConfigDecode(uint32_t offset, uint32_t *bus_addr_out);

    uint32_t DbiRead(uint32_t offset, int size_log2);
    void DbiWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t ConfigRead(uint32_t offset, int size_log2);
    void ConfigWrite(uint32_t offset, uint32_t val, int size_log2);

public:
    PCIHostDWDevice(const char *name, const char *compatible,
                    uint64_t mmio_size, uint64_t mmio64_size, int bus_count);
    ~PCIHostDWDevice() override;

    bool Prepare() override;
    bool Realize() override;
    void BuildFDT(FDTContext &ctx) override;
    Bus *ChildBus() override {return fChildBus;}

    void SendMsi(uint64_t addr, uint32_t data) override;

    DeviceIOAdapter<PCIHostDWDevice, &PCIHostDWDevice::DbiRead,
                    &PCIHostDWDevice::DbiWrite> fDbiIo {*this};
    DeviceIOAdapter<PCIHostDWDevice, &PCIHostDWDevice::ConfigRead,
                    &PCIHostDWDevice::ConfigWrite> fConfigIo {*this};
};
