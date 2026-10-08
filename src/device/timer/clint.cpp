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

uint32_t Clint::DeviceRead(uint32_t offset, int size_log2)
{
    uint32_t val = 0;
    uint32_t hart;

    assert(size_log2 == 2);
    if (offset < CLINT_MTIMECMP_BASE) {
        hart = (offset - CLINT_MSIP_BASE) / 4;
        if (hart < (uint32_t)fHartCount)
            val = fSoftIrq[hart];
    } else if (offset < CLINT_MTIME) {
        hart = (offset - CLINT_MTIMECMP_BASE) / 8;
        if (hart < (uint32_t)fHartCount) {
            if ((offset - CLINT_MTIMECMP_BASE) % 8 == 0)
                val = fTimecmp[hart];
            else
                val = fTimecmp[hart] >> 32;
        }
    } else if (offset == CLINT_MTIME) {
        val = fHost.ClintTime();
    } else if (offset == CLINT_MTIME + 4) {
        val = fHost.ClintTime() >> 32;
    }
    return val;
}

void Clint::DeviceWrite(uint32_t offset, uint32_t val, int size_log2)
{
    uint32_t hart;

    assert(size_log2 == 2);
    if (offset < CLINT_MTIMECMP_BASE) {
        /* a software interrupt to that hart */
        hart = (offset - CLINT_MSIP_BASE) / 4;
        if (hart >= (uint32_t)fHartCount)
            return;
        SetIrq(hart, false, val & 1);
    } else if (offset < CLINT_MTIME) {
        hart = (offset - CLINT_MTIMECMP_BASE) / 8;
        if (hart >= (uint32_t)fHartCount)
            return;
        uint64_t &cmp = fTimecmp[hart];
        if ((offset - CLINT_MTIMECMP_BASE) % 8 == 0)
            cmp = set_bits(cmp, 0, 32, val);
        else
            cmp = set_bits(cmp, 32, 32, val);
        SetIrq(hart, true, false);
        fHost.ClintTimerChanged(cmp);
    }
}
