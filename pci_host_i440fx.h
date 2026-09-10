/*
 * i440FX PCI host bridge
 *
 * Copyright (c) 2017 Fabrice Bellard
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

typedef struct I440FXState I440FXState;

/* The host bridge of the x86 machine: a CF8/CFC configuration window, the
   i440FX function itself and the PIIX3 ISA bridge whose PIRQ registers route
   the four INTx lines onto the PIC. */
I440FXState *i440fx_init(PCIBus **pbus, int *ppiix3_devfn,
                         PhysMemoryMap *mem_map, PhysMemoryMap *port_map,
                         IRQSignal *pic_irqs);

/* In case no BIOS is used, map the interrupts. */
void i440fx_map_interrupts(I440FXState *s, uint8_t *elcr,
                           const uint8_t *pci_irqs);
