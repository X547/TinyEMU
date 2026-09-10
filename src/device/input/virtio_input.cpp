/*
 * VIRTIO driver
 * 
 * Copyright (c) 2016 Fabrice Bellard
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>

#include "cutils.h"
#include "virtio.h"
#include "virtio_priv.h"


enum {
    VIRTIO_INPUT_CFG_UNSET      = 0x00,
    VIRTIO_INPUT_CFG_ID_NAME    = 0x01,
    VIRTIO_INPUT_CFG_ID_SERIAL  = 0x02,
    VIRTIO_INPUT_CFG_ID_DEVIDS  = 0x03,
    VIRTIO_INPUT_CFG_PROP_BITS  = 0x10,
    VIRTIO_INPUT_CFG_EV_BITS    = 0x11,
    VIRTIO_INPUT_CFG_ABS_INFO   = 0x12,
};

#define VIRTIO_INPUT_EV_SYN 0x00
#define VIRTIO_INPUT_EV_KEY 0x01
#define VIRTIO_INPUT_EV_REL 0x02
#define VIRTIO_INPUT_EV_ABS 0x03
#define VIRTIO_INPUT_EV_REP 0x14

#define BTN_LEFT         0x110
#define BTN_RIGHT        0x111
#define BTN_MIDDLE       0x112
#define BTN_GEAR_DOWN    0x150
#define BTN_GEAR_UP      0x151

#define REL_X 0x00
#define REL_Y 0x01
#define REL_Z 0x02
#define REL_WHEEL 0x08

#define ABS_X 0x00
#define ABS_Y 0x01
#define ABS_Z 0x02

struct VIRTIOInputDevice: public VIRTIODevice {
    VirtioInputTypeEnum type {};
    uint32_t buttons_state = 0;

    int RecvRequest(int queue_idx, int desc_idx, int read_size,
                    int write_size) override;
    void ConfigWrite() override;
};

static const uint16_t buttons_list[] = {
    BTN_LEFT, BTN_RIGHT, BTN_MIDDLE
};

int VIRTIOInputDevice::RecvRequest(int queue_idx, int desc_idx, int read_size,
                                   int write_size)
{
    VIRTIODevice *s = this;
    if (queue_idx == 1) {
        /* led & keyboard updates */
        //        printf("%s: write_size=%d\n", __func__, write_size);
        virtio_consume_desc(s, queue_idx, desc_idx, 0);
    }
    return 0;
}

/* return < 0 if could not send key event */
static int virtio_input_queue_event(VIRTIODevice *s,
                                    uint16_t type, uint16_t code,
                                    uint32_t value)
{
    int queue_idx = 0;
    QueueState *qs = &s->queue[queue_idx];
    int desc_idx, buf_len;
    uint16_t avail_idx;
    uint8_t buf[8];

    if (!qs->ready)
        return -1;

    put_le16(buf, type);
    put_le16(buf + 2, code);
    put_le32(buf + 4, value);
    buf_len = 8;
    
    avail_idx = virtio_read16(s, qs->avail_addr + 2);
    if (qs->last_avail_idx == avail_idx)
        return -1;
    desc_idx = virtio_read16(s, qs->avail_addr + 4 + 
                             (qs->last_avail_idx & (qs->num - 1)) * 2);
    //    printf("send: queue_idx=%d desc_idx=%d\n", queue_idx, desc_idx);
    memcpy_to_queue(s, queue_idx, desc_idx, 0, buf, buf_len);
    virtio_consume_desc(s, queue_idx, desc_idx, buf_len);
    qs->last_avail_idx++;
    return 0;
}

int virtio_input_send_key_event(VIRTIODevice *s, bool is_down,
                                uint16_t key_code)
{
    VIRTIOInputDevice *s1 = (VIRTIOInputDevice *)s;
    int ret;
    
    if (s1->type != VIRTIO_INPUT_TYPE_KEYBOARD)
        return -1;
    ret = virtio_input_queue_event(s, VIRTIO_INPUT_EV_KEY, key_code, is_down);
    if (ret)
        return ret;
    return virtio_input_queue_event(s, VIRTIO_INPUT_EV_SYN, 0, 0);
}

/* also used for the tablet */
int virtio_input_send_mouse_event(VIRTIODevice *s, int dx, int dy, int dz,
                                  unsigned int buttons)
{
    VIRTIOInputDevice *s1 = (VIRTIOInputDevice *)s;
    int ret, i, b, last_b;

    if (s1->type != VIRTIO_INPUT_TYPE_MOUSE &&
        s1->type != VIRTIO_INPUT_TYPE_TABLET)
        return -1;
    if (s1->type == VIRTIO_INPUT_TYPE_MOUSE) {
        ret = virtio_input_queue_event(s, VIRTIO_INPUT_EV_REL, REL_X, dx);
        if (ret != 0)
            return ret;
        ret = virtio_input_queue_event(s, VIRTIO_INPUT_EV_REL, REL_Y, dy);
        if (ret != 0)
            return ret;
    } else {
        ret = virtio_input_queue_event(s, VIRTIO_INPUT_EV_ABS, ABS_X, dx);
        if (ret != 0)
            return ret;
        ret = virtio_input_queue_event(s, VIRTIO_INPUT_EV_ABS, ABS_Y, dy);
        if (ret != 0)
            return ret;
    }
    if (dz != 0) {
        ret = virtio_input_queue_event(s, VIRTIO_INPUT_EV_REL, REL_WHEEL, dz);
        if (ret != 0)
            return ret;
    }

    if (buttons != s1->buttons_state) {
        for(i = 0; i < countof(buttons_list); i++) {
            b = (buttons >> i) & 1;
            last_b = (s1->buttons_state >> i) & 1;
            if (b != last_b) {
                ret = virtio_input_queue_event(s, VIRTIO_INPUT_EV_KEY,
                                               buttons_list[i], b);
                if (ret != 0)
                    return ret;
            }
        }
        s1->buttons_state = buttons;
    }

    return virtio_input_queue_event(s, VIRTIO_INPUT_EV_SYN, 0, 0);
}

