/*
 * HID tablet: an absolute pointing device
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

#include "cutils.h"
#include "devices.h"
#include "hid.h"
#include "machine.h"

/* One button byte, two sixteen bit axes and a wheel byte. */
#define TABLET_REPORT_SIZE 6

/* The range the report descriptor declares, which is the one the front ends
   already scale a pointer position into. */
#define TABLET_AXIS_MAX 0x7fff

#define TABLET_BUTTON_COUNT 3


/* An absolute pointer, declared as a mouse whose axes are absolute: a mouse
   driver reads the axes as declared, where a digitiser usage would need a
   driver that knows about pens and contacts. */
static const uint8_t kReportDesc[] = {
    0x05, 0x01,             /* Usage Page (Generic Desktop) */
    0x09, 0x02,             /* Usage (Mouse) */
    0xa1, 0x01,             /* Collection (Application) */
    0x09, 0x01,             /*   Usage (Pointer) */
    0xa1, 0x00,             /*   Collection (Physical) */

    /* the three buttons, one bit each, padded to a byte */
    0x05, 0x09,             /*     Usage Page (Button) */
    0x19, 0x01,             /*     Usage Minimum (Button 1) */
    0x29, 0x03,             /*     Usage Maximum (Button 3) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x25, 0x01,             /*     Logical Maximum (1) */
    0x95, 0x03,             /*     Report Count (3) */
    0x75, 0x01,             /*     Report Size (1) */
    0x81, 0x02,             /*     Input (Data, Variable, Absolute) */
    0x95, 0x01,             /*     Report Count (1) */
    0x75, 0x05,             /*     Report Size (5) */
    0x81, 0x01,             /*     Input (Constant) */

    /* the two absolute axes, over the whole declared range */
    0x05, 0x01,             /*     Usage Page (Generic Desktop) */
    0x09, 0x30,             /*     Usage (X) */
    0x09, 0x31,             /*     Usage (Y) */
    0x15, 0x00,             /*     Logical Minimum (0) */
    0x26, 0xff, 0x7f,       /*     Logical Maximum (32767) */
    0x35, 0x00,             /*     Physical Minimum (0) */
    0x46, 0xff, 0x7f,       /*     Physical Maximum (32767) */
    0x75, 0x10,             /*     Report Size (16) */
    0x95, 0x02,             /*     Report Count (2) */
    0x81, 0x02,             /*     Input (Data, Variable, Absolute) */

    /* the wheel, which is a displacement and not a position */
    0x05, 0x01,             /*     Usage Page (Generic Desktop) */
    0x09, 0x38,             /*     Usage (Wheel) */
    0x15, 0x81,             /*     Logical Minimum (-127) */
    0x25, 0x7f,             /*     Logical Maximum (127) */
    0x35, 0x00,             /*     Physical Minimum (0) */
    0x45, 0x00,             /*     Physical Maximum (0) */
    0x75, 0x08,             /*     Report Size (8) */
    0x95, 0x01,             /*     Report Count (1) */
    0x81, 0x06,             /*     Input (Data, Variable, Relative) */

    0xc0,                   /*   End Collection */
    0xc0,                   /* End Collection */
};


//#pragma mark - HIDTablet

class HIDTablet final: public HIDDevice, public InputEventTarget {
private:
    uint16_t fX = 0;
    uint16_t fY = 0;
    uint8_t fButtons = 0;

    int BuildReport(uint8_t *buf, int size, int wheel) const;

protected:
    int BuildInputReport(uint8_t *buf, int size) const override
        {return BuildReport(buf, size, 0);}

public:
    HIDTablet(): HIDDevice("hid-tablet") {}

    const uint8_t *ReportDescriptor(int *len) const override
        {*len = sizeof(kReportDesc); return kReportDesc;}

    int InputReportSize() const override {return TABLET_REPORT_SIZE;}
    void Reset() override;

    /* InputEventTarget */
    bool MouseIsAbsolute() override {return true;}
    void SendMouseEvent(int dx, int dy, int dz,
                        unsigned int buttons) override;
};


void HIDTablet::Reset()
{
    HIDDevice::Reset();
    fX = 0;
    fY = 0;
    fButtons = 0;
}


int HIDTablet::BuildReport(uint8_t *buf, int size, int wheel) const
{
    if (size < TABLET_REPORT_SIZE) {
        return -1;
    }
    if (wheel > 127) {
        wheel = 127;
    } else if (wheel < -127) {
        wheel = -127;
    }

    buf[0] = fButtons;
    put_le16(buf + 1, fX);
    put_le16(buf + 3, fY);
    buf[5] = (uint8_t)(int8_t)wheel;
    return TABLET_REPORT_SIZE;
}


void HIDTablet::SendMouseEvent(int dx, int dy, int dz, unsigned int buttons)
{
    uint8_t buf[TABLET_REPORT_SIZE];

    /* The front end already scaled the position; this guards the edge of the
       window. */
    if (dx < 0) {
        dx = 0;
    } else if (dx > TABLET_AXIS_MAX) {
        dx = TABLET_AXIS_MAX;
    }
    if (dy < 0) {
        dy = 0;
    } else if (dy > TABLET_AXIS_MAX) {
        dy = TABLET_AXIS_MAX;
    }

    fX = dx;
    fY = dy;
    fButtons = buttons & ((1 << TABLET_BUTTON_COUNT) - 1);

    int len = BuildReport(buf, sizeof(buf), dz);
    if (len > 0) {
        QueueInputReport(buf, len);
    }
}


//#pragma mark - factory

/* Claims the machine's pointer role once the function is attached. */
class HIDTabletNode final: public HIDDeviceNode {
private:
    DeviceContext *fCtx;
    HIDTablet *fTablet;

public:
    HIDTabletNode(HIDTablet *tablet, DeviceContext *ctx, int index):
        HIDDeviceNode("hid-tablet", tablet, index), fCtx(ctx),
        fTablet(tablet) {}

    bool Realize() override
    {
        if (!HIDDeviceNode::Realize()) {
            return false;
        }
        fCtx->mouse = fTablet;
        return true;
    }
};


Device *hid_tablet_node_create(DeviceContext *ctx, int index)
{
    return new HIDTabletNode(new HIDTablet(), ctx, index);
}
