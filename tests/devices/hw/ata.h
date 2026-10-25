/*
 * ATA controllers
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

#include "pci.h"

/* A PCI IDE controller (class 0101h) by the PCI IDE Controller
   Specification and SFF-8038i: each channel in compatibility mode at the
   AT's ports and ISA lines, or in native mode where its BARs say, and the
   bus master in BAR 4. */

#define ATA_DATA    0
#define ATA_ERROR   1
#define ATA_FEATURE 1
#define ATA_NSECT   2
#define ATA_LBAL    3
#define ATA_LBAM    4
#define ATA_LBAH    5
#define ATA_DEVICE  6
#define ATA_STATUS  7
#define ATA_COMMAND 7

#define ATA_ST_ERR  0x01
#define ATA_ST_DRQ  0x08
#define ATA_ST_DSC  0x10
#define ATA_ST_DF   0x20
#define ATA_ST_DRDY 0x40
#define ATA_ST_BSY  0x80

#define ATA_ER_ABRT 0x04
#define ATA_ER_IDNF 0x10
#define ATA_ER_UNC  0x40

#define ATA_CTL_NIEN 0x02
#define ATA_CTL_SRST 0x04

#define BM_COMMAND 0
#define BM_STATUS  2
#define BM_PRD     4
#define BM_START   0x01
#define BM_READ    0x08 /* the engine writes memory */
#define BM_ACTIVE  0x01
#define BM_ERROR   0x02
#define BM_IRQ     0x04

struct AtaChannel {
    PciFunc *fn;
    int index;
    bool native;
    bool io;           /* the command and control blocks are ports */
    uint64_t cmd;      /* the command block */
    uint64_t ctl;      /* the device control / alternate status register */
    uint32_t isa_irq;  /* compatibility mode */
    int bm_bar;        /* the bus master's BAR, -1 without one */
    uint32_t bm_off;   /* the channel's eight bytes in it */

    uint8_t Read(int reg) const;
    void Write(int reg, uint8_t v) const;
    uint16_t ReadData() const;
    void WriteData(uint16_t v) const;
    uint32_t ReadData32() const;
    void WriteData32(uint32_t v) const;
    uint8_t AltStatus() const;
    void Control(uint8_t v) const;
    uint8_t BmRead(int reg) const;
    void BmWrite(int reg, uint8_t v) const;
    void BmWritePrd(uint32_t addr) const;

    /* Select a device and wait for it not to be busy; false on a time
       out. */
    bool Select(int unit) const;
    /* Status without BSY, waited for; 0xff on a time out. */
    uint8_t WaitNotBusy(uint64_t timeout_us = 1000000) const;
    /* Interrupts routed to 'fn', by the ISA line or INTx. */
    bool Route(IrqHandler fn, void *ctx) const;
    void Unroute() const;
};

/* The channels of the PCI IDE controllers, 0 and 1 of the first, then 2
   and 3 of the next; false when there is no such channel. */
bool ata_channel(int index, AtaChannel *ch);

/* What kind of device answers at a unit, by the signature a software reset
   leaves (ATA/ATAPI-6, 9.12): */
enum AtaKind {
    ATA_NONE,
    ATA_DISK,
    ATA_PACKET,
};
AtaKind ata_probe(const AtaChannel &ch, int unit);

/* Reset both devices of a channel with SRST and wait for them. */
bool ata_soft_reset(const AtaChannel &ch);

/* For t_on_cleanup(): every channel reset, decoding as enumeration left
   it. */
void ata_cleanup();
