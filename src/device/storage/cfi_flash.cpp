/*
 * CFI parallel NOR flash
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
#include "cfi_flash.h"

#include <stdio.h>
#include <string.h>

#include <vector>

#include "bits.h"
#include "fdt.h"
#include "iomem.h"
#include "machine.h"

/* Two 16 bit chips side by side on a 32 bit bus, each seeing its own half of
   every word: a command is written to both halves, and each register reads
   back in both. */
#define CFI_BANK_WIDTH 4
#define CFI_CHIP_COUNT 2

/* The write buffer, 32 words per chip. */
#define CFI_WRITE_BUFFER_SIZE 128

#define CFI_SECTOR_SIZE 512
/* The most written back to the image in one request. */
#define CFI_WRITE_BACK_MAX (64 << 10)

/* what QEMU's virt machines report for their flash */
#define CFI_MANUFACTURER_ID 0x0089
#define CFI_DEVICE_ID       0x0018

#define CFI_QUERY_SIZE 0x50

enum {
    CMD_PROGRAM_ALT      = 0x10,
    CMD_ERASE            = 0x20,
    CMD_LOCK_DOWN        = 0x2f,
    CMD_PROGRAM          = 0x40,
    CMD_CLEAR_STATUS     = 0x50,
    CMD_LOCK_SETUP       = 0x60,
    CMD_READ_STATUS      = 0x70,
    CMD_READ_ID          = 0x90,
    CMD_READ_QUERY       = 0x98,
    CMD_SUSPEND          = 0xb0,
    CMD_CONFIRM          = 0xd0,
    CMD_BUFFERED_PROGRAM = 0xe8,
    CMD_READ_ARRAY       = 0xff,
};

/* the second cycle of a lock command */
#define CMD_LOCK 0x01

#define SR_READY         bit_at<uint32_t>(7)
#define SR_ERASE_ERROR   bit_at<uint32_t>(5)
#define SR_PROGRAM_ERROR bit_at<uint32_t>(4)
#define SR_LOCKED        bit_at<uint32_t>(1)

/* what a read returns */
enum CFIReadMode {
    READ_ARRAY,
    READ_STATUS,
    READ_ID,
    READ_QUERY,
};

/* what the next write is taken as */
enum CFIWriteState {
    WRITE_COMMAND,
    WRITE_PROGRAM_DATA,
    WRITE_ERASE_CONFIRM,
    WRITE_LOCK_COMMAND,
    WRITE_BUFFER_COUNT,
    WRITE_BUFFER_DATA,
    WRITE_BUFFER_CONFIRM,
};


class CFIFlashDevice;

/* Told when a write back to the image finishes. */
class CFIWriteBackDone final: public BlockCompletion {
private:
    CFIFlashDevice &fOwner;

public:
    CFIWriteBackDone(CFIFlashDevice &owner): fOwner(owner) {}

    void Complete(int ret) override;
};


class CFIFlashDevice final: public Device {
private:
    int64_t fFixedBase;
    uint32_t fBlockSize;
    bool fReadOnly;
    uint64_t fSize = 0;
    /* the part of the flash the image file holds */
    uint64_t fFileSize = 0;

    Resource *fMmio = nullptr;
    /* The array, read in place. Unmapped in favour of fRegs while a read
       returns anything else. */
    PhysMemoryRange *fArrayRange = nullptr;
    PhysMemoryRange *fRegsRange = nullptr;
    uint8_t *fArray = nullptr;

    CFIReadMode fReadMode = READ_ARRAY;
    CFIWriteState fWriteState = WRITE_COMMAND;
    uint32_t fStatus = SR_READY;
    std::vector<bool> fLocked;
    uint8_t fQuery[CFI_QUERY_SIZE] {};

    /* a buffered program in progress */
    uint64_t fBufferBase = 0;
    int fBufferWrites = 0;
    bool fBufferError = false;
    uint8_t fBuffer[CFI_WRITE_BUFFER_SIZE];

