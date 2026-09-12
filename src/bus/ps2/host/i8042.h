/*
 * Intel 8042 PS/2 keyboard controller
 *
 * Copyright (c) 2003 Fabrice Bellard
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

#include "iomem.h"
#include "ps2.h"

/* The two ports the PC's controller has: the keyboard one, and the auxiliary
   one a pointer hangs off. */
#define I8042_PORT_KBD 0
#define I8042_PORT_AUX 1
#define I8042_PORT_COUNT 2


/* The PC's keyboard controller: two PS/2 ports behind a pair of I/O
   registers, plus the odds and ends a PC put on the same chip -- the A20
   gate and the reset line.

   It is also the only controller here with an AT translator, which is what
   turns the set 2 a keyboard emits into the set 1 a PC guest expects. A
   controller without one (a memory mapped part on a board that never had a
   PC's history) simply passes the keyboard's own bytes through, which is why
   the translation lives here rather than in the keyboard. */
class I8042Controller final {
private:
    /* One port: the bus it provides, the device plugged into it, and the two
       ends of the wire between them. Each port is its own bus target, so
       which port a device lands on is the bus it was declared on rather than
       the order the devices happened to be added in. */
    class Port final: public PS2Port, public PS2BusTarget {
    public:
        I8042Controller *owner = nullptr;
        int index = 0;
        PS2Bus *bus = nullptr;
        PS2Device *dev = nullptr;

        void PS2DataAvailable(bool available) override;
        bool AttachDevice(PS2Device *dev) override;
    };

    Port fPorts[I8042_PORT_COUNT];

    uint8_t fWriteCmd = 0; /* if non zero, a write to the data port follows */
    uint8_t fStatus = 0;
    uint8_t fMode = 0;
    /* Bitmask of ports with data available. */
    uint8_t fPending = 0;

    /* Set while a set 2 release prefix has been swallowed and the code that
       follows it still has to carry the top bit set 1 marks a release
       with. */
    bool fXlateBreak = false;

    IRQSignal *fKbdIrq = nullptr;
    IRQSignal *fAuxIrq = nullptr;

    void UpdateIRQ();
    void QueueFromController(uint8_t val, int port);
    uint8_t ReadFromPort(int port);
    /* False when the byte was a release prefix, which set 1 folds into the
       code that follows it rather than sending on its own. */
    bool Translate(uint8_t val, uint8_t *out);

    uint32_t DataRead(uint32_t offset, int size_log2);
    void DataWrite(uint32_t offset, uint32_t val, int size_log2);
    uint32_t StatusRead(uint32_t offset, int size_log2);
    void CommandWrite(uint32_t offset, uint32_t val, int size_log2);

public:
    I8042Controller(PhysMemoryMap *port_map, IRQSignal *kbd_irq,
                    IRQSignal *aux_irq, uint32_t io_base);
    ~I8042Controller();

    /* The bus one port provides, for a machine that declares what hangs off
       it. The PC machine puts a keyboard and a pointer on them itself. */
    PS2Bus *PortBus(int port);

    DeviceIOAdapter<I8042Controller, &I8042Controller::DataRead,
                    &I8042Controller::DataWrite> fDataIo {*this};
    DeviceIOAdapter<I8042Controller, &I8042Controller::StatusRead,
                    &I8042Controller::CommandWrite> fCmdIo {*this};
};


/* Build a PC controller with the keyboard and the pointer a PC has, and hand
   both back so that the machine can deliver host input events to them. */
I8042Controller *i8042_init(PS2Keyboard **pkbd, PS2Mouse **pmouse,
                            PhysMemoryMap *port_map, IRQSignal *kbd_irq,
                            IRQSignal *aux_irq, uint32_t io_base);
