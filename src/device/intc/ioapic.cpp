/*
 * I/O APIC
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
#include "ioapic.h"

#include "bits.h"
#include "pci.h"

/* the two registers in the window */
#define IOREGSEL 0x00
#define IOWIN    0x10

/* indirect registers */
#define IOAPICID  0x00
#define IOAPICVER 0x01
#define IOAPICARB 0x02
#define IOREDTBL  0x10

#define IOAPIC_VERSION 0x11

/* redirection entry */
#define RTE_DELIVERY_STATUS bit_at(12)
#define RTE_REMOTE_IRR      bit_at(14)
#define RTE_LEVEL           bit_at(15)
#define RTE_MASKED          bit_at(16)
#define RTE_READ_ONLY       (RTE_DELIVERY_STATUS | RTE_REMOTE_IRR)


IOAPIC::IOAPIC(PCIMsiTarget &output, int id):
    fOutput(output), fId(id)
{
    for (int i = 0; i < IOAPIC_PINS; i++)
        fRedirection[i] = RTE_MASKED;
}


/* The message is the one an MSI to the same destination would be. */
void IOAPIC::Send(int pin)
{
    uint64_t rte = fRedirection[pin];
    uint64_t addr = 0xfee00000 | (get_bits(rte, 56, 8) << 12) |
        (get_bit(rte, 11) << 2);
    uint32_t data = get_bits(rte, 0, 11);

    if (rte & RTE_LEVEL) {
        data |= bit_at(15) | bit_at(14); /* level, asserted */
        fRedirection[pin] |= RTE_REMOTE_IRR;
    }
    fOutput.SendMsi(addr, data);
}


/* Sends a level triggered input that is asserted and not already waiting
   for its EOI. */
void IOAPIC::Service(int pin)
{
    uint64_t rte = fRedirection[pin];

    if ((rte & RTE_LEVEL) && !(rte & (RTE_MASKED | RTE_REMOTE_IRR)) &&
        get_bit(fLevels, pin))
        Send(pin);
}


void IOAPIC::SetIRQ(int pin, int level)
{
    bool was = get_bit(fLevels, pin);

    if (level)
        fLevels |= bit_at(pin);
    else
        fLevels &= ~bit_at(pin);

    if (fRedirection[pin] & RTE_LEVEL) {
        Service(pin);
    } else if (level && !was && !(fRedirection[pin] & RTE_MASKED)) {
        /* an edge while masked is lost, as on the real part */
        Send(pin);
    }
}


void IOAPIC::Eoi(int vector)
{
    for (int pin = 0; pin < IOAPIC_PINS; pin++) {
        uint64_t rte = fRedirection[pin];

        if ((rte & RTE_REMOTE_IRR) && get_bits(rte, 0, 8) == (uint64_t)vector) {
            fRedirection[pin] &= ~RTE_REMOTE_IRR;
            Service(pin);
        }
    }
}


uint32_t IOAPIC::DeviceRead(uint32_t offset, int size_log2)
{
    if (offset == IOREGSEL)
        return fSelect;
    if (offset != IOWIN)
        return 0;

    switch (fSelect) {
    case IOAPICID:
    case IOAPICARB:
        return fId << 24;
    case IOAPICVER:
        return ((IOAPIC_PINS - 1) << 16) | IOAPIC_VERSION;
    default:
        if (fSelect >= IOREDTBL && fSelect < IOREDTBL + 2 * IOAPIC_PINS) {
            int pin = (fSelect - IOREDTBL) / 2;
            return fRedirection[pin] >> ((fSelect & 1) * 32);
        }
        return 0;
    }
}


void IOAPIC::DeviceWrite(uint32_t offset, uint32_t val, int size_log2)
{
    if (offset == IOREGSEL) {
        fSelect = val;
        return;
    }
    if (offset != IOWIN)
        return;

    if (fSelect == IOAPICID) {
        fId = get_bits(val, 24, 4);
    } else if (fSelect >= IOREDTBL && fSelect < IOREDTBL + 2 * IOAPIC_PINS) {
        int pin = (fSelect - IOREDTBL) / 2;
        uint64_t rte = fRedirection[pin];

        if (fSelect & 1) {
            rte = (rte & 0xffffffffULL) | ((uint64_t)val << 32);
        } else {
            rte = (rte & ~0xffffffffULL) | (rte & RTE_READ_ONLY) |
                (val & ~(uint32_t)RTE_READ_ONLY);
            /* an edge triggered entry has nothing to wait for */
            if (!(rte & RTE_LEVEL))
                rte &= ~RTE_REMOTE_IRR;
        }
        fRedirection[pin] = rte;
        /* unmasking a level triggered input that is asserted sends it */
        Service(pin);
    }
}
