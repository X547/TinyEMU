/*
 * ATAPI: SCSI commands over an ATA channel
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
#include <algorithm>
#include <memory>
#include <vector>

#include "ata.h"
#include "cutils.h"
#include "device_class.h"
#include "machine.h"
#include "scsi.h"

//#define DEBUG_ATAPI

/* A command packet is twelve bytes: no packet device takes sixteen. */
#define ATAPI_PACKET_SIZE 12

/* The most one command may move. ATAPI never says how much data a command
   has, so the unit is asked and its answer staged here in full; a guest
   asking for more than this is refused rather than given the memory. */
#define ATAPI_MAX_TRANSFER (16 << 20)

/* The interrupt reason, which is what the sector count register reads as
   while a packet command runs. */
#define ATAPI_IR_COD 0x01 /* the device wants the command packet */
#define ATAPI_IR_IO  0x02 /* data goes to the host */

/* Features register bits of the PACKET command. */
#define ATAPI_FEATURE_DMA 0x01
#define ATAPI_FEATURE_OVL 0x02

/* Nothing here is timed, so the fastest DMA mode is the one reported. */
#define ATAPI_DEFAULT_DMA (ATA_XFER_UDMA | 5)

#define ATAPI_FIRMWARE "1.0"
#define ATAPI_SERIAL   "TEMU00000000000000001"


class ATAPIDevice final: public ATADevice, public SCSIBusTarget,
                         public SCSICompletion {
private:
    enum class Phase {
        Idle,
        Identify, /* IDENTIFY PACKET DEVICE data, by PIO */
        Packet,   /* the host is writing the command packet */
        DataOut,  /* the host is writing data, by PIO */
        DataIn,   /* the host is reading data, by PIO */
        Dma,      /* the bus master is moving the data */
        Busy,     /* the unit has the command */
    };

    SCSIDevice *fUnit = nullptr;
    Phase fPhase = Phase::Idle;

    SCSIRequest fReq;
    /* The whole data of a command, padded to a whole number of words. */
    std::vector<uint8_t> fData;
    uint32_t fDataLen = 0;
    uint32_t fDataPos = 0;
    /* What one PIO block moves, which the host bounds in the cylinder
       registers when it issues the command. */
    uint16_t fByteLimit = 0;
    uint32_t fChunk = 0;
    bool fDma = false;

    uint8_t fDmaMode = ATAPI_DEFAULT_DMA;

    void Identify();
    void SetFeatures();
    void CommandDone();

    void StartPacket();
    void PacketReceived();
    void PioOutChunk();
    void PioInChunk();
    void SubmitCommand();
    void Finished();
    void EndCommand();
    void EndError(uint8_t error);
    void CancelCommand();

public:
    ATAPIDevice();

    void Reset() override;
    void SetSignature() override;
    void ExecCommand(uint8_t cmd) override;
    uint32_t DmaMove(uint8_t *mem, uint32_t len) override;
    void DmaComplete(bool ok) override;
    void BufferComplete() override;

    /* SCSIBusTarget */
    bool FindFreeAddress(int *target, int *lun) override;
    bool AttachDevice(SCSIDevice *dev, uint32_t target, uint32_t lun) override;

    /* SCSICompletion */
    void Complete(SCSIRequest *req) override;
};


ATAPIDevice::ATAPIDevice()
{
    Reset();
}


void ATAPIDevice::CancelCommand()
{
    if (fPhase == Phase::Busy && fUnit != nullptr) {
        fUnit->Cancel(&fReq);
    }
    fPhase = Phase::Idle;
}


void ATAPIDevice::Reset()
{
    CancelCommand();
    ATADevice::Reset();
}


/* What tells a packet device from a disk without issuing a command. */
void ATAPIDevice::SetSignature()
{
    fSelect &= 0xf0;
    fNsector = 1;
    fSector = 1;
    fLcyl = 0x14;
    fHcyl = 0xeb;
}


//#pragma mark - SCSI side

