/*
 * ATA disk
 *
 * Copyright (c) 2003-2016 Fabrice Bellard
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
#include <string.h>

#include "ata.h"
#include "cutils.h"
#include "virtio.h"

//#define DEBUG_ATA

/* What a guest sees in the IDENTIFY strings. */
#define ATA_DISK_MODEL    "TinyEMU Hard Disk"
#define ATA_DISK_FIRMWARE "1.0"

/* The geometry every disk reports. Cylinders follow the size; the other two
   are what a translating BIOS expects to find and what every emulated disk
   has claimed since the ATA-1 days. */
#define ATA_DISK_HEADS   16
#define ATA_DISK_SECTORS 63

/* CHS cannot name a cylinder past this, so a disk larger than the geometry
   can describe reports the limit and is reached through LBA instead. */
#define ATA_DISK_MAX_CYLINDERS 16383

/* Addressing is 28 bit, so this many sectors is the most a command can
   name. Anything past it is refused rather than wrapped. */
#define ATA_DISK_MAX_LBA28 ((int64_t)1 << 28)

/* The mode IDENTIFY reports as selected until a guest picks another. A
   driver that finds none selected takes the drive for one that will not
   answer DMA commands and stays on PIO. */
#define ATA_DISK_DEFAULT_DMA (ATA_XFER_UDMA | 5)


class ATADiskDevice final: public ATADevice {
private:
    BlockDevice *fBs;
    bool fReadOnly;
    int64_t fSectorCount = 0;
    int fCylinders = 0;

    /* How many sectors READ/WRITE MULTIPLE moves per interrupt, and how many
       the command in progress is moving right now. */
    int fMultSectors = 0;
    int fReqSectors = 0;
    int fIoSectors = 0;

    /* What to do once the guest has drained or filled the PIO buffer. */
    enum class OnBufferEnd {
        Stop,
        ReadNext,
        ReadDone,
        Write,
    };
    OnBufferEnd fOnEnd = OnBufferEnd::Stop;

    /* DMA in progress: where it is and how much is left. A partial sector
       is held in fDmaSector between calls, because the scatter list may
       split one anywhere. */
    int64_t fDmaLba = 0;
    int fDmaSectorsLeft = 0;
    uint8_t fDmaSector[ATA_SECTOR_SIZE] {};
    int fDmaSectorPos = 0;

    /* The DMA mode SET FEATURES selected, reported back in IDENTIFY. The PIO
       mode is chosen separately and reported nowhere, so it is not kept.
       Nothing here is timed, so the choice changes nothing else. */
    uint8_t fDmaMode = ATA_DISK_DEFAULT_DMA;

    int64_t CurrentLba() const;
    void SetCurrentLba(int64_t lba);
    bool LbaInRange(int64_t lba, int count) const;

    void Identify();
    void PioReadStart();
    void PioReadDone();
    void PioWriteStart();
    void PioWriteFlush();
    bool DmaStart(bool to_memory);
    void SetFeatures();
    void CommandDone(bool irq = true);

public:
    ATADiskDevice(BlockDevice *bs, bool read_only);

    void ExecCommand(uint8_t cmd) override;
    void Reset() override;

    uint32_t DmaMove(uint8_t *mem, uint32_t len) override;
    void DmaComplete(bool ok) override;
    void BufferComplete() override;
};


ATADiskDevice::ATADiskDevice(BlockDevice *bs, bool read_only):
    fBs(bs), fReadOnly(read_only)
{
    fSectorCount = fBs->SectorCount();

    int64_t cylinders = fSectorCount / (ATA_DISK_HEADS * ATA_DISK_SECTORS);
    if (cylinders > ATA_DISK_MAX_CYLINDERS) {
        cylinders = ATA_DISK_MAX_CYLINDERS;
    } else if (cylinders < 2) {
        cylinders = 2;
    }
    fCylinders = (int)cylinders;

    /* Multiple mode is off until a guest asks for it, which is what a drive
       reports at power on. */
    fMultSectors = 0;

    Reset();
}


void ATADiskDevice::Reset()
{
    ATADevice::Reset();
    fOnEnd = OnBufferEnd::Stop;
    fReqSectors = 0;
    fIoSectors = 0;
    fDmaSectorsLeft = 0;
    fDmaSectorPos = 0;
}


