/*
 * i440FX PCI host bridge
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
#include "pci_host_i440fx.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "cutils.h"
#include "machine.h"
#include "pci_bridge.h"

/* PIRQA..PIRQD routing registers of the PIIX3, one byte each. Bit 7 disables
   the line; the low bits name the PIC input it lands on. */
#define PIIX3_PIRQ_ROUTE 0x60

static const uint32_t val_ones[3] = { 0xff, 0xffff, 0xffffffff };

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
        pci_bus_config_write(s->pci_bus,
                             i440fx_config_addr(s->config_reg, offset),
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
    return pci_bus_config_read(s->pci_bus,
                               i440fx_config_addr(s->config_reg, offset),
                               size_log2);
}

void I440FXState::SetIRQ(int irq_num, int irq_level)
{
    I440FXState *s = this;
    PCIDevice *hd = s->piix3_dev;
    uint16_t route = PIIX3_PIRQ_ROUTE + irq_num;
    int pic_irq;

    /* map to the PIC irq (different IRQs can be mapped to the same
       PIC irq) */
    pic_irq = pci_device_get_config(hd, route, 0) & ~0x80;
    pci_device_set_config8(hd, route, pic_irq);
    if (pic_irq < 16) {
        if (irq_level)
            s->pic_irq_state[pic_irq] |= 1 << irq_num;
        else
            s->pic_irq_state[pic_irq] &= ~(1 << irq_num);
        s->pic_irqs[pic_irq].Set((s->pic_irq_state[pic_irq] != 0));
    }
}

//#pragma mark - the configuration node

/* The bus is built in Prepare() rather than Realize(), because the devices
   nested inside the node are added to it before any of them is realized. */
class I440FXDevice final: public Device {
private:
    std::unique_ptr<I440FXState> fState;
    PCIBusPtr fPciBus;
    /* after fPciBus, so the devices go before the functions they registered */
    std::unique_ptr<Bus> fChildBus;
    Resource *fAddrRes = nullptr;
    Resource *fDataRes = nullptr;

public:
    I440FXDevice(const char *name): Device(name) {}

    I440FXState *State() const {return fState.get();}

    bool Prepare() override
    {
        SystemBus *sys = dynamic_cast<SystemBus *>(ParentBus());
        if (sys == nullptr || !sys->IsPortBased()) {
            vm_error("%s: must be attached to a PC system bus\n", Name());
            return false;
        }

        /* The configuration ports, where the PCI BIOS specification put
           them. */
        fAddrRes = AddFixedResource(RES_IO, 0xcf8, 4);
        fDataRes = AddFixedResource(RES_IO, 0xcfc, 4);
        if (fAddrRes == nullptr || fDataRes == nullptr) {
            return false;
        }

        fState = std::make_unique<I440FXState>();
        fPciBus = pci_bus_init(sys->MemMap(), sys->PortMap());
        fState->pci_bus = fPciBus.get();
        /* The lines are the machine's, which on a PC are wired before any
           device exists, so the array is contiguous from line 0. */
        fState->pic_irqs = sys->IrqSignalFor(0);

        /* The four INTx lines land on the bridge itself rather than on a PIC
           input directly: SetIRQ() puts them through the PIIX3 PIRQ routing
           registers first. */
        for (int i = 0; i < 4; i++) {
            IRQSignal sig;
            sig.Init(fState.get(), i);
            pci_bus_set_irq(fPciBus.get(), i, &sig);
        }

        fState->pci_dev = pci_register_device(fPciBus.get(), "i440FX", 0,
                                              0x8086, 0x1237, 0x02, 0x0600);
        /* Red Hat, Inc. / QEMU virtual machine, which is the pair guests
           recognise. */
        pci_device_set_config16(fState->pci_dev, PCI_SUBSYSTEM_VENDOR_ID,
                                0x1af4);
        pci_device_set_config16(fState->pci_dev, PCI_SUBSYSTEM_ID, 0x1100);

        fState->piix3_dev = pci_register_device(fPciBus.get(), "PIIX3", 8,
                                                0x8086, 0x7000, 0x00, 0x0601);
        pci_device_set_config8(fState->piix3_dev, PCI_HEADER_TYPE,
                               PCI_HEADER_TYPE_NORMAL |
                               PCI_HEADER_TYPE_MULTI);

        fChildBus = pci_attach_bus_create(this, fPciBus.get());
        return fChildBus != nullptr;
    }

    bool Realize() override
    {
        SystemBus *sys = static_cast<SystemBus *>(ParentBus());

        sys->PortMap()->RegisterDevice(fAddrRes->base, 1, &fState->fAddrIo,
                                       DEVIO_SIZE32);
        sys->PortMap()->RegisterDevice(fDataRes->base, fDataRes->size,
                                       &fState->fDataIo,
                                       DEVIO_SIZE8 | DEVIO_SIZE16 |
                                       DEVIO_SIZE32);
        return true;
    }

    Bus *ChildBus() override {return fChildBus.get();}
};


Device *i440fx_node_create(const char *name)
{
    return new I440FXDevice(name);
}


I440FXState *i440fx_node_state(Device *dev)
{
    I440FXDevice *node = dynamic_cast<I440FXDevice *>(dev);

    return node != nullptr ? node->State() : nullptr;
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
        pci_device_set_config8(hd, PIIX3_PIRQ_ROUTE + i, irq_num);
        elcr[irq_num >> 3] |= (1 << (irq_num & 7));
    }

    for(devfn = 0; devfn < 256; devfn++) {
        d = pci_bus_get_device(b, devfn);
        if (!d)
            continue;
        if (pci_device_get_config(d, PCI_INTERRUPT_PIN, 0)) {
            /* This bus is a root bus, so the swizzle is the root bus one. */
            irq_num = pci_bus_map_irq(pci_device_get_devfn(d), 0);
            pic_irq = pci_device_get_config(hd, PIIX3_PIRQ_ROUTE + irq_num, 0);
            if (pic_irq < 16) {
                pci_device_set_config8(d, PCI_INTERRUPT_LINE, pic_irq);
            }
        }
    }
}