bool ATAPIDevice::FindFreeAddress(int *target, int *lun)
{
    if (*target < 0) {
        *target = 0;
    }
    if (*lun < 0) {
        *lun = 0;
    }
    return fUnit == nullptr;
}


bool ATAPIDevice::AttachDevice(SCSIDevice *dev, uint32_t target, uint32_t lun)
{
    /* A packet device is addressed as a whole: there is one unit behind it,
       and no way to name another. */
    if (target != 0 || lun != 0) {
        vm_error("atapi: has only one unit, at target 0 and LUN 0\n");
        return false;
    }
    if (fUnit != nullptr) {
        vm_error("atapi: already carries a %s\n", fUnit->Name());
        return false;
    }
    fUnit = dev;
    dev->SetLun(0);
    return true;
}


//#pragma mark - ATA commands

static const char *atapi_type_name(uint8_t type)
{
    switch (type) {
    case SCSI_TYPE_DISK:  return "Disk";
    case 0x01:            return "Tape";
    case SCSI_TYPE_CDROM: return "CD-ROM";
    case 0x07:            return "Optical";
    default:              return "Device";
    }
}


void ATAPIDevice::Identify()
{
    uint8_t *p = fBuffer;
    uint8_t type = fUnit->PeripheralType();
    char model[41];

    memset(p, 0, ATA_SECTOR_SIZE);
    snprintf(model, sizeof(model), "TinyEMU ATAPI %s", atapi_type_name(type));

    /* ATAPI, the unit's type, removable or not, DRQ within 50 us of the
       PACKET command, and twelve byte packets. */
    ata_put_word(p, 0, 0x8000 | ((type & 0x1f) << 8) |
                       (fUnit->Removable() ? 0x80 : 0) | (2 << 5));
    ata_put_string(p + 10 * 2, ATAPI_SERIAL, 20);
    ata_put_string(p + 23 * 2, ATAPI_FIRMWARE, 8);
    ata_put_string(p + 27 * 2, model, 40);
    ata_put_word(p, 49, (1 << 8) | (1 << 9)); /* DMA, LBA */
    /* Words 64 to 70 and 88 carry something. */
    ata_put_word(p, 53, (1 << 1) | (1 << 2));
    ata_put_dma_modes(p, fDmaMode); /* words 63 and 88 */
    ata_put_word(p, 64, 0x0003); /* PIO modes 3 and 4 */
    ata_put_word(p, 65, 120);
    ata_put_word(p, 66, 120);
    ata_put_word(p, 67, 120);
    ata_put_word(p, 68, 120);
    ata_put_word(p, 80, (1 << 4) | (1 << 5)); /* ATA/ATAPI-4 and -5 */
    /* Power management, PACKET, DEVICE RESET; all of them on. */
    ata_put_word(p, 82, (1 << 3) | (1 << 4) | (1 << 9));
    ata_put_word(p, 83, (1 << 14));
    ata_put_word(p, 84, (1 << 14));
    ata_put_word(p, 85, (1 << 3) | (1 << 4) | (1 << 9));
    ata_put_word(p, 87, (1 << 14));
    /* The 80 conductor cable, without which Ultra DMA stays at mode 2. */
    ata_put_word(p, 93, (1 << 0) | (1 << 13) | (1 << 14));
}


void ATAPIDevice::CommandDone()
{
    fStatus = ATA_STAT_READY | ATA_STAT_SEEK;
    fError = 0;
    RaiseIrq();
}


void ATAPIDevice::SetFeatures()
{
    if (fFeature == ATA_FEATURE_SET_TRANSFER) {
        uint8_t mode = fNsector & 0xff;
        if (!ata_transfer_mode_valid(mode)) {
            AbortCommand();
            RaiseIrq();
            return;
        }
        if (ata_transfer_mode_is_dma(mode)) {
            fDmaMode = mode;
        }
    }
    /* Everything else changes nothing here and is accepted. */
    CommandDone();
}