    /* Sectors of the image changed but not yet written back. One request is
       in flight at a time, so that two writes of a sector cannot land out of
       order. */
    std::vector<bool> fDirty;
    std::unique_ptr<uint8_t[]> fWriteBack;
    bool fWriteBackBusy = false;
    bool fWriteBackFailed = false;
    CFIWriteBackDone fWriteBackDone {*this};
    /* last, so that it goes before the buffer its requests write from */
    std::unique_ptr<HostBlockDevice> fBlock;

    uint32_t Read(uint32_t offset, int size_log2);
    void Write(uint32_t offset, uint32_t val, int size_log2);

    DeviceIOAdapter<CFIFlashDevice, &CFIFlashDevice::Read,
                    &CFIFlashDevice::Write> fIo {*this};

    void BuildQuery();
    void SetReadMode(CFIReadMode mode);
    uint32_t ReadWord(uint32_t offset);
    void Command(uint32_t offset, uint8_t cmd);
    bool Writable(uint32_t offset, uint32_t error);
    void Program(uint32_t offset, const uint8_t *data, uint32_t len);
    void MarkDirty(uint64_t offset, uint64_t len);
    void WriteBack();

public:
    CFIFlashDevice(const char *name, std::unique_ptr<HostBlockDevice> bs,
                   int64_t base, uint32_t block_size, bool read_only):
        Device(name),
        fFixedBase(base),
        fBlockSize(block_size),
        fReadOnly(read_only),
        fBlock(std::move(bs)) {}

    bool Prepare() override;
    bool Realize() override;
    void BuildFDT(FDTContext &ctx) override;

    void WriteBackComplete(int ret);
};


void CFIWriteBackDone::Complete(int ret)
{
    fOwner.WriteBackComplete(ret);
}


bool CFIFlashDevice::Prepare()
{
    SystemBus *sys = dynamic_cast<SystemBus *>(ParentBus());
    if (sys == nullptr || sys->IsPortBased()) {
        vm_error("%s: must be attached to an FDT bus\n", Name());
        return false;
    }

    fFileSize = (uint64_t)fBlock->SectorCount() * CFI_SECTOR_SIZE;
    if (fFileSize == 0) {
        vm_error("%s: the image is empty\n", Name());
        return false;
    }
    /* The query gives the size as a power of two, and a driver that finds
       the erase blocks adding up to something else refuses the chip. */
    fSize = fBlockSize;
    while (fSize < fFileSize) {
        fSize <<= 1;
    }
    if (fSize / fBlockSize > 0x10000) {
        vm_error("%s: too many erase blocks; make 'block_size' larger\n",
                 Name());
        return false;
    }

    if (fFixedBase >= 0) {
        if ((fFixedBase & (DEVRAM_PAGE_SIZE - 1)) != 0) {
            vm_error("%s: 'base' must be a multiple of 4 KB\n", Name());
            return false;
        }
        fMmio = AddFixedResource(RES_MMIO, fFixedBase, fSize);
    } else {
        fMmio = AddResource(RES_MMIO, fSize);
    }
    return fMmio != nullptr;
}


bool CFIFlashDevice::Realize()
{
    SystemBus *sys = static_cast<SystemBus *>(ParentBus());
    PhysMemoryMap *map = sys->MemMap();
    int flags = DEVIO_SIZE8 | DEVIO_SIZE16 | DEVIO_SIZE32;

    fArrayRange = map->RegisterRomDevice(fMmio->base, fSize, &fIo, flags, 0);
    fRegsRange = map->RegisterDevice(fMmio->base, fSize, &fIo,
                                     flags | DEVIO_DISABLED);
    fArray = fArrayRange->phys_mem;

    /* past the end of the image the flash starts out erased */
    memset(fArray + fFileSize, 0xff, fSize - fFileSize);
    if (fBlock->Read(0, fArray, fFileSize / CFI_SECTOR_SIZE) < 0) {
        vm_error("%s: could not read the image\n", Name());
        return false;
    }

    fLocked.assign(fSize / fBlockSize, false);
    fDirty.assign(fFileSize / CFI_SECTOR_SIZE, false);
    fWriteBack.reset(new uint8_t[CFI_WRITE_BACK_MAX]);
    BuildQuery();
    return true;
}


