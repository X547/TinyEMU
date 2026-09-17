/*
 * HID keyboard
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
#include <string.h>

#include "bits.h"
#include "devices.h"
#include "hid.h"
#include "machine.h"

/* One modifier byte, one reserved byte, six key usages. Both protocols use
   this shape, so one builder serves them. */
#define KBD_REPORT_SIZE 8
#define KBD_KEYS_IN_REPORT 6

/* Held keys tracked beyond the six the report can name, so that releasing
   one brings the others back. */
#define KBD_MAX_PRESSED 16

/* Modifier usages occupy this range, one bit of byte zero each. */
#define KBD_USAGE_MODIFIER_FIRST 0xe0
#define KBD_USAGE_MODIFIER_LAST  0xe7

/* Reported in every key slot when more keys are down than fit. */
#define KBD_USAGE_ROLLOVER 0x01

/* Every evdev code the front ends produce fits below this. */
#define KBD_EVDEV_KEY_COUNT 128


/* Six key usages plus the eight modifiers, with the LEDs coming back. This is
   the shape a boot keyboard's report protocol is expected to have. */
static const uint8_t kReportDesc[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x06,             /* Usage (Keyboard) */
    0xa1, 0x01,             /* Collection (Application) */

    /* the eight modifier keys, one bit each */
    0x05, 0x07,             /*   Usage Page (Keyboard/Keypad) */
    0x19, 0xe0,             /*   Usage Minimum (Left Control) */
    0x29, 0xe7,             /*   Usage Maximum (Right GUI) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x25, 0x01,             /*   Logical Maximum (1) */
    0x75, 0x01,             /*   Report Size (1) */
    0x95, 0x08,             /*   Report Count (8) */
    0x81, 0x02,             /*   Input (Data, Variable, Absolute) */

    /* the reserved byte */
    0x95, 0x01,             /*   Report Count (1) */
    0x75, 0x08,             /*   Report Size (8) */
    0x81, 0x03,             /*   Input (Constant, Variable, Absolute) */

    /* the five LEDs, and three bits of padding to the byte */
    0x95, 0x05,             /*   Report Count (5) */
    0x75, 0x01,             /*   Report Size (1) */
    0x05, 0x08,             /*   Usage Page (LEDs) */
    0x19, 0x01,             /*   Usage Minimum (Num Lock) */
    0x29, 0x05,             /*   Usage Maximum (Kana) */
    0x91, 0x02,             /*   Output (Data, Variable, Absolute) */
    0x95, 0x01,             /*   Report Count (1) */
    0x75, 0x03,             /*   Report Size (3) */
    0x91, 0x03,             /*   Output (Constant, Variable, Absolute) */

    /* the six key slots */
    0x95, 0x06,             /*   Report Count (6) */
    0x75, 0x08,             /*   Report Size (8) */
    0x15, 0x00,             /*   Logical Minimum (0) */
    0x25, 0xff,             /*   Logical Maximum (255) */
    0x05, 0x07,             /*   Usage Page (Keyboard/Keypad) */
    0x19, 0x00,             /*   Usage Minimum (0) */
    0x29, 0xff,             /*   Usage Maximum (255) */
    0x81, 0x00,             /*   Input (Data, Array) */

    0xc0,                   /* End Collection */
};


/* Linux evdev key code to HID keyboard usage; zero for a key with none. */
static const uint8_t kKeyUsage[KBD_EVDEV_KEY_COUNT] = {
    /* 0 */
    0x00, 0x29, 0x1e, 0x1f, 0x20, 0x21, 0x22, 0x23,
    /* 8: 7 8 9 0 - = backspace tab */
    0x24, 0x25, 0x26, 0x27, 0x2d, 0x2e, 0x2a, 0x2b,
    /* 16: q w e r t y u i */
    0x14, 0x1a, 0x08, 0x15, 0x17, 0x1c, 0x18, 0x0c,
    /* 24: o p [ ] enter leftctrl a s */
    0x12, 0x13, 0x2f, 0x30, 0x28, 0xe0, 0x04, 0x16,
    /* 32: d f g h j k l ; */
    0x07, 0x09, 0x0a, 0x0b, 0x0d, 0x0e, 0x0f, 0x33,
    /* 40: ' ` leftshift backslash z x c v */
    0x34, 0x35, 0xe1, 0x31, 0x1d, 0x1b, 0x06, 0x19,
    /* 48: b n m , . / rightshift kp* */
    0x05, 0x11, 0x10, 0x36, 0x37, 0x38, 0xe5, 0x55,
    /* 56: leftalt space capslock f1 f2 f3 f4 f5 */
    0xe2, 0x2c, 0x39, 0x3a, 0x3b, 0x3c, 0x3d, 0x3e,
    /* 64: f6 f7 f8 f9 f10 numlock scrolllock kp7 */
    0x3f, 0x40, 0x41, 0x42, 0x43, 0x53, 0x47, 0x5f,
    /* 72: kp8 kp9 kp- kp4 kp5 kp6 kp+ kp1 */
    0x60, 0x61, 0x56, 0x5c, 0x5d, 0x5e, 0x57, 0x59,
    /* 80: kp2 kp3 kp0 kp. -- zenkakuhankaku 102nd f11 */
    0x5a, 0x5b, 0x62, 0x63, 0x00, 0x94, 0x64, 0x44,
    /* 88: f12 ro katakana hiragana henkan katakanahiragana muhenkan kpjpcomma */
    0x45, 0x87, 0x92, 0x93, 0x8a, 0x88, 0x8b, 0x8c,
    /* 96: kpenter rightctrl kp/ sysrq rightalt linefeed home up */
    0x58, 0xe4, 0x54, 0x46, 0xe6, 0x00, 0x4a, 0x52,
    /* 104: pageup left right end down pagedown insert delete */
    0x4b, 0x50, 0x4f, 0x4d, 0x51, 0x4e, 0x49, 0x4c,
    /* 112: macro mute volumedown volumeup power kp= kp+- pause */
    0x00, 0x7f, 0x81, 0x80, 0x66, 0x67, 0xd7, 0x48,
    /* 120: scale kp, hangeul hanja yen leftmeta rightmeta compose */
    0x00, 0x85, 0x90, 0x91, 0x89, 0xe3, 0xe7, 0x65,
};


