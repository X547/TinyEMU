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

#include <algorithm>

#include "acpi.h"
#include "bits.h"
#include "cutils.h"
#include "devices.h"
#include "machine.h"
#include "pci_bridge.h"

/* PIRQA..PIRQD routing registers of the PIIX3, one byte each. Bit 7 disables
   the line; the low bits name the PIC input it lands on. */
#define PIIX3_PIRQ_ROUTE 0x60
#define PIIX3_PIRQ_DISABLE 0x80

/* the PIIX3's reset control register: a rising RCPU resets the machine */
#define PIIX3_RESET_CONTROL 0xcf9
#define RC_SRST 0x02
#define RC_RCPU 0x04

/* the bits of the configuration address register that exist */
#define CONFIG_ADDRESS_MASK 0x80fffffc

/* Where a kernel boot routes PIRQA-D: two lines no ISA device of a PC owns,
   leaving 9 for the SCI and 12 for the PS/2 mouse. Firmware picks the same
   two. */
static const uint8_t kPirqLines[4] = { 10, 11, 10, 11 };

/* the port space, which a 16 bit port number names */
#define PC_IO_PORTS 0x10000

#define CLOUDHV_DEVICE_ID 0x0d57
/* Cloud Hypervisor's 32 bit aperture, which its EDK2 build does not look
   up */
#define CLOUDHV_MMIO_BASE 0xc0000000
#define CLOUDHV_MMIO_END 0xf8000000

static const uint32_t val_ones[3] = { 0xff, 0xffff, 0xffffffff };

struct I440FXState: public IRQTarget {
    PCIBus *pci_bus;
    PCIDevice *pci_dev;
    PCIDevice *piix3_dev;
    uint32_t config_reg;
    uint8_t pic_irq_state[16];
    IRQSignal *pic_irqs; /* 16 irqs */
    /* the PIC input each of PIRQA-D drives, or -1 */
    int pirq_line[4] = {-1, -1, -1, -1};
    VirtMachine *machine = nullptr;
    uint8_t reset_control = 0;

    uint32_t ReadAddr(uint32_t offset, int size_log2);
    void WriteAddr(uint32_t offset, uint32_t data, int size_log2);
    uint32_t ReadData(uint32_t offset, int size_log2);
    void WriteData(uint32_t offset, uint32_t data, int size_log2);
    uint32_t ReadReset(uint32_t offset, int size_log2);
    void WriteReset(uint32_t offset, uint32_t data, int size_log2);

    DeviceIOAdapter<I440FXState, &I440FXState::ReadAddr,
                    &I440FXState::WriteAddr> fAddrIo {*this};
    DeviceIOAdapter<I440FXState, &I440FXState::ReadData,
                    &I440FXState::WriteData> fDataIo {*this};
    DeviceIOAdapter<I440FXState, &I440FXState::ReadReset,
                    &I440FXState::WriteReset> fResetIo {*this};

    void SetIRQ(int irq_num, int level) override;
};

void I440FXState::WriteAddr(uint32_t offset, uint32_t data, int size_log2)
{
    I440FXState *s = this;
    s->config_reg = data & CONFIG_ADDRESS_MASK;
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
    return PCI_CONFIG_ADDR(get_bits(config_reg, 16, 8),
                           get_bits(config_reg, 8, 8),
                           set_bits(get_bits(config_reg, 0, 8),
                                    0, 2, offset));
}