void CFIFlashDevice::BuildFDT(FDTContext &ctx)
{
    FDTBuilder *fdt = ctx.fdt;

    fdt->BeginNodeNum("flash", fMmio->base);
    fdt->PropStr("compatible", "cfi-flash");
    fdt->PropU64Range("reg", fMmio->base, fSize);
    fdt->PropU32("bank-width", CFI_BANK_WIDTH);
    fdt->EndNode();
}


/* The query structure of one chip. */
void CFIFlashDevice::BuildQuery()
{
    uint8_t *q = fQuery;
    uint32_t chip_size = fSize / CFI_CHIP_COUNT;
    uint32_t chip_block = fBlockSize / CFI_CHIP_COUNT;
    uint32_t blocks = fSize / fBlockSize;
    int size_log2 = 0;

    while ((1u << size_log2) < chip_size) {
        size_log2++;
    }

    q[0x10] = 'Q';
    q[0x11] = 'R';
    q[0x12] = 'Y';
    q[0x13] = 0x01; /* Intel command set */
    q[0x15] = 0x31; /* where its extended table is */
    q[0x1b] = 0x27; /* Vcc 2.7 to 3.6 V */
    q[0x1c] = 0x36;
    q[0x1f] = 4;    /* word program: 16 us typical */
    q[0x20] = 8;    /* buffer program: 256 us */
    q[0x21] = 10;   /* block erase: 1 s */
    q[0x23] = 4;    /* the most each takes, as powers of two of that */
    q[0x24] = 4;
    q[0x25] = 4;
    q[0x27] = size_log2;
    q[0x28] = 0x01; /* x16 */
    q[0x2a] = 6;    /* 64 byte write buffer */
    q[0x2c] = 1;    /* one region of uniform erase blocks */
    q[0x2d] = get_bits(blocks - 1, 0, 8);
    q[0x2e] = get_bits(blocks - 1, 8, 8);
    q[0x2f] = get_bits(chip_block, 8, 8);
    q[0x30] = get_bits(chip_block, 16, 8);

    /* the Intel extended table, version 1.0, with no optional features */
    q[0x31] = 'P';
    q[0x32] = 'R';
    q[0x33] = 'I';
    q[0x34] = '1';
    q[0x35] = '0';
    q[0x3d] = 0x33; /* Vcc 3.3 V */
    q[0x3f] = 1;    /* one protection register field, all zero */
}


void CFIFlashDevice::SetReadMode(CFIReadMode mode)
{
    bool was_array = fReadMode == READ_ARRAY;

    fReadMode = mode;
    if ((mode == READ_ARRAY) == was_array) {
        return;
    }
    if (mode == READ_ARRAY) {
        fRegsRange->SetAddr(fMmio->base, false);
        fArrayRange->SetAddr(fMmio->base, true);
    } else {
        fArrayRange->SetAddr(fMmio->base, false);
        fRegsRange->SetAddr(fMmio->base, true);
    }
}


/* The bus word at 'offset', as both chips drive it. */
uint32_t CFIFlashDevice::ReadWord(uint32_t offset)
{
    uint32_t val = 0;

    switch (fReadMode) {
    case READ_ARRAY:
        memcpy(&val, fArray + offset, sizeof(val));
        return val;
    case READ_STATUS:
        val = fStatus;
        break;
    case READ_ID:
        /* addressed in chip words from the start of a block */
        switch ((offset % fBlockSize) / CFI_BANK_WIDTH) {
        case 0:
            val = CFI_MANUFACTURER_ID;
            break;
        case 1:
            val = CFI_DEVICE_ID;
            break;
        case 2:
            val = fReadOnly || fLocked[offset / fBlockSize];
            break;
        }
        break;
    case READ_QUERY:
        if (offset / CFI_BANK_WIDTH < CFI_QUERY_SIZE) {
            val = fQuery[offset / CFI_BANK_WIDTH];
        }
        break;
    }
    return val | val << 16;
}


