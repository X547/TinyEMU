/*
 * Configurable device objects
 *
 * Copyright (c) 2016-2018 Fabrice Bellard
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

#include "device.h"
#include "machine.h"
#include "uart.h"
#include "virtio.h"


/* The machine-level objects a device may attach to, and the back references
   the machine collects while the tree is realized. Passed to every factory so
   that no device needs to know the machine type it lives in. */
struct DeviceContext {
    const VirtMachineParams *params = nullptr;
    /* for a device that stops the emulator */
    VirtMachine *machine = nullptr;
    CharacterDevice *console = nullptr;
    SerialOutput *serial_output = nullptr;

    /* filled in as devices are realized */
    VIRTIODevice *console_dev = nullptr;
    /* Whichever devices claimed the keyboard and the pointer roles; the last
       one realized wins. */
    InputEventTarget *keyboard = nullptr;
    InputEventTarget *mouse = nullptr;
    FBDevice *fb_dev = nullptr;
    /* Where the framebuffer was placed, for a machine that has to tell its
       guest in something other than a device tree. */
    uint64_t fb_base = 0;
    SerialState *serial_console = nullptr;
    EthernetDevice *net = nullptr;
    /* The VMware backdoor, when a device answers it. The machine installs the
       port, because reading it means reading the processor's registers. */
    VMPortTarget *vmport = nullptr;
    uint64_t vmport_base = 0;
};


/* Instantiate one configuration node. Returns nullptr and reports if the type
   is unknown or the node is missing something it needs. */
Device *device_create(const VMDeviceNode *node, DeviceContext *ctx);

/* Instantiate a sibling list onto 'bus', recursing into the child bus of any
   device that provides one. */
bool device_build_tree(Bus *bus, VMDeviceNode *nodes, DeviceContext *ctx);