void ATADiskDevice::CommandDone(bool irq)
{
    fStatus = ATA_STAT_READY | ATA_STAT_SEEK;
    fError = 0;
    if (irq) {
        RaiseIrq();
    }
}


//#pragma mark - addressing

int64_t ATADiskDevice::CurrentLba() const
{
    if (fSelect & 0x40) {
        return ((int64_t)(fSelect & 0x0f) << 24) | ((int64_t)fHcyl << 16) |
               ((int64_t)fLcyl << 8) | fSector;
    }
    /* CHS, which a guest still uses while a BIOS is in charge. */
    return (int64_t)(((fHcyl << 8) | fLcyl) * ATA_DISK_HEADS +
                     (fSelect & 0x0f)) * ATA_DISK_SECTORS +
           (fSector - 1);
}


void ATADiskDevice::SetCurrentLba(int64_t lba)
{
    if (fSelect & 0x40) {
        fSelect = (fSelect & 0xf0) | ((lba >> 24) & 0x0f);
        fHcyl = (lba >> 16) & 0xff;
        fLcyl = (lba >> 8) & 0xff;
        fSector = lba & 0xff;
    } else {
        int64_t cyl = lba / (ATA_DISK_HEADS * ATA_DISK_SECTORS);
        int64_t r = lba % (ATA_DISK_HEADS * ATA_DISK_SECTORS);
        fHcyl = (cyl >> 8) & 0xff;
        fLcyl = cyl & 0xff;
        fSelect = (fSelect & 0xf0) | ((r / ATA_DISK_SECTORS) & 0x0f);
        fSector = (r % ATA_DISK_SECTORS) + 1;
    }
}


/* A command that runs off the end of the disk is refused. The back end does
   not check, and a DMA transfer that overran would write whatever the buffer
   last held into guest memory. */
bool ATADiskDevice::LbaInRange(int64_t lba, int count) const
{
    return lba >= 0 && count >= 0 && lba + count <= fSectorCount;
}


//#pragma mark - IDENTIFY

static void ata_put_string(uint8_t *buf, const char *src, int len)
{
    /* ATA strings travel with the two bytes of each word swapped. */
    for (int i = 0; i < len; i++) {
        char c = *src != '\0' ? *src++ : ' ';
        buf[i ^ 1] = c;
    }
}


static void ata_put_word(uint8_t *buf, int index, uint16_t val)
{
    buf[index * 2] = val & 0xff;
    buf[index * 2 + 1] = val >> 8;
}


