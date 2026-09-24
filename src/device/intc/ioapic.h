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
#pragma once

#include <stdint.h>

#include "iomem.h"

class PCIMsiTarget;


#define IOAPIC_ADDR 0xfec00000
#define IOAPIC_SIZE 0x1000
#define IOAPIC_PINS 24


/* An 82093AA: 24 inputs, each sent to the local APICs as the message its
   redirection entry describes. The input polarity is not modelled: a line
   the machine raises is asserted, whichever way the guest says it is
   wired. */
class IOAPIC final: public IRQTarget, public DeviceIO {
private:
    PCIMsiTarget &fOutput;
    uint32_t fId;
    uint8_t fSelect = 0;
    uint64_t fRedirection[IOAPIC_PINS];
    /* bit n is the level of input n */
    uint32_t fLevels = 0;

    void Send(int pin);
    void Service(int pin);

public:
    IOAPIC(PCIMsiTarget &output, int id);

    /* IRQTarget */
    void SetIRQ(int pin, int level) override;

    /* DeviceIO */
    uint32_t DeviceRead(uint32_t offset, int size_log2) override;
    void DeviceWrite(uint32_t offset, uint32_t val, int size_log2) override;

    /* A local APIC ended an interrupt with this vector: the level
       triggered inputs waiting on it may send again. */
    void Eoi(int vector);
};
