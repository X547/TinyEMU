/*
 * IDE emulation
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include "cutils.h"
#include "ide.h"

//#define DEBUG_IDE

/* Bits of HD_STATUS */
#define ERR_STAT		0x01
#define INDEX_STAT		0x02
#define ECC_STAT		0x04	/* Corrected error */
#define DRQ_STAT		0x08
#define SEEK_STAT		0x10
#define SRV_STAT		0x10
#define WRERR_STAT		0x20
#define READY_STAT		0x40
#define BUSY_STAT		0x80

/* Bits for HD_ERROR */
#define MARK_ERR		0x01	/* Bad address mark */
#define TRK0_ERR		0x02	/* couldn't find track 0 */
#define ABRT_ERR		0x04	/* Command aborted */
#define MCR_ERR			0x08	/* media change request */
#define ID_ERR			0x10	/* ID field not found */
#define MC_ERR			0x20	/* media changed */
#define ECC_ERR			0x40	/* Uncorrectable ECC error */
#define BBD_ERR			0x80	/* pre-EIDE meaning:  block marked bad */
#define ICRC_ERR		0x80	/* new meaning:  CRC error during transfer */

/* Bits of HD_NSECTOR */
#define CD			0x01
#define IO			0x02
#define REL			0x04
#define TAG_MASK		0xf8

#define IDE_CMD_RESET           0x04
#define IDE_CMD_DISABLE_IRQ     0x02

/* ATA/ATAPI Commands pre T13 Spec */
#define WIN_NOP				0x00
/*
 *	0x01->0x02 Reserved
 */
#define CFA_REQ_EXT_ERROR_CODE		0x03 /* CFA Request Extended Error Code */
/*
 *	0x04->0x07 Reserved
 */
#define WIN_SRST			0x08 /* ATAPI soft reset command */
#define WIN_DEVICE_RESET		0x08
/*
 *	0x09->0x0F Reserved
 */
#define WIN_RECAL			0x10
#define WIN_RESTORE			WIN_RECAL
/*
 *	0x10->0x1F Reserved
 */
#define WIN_READ			0x20 /* 28-Bit */
#define WIN_READ_ONCE			0x21 /* 28-Bit without retries */
#define WIN_READ_LONG			0x22 /* 28-Bit */
#define WIN_READ_LONG_ONCE		0x23 /* 28-Bit without retries */
#define WIN_READ_EXT			0x24 /* 48-Bit */
#define WIN_READDMA_EXT			0x25 /* 48-Bit */
#define WIN_READDMA_QUEUED_EXT		0x26 /* 48-Bit */
#define WIN_READ_NATIVE_MAX_EXT		0x27 /* 48-Bit */
/*
 *	0x28
 */
#define WIN_MULTREAD_EXT		0x29 /* 48-Bit */
/*
 *	0x2A->0x2F Reserved
 */
#define WIN_WRITE			0x30 /* 28-Bit */
#define WIN_WRITE_ONCE			0x31 /* 28-Bit without retries */
#define WIN_WRITE_LONG			0x32 /* 28-Bit */
#define WIN_WRITE_LONG_ONCE		0x33 /* 28-Bit without retries */
#define WIN_WRITE_EXT			0x34 /* 48-Bit */
#define WIN_WRITEDMA_EXT		0x35 /* 48-Bit */
#define WIN_WRITEDMA_QUEUED_EXT		0x36 /* 48-Bit */
#define WIN_SET_MAX_EXT			0x37 /* 48-Bit */
#define CFA_WRITE_SECT_WO_ERASE		0x38 /* CFA Write Sectors without erase */
#define WIN_MULTWRITE_EXT		0x39 /* 48-Bit */
/*
 *	0x3A->0x3B Reserved
 */
#define WIN_WRITE_VERIFY		0x3C /* 28-Bit */
/*
 *	0x3D->0x3F Reserved
 */
#define WIN_VERIFY			0x40 /* 28-Bit - Read Verify Sectors */
#define WIN_VERIFY_ONCE			0x41 /* 28-Bit - without retries */
#define WIN_VERIFY_EXT			0x42 /* 48-Bit */
/*
 *	0x43->0x4F Reserved
 */
