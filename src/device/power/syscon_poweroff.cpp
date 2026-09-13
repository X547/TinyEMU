/*
 * MMIO power off register
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
#include "syscon_poweroff.h"

#include <stdio.h>

#include "devices.h"
#include "fdt.h"
#include "machine.h"

#define SYSCON_POWEROFF_SIZE 0x1000


class SysconPoweroffDevice final: public Device {
private:
    DeviceContext *fCtx;
    VirtMachine *fMachine = nullptr;
    Resource *fMmio = nullptr;
    bool fResetReported = false;

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t val, int size_log2);

    DeviceIOAdapter<SysconPoweroffDevice, &SysconPoweroffDevice::Read,
                    &SysconPoweroffDevice::Write> fIo {*this};

public:
    SysconPoweroffDevice(DeviceContext *ctx):
        Device("syscon-poweroff"), fCtx(ctx) {}

    bool Prepare() override
    {
        SystemBus *sys = dynamic_cast<SystemBus *>(ParentBus());
        if (sys == nullptr || sys->IsPortBased()) {
            vm_error("%s: must be attached to an FDT bus\n", Name());
            return false;
        }
        fMmio = AddResource(RES_MMIO, SYSCON_POWEROFF_SIZE,
                            SYSCON_POWEROFF_SIZE);
        return fMmio != nullptr;
    }

    bool Realize() override
    {
        SystemBus *sys = static_cast<SystemBus *>(ParentBus());

        fMachine = fCtx->machine;
        if (fMachine == nullptr) {
            vm_error("%s: the machine cannot be powered off\n", Name());
            return false;
        }
        sys->MemMap()->RegisterDevice(fMmio->base, fMmio->size, &fIo,
                                      DEVIO_SIZE32);
        return true;
    }

    void BuildFDT(FDTContext &ctx) override
    {
        FDTBuilder *fdt = ctx.fdt;
        uint32_t phandle = fdt->AllocPhandle();

        fdt->BeginNodeNum("test", fMmio->base);
        fdt->PropStrList("compatible", "sifive,test1", "sifive,test0",
                         "syscon", nullptr);
        fdt->PropU64Range("reg", fMmio->base, fMmio->size);
        fdt->PropU32("phandle", phandle);
        fdt->EndNode();

        fdt->BeginNode("poweroff");
        fdt->PropStr("compatible", "syscon-poweroff");
        fdt->PropU32("regmap", phandle);
        fdt->PropU32("offset", 0);
        fdt->PropU32("value", SYSCON_POWEROFF_PASS);
        fdt->EndNode();
    }
};


uint32_t SysconPoweroffDevice::Read(uint32_t offset, int size_log2)
{
    (void)offset;
    (void)size_log2;
    return 0;
}


void SysconPoweroffDevice::Write(uint32_t offset, uint32_t val, int size_log2)
{
    (void)size_log2;
    if (offset != 0) {
        return;
    }

    switch (val & 0xffff) {
    case SYSCON_POWEROFF_PASS:
        printf("\nPower off.\n");
        fMachine->RequestShutdown(0);
        break;
    case SYSCON_POWEROFF_FAIL: {
        /* a failure never exits with 0 */
        int code = val >> 16;
        if (code == 0) {
            code = 1;
        }
        printf("\nPower off, exit code %d.\n", code);
        fMachine->RequestShutdown(code < 256 ? code : 255);
        break;
    }
    case SYSCON_POWEROFF_RESET:
        /* there is no reset to perform, so the node offers none */
        if (!fResetReported) {
            printf("syscon-poweroff: reset is not supported\n");
            fResetReported = true;
        }
        break;
    default:
        break;
    }
}


//#pragma mark - factory

Device *syscon_poweroff_node_create(DeviceContext *ctx)
{
    return new SysconPoweroffDevice(ctx);
}