void ATAPIDevice::ExecCommand(uint8_t cmd)
{
#ifdef DEBUG_ATAPI
    printf("atapi: command 0x%02x\n", cmd);
#endif
    /* DEVICE RESET is how a host gets a stuck packet device back, so it is
       the one command taken while another runs. */
    if (cmd == ATA_CMD_DEVICE_RESET) {
        CancelCommand();
        if (fUnit != nullptr) {
            fUnit->Reset();
        }
        ATADevice::Reset();
        return;
    }
    if (fPhase != Phase::Idle && fPhase != Phase::Identify) {
        return;
    }
    fPhase = Phase::Idle;
    if (fUnit == nullptr) {
        AbortCommand();
        RaiseIrq();
        return;
    }

    switch (cmd) {
    case ATA_CMD_PACKET:
        StartPacket();
        break;

    case ATA_CMD_IDENTIFY_PACKET:
        Identify();
        fBufferPos = 0;
        fBufferEnd = ATA_SECTOR_SIZE;
        fPhase = Phase::Identify;
        fStatus = ATA_STAT_READY | ATA_STAT_SEEK | ATA_STAT_DRQ;
        fError = 0;
        RaiseIrq();
        break;

    case ATA_CMD_IDENTIFY:
        /* Refused, with the signature put back: that is how a host that
           tried the disk command first learns what it is talking to. */
        SetSignature();
        AbortCommand();
        RaiseIrq();
        break;

    case ATA_CMD_SETFEATURES:
        SetFeatures();
        break;

    case ATA_CMD_CHECKPOWERMODE:
        fNsector = 0xff; /* active */
        CommandDone();
        break;

    case ATA_CMD_IDLE:
    case ATA_CMD_IDLEIMMEDIATE:
    case ATA_CMD_STANDBY:
    case ATA_CMD_STANDBYNOW:
    case ATA_CMD_SLEEP:
        CommandDone();
        break;

    case ATA_CMD_DIAGNOSE:
        fError = 0x01; /* passed */
        SetSignature();
        fStatus = ATA_STAT_READY | ATA_STAT_SEEK;
        RaiseIrq();
        break;

    default:
        AbortCommand();
        RaiseIrq();
        break;
    }
}


//#pragma mark - packet commands

void ATAPIDevice::StartPacket()
{
    if ((fFeature & ATAPI_FEATURE_OVL) != 0) {
        /* Overlapped commands are not offered. */
        AbortCommand();
        RaiseIrq();
        return;
    }
    fDma = (fFeature & ATAPI_FEATURE_DMA) != 0;

    /* A block must be a whole number of words unless it is the last; a limit
       of zero is not allowed, and is taken as the largest there is. */
    uint16_t limit = fLcyl | (fHcyl << 8);
    fByteLimit = limit == 0 ? 0xfffe : (limit & ~1);
    if (fByteLimit == 0) {
        fByteLimit = 2;
    }

    fPhase = Phase::Packet;
    fBufferPos = 0;
    fBufferEnd = ATAPI_PACKET_SIZE;
    fNsector = ATAPI_IR_COD;
    /* The drive asks for the packet within 50 us, so the host polls for DRQ
       and there is no interrupt. */
    fStatus = ATA_STAT_READY | ATA_STAT_SEEK | ATA_STAT_DRQ;
    fError = 0;
}


