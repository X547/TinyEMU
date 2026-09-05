/*
 * IO memory handling
 *
 * Copyright (c) 2016-2017 Fabrice Bellard
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


#define DEVIO_SIZE8  (1 << 0)
#define DEVIO_SIZE16 (1 << 1)
#define DEVIO_SIZE32 (1 << 2)
/* not supported, could add specific 64 bit callbacks when needed */
//#define DEVIO_SIZE64 (1 << 3)
#define DEVIO_DISABLED (1 << 4)

#define DEVRAM_FLAG_ROM        (1 << 0) /* not writable */
#define DEVRAM_FLAG_DIRTY_BITS (1 << 1) /* maintain dirty bits */
#define DEVRAM_FLAG_DISABLED   (1 << 2) /* allocated but not mapped */
#define DEVRAM_PAGE_SIZE_LOG2 12
#define DEVRAM_PAGE_SIZE (1 << DEVRAM_PAGE_SIZE_LOG2)

#define PHYS_MEM_RANGE_MAX 32

class PhysMemoryMap;


/* Implemented by a device to serve reads and writes of one mapped range. */
class DeviceIO {
public:
    virtual ~DeviceIO() = default;

    virtual uint32_t DeviceRead(uint32_t offset, int size_log2) = 0;
    virtual void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) = 0;
};


/* A device that maps several ranges needs one DeviceIO per range, so it holds
   an adapter per range that forwards to a distinct pair of its own methods.
   The member pointers are compile-time template arguments; the dispatch the
   memory map performs is still virtual. */
template <
    typename Owner,
    uint32_t (Owner::*ReadFn)(uint32_t offset, int size_log2),
    void (Owner::*WriteFn)(uint32_t offset, uint32_t val, int size_log2)
>
class DeviceIOAdapter final: public DeviceIO {
private:
    Owner &fOwner;

public:
    DeviceIOAdapter(Owner &owner): fOwner(owner) {}

    uint32_t DeviceRead(uint32_t offset, int size_log2) override
    {
        return (fOwner.*ReadFn)(offset, size_log2);
    }

    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override
    {
        (fOwner.*WriteFn)(offset, val, size_log2);
    }
};


/* Implemented by the CPU so the memory map can invalidate write TLB entries
   when a RAM mapping moves or a dirty page is reclaimed. */
class TlbFlushTarget {
public:
    virtual ~TlbFlushTarget() = default;

    virtual void FlushTlbWriteRange(uint8_t *ram_addr, size_t ram_size) = 0;
};


struct PhysMemoryRange {
    PhysMemoryMap *map;
    uint64_t addr;
    uint64_t org_size; /* original size */
    uint64_t size; /* =org_size or 0 if the mapping is disabled */
    bool is_ram;
    /* the following is used for RAM access */
    int devram_flags;
    uint8_t *phys_mem;
    int dirty_bits_size; /* in bytes */
    uint32_t *dirty_bits; /* nullptr if not used */
    uint32_t *dirty_bits_tab[2];
    int dirty_bits_index; /* 0-1 */
    /* the following is used for I/O access */
    DeviceIO *io;
    int devio_flags;

    /* Fetch the bitmap of dirty pages and reset it. */
    const uint32_t *DirtyBits();
    void ResetDirtyBit(size_t offset);
    void SetAddr(uint64_t addr, bool enabled);

    void SetDirtyBit(size_t offset)
    {
        if (dirty_bits == nullptr) {
            return;
        }
        size_t page_index = offset >> DEVRAM_PAGE_SIZE_LOG2;
        uint32_t mask = 1 << (page_index & 0x1f);
        dirty_bits[page_index >> 5] |= mask;
    }

    bool IsDirtyBit(size_t offset) const
    {
        if (dirty_bits == nullptr) {
            return true;
        }
        size_t page_index = offset >> DEVRAM_PAGE_SIZE_LOG2;
        return (dirty_bits[page_index >> 5] >> (page_index & 0x1f)) & 1;
    }
};


/* The default implementation backs RAM with anonymous memory and keeps the
   dirty bitmap itself; the x86 KVM path overrides the virtual methods to let
   the kernel own both. */
class PhysMemoryMap {
private:
    TlbFlushTarget *fTlbFlushTarget = nullptr;
    int fRangeCount = 0;
    PhysMemoryRange fRanges[PHYS_MEM_RANGE_MAX] {};

protected:
    /* Reserve and fill in a range descriptor without backing it with memory. */
    PhysMemoryRange *RegisterRamEntry(uint64_t addr, uint64_t size,
                                      int devram_flags);

public:
    virtual ~PhysMemoryMap();

    int RangeCount() const {return fRangeCount;}
    PhysMemoryRange *RangeAt(int index) {return &fRanges[index];}
    int IndexOfRange(const PhysMemoryRange *pr) const {return pr - fRanges;}

    void SetTlbFlushTarget(TlbFlushTarget *target) {fTlbFlushTarget = target;}
    void FlushTlbWriteRange(uint8_t *ram_addr, size_t ram_size);

    /* Return nullptr if not found. */
    PhysMemoryRange *FindRange(uint64_t paddr);
    /* Return nullptr if no valid RAM page. The access can only be done in
       the page. */
    uint8_t *GetRamPtr(uint64_t paddr, bool is_rw);

    PhysMemoryRange *RegisterDevice(uint64_t addr, uint64_t size, DeviceIO *io,
                                    int devio_flags);

    virtual PhysMemoryRange *RegisterRam(uint64_t addr, uint64_t size,
                                         int devram_flags);
    virtual void FreeRam(PhysMemoryRange *pr);
    virtual const uint32_t *GetDirtyBits(PhysMemoryRange *pr);
    virtual void SetRamAddr(PhysMemoryRange *pr, uint64_t addr, bool enabled);
};


/* IRQ support */

/* Implemented by an interrupt controller. */
class IRQTarget {
public:
    virtual ~IRQTarget() = default;

    virtual void SetIRQ(int irq_num, int level) = 0;
};


/* One wire into an IRQTarget. Devices hold these by value, so it stays a
   plain assignable object rather than an interface of its own. */
class IRQSignal {
private:
    IRQTarget *fTarget = nullptr;
    int fIrqNum = 0;

public:
    void Init(IRQTarget *target, int irq_num)
    {
        fTarget = target;
        fIrqNum = irq_num;
    }

    void Set(int level) {fTarget->SetIRQ(fIrqNum, level);}
};