#define WIN_FORMAT			0x50
/*
 *	0x51->0x5F Reserved
 */
#define WIN_INIT			0x60
/*
 *	0x61->0x5F Reserved
 */
#define WIN_SEEK			0x70 /* 0x70-0x7F Reserved */
#define CFA_TRANSLATE_SECTOR		0x87 /* CFA Translate Sector */
#define WIN_DIAGNOSE			0x90
#define WIN_SPECIFY			0x91 /* set drive geometry translation */
#define WIN_DOWNLOAD_MICROCODE		0x92
#define WIN_STANDBYNOW2			0x94
#define WIN_STANDBY2			0x96
#define WIN_SETIDLE2			0x97
#define WIN_CHECKPOWERMODE2		0x98
#define WIN_SLEEPNOW2			0x99
/*
 *	0x9A VENDOR
 */
#define WIN_PACKETCMD			0xA0 /* Send a packet command. */
#define WIN_PIDENTIFY			0xA1 /* identify ATAPI device	*/
#define WIN_QUEUED_SERVICE		0xA2
#define WIN_SMART			0xB0 /* self-monitoring and reporting */
#define CFA_ERASE_SECTORS       	0xC0
#define WIN_MULTREAD			0xC4 /* read sectors using multiple mode*/
#define WIN_MULTWRITE			0xC5 /* write sectors using multiple mode */
#define WIN_SETMULT			0xC6 /* enable/disable multiple mode */
#define WIN_READDMA_QUEUED		0xC7 /* read sectors using Queued DMA transfers */
#define WIN_READDMA			0xC8 /* read sectors using DMA transfers */
#define WIN_READDMA_ONCE		0xC9 /* 28-Bit - without retries */
#define WIN_WRITEDMA			0xCA /* write sectors using DMA transfers */
#define WIN_WRITEDMA_ONCE		0xCB /* 28-Bit - without retries */
#define WIN_WRITEDMA_QUEUED		0xCC /* write sectors using Queued DMA transfers */
#define CFA_WRITE_MULTI_WO_ERASE	0xCD /* CFA Write multiple without erase */
#define WIN_GETMEDIASTATUS		0xDA	
#define WIN_ACKMEDIACHANGE		0xDB /* ATA-1, ATA-2 vendor */
#define WIN_POSTBOOT			0xDC
#define WIN_PREBOOT			0xDD
#define WIN_DOORLOCK			0xDE /* lock door on removable drives */
#define WIN_DOORUNLOCK			0xDF /* unlock door on removable drives */
#define WIN_STANDBYNOW1			0xE0
#define WIN_IDLEIMMEDIATE		0xE1 /* force drive to become "ready" */
#define WIN_STANDBY             	0xE2 /* Set device in Standby Mode */
#define WIN_SETIDLE1			0xE3
#define WIN_READ_BUFFER			0xE4 /* force read only 1 sector */
#define WIN_CHECKPOWERMODE1		0xE5
#define WIN_SLEEPNOW1			0xE6
#define WIN_FLUSH_CACHE			0xE7
#define WIN_WRITE_BUFFER		0xE8 /* force write only 1 sector */
#define WIN_WRITE_SAME			0xE9 /* read ata-2 to use */
	/* SET_FEATURES 0x22 or 0xDD */
#define WIN_FLUSH_CACHE_EXT		0xEA /* 48-Bit */
#define WIN_IDENTIFY			0xEC /* ask drive to identify itself	*/
#define WIN_MEDIAEJECT			0xED
#define WIN_IDENTIFY_DMA		0xEE /* same as WIN_IDENTIFY, but DMA */
#define WIN_SETFEATURES			0xEF /* set special drive features */
#define EXABYTE_ENABLE_NEST		0xF0
#define WIN_SECURITY_SET_PASS		0xF1
#define WIN_SECURITY_UNLOCK		0xF2
#define WIN_SECURITY_ERASE_PREPARE	0xF3
#define WIN_SECURITY_ERASE_UNIT		0xF4
#define WIN_SECURITY_FREEZE_LOCK	0xF5
#define WIN_SECURITY_DISABLE		0xF6
#define WIN_READ_NATIVE_MAX		0xF8 /* return the native maximum address */
#define WIN_SET_MAX			0xF9
#define DISABLE_SEAGATE			0xFB

