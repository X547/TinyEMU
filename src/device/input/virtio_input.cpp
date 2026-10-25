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

#include "bits.h"
#include "cutils.h"
#include "devices.h"
#include "virtio.h"


#define VIRTIO_INPUT_DEVICE_ID 18

typedef enum {
    VIRTIO_INPUT_TYPE_KEYBOARD,
    VIRTIO_INPUT_TYPE_MOUSE,
    VIRTIO_INPUT_TYPE_TABLET,
} VirtioInputTypeEnum;

#define VIRTIO_INPUT_ABS_SCALE 32768

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

struct VIRTIOInputDevice final: public VIRTIODevice, public KeyboardTarget,
                                public PointerTarget {
    DeviceContext *ctx;
    VirtioInputTypeEnum type;
    uint32_t buttons_state = 0;

    VIRTIOInputDevice(DeviceContext *ctx, VirtioInputTypeEnum type);

    bool Realize() override;

    int RecvRequest(int queue_idx, int desc_idx, int read_size,
                    int write_size) override;
    void ConfigWrite() override;

    /* KeyboardTarget */
    void SendKeyEvent(bool is_down, uint16_t key_code) override;

    /* PointerTarget */
    void SendMouseEvent(int dx, int dy, int dz,
                        unsigned int buttons) override;
    bool MouseIsAbsolute() override {return type == VIRTIO_INPUT_TYPE_TABLET;}
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
    int desc_idx, buf_len;
    uint8_t buf[8];

    if (!virtio_queue_peek(s, queue_idx, &desc_idx))
        return -1;

    put_le16(buf, type);
    put_le16(buf + 2, code);
    put_le32(buf + 4, value);
    buf_len = 8;

    memcpy_to_queue(s, queue_idx, desc_idx, 0, buf, buf_len);
    virtio_consume_desc(s, queue_idx, desc_idx, buf_len);
    virtio_queue_pop(s, queue_idx);
    return 0;
}

static int virtio_input_send_key_event(VIRTIOInputDevice *s, bool is_down,
                                       uint16_t key_code)
{
    int ret;

    if (s->type != VIRTIO_INPUT_TYPE_KEYBOARD)
        return -1;
    ret = virtio_input_queue_event(s, VIRTIO_INPUT_EV_KEY, key_code, is_down);
    if (ret)
        return ret;
    return virtio_input_queue_event(s, VIRTIO_INPUT_EV_SYN, 0, 0);
}

/* also used for the tablet */
static int virtio_input_send_mouse_event(VIRTIOInputDevice *s, int dx, int dy,
                                         int dz, unsigned int buttons)
{
    int ret, i, b, last_b;

    if (s->type != VIRTIO_INPUT_TYPE_MOUSE &&
        s->type != VIRTIO_INPUT_TYPE_TABLET)
        return -1;
    if (s->type == VIRTIO_INPUT_TYPE_MOUSE) {
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

    if (buttons != s->buttons_state) {
        for(i = 0; i < countof(buttons_list); i++) {
            b = get_bit(buttons, i);
            last_b = get_bit(s->buttons_state, i);
            if (b != last_b) {
                ret = virtio_input_queue_event(s, VIRTIO_INPUT_EV_KEY,
                                               buttons_list[i], b);
                if (ret != 0)
                    return ret;
            }
        }
        s->buttons_state = buttons;
    }

    return virtio_input_queue_event(s, VIRTIO_INPUT_EV_SYN, 0, 0);
}

void VIRTIOInputDevice::SendKeyEvent(bool is_down, uint16_t key_code)
{
    virtio_input_send_key_event(this, is_down, key_code);
}

void VIRTIOInputDevice::SendMouseEvent(int dx, int dy, int dz,
                                       unsigned int buttons)
{
    virtio_input_send_mouse_event(this, dx, dy, dz, buttons);
}

static void bitmap_set(uint8_t *tab, int k)
{
    tab[k / 8] = set_bit(tab[k / 8], k % 8, true);
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
                    bitmap_set(config + 8, buttons_list[i]);
                break;
            case VIRTIO_INPUT_EV_REL:
                config[2] = 2;
                config[8] = 0;
                config[9] = 0;
                bitmap_set(config + 8, REL_X);
                bitmap_set(config + 8, REL_Y);
                bitmap_set(config + 8, REL_WHEEL);
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
                    bitmap_set(config + 8, buttons_list[i]);
                break;
            case VIRTIO_INPUT_EV_REL:
                config[2] = 2;
                config[8] = 0;
                config[9] = 0;
                bitmap_set(config + 8, REL_WHEEL);
                break;
            case VIRTIO_INPUT_EV_ABS:
                config[2] = 1;
                config[8] = 0;
                bitmap_set(config + 8, ABS_X);
                bitmap_set(config + 8, ABS_Y);
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

VIRTIOInputDevice::VIRTIOInputDevice(DeviceContext *ctx,
                                     VirtioInputTypeEnum type):
    VIRTIODevice("virtio-input", VIRTIO_INPUT_DEVICE_ID,
                 0x0980 /* other input */, 256, 2),
    ctx(ctx),
    type(type)
{
    queue[0].manual_recv = true;
    device_features = 0;
}

bool VIRTIOInputDevice::Realize()
{
    if (!VIRTIODevice::Realize())
        return false;
    if (type == VIRTIO_INPUT_TYPE_KEYBOARD) {
        ctx->keyboard = this;
    } else {
        ctx->mouse = this;
    }
    return true;
}


//#pragma mark - class

class VirtioInputClass final: public DeviceClass {
public:
    VirtioInputClass(): DeviceClass("virtio-input") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        const char *kind;
        VirtioInputTypeEnum input_type;

        if (!cfg.GetStr("kind", &kind)) {
            return nullptr;
        }
        if (strcmp(kind, "keyboard") == 0) {
            input_type = VIRTIO_INPUT_TYPE_KEYBOARD;
        } else if (strcmp(kind, "mouse") == 0) {
            input_type = VIRTIO_INPUT_TYPE_MOUSE;
        } else if (strcmp(kind, "tablet") == 0) {
            input_type = VIRTIO_INPUT_TYPE_TABLET;
        } else {
            vm_error("virtio-input: unsupported kind '%s'\n", kind);
            return nullptr;
        }
        return new VIRTIOInputDevice(ctx, input_type);
    }
};

static const VirtioInputClass sVirtioInputClass;
