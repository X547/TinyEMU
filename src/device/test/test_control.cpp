/*
 * Test control device
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
#include "test_control.h"

#include <stdio.h>
#include <stdlib.h>

#include "acpi.h"
#include "device_class.h"
#include "devices.h"
#include "fdt.h"
#include "machine.h"
#include "test_block.h"

#define TEST_CONTROL_SIZE 0x1000
#define TEST_CONTROL_DEFAULT_IRQS 4


class TestControlDevice final: public Device {
private:
    DeviceContext *fCtx;
    VirtMachine *fMachine = nullptr;
    Resource *fMmio = nullptr;
    /* Lines given by number, or else how many to allocate. */
    int fFixedLines[TEST_CONTROL_MAX_IRQS];
    int fIrqCount = 0;
    bool fFixed = false;
    Resource *fIrqRes[TEST_CONTROL_MAX_IRQS] {};
    IRQSignal *fIrq[TEST_CONTROL_MAX_IRQS] {};
    uint32_t fIrqLevel = 0;
    /* the test disk the block registers act on */
    uint32_t fBlockSelect = 0;
    /* the output file, or stderr when the configuration names none */
    FILE *fOutput = stderr;
    bool fOwnsOutput = false;

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t val, int size_log2);
    void BlockWrite(TestBlockControl *disk, uint32_t offset, uint32_t val);

    DeviceIOAdapter<TestControlDevice, &TestControlDevice::Read,
                    &TestControlDevice::Write> fIo {*this};

public:
    TestControlDevice(const char *name, DeviceContext *ctx):
        Device(name), fCtx(ctx) {}
    ~TestControlDevice() override
    {
        if (fOwnsOutput) {
            fclose(fOutput);
        }
    }

    void SetIrqCount(int count) {fIrqCount = count;}
    void AddFixedLine(int line)
    {
        fFixed = true;
        fFixedLines[fIrqCount++] = line;
    }

    bool Open(const char *path)
    {
        fOutput = fopen(path, "wb");
        if (fOutput == nullptr) {
            vm_error("%s: cannot open %s\n", Name(), path);
            fOutput = stderr;
            return false;
        }
        fOwnsOutput = true;
        return true;
    }

    bool Prepare() override
    {
        SystemBus *sys = dynamic_cast<SystemBus *>(ParentBus());
        if (sys == nullptr || (sys->IsPortBased() && !sys->IsAcpi())) {
            vm_error("%s: must be attached to the fdt or the acpi bus\n",
                     Name());
            return false;
        }
        fMmio = AddResource(RES_MMIO, TEST_CONTROL_SIZE, TEST_CONTROL_SIZE);
        if (fMmio == nullptr)
            return false;
        for (int i = 0; i < fIrqCount; i++) {
            fIrqRes[i] = fFixed ? AddFixedResource(RES_IRQ, fFixedLines[i], 1)
                                : AddResource(RES_IRQ, 1);
            if (fIrqRes[i] == nullptr)
                return false;
        }
        return true;
    }

    bool Realize() override
    {
        SystemBus *sys = static_cast<SystemBus *>(ParentBus());

        fMachine = fCtx->machine;
        if (fMachine == nullptr) {
            vm_error("%s: the machine cannot be stopped\n", Name());
            return false;
        }
        for (int i = 0; i < fIrqCount; i++) {
            fIrq[i] = sys->IrqSignalFor(fIrqRes[i]->base);
            if (fIrq[i] == nullptr) {
                vm_error("%s: bad interrupt line %d\n", Name(),
                         (int)fIrqRes[i]->base);
                return false;
            }
        }
        sys->MemMap()->RegisterDevice(fMmio->base, fMmio->size, &fIo,
                                      DEVIO_SIZE32);
        return true;
    }

    void BuildFDT(FDTContext &ctx) override
    {
        FDTBuilder *fdt = ctx.fdt;

        fdt->BeginNodeNum("test-control", fMmio->base);
        fdt->PropStr("compatible", TEST_CONTROL_COMPATIBLE);
        fdt->PropU64Range("reg", fMmio->base, fMmio->size);
        if (fIrqCount > 0) {
            for (int i = 0; i < fIrqCount; i++) {
                fdt->AddCellU32(ctx.irq_phandle);
                fdt_add_irq_spec(ctx, fIrqRes[i]->base);
            }
            fdt->PropCells("interrupts-extended");
        }
        fdt->EndNode();
    }

    void BuildAML(AcpiContext &ctx) override
    {
        std::string name = ctx.NewName("TST");
        AmlBytes r;

        aml_res_memory32_fixed(r, fMmio->base, fMmio->size);
        for (int i = 0; i < fIrqCount; i++)
            ctx.AddInterrupt(r, fIrqRes[i]->base);
        AmlBytes dev = aml_name("_HID", aml_string(TEST_CONTROL_ACPI_HID));
        aml_append(dev, aml_name("_UID",
                                 aml_int(ctx.NewUid(TEST_CONTROL_ACPI_HID))));
        aml_append(dev, aml_name("_CRS", aml_resource_template(r)));
        aml_append(*ctx.aml, aml_device(name.c_str(), dev));
    }
};