#define MAX_MULT_SECTORS 128

struct IDEState;

/* What to run once the current PIO buffer is exhausted. A small closed set,
   so an enum plus a switch replaces the state-machine function pointer. */
enum class EndTransfer {
    Stop,
    SectorRead,
    SectorReadEnd,
    SectorWrite,
    Identify,
};

/* Two outstanding request kinds, so two completion objects. */
class IDEReadCompletion final: public BlockDeviceCompletion {
private:
    IDEState &fState;

public:
    IDEReadCompletion(IDEState &state): fState(state) {}

    void Complete(int ret) override;
};

class IDEWriteCompletion final: public BlockDeviceCompletion {
private:
    IDEState &fState;

public:
    IDEWriteCompletion(IDEState &state): fState(state) {}

    void Complete(int ret) override;
};

struct IDEState {
    IDEIFState *ide_if = nullptr;
    BlockDevice *bs = nullptr;
    int cylinders = 0, heads = 0, sectors = 0;
    int mult_sectors = 0;
    int64_t nb_sectors = 0;

    /* ide regs */
    uint8_t feature = 0;
    uint8_t error = 0;
    uint16_t nsector = 0; /* 0 is 256 to ease computations */
    uint8_t sector = 0;
    uint8_t lcyl = 0;
    uint8_t hcyl = 0;
    uint8_t select = 0;
    uint8_t status = 0;

    int io_nb_sectors = 0;
    int req_nb_sectors = 0;
    EndTransfer end_transfer = EndTransfer::Stop;

    int data_index = 0;
    int data_end = 0;
    uint8_t io_buffer[MAX_MULT_SECTORS*512 + 4] {};

    IDEReadCompletion read_completion {*this};
    IDEWriteCompletion write_completion {*this};
};

struct IDEIFState {
    IRQSignal *irq = nullptr;
    IDEState *cur_drive = nullptr;
    IDEState *drives[2] {};
    /* 0x3f6 command */
    uint8_t cmd = 0;

    uint32_t DataRead(uint32_t offset, int size_log2);
    void DataWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t PortRead(uint32_t offset, int size_log2);
    void PortWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t StatusRead(uint32_t offset, int size_log2);
    void CmdWrite(uint32_t offset, uint32_t val, int size_log2);

    DeviceIOAdapter<IDEIFState, &IDEIFState::DataRead,
                    &IDEIFState::DataWrite> fDataIo {*this};
    DeviceIOAdapter<IDEIFState, &IDEIFState::PortRead,
                    &IDEIFState::PortWrite> fPortIo {*this};
    DeviceIOAdapter<IDEIFState, &IDEIFState::StatusRead,
                    &IDEIFState::CmdWrite> fCmdIo {*this};
};

static void ide_run_end_transfer(IDEState *s);

static void padstr(char *str, const char *src, int len)
{
    int i, v;
    for(i = 0; i < len; i++) {
        if (*src)
            v = *src++;
        else
            v = ' ';
        *(char *)((long)str ^ 1) = v;
        str++;
    }
}

/* little endian assume */
static void stw(uint16_t *buf, int v)
{
    *buf = v;
}

static void ide_identify(IDEState *s)
{
    uint16_t *tab;
    uint32_t oldsize;
    
    tab = (uint16_t *)s->io_buffer;

    memset(tab, 0, 512 * 2);

    stw(tab + 0, 0x0040);
    stw(tab + 1, s->cylinders); 
    stw(tab + 3, s->heads);
    stw(tab + 4, 512 * s->sectors); /* sectors */
    stw(tab + 5, 512); /* sector size */
    stw(tab + 6, s->sectors); 
    stw(tab + 20, 3); /* buffer type */
    stw(tab + 21, 512); /* cache size in sectors */
    stw(tab + 22, 4); /* ecc bytes */
    padstr((char *)(tab + 27), "RISCVEMU HARDDISK", 40);
    stw(tab + 47, 0x8000 | MAX_MULT_SECTORS);
    stw(tab + 48, 0); /* dword I/O */
    stw(tab + 49, 1 << 9); /* LBA supported, no DMA */
    stw(tab + 51, 0x200); /* PIO transfer cycle */
    stw(tab + 52, 0x200); /* DMA transfer cycle */
    stw(tab + 54, s->cylinders);
    stw(tab + 55, s->heads);
    stw(tab + 56, s->sectors);
    oldsize = s->cylinders * s->heads * s->sectors;
    stw(tab + 57, oldsize);
    stw(tab + 58, oldsize >> 16);
    if (s->mult_sectors)
        stw(tab + 59, 0x100 | s->mult_sectors);
    stw(tab + 60, s->nb_sectors);
    stw(tab + 61, s->nb_sectors >> 16);
    stw(tab + 80, (1 << 1) | (1 << 2));
    stw(tab + 82, (1 << 14));
    stw(tab + 83, (1 << 14));
    stw(tab + 84, (1 << 14));
    stw(tab + 85, (1 << 14));
    stw(tab + 86, 0);
    stw(tab + 87, (1 << 14));
}

