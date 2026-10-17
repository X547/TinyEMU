/*
 * Guest RAM as a host hypervisor holds it
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

#include <stddef.h>
#include <stdint.h>


/* Guest RAM as a hypervisor holds it, which HypervisorPhysMemoryMap
   (platform/common) keeps up to date. Called with the device lock held. */
class HostHypervisorRam {
public:
    virtual ~HostHypervisorRam() = default;

    /* Memory for guest RAM, zeroed, as host_ram_alloc() gives it. It comes
       from here because a hypervisor may have to set it up before anything
       is written to it; nullptr on failure. */
    virtual uint8_t *AllocRam(size_t size) = 0;
    virtual void FreeRam(uint8_t *ptr, size_t size) = 0;

    /* Guest RAM at 'addr' backed by 'host_mem'. 'slot' names the range from
       then on; mapping it again moves it, and a size of 0 removes it. */
    virtual void MapRam(int slot, uint64_t addr, uint64_t size,
                        uint8_t *host_mem, bool read_only,
                        bool log_dirty) = 0;
    /* The pages of 'slot' written since the last call, one bit per page in
       64 bit words; the log starts over. */
    virtual void GetDirtyLog(int slot, uint32_t *bitmap) = 0;
};