void ATADiskDevice::Identify()
{
    uint8_t *p = fBuffer;
    uint32_t chs_sectors = (uint32_t)fCylinders * ATA_DISK_HEADS *
                           ATA_DISK_SECTORS;
    /* Only what 28 bit addressing can reach is reported. */
    uint32_t lba_sectors = (uint32_t)(fSectorCount < ATA_DISK_MAX_LBA28
                                      ? fSectorCount : ATA_DISK_MAX_LBA28 - 1);

    memset(p, 0, ATA_SECTOR_SIZE);

    ata_put_word(p, 0, 0x0040); /* fixed device */
    ata_put_word(p, 1, fCylinders);
    ata_put_word(p, 3, ATA_DISK_HEADS);
    ata_put_word(p, 4, ATA_SECTOR_SIZE * ATA_DISK_SECTORS);
    ata_put_word(p, 5, ATA_SECTOR_SIZE);
    ata_put_word(p, 6, ATA_DISK_SECTORS);
    ata_put_string(p + 10 * 2, "TEMU00000000000000000", 20); /* serial */
    ata_put_word(p, 20, 3); /* buffer type */
    ata_put_word(p, 21, 512); /* cache size in sectors */
    ata_put_word(p, 22, 4); /* ecc bytes */
    ata_put_string(p + 23 * 2, ATA_DISK_FIRMWARE, 8);
    ata_put_string(p + 27 * 2, ATA_DISK_MODEL, 40);
    ata_put_word(p, 47, 0x8000 | ATA_MAX_MULT_SECTORS);
    ata_put_word(p, 48, 0); /* dword I/O */
    /* Bit 8 says DMA is there and bit 9 says LBA is. */
    ata_put_word(p, 49, (1 << 8) | (1 << 9));
    ata_put_word(p, 51, 0x200); /* PIO cycle time */
    ata_put_word(p, 52, 0x200); /* DMA cycle time */
    /* Words 54 to 58 and 64 to 70 and 88 carry something, which is what
       these three bits say; without them a driver must ignore them all. */
    ata_put_word(p, 53, (1 << 0) | (1 << 1) | (1 << 2));
    ata_put_word(p, 54, fCylinders);
    ata_put_word(p, 55, ATA_DISK_HEADS);
    ata_put_word(p, 56, ATA_DISK_SECTORS);
    ata_put_word(p, 57, chs_sectors);
    ata_put_word(p, 58, chs_sectors >> 16);
    if (fMultSectors != 0) {
        ata_put_word(p, 59, 0x100 | fMultSectors);
    }
    ata_put_word(p, 60, lba_sectors);
    ata_put_word(p, 61, lba_sectors >> 16);

    /* Multiword DMA: the modes there are, and the one selected. Only one of
       this and word 88 ever carries a selection. */
    uint16_t mwdma = ATA_MWDMA_MODES;
    if ((fDmaMode & ~0x07) == ATA_XFER_MWDMA) {
        mwdma |= 1 << (8 + (fDmaMode & 0x07));
    }
    ata_put_word(p, 63, mwdma);
    ata_put_word(p, 64, 0x0003); /* PIO modes 3 and 4 */
    ata_put_word(p, 65, 120);    /* minimum multiword DMA cycle time */
    ata_put_word(p, 66, 120);
    ata_put_word(p, 67, 120);    /* minimum PIO cycle time */
    ata_put_word(p, 68, 120);

    /* ATA-1 to ATA-5; the Ultra DMA modes past 2 that word 88 offers arrived
       in ATA-5. */
    ata_put_word(p, 80, (1 << 1) | (1 << 2) | (1 << 3) | (1 << 4) | (1 << 5));
    /* Power management, write caching, read look ahead, NOP. Nothing claims
       WRITE BUFFER (bit 12): a claimed command that then aborts reads as a
       fault. */
    ata_put_word(p, 82, (1 << 3) | (1 << 5) | (1 << 6) | (1 << 14));
    ata_put_word(p, 83, (1 << 14) | (1 << 12)); /* FLUSH CACHE */
    ata_put_word(p, 84, (1 << 14));
    /* Words 85 to 87 say which of those are turned on, which is all of
       them. */
    ata_put_word(p, 85, (1 << 3) | (1 << 5) | (1 << 6) | (1 << 14));
    ata_put_word(p, 86, (1 << 12));
    ata_put_word(p, 87, (1 << 14));

    /* Ultra DMA, likewise. */
    uint16_t udma = ATA_UDMA_MODES;
    if ((fDmaMode & ~0x07) == ATA_XFER_UDMA) {
        udma |= 1 << (8 + (fDmaMode & 0x07));
    }
    ata_put_word(p, 88, udma);

    /* Device 0 passed, and the cable is the 80 conductor one. Without the
       cable bit a driver holds Ultra DMA to mode 2. */
    ata_put_word(p, 93, (1 << 0) | (1 << 13) | (1 << 14));
}


//#pragma mark - PIO

void ATADiskDevice::PioReadStart()
{
    int64_t lba = CurrentLba();
    int n = fNsector != 0 ? fNsector : 256;

    if (n > fReqSectors) {
        n = fReqSectors;
    }
    if (!LbaInRange(lba, n)) {
        fStatus = ATA_STAT_READY | ATA_STAT_ERR;
        fError = ATA_ERR_ID;
        RaiseIrq();
        return;
    }
#ifdef DEBUG_ATA
    printf("ata: pio read lba=%lld count=%d\n", (long long)lba, n);
#endif
    if (fBs->ReadAsync(lba, fBuffer, n, nullptr) < 0) {
        AbortCommand();
        RaiseIrq();
        return;
    }

    fIoSectors = n;
    SetCurrentLba(lba + n);
    fNsector = (fNsector - n) & 0xff;
    fOnEnd = fNsector == 0 ? OnBufferEnd::ReadDone : OnBufferEnd::ReadNext;
    fBufferPos = 0;
    fBufferEnd = n * ATA_SECTOR_SIZE;
    fStatus = ATA_STAT_READY | ATA_STAT_SEEK | ATA_STAT_DRQ;
    fError = 0;
    RaiseIrq();
}