//#pragma mark - HIDKeyboard

class HIDKeyboard final: public HIDDevice, public KeyboardTarget {
private:
    uint8_t fModifiers = 0;
    uint8_t fPressed[KBD_MAX_PRESSED] {}; /* held usages, in press order */
    int fPressedCount = 0;
    uint8_t fLeds = 0;

    void PressUsage(uint8_t usage);
    void ReleaseUsage(uint8_t usage);

protected:
    int BuildInputReport(uint8_t *buf, int size) const override;

public:
    HIDKeyboard(): HIDDevice("hid-keyboard") {}

    const uint8_t *ReportDescriptor(int *len) const override
        {*len = sizeof(kReportDesc); return kReportDesc;}

    int InputReportSize() const override {return KBD_REPORT_SIZE;}
    HIDBootProtocolEnum BootProtocol() const override
        {return HID_BOOT_KEYBOARD;}

    bool SetReport(uint8_t type, const uint8_t *buf, int len) override;
    void Reset() override;

    /* KeyboardTarget */
    void SendKeyEvent(bool is_down, uint16_t key_code) override;
};


void HIDKeyboard::Reset()
{
    HIDDevice::Reset();
    fModifiers = 0;
    fPressedCount = 0;
    fLeds = 0;
}


int HIDKeyboard::BuildInputReport(uint8_t *buf, int size) const
{
    if (size < KBD_REPORT_SIZE) {
        return -1;
    }
    memset(buf, 0, KBD_REPORT_SIZE);
    buf[0] = fModifiers;

    if (fPressedCount > KBD_KEYS_IN_REPORT) {
        /* More keys are down than the report can name. */
        for (int i = 0; i < KBD_KEYS_IN_REPORT; i++) {
            buf[2 + i] = KBD_USAGE_ROLLOVER;
        }
        return KBD_REPORT_SIZE;
    }
    for (int i = 0; i < fPressedCount; i++) {
        buf[2 + i] = fPressed[i];
    }
    return KBD_REPORT_SIZE;
}


void HIDKeyboard::PressUsage(uint8_t usage)
{
    for (int i = 0; i < fPressedCount; i++) {
        if (fPressed[i] == usage) {
            /* Already down: the front end is repeating the key. */
            return;
        }
    }
    if (fPressedCount >= KBD_MAX_PRESSED) {
        return;
    }
    fPressed[fPressedCount++] = usage;
}


void HIDKeyboard::ReleaseUsage(uint8_t usage)
{
    for (int i = 0; i < fPressedCount; i++) {
        if (fPressed[i] != usage) {
            continue;
        }
        /* Keep the press order of the rest. */
        for (int j = i; j + 1 < fPressedCount; j++) {
            fPressed[j] = fPressed[j + 1];
        }
        fPressedCount--;
        return;
    }
}


void HIDKeyboard::SendKeyEvent(bool is_down, uint16_t key_code)
{
    uint8_t buf[KBD_REPORT_SIZE];

    if (key_code >= KBD_EVDEV_KEY_COUNT) {
        return;
    }
    uint8_t usage = kKeyUsage[key_code];
    if (usage == 0) {
        return;
    }

    if (usage >= KBD_USAGE_MODIFIER_FIRST && usage <= KBD_USAGE_MODIFIER_LAST) {
        fModifiers = set_bit(fModifiers,
                             usage - KBD_USAGE_MODIFIER_FIRST,
                             is_down);
    } else if (is_down) {
        PressUsage(usage);
    } else {
        ReleaseUsage(usage);
    }

    int len = BuildInputReport(buf, sizeof(buf));
    if (len > 0) {
        QueueInputReport(buf, len);
    }
}


bool HIDKeyboard::SetReport(uint8_t type, const uint8_t *buf, int len)
{
    if (type != HID_REPORT_OUTPUT || len < 1) {
        return false;
    }
    /* The LED state; nothing here displays it. */
    fLeds = buf[0];
    return true;
}


//#pragma mark - factory

/* Claims the machine's keyboard role once the function is attached. */
class HIDKeyboardNode final: public HIDDeviceNode {
private:
    DeviceContext *fCtx;
    HIDKeyboard *fKeyboard;

public:
    HIDKeyboardNode(HIDKeyboard *keyboard, DeviceContext *ctx, int index):
        HIDDeviceNode("hid-keyboard", keyboard, index), fCtx(ctx),
        fKeyboard(keyboard) {}

    bool Realize() override
    {
        if (!HIDDeviceNode::Realize()) {
            return false;
        }
        fCtx->keyboard = fKeyboard;
        return true;
    }
};


Device *hid_keyboard_node_create(DeviceContext *ctx, int index)
{
    return new HIDKeyboardNode(new HIDKeyboard(), ctx, index);
}
