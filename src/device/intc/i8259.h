/*
 * 8259 PIC
 *
 * Copyright (c) 2011-2017 Fabrice Bellard
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

#include <memory>

#include "iomem.h"

struct PICState;


/* The lines of a PC's two 8259s. */
#define I8259_IRQ_COUNT 16


/* Implemented by the machine: raises or lowers the CPU's INTR line. */
class CPUIRQTarget {
public:
    virtual ~CPUIRQTarget() = default;

    virtual void SetCPUIRQ(int level) = 0;
};


/* The two cascaded 8259s of a PC, the second on input 2 of the first, with
   the PIIX ELCRs that make each input edge or level triggered. */
class I8259Pair final: public IRQTarget {
private:
    friend struct PICState;

    std::unique_ptr<PICState> fPics[2];
    CPUIRQTarget &fCpu;

    /* Raises or lowers INTR; called whenever the request may change. */
    void Update();

public:
    /* Each controller at its two ports, its ELCR at one more, and 'irqs'
       set up as its I8259_IRQ_COUNT inputs. */
    I8259Pair(PhysMemoryMap *port_map, uint32_t addr0, uint32_t addr1,
              uint32_t elcr_addr0, uint32_t elcr_addr1, CPUIRQTarget &cpu,
              IRQSignal *irqs);
    ~I8259Pair() override;

    /* Sets both ELCRs, as firmware would. */
    void SetElcr(const uint8_t *elcr);

    /* The vector of the interrupt the processor takes, which the controllers
       then count as in service. */
    int Acknowledge();

    /* IRQTarget */
    void SetIRQ(int irq, int level) override;
};