void ATADiskDevice::PioReadDone()
{
    fBufferPos = 0;
    fBufferEnd = 0;
    fOnEnd = OnBufferEnd::Stop;
    fStatus = ATA_STAT_READY | ATA_STAT_SEEK;
    fError = 0;
}


void ATADiskDevice::PioWriteStart()
{
    int n = fNsector != 0 ? fNsector : 256;

    if (n > fReqSectors) {
        n = fReqSectors;
    }
    if (!LbaInRange(CurrentLba(), n)) {
        fStatus = ATA_STAT_READY | ATA_STAT_ERR;
        fError = ATA_ERR_ID;
        RaiseIrq();
        return;
    }
    fIoSectors = n;
    fOnEnd = OnBufferEnd::Write;
    fBufferPos = 0;
    fBufferEnd = n * ATA_SECTOR_SIZE;
    /* The first block is asked for without an interrupt; the guest knows to
       start writing because DRQ is up. */
    fStatus = ATA_STAT_READY | ATA_STAT_SEEK | ATA_STAT_DRQ;
}


void ATADiskDevice::PioWriteFlush()
{
    int64_t lba = CurrentLba();
    int n = fIoSectors;

    fBufferPos = 0;
    fBufferEnd = 0;
#ifdef DEBUG_ATA
    printf("ata: pio write lba=%lld count=%d\n", (long long)lba, n);
#endif
    if (fReadOnly || fBs->WriteAsync(lba, fBuffer, n, nullptr) < 0) {
        fStatus = ATA_STAT_READY | ATA_STAT_ERR;
        fError = fReadOnly ? ATA_ERR_ABRT : ATA_ERR_ECC;
        fOnEnd = OnBufferEnd::Stop;
        RaiseIrq();
        return;
    }

    SetCurrentLba(lba + n);
    fNsector = (fNsector - n) & 0xff;
    if (fNsector == 0) {
        fOnEnd = OnBufferEnd::Stop;
        fStatus = ATA_STAT_READY | ATA_STAT_SEEK;
    } else {
        n = fNsector;
        if (n > fReqSectors) {
            n = fReqSectors;
        }
        fIoSectors = n;
        fBufferEnd = n * ATA_SECTOR_SIZE;
        fStatus = ATA_STAT_READY | ATA_STAT_SEEK | ATA_STAT_DRQ;
    }
    fError = 0;
    RaiseIrq();
}


void ATADiskDevice::BufferComplete()
{
    switch (fOnEnd) {
    case OnBufferEnd::ReadNext:
        PioReadStart();
        break;
    case OnBufferEnd::ReadDone:
        PioReadDone();
        break;
    case OnBufferEnd::Write:
        PioWriteFlush();
        break;
    case OnBufferEnd::Stop:
        fBufferPos = 0;
        fBufferEnd = 0;
        fStatus = ATA_STAT_READY;
        break;
    }
}


//#pragma mark - DMA

bool ATADiskDevice::DmaStart(bool to_memory)
{
    int64_t lba = CurrentLba();
    int n = fNsector != 0 ? fNsector : 256;

    if (!LbaInRange(lba, n) || (!to_memory && fReadOnly)) {
        fStatus = ATA_STAT_READY | ATA_STAT_ERR;
        fError = !to_memory && fReadOnly ? ATA_ERR_ABRT : ATA_ERR_ID;
        RaiseIrq();
        return false;
    }
#ifdef DEBUG_ATA
    printf("ata: dma %s lba=%lld count=%d\n", to_memory ? "read" : "write",
           (long long)lba, n);
#endif
    fDmaLba = lba;
    fDmaSectorsLeft = n;
    fDmaSectorPos = 0;
    /* Busy until the bus master has moved it all. There is no interrupt
       yet: the one that ends the command comes from DmaComplete(). */
    fStatus = ATA_STAT_READY | ATA_STAT_SEEK | ATA_STAT_DRQ | ATA_STAT_BUSY;
    fError = 0;
    /* Last: if the engine is already running this moves the data and
       finishes the command before it returns. */
    RequestDma(to_memory);
    return true;
}