uint32_t TestControlDevice::Read(uint32_t offset, int size_log2)
{
    (void)size_log2;
    switch (offset) {
    case TEST_CONTROL_ID:
        return TEST_CONTROL_MAGIC;
    case TEST_CONTROL_VERSION:
        return TEST_CONTROL_REVISION;
    case TEST_CONTROL_IRQ_COUNT:
        return fIrqCount;
    case TEST_CONTROL_IRQ_LEVEL:
        return fIrqLevel;
    case TEST_CONTROL_BLOCK_COUNT:
        return test_block_count();
    case TEST_CONTROL_BLOCK_SELECT:
        return fBlockSelect;
    default:
        break;
    }
    TestBlockControl *disk = test_block_get(fBlockSelect);
    if (disk == nullptr)
        return 0;
    switch (offset) {
    case TEST_CONTROL_BLOCK_SECTORS:
        return disk->Sectors();
    case TEST_CONTROL_BLOCK_MODE:
        return disk->Mode();
    case TEST_CONTROL_BLOCK_FAIL:
        return disk->FailCount();
    case TEST_CONTROL_BLOCK_HELD:
        return disk->Held();
    case TEST_CONTROL_BLOCK_READS:
        return disk->Reads();
    case TEST_CONTROL_BLOCK_WRITES:
        return disk->Writes();
    case TEST_CONTROL_BLOCK_CANCELS:
        return disk->Cancels();
    default:
        return 0;
    }
}


void TestControlDevice::Write(uint32_t offset, uint32_t val, int size_log2)
{
    (void)size_log2;
    switch (offset) {
    case TEST_CONTROL_OUTPUT:
        /* a line at a time, so that a runner reading it as it is written
           never sees half of one */
        putc(val & 0xff, fOutput);
        if ((val & 0xff) == '\n') {
            fflush(fOutput);
        }
        break;
    case TEST_CONTROL_EXIT:
        fflush(fOutput);
        /* as for HTIF: a status that does not fit is a failure, not 0 */
        fMachine->RequestShutdown(val < 256 ? (int)val : 255);
        break;
    case TEST_CONTROL_IRQ_LEVEL:
        val &= (1u << fIrqCount) - 1;
        for (int i = 0; i < fIrqCount; i++) {
            if (((val ^ fIrqLevel) >> i) & 1)
                fIrq[i]->Set((val >> i) & 1);
        }
        fIrqLevel = val;
        break;
    case TEST_CONTROL_BLOCK_SELECT:
        fBlockSelect = val;
        break;
    default:
        if (TestBlockControl *disk = test_block_get(fBlockSelect))
            BlockWrite(disk, offset, val);
        break;
    }
}


void TestControlDevice::BlockWrite(TestBlockControl *disk, uint32_t offset,
                                   uint32_t val)
{
    switch (offset) {
    case TEST_CONTROL_BLOCK_MODE:
        disk->SetMode(val);
        break;
    case TEST_CONTROL_BLOCK_FAIL:
        disk->SetFailCount(val);
        break;
    case TEST_CONTROL_BLOCK_RELEASE:
        disk->Release(val & ~TEST_BLOCK_RELEASE_NEWEST,
                      (val & TEST_BLOCK_RELEASE_NEWEST) != 0);
        break;
    default:
        break;
    }
}


//#pragma mark - class

/* The guest's end of the device test suite: an output stream for its report
   that leaves the consoles alone, a way to stop the emulator with an exit
   status, and interrupt lines the guest drives itself, to test interrupt
   controllers with. Only in builds with the test_fixtures option. */
class TestControlClass final: public DeviceClass {
public:
    TestControlClass(): DeviceClass("test-control") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        const char *output;
        if (!cfg.GetStrOpt("output", &output)) {
            return nullptr;
        }
        auto *dev = new TestControlDevice(cfg.IdOr("test-control"), ctx);
        /* "irqs": how many lines to allocate, or the lines themselves */
        JSONValue irqs = cfg.Get("irqs");
        if (json_is_undefined(irqs)) {
            dev->SetIrqCount(TEST_CONTROL_DEFAULT_IRQS);
        } else if (irqs.type == JSON_INT && irqs.u.int32 >= 0 &&
                   irqs.u.int32 <= TEST_CONTROL_MAX_IRQS) {
            dev->SetIrqCount(irqs.u.int32);
        } else if (irqs.type == JSON_ARRAY &&
                   irqs.u.array->Length() <= TEST_CONTROL_MAX_IRQS) {
            for (int i = 0; i < irqs.u.array->Length(); i++) {
                JSONValue v = json_array_get(irqs, i);
                if (v.type != JSON_INT || v.u.int32 < 0) {
                    vm_error("%s: \"irqs\" holds line numbers\n",
                             dev->Name());
                    delete dev;
                    return nullptr;
                }
                dev->AddFixedLine(v.u.int32);
            }
        } else {
            vm_error("%s: \"irqs\" is a count of at most %d or an array of "
                     "lines\n", dev->Name(), TEST_CONTROL_MAX_IRQS);
            delete dev;
            return nullptr;
        }
        if (output != nullptr) {
            char *path = cfg.ResolvePath(output);
            bool ok = dev->Open(path);
            free(path);
            if (!ok) {
                delete dev;
                return nullptr;
            }
        }
        return dev;
    }
};

static const TestControlClass sTestControlClass;