void I440FXState::WriteData(uint32_t offset, uint32_t data, int size_log2)
{
    I440FXState *s = this;
    if (get_bit(s->config_reg, 31)) {
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
    if (!get_bit(s->config_reg, 31))
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

/* Drives 'line' for PIRQ 'irq_num'; several PIRQs may share a line. */
static void i440fx_drive(I440FXState *s, int line, int irq_num, int level)
{
    s->pic_irq_state[line] = set_bit(s->pic_irq_state[line], irq_num, level);
    s->pic_irqs[line].Set(s->pic_irq_state[line] != 0);
}

/* A PIRQ goes to the PIC input its route register names, by the PIIX3 data
   sheet: none while bit 7 is set, and none for the inputs the register
   reserves. A PIRQ that falls is let go of where it was raised. */
void I440FXState::SetIRQ(int irq_num, int irq_level)
{
    I440FXState *s = this;
    uint8_t route = pci_device_get_config(s->piix3_dev,
                                          PIIX3_PIRQ_ROUTE + irq_num, 0);
    int line = get_bits(route, 0, 4);

    if ((route & PIIX3_PIRQ_DISABLE) || line <= 2 || line == 8 || line == 13)
        line = -1;
    if (s->pirq_line[irq_num] >= 0 && (!irq_level ||
                                       s->pirq_line[irq_num] != line)) {
        i440fx_drive(s, s->pirq_line[irq_num], irq_num, 0);
        s->pirq_line[irq_num] = -1;
    }
    if (irq_level && line >= 0) {
        i440fx_drive(s, line, irq_num, 1);
        s->pirq_line[irq_num] = line;
    }
}

uint32_t I440FXState::ReadReset(uint32_t offset, int size_log2)
{
    (void)offset;
    (void)size_log2;
    return reset_control;
}

void I440FXState::WriteReset(uint32_t offset, uint32_t data, int size_log2)
{
    (void)offset;
    (void)size_log2;
    bool rising = (data & RC_RCPU) && !(reset_control & RC_RCPU);
    reset_control = data & (RC_SRST | RC_RCPU);
    if (rising && machine != nullptr)
        machine->RequestReset();
}

void pc_pci_port_windows(AmlBytes &r, const RangeAllocator &io)
{
    uint64_t next = 0;

    for (int i = 0; i < io.Count(); i++) {
        const AllocEntry &e = io.At(i);
        if (e.base > next)
            aml_res_word_space(r, 1, 3, next, e.base - 1);
        next = std::max(next, e.base + e.size);
    }
    if (next < PC_IO_PORTS)
        aml_res_word_space(r, 1, 3, next, PC_IO_PORTS - 1);
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
    Resource *fMmioRes = nullptr;
    Resource *fMmio64Res = nullptr;
    Resource *fPirqRes[2] {};
    I440FXVariant fVariant;
    PcPciApertures fSpace;
    VirtMachine *fMachine;

public:
    I440FXDevice(const char *name, I440FXVariant variant,
                 const PcPciApertures &space, VirtMachine *machine):
        Device(name), fVariant(variant), fSpace(space), fMachine(machine) {}

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
        /* the lines PIRQA-D are routed to, which nothing else may take */
        for (int i = 0; i < 2; i++) {
            fPirqRes[i] = AddFixedResource(RES_IRQ, kPirqLines[i], 1);
            if (fPirqRes[i] == nullptr) {
                return false;
            }
        }

        /* The apertures the BARs go in. The devices map their BARs
           themselves, so these are only reserved. */
        uint64_t mmio_base = fSpace.mmio_base;
        uint64_t mmio_end = fSpace.mmio_end;
        if (fVariant == I440FX_CLOUD_HYPERVISOR) {
            mmio_base = CLOUDHV_MMIO_BASE;
            mmio_end = CLOUDHV_MMIO_END;
        }
        fMmioRes = AddFixedResource(RES_MMIO, mmio_base,
                                    mmio_end - mmio_base);
        if (fMmioRes == nullptr) {
            return false;
        }
        if (fSpace.mmio64_end > fSpace.mmio64_base) {
            fMmio64Res = AddFixedResource(RES_MMIO, fSpace.mmio64_base,
                                          fSpace.mmio64_end -
                                          fSpace.mmio64_base);
            if (fMmio64Res == nullptr) {
                return false;
            }
        }

        fState = std::make_unique<I440FXState>();
        fState->machine = fMachine;
        fPciBus = pci_bus_init(sys->MemMap(), sys->PortMap());
        /* The messages go to the machine's local APICs, when it has them. */
        if (sys->MsiTarget() != nullptr) {
            pci_bus_set_msi_target(fPciBus.get(), sys->MsiTarget());
        }
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

        uint16_t device_id = fVariant == I440FX_CLOUD_HYPERVISOR ?
            CLOUDHV_DEVICE_ID : 0x1237;
        fState->pci_dev = pci_register_device(fPciBus.get(), "i440FX", 0,
                                              0x8086, device_id, 0x02,
                                              0x0600);
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
        sys->PortMap()->RegisterDevice(PIIX3_RESET_CONTROL, 1,
                                       &fState->fResetIo, DEVIO_SIZE8);
        return true;
    }

    /* Every bus, the port space but for the configuration ports, the VGA
       hole and the apertures. Every slot's INTx pins are swizzled onto
       PIRQA-D the way the root bus does it. */
    void BuildAML(AcpiContext &ctx) override
    {
        AmlBytes r;

        aml_res_word_space(r, 2, 0, 0x00, 0xff);
        /* the configuration ports, which it consumes */
        aml_res_io(r, fAddrRes->base, 8);
        SystemBus *sys = static_cast<SystemBus *>(ParentBus());
        pc_pci_port_windows(r, sys->IoAlloc());
        aml_res_dword_memory(r, 0xa0000, 0xbffff);
        aml_res_dword_memory(r, fMmioRes->base, fMmioRes->End() - 1);
        if (fMmio64Res != nullptr) {
            aml_res_qword_memory(r, fMmio64Res->base, fMmio64Res->End() - 1);
        }

        std::vector<AmlBytes> prt;
        for (int slot = 0; slot < 32; slot++) {
            for (int pin = 0; pin < 4; pin++) {
                prt.push_back(aml_package({
                    aml_int((slot << 16) | 0xffff), aml_int(pin), aml_int(0),
                    aml_int(kPirqLines[(pin + slot - 1) & 3]),
                }));
            }
        }

        AmlBytes dev = aml_name("_HID", aml_eisaid("PNP0A03"));
        aml_append(dev, aml_name("_ADR", aml_int(0)));
        aml_append(dev, aml_name("_CRS", aml_resource_template(r)));
        aml_append(dev, aml_name("_PRT", aml_package(prt)));
        aml_append(*ctx.aml, aml_device(ctx.NewName("PCI").c_str(), dev));
    }

    Bus *ChildBus() override {return fChildBus.get();}
};


/* The host bridge of a PC. It decodes the CF8/CFC configuration ports and the
   apertures, carries the host bridge function and the PIIX3 ISA bridge whose
   PIRQ registers route the four INTx lines onto the PIC, and provides the PCI
   bus the machine's devices hang from. The apertures are what the machine
   leaves free: below 4 GB from the end of RAM to its device window, and above
   RAM up to what the processors can address. */
class I440FXClass final: public DeviceClass {
private:
    I440FXVariant fVariant;

public:
    I440FXClass(const char *name, I440FXVariant variant):
        DeviceClass(name), fVariant(variant) {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        if (ctx->pc_pci_space == nullptr) {
            vm_error("%s: only the pc machine has one\n", cfg.Type());
            return nullptr;
        }
        return new I440FXDevice(cfg.IdOr("i440fx"), fVariant,
                                *ctx->pc_pci_space, ctx->machine);
    }
};

static const I440FXClass sI440FXClass("pci-host-i440fx", I440FX_PC);
static const I440FXClass sCloudHVClass("pci-host-cloudhv",
                                       I440FX_CLOUD_HYPERVISOR);


I440FXState *i440fx_node_state(Device *dev)
{
    I440FXDevice *node = dynamic_cast<I440FXDevice *>(dev);

    return node != nullptr ? node->State() : nullptr;
}

/* in case no BIOS is used, map the interrupts. */
void i440fx_map_interrupts(I440FXState *s, uint8_t *elcr)
{
    PCIBus *b = s->pci_bus;
    PCIDevice *d, *hd;
    int irq_num, pic_irq, devfn, i;

    /* set a default PCI IRQ mapping to PIC IRQs */
    hd = s->piix3_dev;

    elcr[0] = 0;
    elcr[1] = 0;
    for(i = 0; i < 4; i++) {
        irq_num = kPirqLines[i];
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
