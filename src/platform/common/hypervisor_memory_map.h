/*
 * Guest memory map backed by a host hypervisor
 *
 * Copyright (c) 2011-2017 Fabrice Bellard
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

#include "host_hypervisor_ram.h"
#include "iomem.h"


/* With a hypervisor the host reads guest RAM directly and keeps the dirty
   log, so the memory map tells it about each mapping instead of managing
   them itself. The hypervisor's slot for a range is the range's index.
   It must go before the hypervisor does, giving its RAM back. */
class HypervisorPhysMemoryMap final: public PhysMemoryMap {
private:
    HostHypervisorRam &fHypervisor;

    void MapRam(PhysMemoryRange *pr);

public:
    HypervisorPhysMemoryMap(HostHypervisorRam &hypervisor):
        fHypervisor(hypervisor) {}
    ~HypervisorPhysMemoryMap() override;

    PhysMemoryRange *RegisterRam(uint64_t addr, uint64_t size,
                                 int devram_flags) override;
    const uint32_t *GetDirtyBits(PhysMemoryRange *pr) override;
    void SetRamAddr(PhysMemoryRange *pr, uint64_t addr, bool enabled) override;
};