uint32_t CFIFlashDevice::Read(uint32_t offset, int size_log2)
{
    uint32_t shift = (offset % CFI_BANK_WIDTH) * 8;
    uint32_t val = ReadWord(offset & ~(CFI_BANK_WIDTH - 1)) >> shift;

    return get_bits(val, 0, 8 << size_log2);
}


/* Whether the block holding 'offset' takes a program or an erase; if not,
   'error' goes in the status. */
bool CFIFlashDevice::Writable(uint32_t offset, uint32_t error)
{
    if (fReadOnly || fLocked[offset / fBlockSize]) {
        fStatus |= error | SR_LOCKED;
        return false;
    }
    return true;
}


/* Programming only clears bits; it takes an erase to set them. */
void CFIFlashDevice::Program(uint32_t offset, const uint8_t *data,
                             uint32_t len)
{
    for (uint32_t i = 0; i < len; i++) {
        fArray[offset + i] &= data[i];
    }
    MarkDirty(offset, len);
}


void CFIFlashDevice::Command(uint32_t offset, uint8_t cmd)
{
    switch (cmd) {
    case CMD_READ_ARRAY:
        SetReadMode(READ_ARRAY);
        break;
    case CMD_READ_STATUS:
        SetReadMode(READ_STATUS);
        break;
    case CMD_READ_ID:
        SetReadMode(READ_ID);
        break;
    case CMD_READ_QUERY:
        SetReadMode(READ_QUERY);
        break;
    case CMD_CLEAR_STATUS:
        fStatus = SR_READY;
        break;
    case CMD_PROGRAM:
    case CMD_PROGRAM_ALT:
        fWriteState = WRITE_PROGRAM_DATA;
        SetReadMode(READ_STATUS);
        break;
    case CMD_ERASE:
        fWriteState = WRITE_ERASE_CONFIRM;
        SetReadMode(READ_STATUS);
        break;
    case CMD_LOCK_SETUP:
        fWriteState = WRITE_LOCK_COMMAND;
        SetReadMode(READ_STATUS);
        break;
    case CMD_BUFFERED_PROGRAM:
        /* the buffer is free at once, and the status says so */
        fBufferBase = offset & ~(CFI_WRITE_BUFFER_SIZE - 1);
        fWriteState = WRITE_BUFFER_COUNT;
        SetReadMode(READ_STATUS);
        break;
    case CMD_SUSPEND:
    case CMD_CONFIRM:
        /* nothing takes long enough to suspend, or to resume */
        SetReadMode(READ_STATUS);
        break;
    default:
        break;
    }
}


