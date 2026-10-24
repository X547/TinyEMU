/*
 * Interrupt controllers
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

#include "arch.h"
#include "registry.h"

/* A controller the suite can deliver device interrupts through, to the
   boot processor at the privilege the image runs at. Device tests use it
   through irq_route() and do not care which controller it is; the tests
   of a controller drive its registers themselves. */
class IntController {
public:
    virtual ~IntController() = default;
    virtual const char *Kind() const = 0;
    /* Deliver input 'line' to 'fn', which quiets the device; the
       controller acknowledges the interrupt around it. */
    virtual bool Route(uint32_t line, IrqHandler fn, void *ctx) = 0;
    virtual void Unroute(uint32_t line) = 0;
    /* Between tests: every input masked, nothing left in service. */
    virtual void Reset() = 0;
};

/* The controller and input that a device's 'index'th interrupt reaches,
   as the tables say. False when it is none the suite drives. */
bool intc_resolve(const HwDevice *dev, int index, IntController **ctrl,
                  uint32_t *line);

/* intc_resolve() and Route() in one. */
bool irq_route(const HwDevice *dev, int index, IrqHandler fn, void *ctx);
void irq_unroute(const HwDevice *dev, int index);

/* Reset every controller that was used. */
void intc_reset_all();

/* For the drivers, as each is created: remember a controller so that it is
   reset, whether a test routes through it or drives it directly. */
void intc_register(IntController *ctrl);

/* RISC-V: the phandle of a hart's local interrupt controller, the
   "riscv,cpu-intc" node in its cpu node; 0 when there is none. */
uint32_t hart_intc_phandle(uint32_t hart);
/* And the hart whose controller has 'phandle', or -1. */
int hart_of_intc(uint32_t phandle);