void ATAPIDevice::PacketReceived()
{
    SCSIDirEnum dir;
    uint32_t len;

    fReq = SCSIRequest();
    memcpy(fReq.cdb, fBuffer, ATAPI_PACKET_SIZE);
    fReq.cdb_len = ATAPI_PACKET_SIZE;
    fBufferPos = 0;
    fBufferEnd = 0;
#ifdef DEBUG_ATAPI
    printf("atapi: packet %02x %02x %02x %02x %02x %02x %02x %02x %02x %02x "
           "%02x %02x%s\n", fReq.cdb[0], fReq.cdb[1], fReq.cdb[2],
           fReq.cdb[3], fReq.cdb[4], fReq.cdb[5], fReq.cdb[6], fReq.cdb[7],
           fReq.cdb[8], fReq.cdb[9], fReq.cdb[10], fReq.cdb[11],
           fDma ? " dma" : "");
#endif

    /* The packet says nothing about its data, so the unit says how much
       there is and which way it goes. A command it does not know goes in
       with none, for it to refuse. */
    if (!fUnit->DataPhase(fReq.cdb, &dir, &len) || dir == SCSI_DIR_NONE) {
        dir = SCSI_DIR_NONE;
        len = 0;
    }
    if (len > ATAPI_MAX_TRANSFER) {
        if (dir == SCSI_DIR_TO_DEV) {
            EndError((SCSI_SENSE_ILLEGAL_REQUEST << 4) | ATA_ERR_ABRT);
            return;
        }
        /* The unit refuses a read that does not fit, and cuts any other
           reply to it. */
        len = ATAPI_MAX_TRANSFER;
    }

    fData.resize(len + (len & 1));
    fReq.lun = fUnit->Lun();
    fReq.dir = dir;
    fReq.buf = fData.data();
    fReq.buf_len = len;
    fReq.completion = this;
    fDataLen = len;
    fDataPos = 0;

    if (dir == SCSI_DIR_TO_DEV && len > 0) {
        /* The data goes in first, then the unit has the command. */
        if (fDma) {
            fPhase = Phase::Dma;
            fStatus = ATA_STAT_READY | ATA_STAT_SEEK | ATA_STAT_DRQ |
                      ATA_STAT_BUSY;
            RequestDma(false);
        } else {
            fPhase = Phase::DataOut;
            PioOutChunk();
        }
        return;
    }
    SubmitCommand();
}


void ATAPIDevice::PioOutChunk()
{
    fChunk = std::min<uint32_t>(fDataLen - fDataPos, fByteLimit);
    fBufferPos = 0;
    fBufferEnd = (fChunk + 1) & ~1;
    fLcyl = fChunk & 0xff;
    fHcyl = fChunk >> 8;
    fNsector = 0;
    fStatus = ATA_STAT_READY | ATA_STAT_SEEK | ATA_STAT_DRQ;
    fError = 0;
    RaiseIrq();
}


void ATAPIDevice::PioInChunk()
{
    fChunk = std::min<uint32_t>(fDataLen - fDataPos, fByteLimit);
    /* An odd last block is read as whole words; the pad is the byte after
       the data, which the staging buffer keeps for this. */
    memcpy(fBuffer, fData.data() + fDataPos, (fChunk + 1) & ~1);
    fBufferPos = 0;
    fBufferEnd = (fChunk + 1) & ~1;
    fLcyl = fChunk & 0xff;
    fHcyl = fChunk >> 8;
    fNsector = ATAPI_IR_IO;
    fStatus = ATA_STAT_READY | ATA_STAT_SEEK | ATA_STAT_DRQ;
    fError = 0;
    RaiseIrq();
}


void ATAPIDevice::SubmitCommand()
{
    fPhase = Phase::Busy;
    fStatus = ATA_STAT_READY | ATA_STAT_SEEK | ATA_STAT_BUSY;
    fError = 0;
    if (fUnit->Submit(&fReq)) {
        Finished();
    }
}


void ATAPIDevice::Complete(SCSIRequest *req)
{
    (void)req;
    Finished();
}


/* The unit is done with the command: what it read goes to the host, or the
   status does. */