void CFIFlashDevice::Write(uint32_t offset, uint32_t val, int size_log2)
{
    uint32_t len = 1 << size_log2;
    uint8_t cmd = get_bits(val, 0, 8);
    uint8_t data[4];

    memcpy(data, &val, sizeof(data));

    switch (fWriteState) {
    case WRITE_COMMAND:
        Command(offset, cmd);
        return;

    case WRITE_PROGRAM_DATA:
        fWriteState = WRITE_COMMAND;
        if (Writable(offset, SR_PROGRAM_ERROR)) {
            Program(offset, data, len);
        }
        return;

    case WRITE_ERASE_CONFIRM:
        fWriteState = WRITE_COMMAND;
        if (cmd != CMD_CONFIRM) {
            fStatus |= SR_ERASE_ERROR | SR_PROGRAM_ERROR;
        } else if (Writable(offset, SR_ERASE_ERROR)) {
            uint32_t block = offset & ~(fBlockSize - 1);
            memset(fArray + block, 0xff, fBlockSize);
            MarkDirty(block, fBlockSize);
        }
        return;

    case WRITE_LOCK_COMMAND:
        fWriteState = WRITE_COMMAND;
        if (cmd == CMD_LOCK || cmd == CMD_LOCK_DOWN) {
            fLocked[offset / fBlockSize] = true;
        } else if (cmd == CMD_CONFIRM) {
            fLocked[offset / fBlockSize] = false;
        } else {
            /* the configuration register, which nothing here reads */
        }
        return;

    case WRITE_BUFFER_COUNT: {
        /* words per chip less one, which is also the bus words to come */
        int count = get_bits(val, 0, 16) + 1;
        if (count * CFI_BANK_WIDTH > CFI_WRITE_BUFFER_SIZE) {
            fStatus |= SR_ERASE_ERROR | SR_PROGRAM_ERROR;
            fWriteState = WRITE_COMMAND;
            return;
        }
        memset(fBuffer, 0xff, sizeof(fBuffer));
        fBufferWrites = count;
        fBufferError = false;
        fWriteState = WRITE_BUFFER_DATA;
        return;
    }

    case WRITE_BUFFER_DATA:
        if (offset >= fBufferBase &&
            offset + len <= fBufferBase + CFI_WRITE_BUFFER_SIZE) {
            memcpy(fBuffer + (offset - fBufferBase), data, len);
        } else {
            fBufferError = true;
        }
        if (--fBufferWrites == 0) {
            fWriteState = WRITE_BUFFER_CONFIRM;
        }
        return;

    case WRITE_BUFFER_CONFIRM:
        fWriteState = WRITE_COMMAND;
        if (cmd != CMD_CONFIRM || fBufferError) {
            fStatus |= SR_ERASE_ERROR | SR_PROGRAM_ERROR;
        } else if (Writable(fBufferBase, SR_PROGRAM_ERROR)) {
            /* bytes the buffer was not given stay 0xff and change nothing */
            Program(fBufferBase, fBuffer, CFI_WRITE_BUFFER_SIZE);
        }
        return;
    }
}


//#pragma mark - write back

void CFIFlashDevice::MarkDirty(uint64_t offset, uint64_t len)
{
    /* past the image, changes last only as long as the machine */
    if (offset >= fFileSize) {
        return;
    }
    if (offset + len > fFileSize) {
        len = fFileSize - offset;
    }
    uint64_t first = offset / CFI_SECTOR_SIZE;
    uint64_t last = (offset + len - 1) / CFI_SECTOR_SIZE;
    for (uint64_t i = first; i <= last; i++) {
        fDirty[i] = true;
    }
    WriteBack();
}


void CFIFlashDevice::WriteBack()
{
    uint64_t sectors = fDirty.size();
    uint64_t first = 0;

    while (!fWriteBackBusy) {
        while (first < sectors && !fDirty[first]) {
            first++;
        }
        if (first == sectors) {
            return;
        }
        uint64_t n = 0;
        while (first + n < sectors && fDirty[first + n] &&
               n < CFI_WRITE_BACK_MAX / CFI_SECTOR_SIZE) {
            fDirty[first + n] = false;
            n++;
        }
        memcpy(fWriteBack.get(), fArray + first * CFI_SECTOR_SIZE,
               n * CFI_SECTOR_SIZE);
        int ret = fBlock->WriteAsync(first, fWriteBack.get(), n,
                                     &fWriteBackDone);
        if (ret > 0) {
            fWriteBackBusy = true;
        } else if (ret < 0 && !fWriteBackFailed) {
            vm_error("%s: could not save what the guest wrote\n", Name());
            fWriteBackFailed = true;
        }
        first += n;
    }
}


void CFIFlashDevice::WriteBackComplete(int ret)
{
    fWriteBackBusy = false;
    if (ret < 0 && !fWriteBackFailed) {
        vm_error("%s: could not save what the guest wrote\n", Name());
        fWriteBackFailed = true;
    }
    WriteBack();
}


//#pragma mark - factory

Device *cfi_flash_node_create(const char *name,
                              std::unique_ptr<HostBlockDevice> bs,
                              int64_t base, uint32_t block_size,
                              bool read_only)
{
    return new CFIFlashDevice(name, std::move(bs), base, block_size,
                              read_only);
}