static void set_bit(uint8_t *tab, int k)
{
    tab[k >> 3] |= 1 << (k & 7);
}

void VIRTIOInputDevice::ConfigWrite()
{
    VIRTIODevice *s = this;
    VIRTIOInputDevice *s1 = (VIRTIOInputDevice *)s;
    uint8_t *config = s->config_space;
    int i;
    
    //    printf("config_write: %02x %02x\n", config[0], config[1]);
    switch(config[0]) {
    case VIRTIO_INPUT_CFG_UNSET:
        break;
    case VIRTIO_INPUT_CFG_ID_NAME:
        {
            const char *name;
            int len;
            switch(s1->type) {
            case VIRTIO_INPUT_TYPE_KEYBOARD:
                name = "virtio_keyboard";
                break;
            case VIRTIO_INPUT_TYPE_MOUSE:
                name = "virtio_mouse";
                break;
            case VIRTIO_INPUT_TYPE_TABLET:
                name = "virtio_tablet";
                break;
            default:
                abort();
            }
            len = strlen(name);
            config[2] = len;
            memcpy(config + 8, name, len);
        }
        break;
    default:
    case VIRTIO_INPUT_CFG_ID_SERIAL:
    case VIRTIO_INPUT_CFG_ID_DEVIDS:
    case VIRTIO_INPUT_CFG_PROP_BITS:
        config[2] = 0; /* size of reply */
        break;
    case VIRTIO_INPUT_CFG_EV_BITS:
        config[2] = 0;
        switch(s1->type) {
        case VIRTIO_INPUT_TYPE_KEYBOARD:
            switch(config[1]) {
            case VIRTIO_INPUT_EV_KEY:
                config[2] = 128 / 8;
                memset(config + 8, 0xff, 128 / 8); /* bitmap */
                break;
            case VIRTIO_INPUT_EV_REP: /* allow key repetition */
                config[2] = 1;
                break;
            default:
                break;
            }
            break;
        case VIRTIO_INPUT_TYPE_MOUSE:
            switch(config[1]) {
            case VIRTIO_INPUT_EV_KEY:
                config[2] = 512 / 8;
                memset(config + 8, 0, 512 / 8); /* bitmap */
                for(i = 0; i < countof(buttons_list); i++)
                    set_bit(config + 8, buttons_list[i]);
                break;
            case VIRTIO_INPUT_EV_REL:
                config[2] = 2;
                config[8] = 0;
                config[9] = 0;
                set_bit(config + 8, REL_X);
                set_bit(config + 8, REL_Y);
                set_bit(config + 8, REL_WHEEL);
                break;
            default:
                break;
            }
            break;
        case VIRTIO_INPUT_TYPE_TABLET:
            switch(config[1]) {
            case VIRTIO_INPUT_EV_KEY:
                config[2] = 512 / 8;
                memset(config + 8, 0, 512 / 8); /* bitmap */
                for(i = 0; i < countof(buttons_list); i++)
                    set_bit(config + 8, buttons_list[i]);
                break;
            case VIRTIO_INPUT_EV_REL:
                config[2] = 2;
                config[8] = 0;
                config[9] = 0;
                set_bit(config + 8, REL_WHEEL);
                break;
            case VIRTIO_INPUT_EV_ABS:
                config[2] = 1;
                config[8] = 0;
                set_bit(config + 8, ABS_X);
                set_bit(config + 8, ABS_Y);
                break;
            default:
                break;
            }
            break;
        default:
            abort();
        }
        break;
    case VIRTIO_INPUT_CFG_ABS_INFO:
        if (s1->type == VIRTIO_INPUT_TYPE_TABLET && config[1] <= 1) {
            /* for ABS_X and ABS_Y */
            config[2] = 5 * 4;
            put_le32(config + 8, 0); /* min */
            put_le32(config + 12, VIRTIO_INPUT_ABS_SCALE - 1) ; /* max */
            put_le32(config + 16, 0); /* fuzz */
            put_le32(config + 20, 0); /* flat */
            put_le32(config + 24, 0); /* res */
        }
        break;
    }
}

VIRTIODevice *virtio_input_init(VIRTIOBusDef *bus, VirtioInputTypeEnum type)
{
    VIRTIOInputDevice *s;

    s = new VIRTIOInputDevice();
    virtio_init(s, bus, 18, 256);
    s->queue[0].manual_recv = true;
    s->device_features = 0;
    s->type = type;
    return s;
}