static void ide_set_signature(IDEState *s) 
{
    s->select &= 0xf0;
    s->nsector = 1;
    s->sector = 1;
    s->lcyl = 0;
    s->hcyl = 0;
}

static void ide_abort_command(IDEState *s) 
{
    s->status = READY_STAT | ERR_STAT;
    s->error = ABRT_ERR;
}

static void ide_set_irq(IDEState *s) 
{
    IDEIFState *ide_if = s->ide_if;
    if (!(ide_if->cmd & IDE_CMD_DISABLE_IRQ)) {
        ide_if->irq->Set(1);
    }
}

/* prepare data transfer and tell what to do after */
static void ide_transfer_start(IDEState *s, int size, EndTransfer end_transfer)
{
    s->end_transfer = end_transfer;
    s->data_index = 0;
    s->data_end = size;
}

static void ide_transfer_stop(IDEState *s)
{
    s->end_transfer = EndTransfer::Stop;
    s->data_index = 0;
    s->data_end = 0;
}

static int64_t ide_get_sector(IDEState *s)
{
    int64_t sector_num;
    if (s->select & 0x40) {
        /* lba */
        sector_num = ((s->select & 0x0f) << 24) | (s->hcyl << 16) |
            (s->lcyl << 8) | s->sector;
    } else {
        sector_num = ((s->hcyl << 8) | s->lcyl) * 
            s->heads * s->sectors +
            (s->select & 0x0f) * s->sectors + (s->sector - 1);
    }
    return sector_num;
}

static void ide_set_sector(IDEState *s, int64_t sector_num)
{
    unsigned int cyl, r;
    if (s->select & 0x40) {
        s->select = (s->select & 0xf0) | ((sector_num >> 24) & 0x0f);
        s->hcyl = (sector_num >> 16) & 0xff;
        s->lcyl = (sector_num >> 8) & 0xff;
        s->sector = sector_num & 0xff;
    } else {
        cyl = sector_num / (s->heads * s->sectors);
        r = sector_num % (s->heads * s->sectors);
        s->hcyl = (cyl >> 8) & 0xff;
        s->lcyl = cyl & 0xff;
        s->select = (s->select & 0xf0) | ((r / s->sectors) & 0x0f);
        s->sector = (r % s->sectors) + 1;
    }
}

static void ide_sector_read(IDEState *s)
{
    int64_t sector_num;
    int ret, n;

    sector_num = ide_get_sector(s);
    n = s->nsector;
    if (n == 0) 
        n = 256;
    if (n > s->req_nb_sectors)
        n = s->req_nb_sectors;
#if defined(DEBUG_IDE)
    printf("read sector=%" PRId64 " count=%d\n", sector_num, n);
#endif
    s->io_nb_sectors = n;
    ret = s->bs->ReadAsync(sector_num, s->io_buffer, n, &s->read_completion);
    if (ret < 0) {
        /* error */
        ide_abort_command(s);
        ide_set_irq(s);
    } else if (ret == 0) {
        /* synchronous case (needed for performance) */
        s->read_completion.Complete(0);
    } else {
        /* async case */
        s->status = READY_STAT | SEEK_STAT | BUSY_STAT;
        s->error = 0; /* not needed by IDE spec, but needed by Windows */
    }
}