void ATAPIDevice::Finished()
{
    fPhase = Phase::Idle;

    if (fReq.phase_error) {
        EndError(ATA_ERR_ABRT);
        return;
    }
    if (fReq.status != SCSI_STATUS_GOOD) {
        /* The sense key goes in the top of the error register; the rest
           of the sense data is for REQUEST SENSE. */
        uint8_t key = fReq.sense_len > 0 ? fReq.sense[2] & 0x0f
                                         : SCSI_SENSE_ABORTED_COMMAND;
        EndError(key << 4);
        return;
    }
    if (fReq.dir == SCSI_DIR_FROM_DEV && fReq.actual_length > 0) {
        fDataLen = fReq.actual_length;
        fDataPos = 0;
        if (fDma) {
            fPhase = Phase::Dma;
            fStatus = ATA_STAT_READY | ATA_STAT_SEEK | ATA_STAT_DRQ |
                      ATA_STAT_BUSY;
            /* Last: if the engine is already running this moves the data
               and ends the command before it returns. */
            RequestDma(true);
        } else {
            fPhase = Phase::DataIn;
            PioInChunk();
        }
        return;
    }
    EndCommand();
}


void ATAPIDevice::EndCommand()
{
    fPhase = Phase::Idle;
    fBufferPos = 0;
    fBufferEnd = 0;
    fNsector = ATAPI_IR_IO | ATAPI_IR_COD;
    fStatus = ATA_STAT_READY | ATA_STAT_SEEK;
    fError = 0;
    RaiseIrq();
}


void ATAPIDevice::EndError(uint8_t error)
{
    fPhase = Phase::Idle;
    fBufferPos = 0;
    fBufferEnd = 0;
    fDmaPending = false;
    fNsector = ATAPI_IR_IO | ATAPI_IR_COD;
    fStatus = ATA_STAT_READY | ATA_STAT_SEEK | ATA_STAT_ERR;
    fError = error;
    RaiseIrq();
}


void ATAPIDevice::BufferComplete()
{
    switch (fPhase) {
    case Phase::Identify:
        fPhase = Phase::Idle;
        fBufferPos = 0;
        fBufferEnd = 0;
        fStatus = ATA_STAT_READY | ATA_STAT_SEEK;
        break;

    case Phase::Packet:
        PacketReceived();
        break;

    case Phase::DataOut:
        memcpy(fData.data() + fDataPos, fBuffer, fChunk);
        fDataPos += fChunk;
        if (fDataPos < fDataLen) {
            PioOutChunk();
        } else {
            SubmitCommand();
        }
        break;

    case Phase::DataIn:
        fDataPos += fChunk;
        if (fDataPos < fDataLen) {
            PioInChunk();
        } else {
            EndCommand();
        }
        break;

    default:
        fBufferPos = 0;
        fBufferEnd = 0;
        break;
    }
}


//#pragma mark - DMA

uint32_t ATAPIDevice::DmaMove(uint8_t *mem, uint32_t len)
{
    uint32_t n = std::min(fDataLen - fDataPos, len);

    if (fDmaToMemory) {
        memcpy(mem, fData.data() + fDataPos, n);
    } else {
        memcpy(fData.data() + fDataPos, mem, n);
    }
    fDataPos += n;
    return n;
}


void ATAPIDevice::DmaComplete(bool ok)
{
    fDmaPending = false;
    if (fPhase != Phase::Dma) {
        return;
    }
    if (!ok || fDataPos < fDataLen) {
        /* The scatter list ran out before the data did, or something on
           the way failed. */
        EndError(ATA_ERR_ABRT);
    } else if (!fDmaToMemory) {
        SubmitCommand();
    } else {
        EndCommand();
    }
}


//#pragma mark - class

/* A packet device: a bridge that carries the SCSI commands of the one unit on
   the SCSI bus it provides, a CD drive or anything else. */
class ATAPIClass final: public DeviceClass {
public:
    ATAPIClass(): DeviceClass("atapi") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        (void)ctx;
        if (!cfg.HasChildren()) {
            vm_error("atapi: needs a nested SCSI bus with a device on it\n");
            return nullptr;
        }
        auto dev = std::make_unique<ATAPIDevice>();
        ATAPIDevice *atapi = dev.get();
        ATADeviceNode *node = new ATADeviceNode("atapi", std::move(dev));

        node->SetChildBus(std::make_unique<SCSIBus>(node, atapi));
        return node;
    }
};

static const ATAPIClass sATAPIClass;
