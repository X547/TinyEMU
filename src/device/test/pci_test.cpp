/*
 * PCI test function
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
#include "bits.h"
#include "cutils.h"
#include "device.h"
#include "device_class.h"
#include "machine.h"
#include "pci.h"
#include "pci_test.h"

#define TEST_PCI_DEFAULT_VECTORS 4
#define TEST_PCI_DEFAULT_MEM64_KB 1024


class PCITestDevice final: public Device, public PCIBarTarget {
private:
    int fDevfn;
    int fPin;
    int fVectors;
    uint64_t fMem64Size;
    bool fMultifunction;

    PCIDevice *fPciDev = nullptr;
    IRQSignal *fIrq = nullptr;
    PCIMessageIrq fMsgIrq {};
    PhysMemoryMap *fMemMap = nullptr;
    PhysMemoryRange *fRange[PCI_NUM_REGIONS] {};

    uint32_t fIntx = 0;
    uint32_t fScratch = 0;
    uint32_t fDmaLo = 0;
    uint32_t fDmaHi = 0;
    uint32_t fDmaStatus = 0;

    /* the probe window, 0 for none */
    int fProbeFlags;
    uint8_t fProbe[TEST_PCI_PROBE_SIZE];
    struct ProbeAccess {
        uint32_t offset;
        int size_log2;
        bool write;
        uint64_t val;
    } fProbeLog[TEST_PCI_PROBE_LOG];
    uint32_t fProbeCount = 0;
    uint32_t fProbeSelect = 0;

    uint32_t RegRead(uint32_t offset, int size_log2);
    void RegWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t Mem64Read(uint32_t offset, int size_log2)
        {return RegRead(offset % TEST_PCI_REGS_SIZE, size_log2);}
    void Mem64Write(uint32_t offset, uint32_t val, int size_log2)
        {RegWrite(offset % TEST_PCI_REGS_SIZE, val, size_log2);}
    uint32_t MsixRead(uint32_t offset, int size_log2);
    void MsixWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t RomRead(uint32_t offset, int size_log2);
    void RomWrite(uint32_t, uint32_t, int) {}
    uint64_t ProbeRead(uint32_t offset, int size_log2);
    void ProbeWrite(uint32_t offset, uint64_t val, int size_log2);
    void ProbeLog(uint32_t offset, int size_log2, bool write, uint64_t val);
    void ProbeFill();

    DeviceIOAdapter<PCITestDevice, &PCITestDevice::RegRead,
                    &PCITestDevice::RegWrite> fRegIo {*this};
    DeviceIOAdapter<PCITestDevice, &PCITestDevice::Mem64Read,
                    &PCITestDevice::Mem64Write> fMem64Io {*this};
    DeviceIOAdapter<PCITestDevice, &PCITestDevice::MsixRead,
                    &PCITestDevice::MsixWrite> fMsixIo {*this};
    DeviceIOAdapter<PCITestDevice, &PCITestDevice::RomRead,
                    &PCITestDevice::RomWrite> fRomIo {*this};
    DeviceIOAdapter<PCITestDevice, &PCITestDevice::ProbeRead,
                    &PCITestDevice::ProbeWrite> fProbeIo {*this};

    bool Map(PhysMemoryMap *map, int bar, uint64_t size, int type,
             DeviceIO *io,
             int flags = DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32);

public:
    PCITestDevice(const char *name, int devfn, int pin, int vectors,
                  uint64_t mem64_size, bool multifunction, int probe_flags):
        Device(name), fDevfn(devfn), fPin(pin), fVectors(vectors),
        fMem64Size(mem64_size), fMultifunction(multifunction),
        fProbeFlags(probe_flags)
    {
        ProbeFill();
    }

    bool Prepare() override
    {
        if (ParentBus()->AsPCIBus() == nullptr) {
            vm_error("%s: must be attached to a PCI bus\n", Name());
            return false;
        }
        return true;
    }

    bool Realize() override;

    void SetBar(int bar_num, uint64_t addr, bool enabled) override
    {
        if (fRange[bar_num] != nullptr)
            fRange[bar_num]->SetAddr(addr, enabled);
    }
};


bool PCITestDevice::Map(PhysMemoryMap *map, int bar, uint64_t size, int type,
                        DeviceIO *io, int flags)
{
    fRange[bar] = map->RegisterDevice(0, size, io, flags | DEVIO_DISABLED);
    if (fRange[bar] == nullptr) {
        vm_error("%s: no room to map BAR %d\n", Name(), bar);
        return false;
    }
    pci_register_bar(fPciDev, bar, size, type, this);
    return true;
}


