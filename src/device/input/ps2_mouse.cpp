/*
 * PS/2 mouse
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
#include <stdio.h>

#include "ps2.h"

/* debug PS/2 mouse */
//#define DEBUG_MOUSE

/* Mouse commands */
#define AUX_SET_SCALE11 0xe6
#define AUX_SET_SCALE21 0xe7
#define AUX_SET_RES     0xe8
#define AUX_GET_SCALE   0xe9
#define AUX_SET_STREAM  0xea
#define AUX_POLL        0xeb
#define AUX_RESET_WRAP  0xec
#define AUX_SET_WRAP    0xee
#define AUX_SET_REMOTE  0xf0
#define AUX_GET_TYPE    0xf2
#define AUX_SET_SAMPLE  0xf3
#define AUX_ENABLE_DEV  0xf4
#define AUX_DISABLE_DEV 0xf5
#define AUX_SET_DEFAULT 0xf6
#define AUX_RESET       0xff

#define MOUSE_STATUS_REMOTE  0x40
#define MOUSE_STATUS_ENABLED 0x20
#define MOUSE_STATUS_SCALE21 0x10

/* What AUX_GET_TYPE reports, which is also what selects the packet
   format. The extra byte a wheel adds is only sent once a guest has run the
   sample rate sequence that asks for it. */
#define MOUSE_TYPE_PS2   0
#define MOUSE_TYPE_IMPS2 3 /* one wheel, one extra byte */
#define MOUSE_TYPE_IMEX  4 /* wheel and two more buttons */


/* A plain PS/2 pointer, which reports a wheel and two extra buttons to a
   guest that runs the sample rate knock for them. Nothing here depends on
   the controller it is behind: the packet bytes are the same everywhere. */
class PS2MouseDevice final: public PS2Mouse {
private:
    int32_t fWriteCmd = -1;
    uint8_t fStatus = 0;
    uint8_t fResolution = 0;
    uint8_t fSampleRate = 0;
    uint8_t fWrap = 0;
    uint8_t fType = MOUSE_TYPE_PS2;
    uint8_t fDetectState = 0;
    int fDx = 0; /* current values, needed for 'poll' mode */
    int fDy = 0;
    int fDz = 0;
    uint8_t fButtons = 0;

    void SendPacket();

public:
    PS2MouseDevice() {Reset();}

    void Write(uint8_t val) override;
    void Reset() override;
    void MouseEvent(int dx, int dy, int dz, int buttons_state) override;
};


void PS2MouseDevice::Reset()
{
    fWriteCmd = -1;
    fStatus = 0;
    fResolution = 0;
    fSampleRate = 0;
    fWrap = 0;
    fType = MOUSE_TYPE_PS2;
    fDetectState = 0;
    fDx = 0;
    fDy = 0;
    fDz = 0;
    fButtons = 0;
    ClearQueue();
}


void PS2MouseDevice::SendPacket()
{
    unsigned int b;
    int dx1, dy1, dz1;

    dx1 = fDx;
    dy1 = fDy;
    dz1 = fDz;
    /* XXX: increase range to 8 bits ? */
    if (dx1 > 127)
        dx1 = 127;
    else if (dx1 < -127)
        dx1 = -127;
    if (dy1 > 127)
        dy1 = 127;
    else if (dy1 < -127)
        dy1 = -127;
    b = 0x08 | ((dx1 < 0) << 4) | ((dy1 < 0) << 5) | (fButtons & 0x07);
    Queue(b);
    Queue(dx1 & 0xff);
    Queue(dy1 & 0xff);
    /* extra byte for IMPS/2 or IMEX */
    switch (fType) {
    default:
        break;
    case MOUSE_TYPE_IMPS2:
        if (dz1 > 127)
            dz1 = 127;
        else if (dz1 < -127)
            dz1 = -127;
        Queue(dz1 & 0xff);
        break;
    case MOUSE_TYPE_IMEX:
        if (dz1 > 7)
            dz1 = 7;
        else if (dz1 < -7)
            dz1 = -7;
        b = (dz1 & 0x0f) | ((fButtons & 0x18) << 1);
        Queue(b);
        break;
    }

    /* update deltas */
    fDx -= dx1;
    fDy -= dy1;
    fDz -= dz1;
}


