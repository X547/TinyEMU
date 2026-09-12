/*
 * PS/2 bus and devices
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

#include <stdint.h>

#include "device.h"

/* What one device may have waiting for the host. */
#define PS2_QUEUE_SIZE 256

/* Replies every device on the bus shares. */
#define PS2_REPLY_ACK    0xfa
#define PS2_REPLY_RESEND 0xfe
#define PS2_REPLY_POR    0xaa /* self test passed */

/* Prefixes that appear in the scancode stream rather than being codes. The
   extend prefix means the same thing in every set; the release prefix exists
   only in sets 2 and 3, where set 1 sets the top bit of the code instead. */
#define PS2_SCAN_EXTEND  0xe0
#define PS2_SCAN_RELEASE 0xf0


/* One channel of a controller: the wire a single device hangs off. The
   controller implements this so that a device can say when it has something
   to send. */
class PS2Port {
public:
    virtual ~PS2Port() = default;

    /* The device on this port now has bytes waiting, or has run out. */
    virtual void PS2DataAvailable(bool available) = 0;
};


/* One device on a PS/2 port: a keyboard or a pointer. It takes the command
   bytes the host writes and answers with bytes of its own, which the
   controller collects one at a time. Nothing here knows how a controller is
   addressed, which is what lets the same keyboard sit behind the PC's i8042
   and behind a memory mapped controller. */
class PS2Device {
private:
    PS2Port *fPort = nullptr;
    uint8_t fQueue[PS2_QUEUE_SIZE] {};
    /* Which queued bytes are part of the key event stream. A controller that
       translates scancodes must convert those and pass everything else --
       acknowledgements, identifiers, self test results -- through untouched,
       and only the device that produced a byte knows which it is. */
    bool fIsScancode[PS2_QUEUE_SIZE] {};
    int fReadPos = 0;
    int fWritePos = 0;
    int fCount = 0;

public:
    virtual ~PS2Device() = default;

    /* Called by the controller when the device is plugged into a port. */
    void SetPort(PS2Port *port) {fPort = port;}

    /* A byte the host wrote to this device. */
    virtual void Write(uint8_t val) = 0;

    /* The state a power on, or the controller's reset, leaves it in. */
    virtual void Reset() = 0;

    /* Queue one byte for the host. A device marks its key events as
       scancodes; a controller putting a byte of its own into the stream
       leaves the default, because nothing it invents is one. */
    void Queue(uint8_t val, bool is_scancode = false);

    bool HasData() const {return fCount > 0;}
    int QueueRoom() const {return PS2_QUEUE_SIZE - fCount;}

    /* The next byte, and through 'is_scancode' whether a translating
       controller should convert it. Reading with nothing queued repeats the
       last byte, which is what real hardware does and what some firmware
       depends on. */
    uint8_t Read(bool *is_scancode = nullptr);

    void ClearQueue();
};


/* A keyboard, which additionally takes host key events. Kept separate from
   the byte level device so that a machine's input plumbing can hand events
   to whatever is on the port without knowing which model it is. */
class PS2Keyboard: public PS2Device {
public:
    /* 'keycode' is a Linux evdev key code, which is what the front ends
       produce. The bytes that reach the host are in whichever scancode set
       the guest last selected. */
    virtual void PutKeycode(bool is_down, int keycode) = 0;
};


/* A pointer, likewise. */
class PS2Mouse: public PS2Device {
public:
    virtual void MouseEvent(int dx, int dy, int dz, int buttons_state) = 0;
};


PS2Keyboard *ps2_keyboard_create();
PS2Mouse *ps2_mouse_create();


/* The correspondence between scancode set 1 and set 2, which is the whole of
   what an AT controller's translation does.

   Only one direction is written down and the other is derived from it, so
   that a keyboard emitting set 2 and a controller translating back to set 1
   cannot disagree: whatever the table says, the round trip returns the code
   the keyboard started from. 0 means the code has no counterpart. */
uint8_t ps2_set1_to_set2(uint8_t code);
uint8_t ps2_set2_to_set1(uint8_t code);


/* Implemented by a controller so that a device declared in the configuration
   can be plugged into one of its ports. */
class PS2BusTarget {
public:
    virtual ~PS2BusTarget() = default;

    /* A port carries one device; a second one is reported rather than
       silently ignored. */
    virtual bool AttachDevice(PS2Device *dev) = 0;
};


/* The bus one controller port provides. Like the SD and USB buses, what it
   hands out is a place on a bus rather than host address space, so it
   assigns no resource records. */
class PS2Bus final: public Bus {
private:
    PS2BusTarget *fTarget;

public:
    PS2Bus(Device *owner, PS2BusTarget *target):
        Bus(owner), fTarget(target) {}

    const char *Type() const override {return "ps2";}
    PS2BusTarget *Target() const {return fTarget;}

    bool AssignResources(Device *dev) override;
};


/* Attaches one device to the port it was declared on, the way SDDeviceNode
   attaches a card. A controller whose devices come from the configuration
   rather than from its own construction wraps each of them in one of
   these. */
class PS2DeviceNode final: public Device {
private:
    PS2Device *fDev;

public:
    PS2DeviceNode(const char *name, PS2Device *dev);
    ~PS2DeviceNode() override;

    PS2Device *Dev() const {return fDev;}

    bool Realize() override;
};
