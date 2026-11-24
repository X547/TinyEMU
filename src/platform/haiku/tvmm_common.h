/*
 * What the processor back ends on tvmm share: the guest RAM the kernel holds
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

#include <vector>

#include <TvmmClient.h>


/* A failed call to the kernel, which the machine cannot go on without. */
[[noreturn]] void tvmm_fail(const char *what, status_t err);
void tvmm_check(const char *what, status_t err);

/* The device opened with a VM in it, after checking that the kernel add-on
   is one this client can use; false, having said why, if not. 'info' is
   what the add-on reported. */
bool tvmm_open(TvmmDevice &device, TvmmArch arch, uint32 vm_flags,
               TvmmInfo &info);


/* The guest RAM of a VM, which the kernel allocates and keeps mapped where
   the guest has it; the platform knows it by its address in this team. */
class TvmmRamMap {
private:
    const TvmmDevice &fDevice;
    struct Ram {
        uint8_t *ptr;
        uint64_t size;
        uint32 id;
    };
    std::vector<Ram> fRam;
    /* what each slot maps, for the dirty log and for freeing the RAM */
    struct Slot {
        uint64_t size;
        uint32 ram_id;
        bool log_dirty;
    };
    std::vector<Slot> fSlots;

public:
    explicit TvmmRamMap(const TvmmDevice &device): fDevice(device) {}

    /* HostHypervisorRam */
    uint8_t *Alloc(size_t size);
    void Free(uint8_t *ptr);
    void Map(int slot, uint64_t addr, uint64_t size, uint8_t *host_mem,
             bool read_only, bool log_dirty);
    void GetDirtyLog(int slot, uint32_t *bitmap);
};
