/*
 * PS/2 keyboard
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
#include <stdint.h>

#include "ps2.h"

/* Keyboard commands */
#define KBD_CMD_SET_LEDS      0xed
#define KBD_CMD_ECHO          0xee
#define KBD_CMD_SCANCODE      0xf0 /* select or report the scancode set */
#define KBD_CMD_GET_ID        0xf2
#define KBD_CMD_SET_RATE      0xf3
#define KBD_CMD_ENABLE        0xf4 /* enable scanning */
#define KBD_CMD_RESET_DISABLE 0xf5 /* reset and disable scanning */
#define KBD_CMD_RESET_ENABLE  0xf6 /* reset and enable scanning */
#define KBD_CMD_RESET         0xff

/* What a keyboard answers KBD_CMD_GET_ID with. A guest reaching it through a
   translating controller sees the second byte translated, and the drivers
   that care know both spellings. */
#define KBD_ID_BYTE0 0xab
#define KBD_ID_BYTE1 0x83

/* The set a keyboard powers up in. Set 1 exists because a guest may ask for
   it; set 3 is not modelled, and a guest that asks for it is told so by
   reading the set back. */
#define KBD_SCANCODE_SET_DEFAULT 2

/* Evdev key codes below this are the set 1 make code of the same key: the
   Linux key code table was drawn up from the set 1 scancodes and never
   diverged over this range. Above it a code stands for a key that set 1
   reaches through the extend prefix, and the table below says which. */
#define KBD_EVDEV_DIRECT_MAX   95
#define KBD_EVDEV_EXTENDED_MAX 127

static const uint8_t kEvdevToSet1Extended[
        KBD_EVDEV_EXTENDED_MAX - KBD_EVDEV_DIRECT_MAX] = {
    0x1c, 0x1d, 0x35, 0x00, 0x38, 0x00, 0x47, 0x48,
    0x49, 0x4b, 0x4d, 0x4f, 0x50, 0x51, 0x52, 0x53,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x5b, 0x5c, 0x5d,
};


/* A plain AT keyboard. It emits whichever scancode set the guest selected,
   set 2 until one does; converting that to the set 1 a PC guest expects is
   the controller's job, because only a controller with an AT translator does
   it. */
class PS2KeyboardDevice final: public PS2Keyboard {
private:
    int32_t fWriteCmd = -1;
    int fScanSet = KBD_SCANCODE_SET_DEFAULT;
    bool fScanEnabled = true;

    void QueueScancode(bool is_down, uint8_t set1, bool extended);

public:
    PS2KeyboardDevice() {Reset();}

    void Write(uint8_t val) override;
    void Reset() override;
    void PutKeycode(bool is_down, int keycode) override;
};


void PS2KeyboardDevice::Reset()
{
    fWriteCmd = -1;
    fScanSet = KBD_SCANCODE_SET_DEFAULT;
    fScanEnabled = true;
    ClearQueue();
}


void PS2KeyboardDevice::QueueScancode(bool is_down, uint8_t set1,
                                      bool extended)
{
    if (extended) {
        /* The prefix means the same thing in every set. */
        Queue(PS2_SCAN_EXTEND, true);
    }
    if (fScanSet == 1) {
        Queue(set1 | (is_down ? 0x00 : 0x80), true);
        return;
    }
    uint8_t set2 = ps2_set1_to_set2(set1);
    if (set2 == 0) {
        return;
    }
    if (!is_down) {
        Queue(PS2_SCAN_RELEASE, true);
    }
    Queue(set2, true);
}


void PS2KeyboardDevice::PutKeycode(bool is_down, int keycode)
{
    if (!fScanEnabled) {
        return;
    }

    bool extended = false;
    if (keycode > KBD_EVDEV_DIRECT_MAX) {
        if (keycode > KBD_EVDEV_EXTENDED_MAX) {
            return;
        }
        keycode = kEvdevToSet1Extended[keycode - KBD_EVDEV_DIRECT_MAX - 1];
        if (keycode == 0) {
            return;
        }
        extended = true;
    }
    QueueScancode(is_down, (uint8_t)keycode, extended);
}


void PS2KeyboardDevice::Write(uint8_t val)
{
    switch (fWriteCmd) {
    default:
    case -1:
        switch (val) {
        case 0x00:
            Queue(PS2_REPLY_ACK);
            break;
        case 0x05:
            Queue(PS2_REPLY_RESEND);
            break;
        case KBD_CMD_GET_ID:
            Queue(PS2_REPLY_ACK);
            Queue(KBD_ID_BYTE0);
            Queue(KBD_ID_BYTE1);
            break;
        case KBD_CMD_ECHO:
            Queue(KBD_CMD_ECHO);
            break;
        case KBD_CMD_ENABLE:
            fScanEnabled = true;
            Queue(PS2_REPLY_ACK);
            break;
        case KBD_CMD_SET_LEDS:
        case KBD_CMD_SET_RATE:
        case KBD_CMD_SCANCODE:
            fWriteCmd = val;
            Queue(PS2_REPLY_ACK);
            break;
        case KBD_CMD_RESET_DISABLE:
            Reset();
            fScanEnabled = false;
            Queue(PS2_REPLY_ACK);
            break;
        case KBD_CMD_RESET_ENABLE:
            Reset();
            Queue(PS2_REPLY_ACK);
            break;
        case KBD_CMD_RESET:
            Reset();
            Queue(PS2_REPLY_ACK);
            Queue(PS2_REPLY_POR);
            break;
        default:
            Queue(PS2_REPLY_ACK);
            break;
        }
        break;

    case KBD_CMD_SCANCODE:
        Queue(PS2_REPLY_ACK);
        if (val == 0) {
            /* Report rather than select. The set number goes back as a plain
               reply rather than as a scancode, so a guest that asked for a
               set this keyboard does not have reads the one it really got
               and falls back, whichever controller it is behind. */
            Queue((uint8_t)fScanSet);
        } else if (val == 1 || val == 2) {
            fScanSet = val;
        }
        /* Set 3 is acknowledged and ignored: a driver that wants it reads
           the set back, finds the one it did not ask for, and falls back. */
        fWriteCmd = -1;
        break;

    case KBD_CMD_SET_LEDS:
    case KBD_CMD_SET_RATE:
        Queue(PS2_REPLY_ACK);
        fWriteCmd = -1;
        break;
    }
}


Device *ps2_keyboard_node_create(int port)
{
    return new PS2DeviceNode("ps2-keyboard",
                             std::make_unique<PS2KeyboardDevice>(), port);
}