void PS2MouseDevice::MouseEvent(int dx, int dy, int dz, int buttons_state)
{
    /* check if deltas are recorded when disabled */
    if (!(fStatus & MOUSE_STATUS_ENABLED))
        return;

    fDx += dx;
    fDy -= dy;
    fDz += dz;
    /* XXX: SDL sometimes generates nul events: we delete them */
    if (fDx == 0 && fDy == 0 && fDz == 0 && fButtons == buttons_state)
        return;
    fButtons = buttons_state;

    if (!(fStatus & MOUSE_STATUS_REMOTE) && QueueRoom() >= 16) {
        for (;;) {
            /* if not remote, send event. Multiple events are sent if
               too big deltas */
            SendPacket();
            if (fDx == 0 && fDy == 0 && fDz == 0)
                break;
        }
    }
}


void PS2MouseDevice::Write(uint8_t val)
{
#ifdef DEBUG_MOUSE
    printf("ps2: write mouse 0x%02x\n", val);
#endif
    switch (fWriteCmd) {
    default:
    case -1:
        /* mouse command */
        if (fWrap) {
            if (val == AUX_RESET_WRAP) {
                fWrap = 0;
                Queue(PS2_REPLY_ACK);
                return;
            } else if (val != AUX_RESET) {
                Queue(val);
                return;
            }
        }
        switch (val) {
        case AUX_SET_SCALE11:
            fStatus &= ~MOUSE_STATUS_SCALE21;
            Queue(PS2_REPLY_ACK);
            break;
        case AUX_SET_SCALE21:
            fStatus |= MOUSE_STATUS_SCALE21;
            Queue(PS2_REPLY_ACK);
            break;
        case AUX_SET_STREAM:
            fStatus &= ~MOUSE_STATUS_REMOTE;
            Queue(PS2_REPLY_ACK);
            break;
        case AUX_SET_WRAP:
            fWrap = 1;
            Queue(PS2_REPLY_ACK);
            break;
        case AUX_SET_REMOTE:
            fStatus |= MOUSE_STATUS_REMOTE;
            Queue(PS2_REPLY_ACK);
            break;
        case AUX_GET_TYPE:
            Queue(PS2_REPLY_ACK);
            Queue(fType);
            break;
        case AUX_SET_RES:
        case AUX_SET_SAMPLE:
            fWriteCmd = val;
            Queue(PS2_REPLY_ACK);
            break;
        case AUX_GET_SCALE:
            Queue(PS2_REPLY_ACK);
            Queue(fStatus);
            Queue(fResolution);
            Queue(fSampleRate);
            break;
        case AUX_POLL:
            Queue(PS2_REPLY_ACK);
            SendPacket();
            break;
        case AUX_ENABLE_DEV:
            fStatus |= MOUSE_STATUS_ENABLED;
            Queue(PS2_REPLY_ACK);
            break;
        case AUX_DISABLE_DEV:
            fStatus &= ~MOUSE_STATUS_ENABLED;
            Queue(PS2_REPLY_ACK);
            break;
        case AUX_SET_DEFAULT:
            fSampleRate = 100;
            fResolution = 2;
            fStatus = 0;
            Queue(PS2_REPLY_ACK);
            break;
        case AUX_RESET:
            fSampleRate = 100;
            fResolution = 2;
            fStatus = 0;
            fType = MOUSE_TYPE_PS2;
            Queue(PS2_REPLY_ACK);
            Queue(PS2_REPLY_POR);
            Queue(fType);
            break;
        default:
            break;
        }
        break;

    case AUX_SET_SAMPLE:
        fSampleRate = val;
        /* detect IMPS/2 or IMEX */
        switch (fDetectState) {
        default:
        case 0:
            if (val == 200)
                fDetectState = 1;
            break;
        case 1:
            if (val == 100)
                fDetectState = 2;
            else if (val == 200)
                fDetectState = 3;
            else
                fDetectState = 0;
            break;
        case 2:
            if (val == 80)
                fType = MOUSE_TYPE_IMPS2;
            fDetectState = 0;
            break;
        case 3:
            if (val == 80)
                fType = MOUSE_TYPE_IMEX;
            fDetectState = 0;
            break;
        }
        Queue(PS2_REPLY_ACK);
        fWriteCmd = -1;
        break;

    case AUX_SET_RES:
        fResolution = val;
        Queue(PS2_REPLY_ACK);
        fWriteCmd = -1;
        break;
    }
}


PS2Mouse *ps2_mouse_create()
{
    return new PS2MouseDevice();
}