void IDEReadCompletion::Complete(int ret)
{
    (void)ret;
    IDEState *s = &fState;
    int n;
    EndTransfer func;

    n = s->io_nb_sectors;
    ide_set_sector(s, ide_get_sector(s) + n);
    s->nsector = (s->nsector - n) & 0xff;
    if (s->nsector == 0)
        func = EndTransfer::SectorReadEnd;
    else
        func = EndTransfer::SectorRead;
    ide_transfer_start(s, 512 * n, func);
    ide_set_irq(s);
    s->status = READY_STAT | SEEK_STAT | DRQ_STAT;
    s->error = 0; /* not needed by IDE spec, but needed by Windows */
}

static void ide_sector_read_cb_end(IDEState *s)
{
    /* no more sector to read from disk */
    s->status = READY_STAT | SEEK_STAT;
    s->error = 0; /* not needed by IDE spec, but needed by Windows */
    ide_transfer_stop(s);
}

static void ide_sector_write_cb1(IDEState *s)
{
    int64_t sector_num;
    int ret;

    ide_transfer_stop(s);
    sector_num = ide_get_sector(s);
#if defined(DEBUG_IDE)
    printf("write sector=%" PRId64 "  count=%d\n",
           sector_num, s->io_nb_sectors);
#endif
    ret = s->bs->WriteAsync(sector_num, s->io_buffer, s->io_nb_sectors,
                            &s->write_completion);
    if (ret < 0) {
        /* error */
        ide_abort_command(s);
        ide_set_irq(s);
    } else if (ret == 0) {
        /* synchronous case (needed for performance) */
        s->write_completion.Complete(0);
    } else {
        /* async case */
        s->status = READY_STAT | SEEK_STAT | BUSY_STAT;
    }
}

void IDEWriteCompletion::Complete(int ret)
{
    (void)ret;
    IDEState *s = &fState;
    int n;

    n = s->io_nb_sectors;
    ide_set_sector(s, ide_get_sector(s) + n);
    s->nsector = (s->nsector - n) & 0xff;
    if (s->nsector == 0) {
        /* no more sectors to write */
        s->status = READY_STAT | SEEK_STAT;
    } else {
        n = s->nsector;
        if (n > s->req_nb_sectors)
            n = s->req_nb_sectors;
        s->io_nb_sectors = n;
        ide_transfer_start(s, 512 * n, EndTransfer::SectorWrite);
        s->status = READY_STAT | SEEK_STAT | DRQ_STAT;
    }
    ide_set_irq(s);
}

static void ide_sector_write(IDEState *s)
{
    int n;
    n = s->nsector;
    if (n == 0)
        n = 256;
    if (n > s->req_nb_sectors)
        n = s->req_nb_sectors;
    s->io_nb_sectors = n;
    ide_transfer_start(s, 512 * n, EndTransfer::SectorWrite);
    s->status = READY_STAT | SEEK_STAT | DRQ_STAT;
}

static void ide_identify_cb(IDEState *s)
{
    ide_transfer_stop(s);
    s->status = READY_STAT;
}

static void ide_run_end_transfer(IDEState *s)
{
    switch (s->end_transfer) {
        case EndTransfer::Stop:
            ide_transfer_stop(s);
            break;
        case EndTransfer::SectorRead:
            ide_sector_read(s);
            break;
        case EndTransfer::SectorReadEnd:
            ide_sector_read_cb_end(s);
            break;
        case EndTransfer::SectorWrite:
            ide_sector_write_cb1(s);
            break;
        case EndTransfer::Identify:
            ide_identify_cb(s);
            break;
        default:
            break;
    }
}

