/*
 * RISC-V CLINT
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
#include "clint.h"

#include <assert.h>

#include "bits.h"

/* register layout: one msip word and one 64 bit mtimecmp per hart */
#define CLINT_MSIP_BASE     0x0000
#define CLINT_MTIMECMP_BASE 0x4000
#define CLINT_MTIME         0xbff8

Clint::Clint(ClintHost &host, int hart_count):
    fHost(host),
    fHartCount(hart_count),
    fTimecmp(std::make_unique<uint64_t[]>(hart_count)),
    fSoftIrq(std::make_unique<bool[]>(hart_count)),
    fTimerIrq(std::make_unique<bool[]>(hart_count))
{
}

void Clint::SetIrq(int hart, bool timer, bool level)
{
    (timer ? fTimerIrq : fSoftIrq)[hart] = level;
    fHost.SetClintIrq(hart, timer, level);
}

uint64_t Clint::RunTimer(int hart, uint64_t now)
{
    if (fTimerIrq[hart])
        return UINT64_MAX;
    if (now >= fTimecmp[hart]) {
        SetIrq(hart, true, true);
        return UINT64_MAX;
    }
    return fTimecmp[hart];
}

uint64_t Clint::DeviceRead(uint32_t offset, int size_log2)
{
    switch (size_log2) {
        case 2: {
            if (offset < CLINT_MTIMECMP_BASE) {
                uint32_t hart = (offset - CLINT_MSIP_BASE) / 4;
                return hart < (uint32_t)fHartCount ? fSoftIrq[hart] : 0;
            }
            uint64_t val = DeviceRead(offset & ~7u, 3);
            return offset & 4 ? val >> 32 : (uint32_t)val;
        }
        case 3: {
            offset &= ~7u;
            if (offset < CLINT_MTIMECMP_BASE)
                return concat_bits(DeviceRead(offset + 4, 2), DeviceRead(offset, 2),
                                   32);
            if (offset < CLINT_MTIME) {
                uint32_t hart = (offset - CLINT_MTIMECMP_BASE) / 8;
                return hart < (uint32_t)fHartCount ? fTimecmp[hart] : 0;
            }
            if (offset == CLINT_MTIME)
                return fHost.ClintTime();
            return 0;
        }
        default:
            abort();
    }
}

void Clint::DeviceWrite(uint32_t offset, uint64_t val, int size_log2)
{
    switch (size_log2) {
        case 2: {
            if (offset < CLINT_MTIMECMP_BASE) {
                /* a software interrupt to that hart */
                uint32_t hart = (offset - CLINT_MSIP_BASE) / 4;
                if (hart < (uint32_t)fHartCount)
                    SetIrq(hart, false, val & 1);
                return;
            }
            /* one half of a 64 bit register */
            uint32_t reg = offset & ~7u;
            DeviceWrite(reg, set_bits(DeviceRead(reg, 3), (offset & 4) * 8, 32,
                                      val), 3);
            break;
        }
        case 3: {
            offset &= ~7u;
            if (offset < CLINT_MTIMECMP_BASE) {
                DeviceWrite(offset, get_bits(val, 0, 32), 2);
                DeviceWrite(offset + 4, get_bits(val, 32, 32), 2);
            } else if (offset < CLINT_MTIME) {
                uint32_t hart = (offset - CLINT_MTIMECMP_BASE) / 8;
                if (hart >= (uint32_t)fHartCount)
                    return;
                fTimecmp[hart] = val;
                SetIrq(hart, true, false);
                fHost.ClintTimerChanged(val);
            } else if (offset == CLINT_MTIME) {
                /* mtime is read-write; every hart's comparison is made again */
                for (int hart = 0; hart < fHartCount; hart++)
                    SetIrq(hart, true, false);
                fHost.SetClintTime(val);
            }
            break;
        }
        default:
            abort();
    }
}