bool PCITestDevice::Realize()
{
    PCIBus *bus = ParentBus()->AsPCIBus();

    fPciDev = pci_register_device(bus, Name(), fDevfn, TEST_PCI_VENDOR_ID,
                                  TEST_PCI_DEVICE_ID, TEST_PCI_REVISION,
                                  TEST_PCI_CLASS);
    if (fPciDev == nullptr) {
        vm_error("%s: could not register the PCI function\n", Name());
        return false;
    }
    if (fMultifunction) {
        pci_device_set_config8(fPciDev, PCI_HEADER_TYPE,
                               PCI_HEADER_TYPE_NORMAL |
                               PCI_HEADER_TYPE_MULTI);
    }
    pci_device_set_config8(fPciDev, PCI_INTERRUPT_PIN, fPin);
    if (fPin != 0)
        fIrq = pci_device_get_irq(fPciDev, fPin - 1);

    fMemMap = pci_device_get_mem_map(fPciDev);
    if (!Map(fMemMap, TEST_PCI_BAR_REGS, TEST_PCI_REGS_SIZE,
             PCI_ADDRESS_SPACE_MEM, &fRegIo) ||
        !Map(fMemMap, TEST_PCI_BAR_MEM64, fMem64Size,
             PCI_ADDRESS_SPACE_MEM | PCI_ADDRESS_SPACE_MEM_TYPE_64 |
             PCI_ADDRESS_SPACE_MEM_PREFETCH, &fMem64Io) ||
        !Map(fMemMap, TEST_PCI_BAR_MSIX, TEST_PCI_MSIX_SIZE,
             PCI_ADDRESS_SPACE_MEM | PCI_ADDRESS_SPACE_MEM_PREFETCH,
             &fMsixIo) ||
        !Map(fMemMap, PCI_ROM_SLOT, TEST_PCI_ROM_SIZE, PCI_ADDRESS_SPACE_MEM,
             &fRomIo)) {
        return false;
    }
    if (fProbeFlags != 0 &&
        !Map(fMemMap, TEST_PCI_BAR_PROBE, TEST_PCI_PROBE_SIZE,
             PCI_ADDRESS_SPACE_MEM, &fProbeIo, fProbeFlags)) {
        return false;
    }
    PhysMemoryMap *port_map = pci_device_get_port_map(fPciDev);
    if (port_map != nullptr &&
        !Map(port_map, TEST_PCI_BAR_IO, TEST_PCI_IO_SIZE,
             PCI_ADDRESS_SPACE_IO, &fRegIo)) {
        return false;
    }

    if (fVectors > 0) {
        fMsgIrq.Init(fPciDev, fVectors, TEST_PCI_BAR_MSIX,
                     TEST_PCI_MSIX_TABLE, TEST_PCI_MSIX_PBA);
    }
    return true;
}


uint32_t PCITestDevice::RegRead(uint32_t offset, int size_log2)
{
    uint32_t val;

    switch (offset & ~3u) {
    case TEST_PCI_ID:
        val = TEST_PCI_MAGIC;
        break;
    case TEST_PCI_VERSION:
        val = TEST_PCI_VERSION_VALUE;
        break;
    case TEST_PCI_INTX:
        val = fIntx;
        break;
    case TEST_PCI_MSI_INFO:
        val = (fMsgIrq.Present() ? TEST_PCI_MSI_PRESENT : 0) |
            (fMsgIrq.Enabled() ? TEST_PCI_MSI_ENABLED : 0) |
            (uint32_t)fMsgIrq.VectorCount() << TEST_PCI_MSI_COUNT_SHIFT;
        break;
    case TEST_PCI_SCRATCH:
        val = fScratch;
        break;
    case TEST_PCI_DMA_LO:
        val = fDmaLo;
        break;
    case TEST_PCI_DMA_HI:
        val = fDmaHi;
        break;
    case TEST_PCI_DMA_DATA: {
        uint8_t *ptr = pci_device_get_dma_ptr(
            fPciDev, concat_bits(fDmaHi, fDmaLo, 32), false);
        fDmaStatus = ptr == nullptr;
        val = ptr != nullptr ? get_le32(ptr) : 0;
        break;
    }
    case TEST_PCI_DMA_STATUS:
        val = fDmaStatus;
        break;
    case TEST_PCI_PROBE_COUNT:
        val = fProbeCount;
        break;
    case TEST_PCI_PROBE_SELECT:
        val = fProbeSelect;
        break;
    case TEST_PCI_PROBE_INFO:
        val = (fProbeFlags & DEVIO_SIZES) |
            (fProbeFlags & DEVIO_WRITE_MERGE ? TEST_PCI_PROBE_MERGE : 0) |
            (fProbeFlags & DEVIO_UNALIGNED ? TEST_PCI_PROBE_UNALIGNED : 0);
        break;
    case TEST_PCI_PROBE_ACCESS:
    case TEST_PCI_PROBE_VAL_LO:
    case TEST_PCI_PROBE_VAL_HI: {
        if (fProbeSelect >= TEST_PCI_PROBE_LOG ||
            fProbeSelect >= fProbeCount) {
            val = 0;
            break;
        }
        const ProbeAccess &a = fProbeLog[fProbeSelect];
        if ((offset & ~3u) == TEST_PCI_PROBE_ACCESS) {
            val = a.offset | a.size_log2 << TEST_PCI_PROBE_SIZE_SHIFT |
                (a.write ? TEST_PCI_PROBE_WRITE : 0);
        } else if ((offset & ~3u) == TEST_PCI_PROBE_VAL_LO) {
            val = a.val;
        } else {
            val = a.val >> 32;
        }
        break;
    }
    default:
        val = 0;
        break;
    }
    return get_bits(val, (offset & 3) * 8, 8 << size_log2);
}


