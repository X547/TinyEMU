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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tvmm_common.h"


#define TVMM_PAGE_SIZE 4096


void tvmm_fail(const char *what, status_t err)
{
    fprintf(stderr, "tvmm: %s failed: %s\n", what, strerror(err));
    exit(1);
}


void tvmm_check(const char *what, status_t err)
{
    if (err < B_OK)
        tvmm_fail(what, err);
}


bool tvmm_open(TvmmDevice &device, TvmmArch arch, uint32 vm_flags,
               TvmmInfo &info)
{
    status_t err = device.Open();
    if (err < B_OK) {
        fprintf(stderr, "tvmm not available: %s\n", strerror(err));
        return false;
    }
    tvmm_check("reading the capabilities", device.GetInfo(info));
    if (info.abiVersion != kTvmmAbiVersion) {
        fprintf(stderr, "tvmm: the kernel add-on has interface version %u, "
                "this needs %u\n", info.abiVersion, kTvmmAbiVersion);
        return false;
    }
    if (info.arch != arch) {
        fprintf(stderr, "tvmm: the kernel add-on runs other processors\n");
        return false;
    }
    if (info.hostStatus < B_OK) {
        fprintf(stderr, "tvmm: this host cannot run guests: %s\n",
                strerror(info.hostStatus));
        return false;
    }
    err = device.CreateVm(vm_flags);
    if (err < B_OK) {
        fprintf(stderr, "tvmm: cannot create a machine: %s\n", strerror(err));
        return false;
    }
    return true;
}


static uint64_t round_to_page(uint64_t size)
{
    return (size + TVMM_PAGE_SIZE - 1) & ~(uint64_t)(TVMM_PAGE_SIZE - 1);
}


/* The RAM comes zeroed from the kernel. */
uint8_t *TvmmRamMap::Alloc(size_t size)
{
    uint64_t rounded = round_to_page(size);
    uint32 id;
    uint8 *ptr;
    if (fDevice.AllocRam(rounded, id, ptr) < B_OK)
        return nullptr;
    fRam.push_back(Ram {ptr, rounded, id});
    return ptr;
}


/* The kernel keeps RAM a slot maps, so the slots go first. */
void TvmmRamMap::Free(uint8_t *ptr)
{
    for (auto it = fRam.begin(); it != fRam.end(); ++it) {
        if (it->ptr == ptr) {
            for (size_t slot = 0; slot < fSlots.size(); slot++) {
                if (fSlots[slot].size != 0 && fSlots[slot].ram_id == it->id)
                    Map(slot, 0, 0, nullptr, false, false);
            }
            tvmm_check("freeing RAM", fDevice.FreeRam(it->id, ptr));
            fRam.erase(it);
            return;
        }
    }
}


void TvmmRamMap::Map(int slot, uint64_t addr, uint64_t size,
                     uint8_t *host_mem, bool read_only, bool log_dirty)
{
    if (slot >= (int)fSlots.size())
        fSlots.resize(slot + 1, Slot {0, 0, false});
    if (size == 0) {
        tvmm_check("unmapping RAM", fDevice.MapRam(slot, 0, 0, 0, 0, 0));
        fSlots[slot] = Slot {0, 0, false};
        return;
    }
    uint32 flags = 0;
    if (read_only)
        flags |= TvmmMapReadOnly;
    if (log_dirty)
        flags |= TvmmMapLogDirty;
    for (const Ram &ram: fRam) {
        if (host_mem >= ram.ptr && host_mem < ram.ptr + ram.size) {
            uint64_t offset = host_mem - ram.ptr;
            tvmm_check("mapping RAM",
                       fDevice.MapRam(slot, addr, round_to_page(size), ram.id,
                                      offset, flags));
            fSlots[slot] = Slot {size, ram.id, log_dirty};
            return;
        }
    }
    fprintf(stderr, "tvmm: RAM at %p is not the kernel's\n", host_mem);
    exit(1);
}


/* From the kernel for a slot mapped with its log; a slot without one counts
   as written all over. */
void TvmmRamMap::GetDirtyLog(int slot, uint32_t *bitmap)
{
    uint64_t pages = round_to_page(fSlots[slot].size) / TVMM_PAGE_SIZE;
    size_t words = (pages + 63) / 64;
    if (!fSlots[slot].log_dirty) {
        memset(bitmap, 0xff, words * 8);
        return;
    }
    tvmm_check("reading the dirty log",
               fDevice.GetDirtyLog(slot, reinterpret_cast<uint64 *>(bitmap),
                                   words));
}