static void ide_exec_cmd(IDEState *s, int val)
{
#if defined(DEBUG_IDE)
    printf("ide: exec_cmd=0x%02x\n", val);
#endif
    switch(val) {
    case WIN_IDENTIFY:
        ide_identify(s);
        s->status = READY_STAT | SEEK_STAT | DRQ_STAT;
        ide_transfer_start(s, 512, EndTransfer::Identify);
        ide_set_irq(s);
        break;
    case WIN_SPECIFY:
    case WIN_RECAL:
        s->error = 0;
        s->status = READY_STAT | SEEK_STAT;
        ide_set_irq(s);
        break;
    case WIN_SETMULT:
        if (s->nsector > MAX_MULT_SECTORS || 
            (s->nsector & (s->nsector - 1)) != 0) {
            ide_abort_command(s);
        } else {
            s->mult_sectors = s->nsector;
#if defined(DEBUG_IDE)
            printf("ide: setmult=%d\n", s->mult_sectors);
#endif
            s->status = READY_STAT;
        }
        ide_set_irq(s);
        break;
    case WIN_READ:
    case WIN_READ_ONCE:
        s->req_nb_sectors = 1;
        ide_sector_read(s);
        break;
    case WIN_WRITE:
    case WIN_WRITE_ONCE:
        s->req_nb_sectors = 1;
        ide_sector_write(s);
        break;
    case WIN_MULTREAD:
        if (!s->mult_sectors) {
            ide_abort_command(s);
            ide_set_irq(s);
        } else {
            s->req_nb_sectors = s->mult_sectors;
            ide_sector_read(s);
        }
        break;
    case WIN_MULTWRITE:
        if (!s->mult_sectors) {
            ide_abort_command(s);
            ide_set_irq(s);
        } else {
            s->req_nb_sectors = s->mult_sectors;
            ide_sector_write(s);
        }
        break;
    case WIN_READ_NATIVE_MAX:
        ide_set_sector(s, s->nb_sectors - 1);
        s->status = READY_STAT;
        ide_set_irq(s);
        break;
    default:
        ide_abort_command(s);
        ide_set_irq(s);
        break;
    }
}

void IDEIFState::PortWrite(uint32_t offset, uint32_t val, int size_log2)
{
    IDEIFState *s1 = this;
    IDEState *s = s1->cur_drive;
    int addr = offset + 1;
    
#ifdef DEBUG_IDE
    printf("ide: write addr=0x%02x val=0x%02x\n", addr, val);
#endif
    switch(addr) {
    case 0:
        break;
    case 1:
        if (s) {
            s->feature = val;
        }
        break;
    case 2:
        if (s) {
            s->nsector = val;
        }
        break;
    case 3:
        if (s) {
            s->sector = val;
        }
        break;
    case 4:
        if (s) {
            s->lcyl = val;
        }
        break;
    case 5:
        if (s) {
            s->hcyl = val;
        }
        break;
    case 6:
        /* select drive */
        s = s1->cur_drive = s1->drives[(val >> 4) & 1];
        if (s) {
            s->select = val;
        }
        break;
    default:
    case 7:
        /* command */
        if (s) {
            ide_exec_cmd(s, val);
        }
        break;
    }
}

uint32_t IDEIFState::PortRead(uint32_t offset, int size_log2)
{
    IDEIFState *s1 = this;
    IDEState *s = s1->cur_drive;
    int ret, addr = offset + 1;

    if (!s) {
        ret = 0x00;
    } else {
        switch(addr) {
        case 0:
           ret = 0xff;
           break;
        case 1:
            ret = s->error;
            break;
        case 2:
            ret = s->nsector;
            break;
        case 3:
            ret = s->sector;
            break;
        case 4:
            ret = s->lcyl;
            break;
        case 5:
            ret = s->hcyl;
            break;
        case 6:
            ret = s->select;
            break;
        default:
        case 7:
            ret = s->status;
            s1->irq->Set(0);
            break;
        }
    }
#ifdef DEBUG_IDE
    printf("ide: read addr=0x%02x val=0x%02x\n", addr, ret);
#endif
    return ret;
}

uint32_t IDEIFState::StatusRead(uint32_t offset, int size_log2)
{
    IDEIFState *s1 = this;
    IDEState *s = s1->cur_drive;
    int ret;

    if (s) {
        ret = s->status;
    } else {
        ret = 0;
    }
#ifdef DEBUG_IDE
    printf("ide: read status=0x%02x\n", ret);
#endif
    return ret;
}

