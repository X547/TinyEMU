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

void Clint::Map(PhysMemoryMap *map, uint64_t base)
{
    map->RegisterDevice(base + CLINT_MSIP_BASE, CLINT_MTIMECMP_BASE, &fMswiIo,
                        DEVIO_SIZE32);
    /* mtime and mtimecmp are read and written whole by an 8 byte access, and
       by halves with 4 byte ones */
    map->RegisterDevice(base + CLINT_MTIMECMP_BASE,
                        CLINT_SIZE - CLINT_MTIMECMP_BASE, &fMtimerIo,
                        DEVIO_SIZE64 | DEVIO_WRITE_MERGE);
}

uint32_t Clint::MswiRead(uint32_t offset, int size_log2)
{
    (void)size_log2;
    uint32_t hart = offset / 4;
    return hart < (uint32_t)fHartCount ? fSoftIrq[hart] : 0;
}

void Clint::MswiWrite(uint32_t offset, uint32_t val, int size_log2)
{
    (void)size_log2;
    /* a software interrupt to that hart */
    uint32_t hart = offset / 4;
    if (hart < (uint32_t)fHartCount)
        SetIrq(hart, false, val & 1);
}

uint64_t Clint::MtimerRead(uint32_t offset, int size_log2)
{
    (void)size_log2;
    offset += CLINT_MTIMECMP_BASE;
    if (offset == CLINT_MTIME)
        return fHost.ClintTime();
    uint32_t hart = (offset - CLINT_MTIMECMP_BASE) / 8;
    return hart < (uint32_t)fHartCount ? fTimecmp[hart] : 0;
}

void Clint::MtimerWrite(uint32_t offset, uint64_t val, int size_log2)
{
    (void)size_log2;
    offset += CLINT_MTIMECMP_BASE;
    if (offset == CLINT_MTIME) {
        /* mtime is read-write; every hart's comparison is made again */
        for (int hart = 0; hart < fHartCount; hart++)
            SetIrq(hart, true, false);
        fHost.SetClintTime(val);
        return;
    }
    uint32_t hart = (offset - CLINT_MTIMECMP_BASE) / 8;
    if (hart >= (uint32_t)fHartCount)
        return;
    fTimecmp[hart] = val;
    SetIrq(hart, true, false);
    fHost.ClintTimerChanged(val);
}
