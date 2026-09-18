/*
 * PLDA XpressRich PCI Express host controller (StarFive JH7110)
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

/* The bridge registers, with the root port's configuration space mirrored
   at 0x1000 above them. The rest of the window is unused. */
#define PCIE_PLDA_APB_SIZE 0x10000
#define PCIE_PLDA_REG_SIZE 0x1000
#define PCIE_PLDA_RC_CONFIG_OFFSET 0x1000

/* The system register block the driver reads the link state from, and the
   reset lines it deasserts. Each is a page of its own. */
#define PCIE_PLDA_SYSCON_SIZE 0x1000
#define PCIE_PLDA_RESET_SIZE 0x1000

#define PCIE_PLDA_DEFAULT_MMIO_SIZE 0x10000000 /* 256 MB */
#define PCIE_PLDA_DEFAULT_MMIO64_SIZE 0x100000000ull /* 4 GB */

/* Bus 0 holds the root port and the buses behind it hold everything else, so
   the default leaves room for a few tiers of bridges. */
#define PCIE_PLDA_DEFAULT_BUS_COUNT 16


/* A PLDA XpressRich root complex, of the kind the StarFive JH7110 has two
   of. As with the DesignWare one, bus 0 holds a root port and the devices go
   on the bus behind it; unlike that one, configuration space is a plain ECAM
   window. INTx and message signalled interrupts are collected in the bridge
   registers and leave on a single line. There is no I/O aperture: the part
   has none. */
class PCIHostPLDADevice final: public Device, public PCIMsiTarget,
                               public IRQTarget {
private:
    PCIBusPtr fRootBus;         /* bus 0: the root port alone */
    PCIBus *fDevBus = nullptr;  /* the secondary bus, where devices live */
    /* after fRootBus, so the devices go before the functions they registered */
    std::unique_ptr<Bus> fChildBus;

    Resource *fCfgRes = nullptr;
    Resource *fApbRes = nullptr;
    Resource *fSysconRes = nullptr;
    Resource *fResetRes = nullptr;
    Resource *fMmioRes = nullptr;
    Resource *fMmio64Res = nullptr;
    Resource *fIrqRes = nullptr;

    uint64_t fMmioSize;
    uint64_t fMmio64Size;
    int fBusCount;

    /* Bridge registers the guest may write and read back. Only the
       interrupt ones change how the model behaves. */
    uint32_t fRegs[PCIE_PLDA_REG_SIZE / 4] {};
    uint32_t fSyscon[PCIE_PLDA_SYSCON_SIZE / 4] {};
    uint32_t fResets = 0;

    uint32_t fIntxLevel = 0; /* one bit per pin, INTA first */
    uint32_t fMsiStatus = 0; /* one bit per vector */
    IRQSignal *fIrq = nullptr;
    IRQSignal fIntx[4];
    bool fLevel = false;

    uint32_t LocalStatus() const;
    void IrqUpdate();

    uint32_t ApbRead(uint32_t offset, int size_log2);
    void ApbWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t CfgRead(uint32_t offset, int size_log2);
    void CfgWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t SysconRead(uint32_t offset, int size_log2);
    void SysconWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t ResetRead(uint32_t offset, int size_log2);
    void ResetWrite(uint32_t offset, uint32_t val, int size_log2);

public:
    PCIHostPLDADevice(const char *name, uint64_t mmio_size,
                      uint64_t mmio64_size, int bus_count);

    bool Prepare() override;
    bool Realize() override;
    void BuildFDT(FDTContext &ctx) override;
    Bus *ChildBus() override {return fChildBus.get();}

    void SendMsi(uint64_t addr, uint32_t data) override;
    void SetIRQ(int irq_num, int level) override;

    DeviceIOAdapter<PCIHostPLDADevice, &PCIHostPLDADevice::ApbRead,
                    &PCIHostPLDADevice::ApbWrite> fApbIo {*this};
    DeviceIOAdapter<PCIHostPLDADevice, &PCIHostPLDADevice::CfgRead,
                    &PCIHostPLDADevice::CfgWrite> fCfgIo {*this};
    DeviceIOAdapter<PCIHostPLDADevice, &PCIHostPLDADevice::SysconRead,
                    &PCIHostPLDADevice::SysconWrite> fSysconIo {*this};
    DeviceIOAdapter<PCIHostPLDADevice, &PCIHostPLDADevice::ResetRead,
                    &PCIHostPLDADevice::ResetWrite> fResetIo {*this};
};