uint32_t ATADiskDevice::DmaMove(uint8_t *mem, uint32_t len)
{
    uint32_t moved = 0;

    while (moved < len && fDmaSectorsLeft > 0) {
        /* Whole sectors go straight between the disk and guest memory; only
           a scatter list that splits one needs the staging buffer. */
        if (fDmaSectorPos == 0) {
            uint32_t whole = (len - moved) / ATA_SECTOR_SIZE;
            if (whole > (uint32_t)fDmaSectorsLeft) {
                whole = fDmaSectorsLeft;
            }
            if (whole > 0) {
                int ret;
                if (fDmaToMemory) {
                    ret = fBs->ReadAsync(fDmaLba, mem + moved, whole, nullptr);
                } else {
                    ret = fBs->WriteAsync(fDmaLba, mem + moved, whole,
                                          nullptr);
                }
                if (ret < 0) {
                    break;
                }
                fDmaLba += whole;
                fDmaSectorsLeft -= whole;
                moved += whole * ATA_SECTOR_SIZE;
                continue;
            }
        }

        if (fDmaToMemory) {
            if (fDmaSectorPos == 0 &&
                fBs->ReadAsync(fDmaLba, fDmaSector, 1, nullptr) < 0) {
                break;
            }
            uint32_t n = ATA_SECTOR_SIZE - fDmaSectorPos;
            if (n > len - moved) {
                n = len - moved;
            }
            memcpy(mem + moved, fDmaSector + fDmaSectorPos, n);
            fDmaSectorPos += n;
            moved += n;
        } else {
            uint32_t n = ATA_SECTOR_SIZE - fDmaSectorPos;
            if (n > len - moved) {
                n = len - moved;
            }
            memcpy(fDmaSector + fDmaSectorPos, mem + moved, n);
            fDmaSectorPos += n;
            moved += n;
        }

        if (fDmaSectorPos == ATA_SECTOR_SIZE) {
            if (!fDmaToMemory &&
                fBs->WriteAsync(fDmaLba, fDmaSector, 1, nullptr) < 0) {
                break;
            }
            fDmaSectorPos = 0;
            fDmaLba++;
            fDmaSectorsLeft--;
        }
    }
    return moved;
}


void ATADiskDevice::DmaComplete(bool ok)
{
    fDmaPending = false;
    if (!ok || fDmaSectorsLeft > 0) {
        /* The scatter list ran out before the command did, or something on
           the way failed. Either way the command did not finish. */
        fStatus = ATA_STAT_READY | ATA_STAT_SEEK | ATA_STAT_ERR;
        fError = ATA_ERR_ABRT;
    } else {
        SetCurrentLba(fDmaLba);
        fNsector = 0;
        fStatus = ATA_STAT_READY | ATA_STAT_SEEK;
        fError = 0;
    }
    fDmaSectorsLeft = 0;
    fDmaSectorPos = 0;
    RaiseIrq();
}


//#pragma mark - commands

void ATADiskDevice::SetFeatures()
{
    switch (fFeature) {
    case ATA_FEATURE_SET_TRANSFER: {
        uint8_t mode = fNsector & 0xff;
        uint8_t cls = mode & ~0x07;
        uint8_t index = mode & 0x07;
        bool ok;
        switch (cls) {
        case ATA_XFER_PIO_SLOW: ok = index <= 1; break;
        case ATA_XFER_PIO:      ok = index <= 4; break;
        case ATA_XFER_MWDMA:    ok = (ATA_MWDMA_MODES >> index) & 1; break;
        case ATA_XFER_UDMA:     ok = (ATA_UDMA_MODES >> index) & 1; break;
        default:                ok = false; break;
        }
        if (!ok) {
            AbortCommand();
            RaiseIrq();
            return;
        }
        if (cls == ATA_XFER_MWDMA || cls == ATA_XFER_UDMA) {
            /* A guest selects a PIO mode and a DMA one in turn, so a PIO
               mode must not displace the DMA mode IDENTIFY reports. */
            fDmaMode = mode;
        }
        break;
    }
    default:
        /* Everything else a guest asks to turn on or off -- write caching,
           read look ahead, defect reassignment -- changes nothing here, so
           it is accepted rather than refused. */
        break;
    }
    CommandDone();
}