void PCITestDevice::RegWrite(uint32_t offset, uint32_t val, int size_log2)
{
    if (size_log2 != 2)
        return;
    switch (offset) {
    case TEST_PCI_INTX:
        fIntx = val & 1;
        if (fIrq != nullptr)
            fIrq->Set(fIntx);
        break;
    case TEST_PCI_MSI_SEND:
        fMsgIrq.Send(val);
        break;
    case TEST_PCI_SCRATCH:
        fScratch = val;
        break;
    case TEST_PCI_DMA_LO:
        fDmaLo = val;
        break;
    case TEST_PCI_DMA_HI:
        fDmaHi = val;
        break;
    case TEST_PCI_DMA_DATA: {
        uint8_t *ptr = pci_device_get_dma_ptr(
            fPciDev, concat_bits(fDmaHi, fDmaLo, 32), true);
        fDmaStatus = ptr == nullptr;
        if (ptr != nullptr)
            put_le32(ptr, val);
        break;
    }
    case TEST_PCI_PROBE_COUNT:
        ProbeFill();
        break;
    case TEST_PCI_PROBE_SELECT:
        fProbeSelect = val;
        break;
    }
}


uint32_t PCITestDevice::MsixRead(uint32_t offset, int size_log2)
{
    if (offset >= TEST_PCI_MSIX_PBA)
        return fMsgIrq.PbaRead(offset - TEST_PCI_MSIX_PBA, size_log2);
    return fMsgIrq.TableRead(offset - TEST_PCI_MSIX_TABLE, size_log2);
}


void PCITestDevice::MsixWrite(uint32_t offset, uint32_t val, int size_log2)
{
    if (offset < TEST_PCI_MSIX_PBA)
        fMsgIrq.TableWrite(offset - TEST_PCI_MSIX_TABLE, val, size_log2);
}


uint32_t PCITestDevice::RomRead(uint32_t offset, int size_log2)
{
    uint32_t val = 0;

    for (int i = 0; i < (1 << size_log2); i++) {
        uint32_t at = offset + i;
        uint8_t b = at == 0 ? 0x55 : at == 1 ? 0xaa : (uint8_t)at;
        val |= (uint32_t)b << (8 * i);
    }
    return val;
}


/* Whatever arrives at the window is taken as it is, so that the log shows
   just what the memory map passed on. */
uint64_t PCITestDevice::ProbeRead(uint32_t offset, int size_log2)
{
    uint64_t val = 0;

    for (int i = 0; i < (1 << size_log2); i++) {
        if (offset + i < TEST_PCI_PROBE_SIZE)
            val |= (uint64_t)fProbe[offset + i] << (8 * i);
    }
    ProbeLog(offset, size_log2, false, val);
    return val;
}


void PCITestDevice::ProbeWrite(uint32_t offset, uint64_t val, int size_log2)
{
    for (int i = 0; i < (1 << size_log2); i++) {
        if (offset + i < TEST_PCI_PROBE_SIZE)
            fProbe[offset + i] = val >> (8 * i);
    }
    ProbeLog(offset, size_log2, true, val);
}