void IDEIFState::CmdWrite(uint32_t offset, uint32_t val, int size_log2)
{
    IDEIFState *s1 = this;
    IDEState *s;
    int i;
    
#ifdef DEBUG_IDE
    printf("ide: cmd write=0x%02x\n", val);
#endif
    if (!(s1->cmd & IDE_CMD_RESET) && (val & IDE_CMD_RESET)) {
        /* low to high */
        for(i = 0; i < 2; i++) {
            s = s1->drives[i];
            if (s) {
                s->status = BUSY_STAT | SEEK_STAT;
                s->error = 0x01;
            }
        }
    } else if ((s1->cmd & IDE_CMD_RESET) && !(val & IDE_CMD_RESET)) {
        /* high to low */
        for(i = 0; i < 2; i++) {
            s = s1->drives[i];
            if (s) {
                s->status = READY_STAT | SEEK_STAT;
                ide_set_signature(s);
            }
        }
    }
    s1->cmd = val;
}

void IDEIFState::DataWrite(uint32_t offset, uint32_t val, int size_log2)
{
    IDEIFState *s1 = this;
    IDEState *s = s1->cur_drive;
    int p;
    uint8_t *tab;
    
    if (!s)
        return;
    p = s->data_index;
    tab = s->io_buffer;
    tab[p] = val & 0xff;
    tab[p + 1] = (val >> 8) & 0xff;
    p += 2;
    s->data_index = p;
    if (p >= s->data_end)
        ide_run_end_transfer(s);
}

uint32_t IDEIFState::DataRead(uint32_t offset, int size_log2)
{
    IDEIFState *s1 = this;
    IDEState *s = s1->cur_drive;
    int p, ret;
    uint8_t *tab;
    
    if (!s) {
        ret = 0;
    } else {
        p = s->data_index;
        tab = s->io_buffer;
        ret = tab[p] | (tab[p + 1] << 8);
        p += 2;
        s->data_index = p;
        if (p >= s->data_end)
            ide_run_end_transfer(s);
    }
    return ret;
}

static IDEState *ide_drive_init(IDEIFState *ide_if, BlockDevice *bs)
{
    IDEState *s;
    uint32_t cylinders;
    uint64_t nb_sectors;

    s = new IDEState();

    s->ide_if = ide_if;
    s->bs = bs;

    nb_sectors = s->bs->SectorCount();
    cylinders = nb_sectors / (16 * 63);
    if (cylinders > 16383)
        cylinders = 16383;
    else if (cylinders < 2)
        cylinders = 2;
    s->cylinders = cylinders;
    s->heads = 16;
    s->sectors = 63;
    s->nb_sectors = nb_sectors;

    s->mult_sectors = MAX_MULT_SECTORS;
    /* ide regs */
    s->feature = 0;
    s->error = 0;
    s->nsector = 0;
    s->sector = 0;
    s->lcyl = 0;
    s->hcyl = 0;
    s->select = 0xa0;
    s->status = READY_STAT | SEEK_STAT;

    /* init I/O buffer */
    s->data_index = 0;
    s->data_end = 0;
    s->end_transfer = EndTransfer::Stop;

    s->req_nb_sectors = 0; /* temp for read/write */
    s->io_nb_sectors = 0; /* temp for read/write */
    return s;
}

IDEIFState *ide_init(PhysMemoryMap *port_map, uint32_t addr, uint32_t addr2,
                     IRQSignal *irq, BlockDevice **tab_bs)
{
    int i;
    IDEIFState *s;
    
    s = new IDEIFState();

    s->irq = irq;
    s->cmd = 0;

    port_map->RegisterDevice(addr, 1, &s->fDataIo, DEVIO_SIZE16);
    port_map->RegisterDevice(addr + 1, 7, &s->fPortIo, DEVIO_SIZE8);
    if (addr2) {
        port_map->RegisterDevice(addr2, 1, &s->fCmdIo, DEVIO_SIZE8);
    }
    
    for(i = 0; i < 2; i++) {
        if (tab_bs[i])
            s->drives[i] = ide_drive_init(s, tab_bs[i]);
    }
    s->cur_drive = s->drives[0];
    return s;
}

/* dummy PCI device for the IDE */
PCIDevice *piix3_ide_init(PCIBus *pci_bus, int devfn)
{
    PCIDevice *d;
    d = pci_register_device(pci_bus, "PIIX3 IDE", devfn, 0x8086, 0x7010, 0x00, 0x0101);
    pci_device_set_config8(d, 0x09, 0x00); /* ISA IDE ports, no DMA */
    return d;
}
