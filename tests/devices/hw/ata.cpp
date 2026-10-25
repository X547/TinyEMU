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
#include "ata.h"

#include "mmio.h"
#include "test.h"

#define PCI_CLASS_IDE 0x0101

/* compatibility mode: where the AT had them */
static const uint16_t kCompatCmd[2] = {0x1f0, 0x170};
static const uint16_t kCompatCtl[2] = {0x3f6, 0x376};
static const uint32_t kCompatIrq[2] = {14, 15};


/* A command or control block register: a port in compatibility mode, the
   channel's BAR in native mode. */
static uint32_t block_read(const AtaChannel &ch, bool ctl, int reg,
                           int size_log2)
{
    if (!ch.native) {
#if defined(__i386__) || defined(__x86_64__)
        uint16_t port = (ctl ? ch.ctl : ch.cmd) + reg;
        switch (size_log2) {
        case 0: return inb(port);
        case 1: return inw(port);
        default: return inl(port);
        }
#else
        return 0xffffffff;
#endif
    }
    int bar = ch.index % 2 * 2 + (ctl ? 1 : 0);
    return pci_bar_read(ch.fn, bar, (ctl ? 2 : 0) + reg, size_log2);
}

static void block_write(const AtaChannel &ch, bool ctl, int reg, uint32_t v,
                        int size_log2)
{
    if (!ch.native) {
#if defined(__i386__) || defined(__x86_64__)
        uint16_t port = (ctl ? ch.ctl : ch.cmd) + reg;
        switch (size_log2) {
        case 0: outb(port, v); break;
        case 1: outw(port, v); break;
        default: outl(port, v); break;
        }
#endif
        return;
    }
    int bar = ch.index % 2 * 2 + (ctl ? 1 : 0);
    pci_bar_write(ch.fn, bar, (ctl ? 2 : 0) + reg, v, size_log2);
}


uint8_t AtaChannel::Read(int reg) const
    {return block_read(*this, false, reg, 0);}
void AtaChannel::Write(int reg, uint8_t v) const
    {block_write(*this, false, reg, v, 0);}
uint16_t AtaChannel::ReadData() const
    {return block_read(*this, false, ATA_DATA, 1);}
void AtaChannel::WriteData(uint16_t v) const
    {block_write(*this, false, ATA_DATA, v, 1);}
uint32_t AtaChannel::ReadData32() const
    {return block_read(*this, false, ATA_DATA, 2);}
void AtaChannel::WriteData32(uint32_t v) const
    {block_write(*this, false, ATA_DATA, v, 2);}
uint8_t AtaChannel::AltStatus() const {return block_read(*this, true, 0, 0);}
void AtaChannel::Control(uint8_t v) const {block_write(*this, true, 0, v, 0);}

uint8_t AtaChannel::BmRead(int reg) const
{
    return bm_bar < 0 ? 0xff : pci_bar_read(fn, bm_bar, bm_off + reg, 0);
}

void AtaChannel::BmWrite(int reg, uint8_t v) const
{
    if (bm_bar >= 0)
        pci_bar_write(fn, bm_bar, bm_off + reg, v, 0);
}

void AtaChannel::BmWritePrd(uint32_t addr) const
{
    if (bm_bar >= 0)
        pci_bar_write(fn, bm_bar, bm_off + BM_PRD, addr, 2);
}


uint8_t AtaChannel::WaitNotBusy(uint64_t timeout_us) const
{
    uint8_t st = 0xff;
    if (!t_wait_until([&] {
            st = AltStatus();
            return (st & ATA_ST_BSY) == 0;
        }, timeout_us))
        return 0xff;
    return st;
}


bool AtaChannel::Select(int unit) const
{
    Write(ATA_DEVICE, 0xa0 | unit << 4);
    return WaitNotBusy() != 0xff;
}


bool AtaChannel::Route(IrqHandler fn_, void *ctx) const
{
    if (native)
        return pci_intx_route(fn, fn_, ctx);
    return irq_route_isa(isa_irq, fn_, ctx);
}


void AtaChannel::Unroute() const
{
    if (native)
        pci_intx_unroute(fn);
    else
        irq_unroute_isa(isa_irq);
}


bool ata_channel(int index, AtaChannel *ch)
{
    pci_enumerate();
    int n = index;
    for (int i = 0; i < pci_func_count(); i++) {
        PciFunc *f = pci_func(i);
        if ((f->class_rev >> 16) != PCI_CLASS_IDE)
            continue;
        if (n >= 2) {
            n -= 2;
            continue;
        }
        uint8_t progif = f->class_rev >> 8;
        *ch = AtaChannel {};
        ch->fn = f;
        ch->index = n;
        ch->native = (progif >> (2 * n)) & 1;
        if (ch->native) {
            ch->io = f->bars[2 * n].io;
        } else {
#if !defined(__i386__) && !defined(__x86_64__)
            return false;
#endif
            ch->io = true;
            ch->cmd = kCompatCmd[n];
            ch->ctl = kCompatCtl[n];
            ch->isa_irq = kCompatIrq[n];
        }
        ch->bm_bar = (progif & 0x80) && f->bars[4].size ? 4 : -1;
        ch->bm_off = 8 * n;
        return true;
    }
    return false;
}


bool ata_soft_reset(const AtaChannel &ch)
{
    ch.Control(ATA_CTL_SRST | ATA_CTL_NIEN);
    t_delay_us(10);
    ch.Control(ATA_CTL_NIEN);
    return ch.WaitNotBusy() != 0xff;
}


AtaKind ata_probe(const AtaChannel &ch, int unit)
{
    if (!ata_soft_reset(ch))
        return ATA_NONE;
    ch.Write(ATA_DEVICE, 0xa0 | unit << 4);
    uint8_t st = ch.WaitNotBusy();
    if (st == 0 || st == 0xff)
        return ATA_NONE;
    uint8_t lm = ch.Read(ATA_LBAM), lh = ch.Read(ATA_LBAH);
    if (lm == 0x14 && lh == 0xeb)
        return ATA_PACKET;
    if (lm == 0 && lh == 0)
        return ATA_DISK;
    return ATA_NONE;
}


void ata_cleanup()
{
    AtaChannel ch;
    for (int i = 0; ata_channel(i, &ch); i++) {
        ata_soft_reset(ch);
        ch.Control(0);
        if (ch.bm_bar >= 0) {
            ch.BmWrite(BM_COMMAND, 0);
            ch.BmWrite(BM_STATUS, BM_ERROR | BM_IRQ);
        }
        ch.Read(ATA_STATUS);
    }
    pci_restore_all();
}