void PCITestDevice::ProbeLog(uint32_t offset, int size_log2, bool write,
                             uint64_t val)
{
    if (fProbeCount < TEST_PCI_PROBE_LOG)
        fProbeLog[fProbeCount] = {offset, size_log2, write, val};
    fProbeCount++;
}


void PCITestDevice::ProbeFill()
{
    for (uint32_t i = 0; i < TEST_PCI_PROBE_SIZE; i++)
        fProbe[i] = TEST_PCI_PROBE_BYTE(i);
    fProbeCount = 0;
    fProbeSelect = 0;
}


//#pragma mark - class

/* A PCI function for the device test suite to test the buses and host
   bridges with: base address registers of every kind, an INTx pin, MSI
   and MSI-X it sends on request, and DMA to RAM; and, for the memory map,
   a window that logs the accesses it is handed. Only in builds with the
   test_fixtures option. */
class PCITestClass final: public DeviceClass {
private:
    static bool GetBool(const DeviceConfig &cfg, const char *name, bool *val)
    {
        JSONValue v = cfg.Get(name);
        if (v.type == JSON_BOOL) {
            *val = v.u.b;
        } else if (!json_is_undefined(v)) {
            vm_error("%s: \"%s\" is true or false\n", cfg.Type(), name);
            return false;
        }
        return true;
    }

    /* "probe": the widths in bytes the window takes, none for no window;
       "probe_merge" and "probe_unaligned" add those flags. */
    static bool GetProbe(const DeviceConfig &cfg, int *flags)
    {
        bool merge = false, unaligned = false;

        *flags = 0;
        JSONValue widths = cfg.Get("probe");
        if (json_is_undefined(widths))
            return true;
        if (widths.type != JSON_ARRAY || widths.u.array->Length() == 0) {
            vm_error("%s: \"probe\" is an array of widths\n", cfg.Type());
            return false;
        }
        for (int i = 0; i < widths.u.array->Length(); i++) {
            JSONValue v = json_array_get(widths, i);
            int w = v.type == JSON_INT ? v.u.int32 : 0;
            if (w != 1 && w != 2 && w != 4 && w != 8) {
                vm_error("%s: \"probe\" holds widths of 1, 2, 4 or 8 "
                         "bytes\n", cfg.Type());
                return false;
            }
            *flags |= bit_at(__builtin_ctz(w)); /* DEVIO_SIZE8 and on */
        }
        if (!GetBool(cfg, "probe_merge", &merge) ||
            !GetBool(cfg, "probe_unaligned", &unaligned))
            return false;
        *flags |= (merge ? DEVIO_WRITE_MERGE : 0) |
            (unaligned ? DEVIO_UNALIGNED : 0);
        return true;
    }

public:
    PCITestClass(): DeviceClass("pci-test") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        int slot, function, pin, vectors, mem64_kb, probe_flags;
        bool multifunction = false;

        (void)ctx;
        if (!cfg.GetInt("slot", &slot, -1) ||
            !cfg.GetInt("function", &function, 0) ||
            !cfg.GetInt("pin", &pin, 1) ||
            !cfg.GetInt("vectors", &vectors, TEST_PCI_DEFAULT_VECTORS) ||
            !cfg.GetInt("mem64_size", &mem64_kb,
                        TEST_PCI_DEFAULT_MEM64_KB) ||
            !GetBool(cfg, "multifunction", &multifunction) ||
            !GetProbe(cfg, &probe_flags)) {
            return nullptr;
        }
        if (slot < -1 || slot > 31 || function < 0 || function > 7 ||
            (slot < 0 && function != 0)) {
            vm_error("%s: \"slot\" is 0 to 31 and \"function\" 0 to 7, which "
                     "needs a slot\n", cfg.Type());
            return nullptr;
        }
        if (pin < 0 || pin > 4) {
            vm_error("%s: \"pin\" is 0 (none) or 1 to 4\n", cfg.Type());
            return nullptr;
        }
        if (vectors < 0 || vectors > 32) {
            vm_error("%s: \"vectors\" is 0 to 32\n", cfg.Type());
            return nullptr;
        }
        if (mem64_kb < 4 || (mem64_kb & (mem64_kb - 1)) != 0) {
            vm_error("%s: \"mem64_size\" is a power of two of at least 4 KB\n",
                     cfg.Type());
            return nullptr;
        }
        return new PCITestDevice(cfg.IdOr("pci-test"),
                                 slot < 0 ? -1 : slot << 3 | function, pin,
                                 vectors, (uint64_t)mem64_kb << 10,
                                 multifunction, probe_flags);
    }
};

static const PCITestClass sPCITestClass;
