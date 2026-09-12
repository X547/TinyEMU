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
#include "ps2.h"

#include <string.h>

#include "cutils.h"
#include "machine.h"


//#pragma mark - PS2Device

void PS2Device::Queue(uint8_t val, bool is_scancode)
{
    if (fCount >= PS2_QUEUE_SIZE) {
        return;
    }
    fQueue[fWritePos] = val;
    fIsScancode[fWritePos] = is_scancode;
    if (++fWritePos == PS2_QUEUE_SIZE) {
        fWritePos = 0;
    }
    fCount++;
    if (fPort != nullptr) {
        fPort->PS2DataAvailable(true);
    }
}


uint8_t PS2Device::Read(bool *is_scancode)
{
    if (fCount == 0) {
        /* Nothing left, so the last byte is handed over again. A real
           controller keeps driving its output buffer, and firmware that
           reads one byte too many depends on seeing the same value. */
        int index = fReadPos - 1;
        if (index < 0) {
            index = PS2_QUEUE_SIZE - 1;
        }
        if (is_scancode != nullptr) {
            *is_scancode = fIsScancode[index];
        }
        return fQueue[index];
    }

    uint8_t val = fQueue[fReadPos];
    if (is_scancode != nullptr) {
        *is_scancode = fIsScancode[fReadPos];
    }
    if (++fReadPos == PS2_QUEUE_SIZE) {
        fReadPos = 0;
    }
    fCount--;
    if (fPort != nullptr) {
        /* Taking a byte drops the line, and raises it again when more is
           waiting, so that a controller sees an edge per byte. */
        fPort->PS2DataAvailable(false);
        fPort->PS2DataAvailable(fCount != 0);
    }
    return val;
}


void PS2Device::ClearQueue()
{
    fReadPos = 0;
    fWritePos = 0;
    fCount = 0;
    if (fPort != nullptr) {
        fPort->PS2DataAvailable(false);
    }
}


//#pragma mark - scancode sets

/* Set 1 make codes and the set 2 codes that stand for the same key. Only the
   keys a front end can produce are listed, which is every key of a 105 key
   keyboard bar the two multi-byte oddities (Pause and Print Screen).

   The extend prefix is not part of an entry: it means the same in both sets,
   so a key that carries one carries it either way and only the code after it
   is looked up here. The set 2 codes of the extended keys are the same as
   those of the unextended keys they mirror, which is why several appear
   twice in the set 2 column.

   A handful of evdev codes just below 96 -- 85, 89, 90, 94 and 95, which
   name keys of a Japanese layout -- reach this table as set 1 codes that no
   keyboard assigns, and so have no entry and produce nothing. The code they
   reached it as was the evdev number itself, which named a different key or
   none, so nothing that ever worked is lost by dropping them. */
struct PS2SetPair {
    uint8_t set1;
    uint8_t set2;
};