void ATADiskDevice::ExecCommand(uint8_t cmd)
{
#ifdef DEBUG_ATA
    printf("ata: command 0x%02x\n", cmd);
#endif
    switch (cmd) {
    case ATA_CMD_IDENTIFY:
        Identify();
        fBufferPos = 0;
        fBufferEnd = ATA_SECTOR_SIZE;
        fOnEnd = OnBufferEnd::Stop;
        fStatus = ATA_STAT_READY | ATA_STAT_SEEK | ATA_STAT_DRQ;
        fError = 0;
        RaiseIrq();
        break;

    case ATA_CMD_SPECIFY:
    case ATA_CMD_RECALIBRATE:
    case ATA_CMD_SEEK:
    case ATA_CMD_IDLE:
    case ATA_CMD_IDLEIMMEDIATE:
    case ATA_CMD_STANDBY:
    case ATA_CMD_STANDBYNOW:
    case ATA_CMD_SLEEP:
    case ATA_CMD_FLUSH_CACHE:
        /* Nothing here is cached or spun, so these are all a state change
           the model does not have. They still have to succeed: a guest that
           cannot flush treats the disk as broken. */
        CommandDone();
        break;

    case ATA_CMD_CHECKPOWERMODE:
        fNsector = 0xff; /* active */
        CommandDone();
        break;

    case ATA_CMD_DIAGNOSE:
        fError = 0x01; /* device 0 passed, device 1 passed or absent */
        SetSignature();
        fStatus = ATA_STAT_READY | ATA_STAT_SEEK;
        RaiseIrq();
        break;

    case ATA_CMD_SETFEATURES:
        SetFeatures();
        break;

    case ATA_CMD_SETMULT:
        /* Zero turns multiple mode off again; anything else has to be a
           power of two the buffer holds. */
        if (fNsector > ATA_MAX_MULT_SECTORS ||
            (fNsector & (fNsector - 1)) != 0) {
            AbortCommand();
            RaiseIrq();
        } else {
            fMultSectors = fNsector;
            CommandDone();
        }
        break;

    case ATA_CMD_VERIFY:
    case ATA_CMD_VERIFY_ONCE: {
        int n = fNsector != 0 ? fNsector : 256;
        if (!LbaInRange(CurrentLba(), n)) {
            fStatus = ATA_STAT_READY | ATA_STAT_ERR;
            fError = ATA_ERR_ID;
            RaiseIrq();
        } else {
            SetCurrentLba(CurrentLba() + n - 1);
            fNsector = 0;
            CommandDone();
        }
        break;
    }

    case ATA_CMD_READ:
    case ATA_CMD_READ_ONCE:
        fReqSectors = 1;
        PioReadStart();
        break;

    case ATA_CMD_WRITE:
    case ATA_CMD_WRITE_ONCE:
        fReqSectors = 1;
        PioWriteStart();
        break;

    case ATA_CMD_MULTREAD:
        if (fMultSectors == 0) {
            AbortCommand();
            RaiseIrq();
        } else {
            fReqSectors = fMultSectors;
            PioReadStart();
        }
        break;

    case ATA_CMD_MULTWRITE:
        if (fMultSectors == 0) {
            AbortCommand();
            RaiseIrq();
        } else {
            fReqSectors = fMultSectors;
            PioWriteStart();
        }
        break;

    case ATA_CMD_READDMA:
    case ATA_CMD_READDMA_ONCE:
        DmaStart(true);
        break;

    case ATA_CMD_WRITEDMA:
    case ATA_CMD_WRITEDMA_ONCE:
        DmaStart(false);
        break;

    case ATA_CMD_READ_NATIVE_MAX:
        SetCurrentLba((fSectorCount < ATA_DISK_MAX_LBA28 ? fSectorCount
                                                         : ATA_DISK_MAX_LBA28)
                      - 1);
        CommandDone();
        break;

    default:
        AbortCommand();
        RaiseIrq();
        break;
    }
}


//#pragma mark - factory

ATADevice *ata_disk_create(BlockDevice *bs, bool read_only)
{
    return new ATADiskDevice(bs, read_only);
}


Device *ata_disk_node_create(BlockDevice *bs, bool read_only)
{
    return new ATADeviceNode("ata-disk", ata_disk_create(bs, read_only));
}