static const PS2SetPair kSetPairs[] = {
    {0x01, 0x76}, /* Esc */
    {0x02, 0x16}, {0x03, 0x1e}, {0x04, 0x26}, {0x05, 0x25}, /* 1 2 3 4 */
    {0x06, 0x2e}, {0x07, 0x36}, {0x08, 0x3d}, {0x09, 0x3e}, /* 5 6 7 8 */
    {0x0a, 0x46}, {0x0b, 0x45},                             /* 9 0 */
    {0x0c, 0x4e}, {0x0d, 0x55},                             /* - = */
    {0x0e, 0x66}, /* Backspace */
    {0x0f, 0x0d}, /* Tab */
    {0x10, 0x15}, {0x11, 0x1d}, {0x12, 0x24}, {0x13, 0x2d}, /* Q W E R */
    {0x14, 0x2c}, {0x15, 0x35}, {0x16, 0x3c}, {0x17, 0x43}, /* T Y U I */
    {0x18, 0x44}, {0x19, 0x4d},                             /* O P */
    {0x1a, 0x54}, {0x1b, 0x5b},                             /* [ ] */
    {0x1c, 0x5a}, /* Enter, and keypad Enter behind the extend prefix */
    {0x1d, 0x14}, /* Control, either one */
    {0x1e, 0x1c}, {0x1f, 0x1b}, {0x20, 0x23}, {0x21, 0x2b}, /* A S D F */
    {0x22, 0x34}, {0x23, 0x33}, {0x24, 0x3b}, {0x25, 0x42}, /* G H J K */
    {0x26, 0x4b},                                           /* L */
    {0x27, 0x4c}, {0x28, 0x52}, {0x29, 0x0e},               /* ; ' ` */
    {0x2a, 0x12}, /* left shift */
    {0x2b, 0x5d}, /* backslash */
    {0x2c, 0x1a}, {0x2d, 0x22}, {0x2e, 0x21}, {0x2f, 0x2a}, /* Z X C V */
    {0x30, 0x32}, {0x31, 0x31}, {0x32, 0x3a},               /* B N M */
    {0x33, 0x41}, {0x34, 0x49},                             /* , . */
    {0x35, 0x4a}, /* slash, and keypad divide behind the extend prefix */
    {0x36, 0x59}, /* right shift */
    {0x37, 0x7c}, /* keypad multiply */
    {0x38, 0x11}, /* Alt, either one */
    {0x39, 0x29}, /* Space */
    {0x3a, 0x58}, /* Caps Lock */
    {0x3b, 0x05}, {0x3c, 0x06}, {0x3d, 0x04}, {0x3e, 0x0c}, /* F1..F4 */
    {0x3f, 0x03}, {0x40, 0x0b}, {0x41, 0x83}, {0x42, 0x0a}, /* F5..F8 */
    {0x43, 0x01}, {0x44, 0x09},                             /* F9 F10 */
    {0x45, 0x77}, /* Num Lock */
    {0x46, 0x7e}, /* Scroll Lock */
    {0x47, 0x6c}, {0x48, 0x75}, {0x49, 0x7d},   /* keypad 7 8 9, Home... */
    {0x4a, 0x7b},                               /* keypad minus */
    {0x4b, 0x6b}, {0x4c, 0x73}, {0x4d, 0x74},   /* keypad 4 5 6 */
    {0x4e, 0x79},                               /* keypad plus */
    {0x4f, 0x69}, {0x50, 0x72}, {0x51, 0x7a},   /* keypad 1 2 3 */
    {0x52, 0x70}, {0x53, 0x71},                 /* keypad 0 and period */
    {0x54, 0x84}, /* SysRq */
    {0x56, 0x61}, /* the key beside the left shift on a 102 key board */
    {0x57, 0x78}, {0x58, 0x07}, /* F11 F12 */
    {0x5b, 0x1f}, {0x5c, 0x27}, /* the two meta keys, behind the prefix */
    {0x5d, 0x2f}, /* menu, behind the prefix */
};

static uint8_t gSet1ToSet2[256];
static uint8_t gSet2ToSet1[256];
static bool gSetTablesReady;

static void ps2_build_set_tables()
{
    if (gSetTablesReady) {
        return;
    }
    for (size_t i = 0; i < countof(kSetPairs); i++) {
        gSet1ToSet2[kSetPairs[i].set1] = kSetPairs[i].set2;
        /* The extended keys repeat the set 2 code of the key they mirror,
           and the first entry wins: both directions then describe the same
           pairing, which is what makes the round trip exact. */
        if (gSet2ToSet1[kSetPairs[i].set2] == 0) {
            gSet2ToSet1[kSetPairs[i].set2] = kSetPairs[i].set1;
        }
    }
    gSetTablesReady = true;
}


uint8_t ps2_set1_to_set2(uint8_t code)
{
    ps2_build_set_tables();
    return gSet1ToSet2[code];
}


uint8_t ps2_set2_to_set1(uint8_t code)
{
    ps2_build_set_tables();
    return gSet2ToSet1[code];
}


//#pragma mark - PS2Bus

bool PS2Bus::AssignResources(Device *dev)
{
    /* A device is reached through its controller's port, so it holds no host
       address space and no interrupt line of its own. */
    for (int i = 0; i < dev->ResourceCount(); i++) {
        if (dev->ResourceAt(i)->type != RES_NONE) {
            vm_error("ps2 bus: device '%s' declared a resource, but a PS/2 "
                     "device has none of its own\n", dev->Name());
            return false;
        }
    }
    return true;
}


//#pragma mark - PS2DeviceNode

PS2DeviceNode::PS2DeviceNode(const char *name, PS2Device *dev):
    Device(name), fDev(dev)
{
}


PS2DeviceNode::~PS2DeviceNode()
{
    delete fDev;
}


bool PS2DeviceNode::Realize()
{
    PS2Bus *bus = dynamic_cast<PS2Bus *>(ParentBus());
    if (bus == nullptr) {
        vm_error("%s: must be attached to a PS/2 bus\n", Name());
        return false;
    }
    return bus->Target()->AttachDevice(fDev);
}
