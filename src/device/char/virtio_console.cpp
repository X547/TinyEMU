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
#include "devices.h"
#include "virtio.h"


#define VIRTIO_CONSOLE_DEVICE_ID 3

struct VIRTIOConsoleDevice final: public VIRTIODevice, public ConsoleTarget {
    DeviceContext *ctx;
    /* null discards the output */
    HostConsole *cs = nullptr;

    VIRTIOConsoleDevice(DeviceContext *ctx);

    bool Realize() override;

    int RecvRequest(int queue_idx, int desc_idx, int read_size,
                    int write_size) override;
    void ManualQueueNotify(int queue_idx) override
    {
        if (queue_idx == 0 && cs != nullptr)
            cs->TargetReady();
    }

    /* ConsoleTarget */
    int ReceiveRoom() override;
    void Receive(const uint8_t *buf, int len) override;
    void Resize(int width, int height) override;
};

int VIRTIOConsoleDevice::RecvRequest(int queue_idx, int desc_idx, int read_size,
                                     int write_size)
{
    (void)write_size;

    if (queue_idx == 1) {
        /* to the console, a piece at a time however long the buffer is */
        uint8_t buf[4096];
        int len;
        for (int pos = 0; pos < read_size; pos += len) {
            len = min_int(read_size - pos, sizeof(buf));
            if (memcpy_from_queue(this, buf, queue_idx, desc_idx, pos,
                                  len) < 0)
                break;
            if (cs != nullptr)
                cs->WriteData(buf, len);
        }
        virtio_consume_desc(this, queue_idx, desc_idx, 0);
    }
    return 0;
}

/* the size of the receive buffer the guest has queued, 0 if none */
int VIRTIOConsoleDevice::ReceiveRoom()
{
    int desc_idx, read_size, write_size;

    if (!virtio_queue_peek(this, 0, &desc_idx))
        return 0;
    if (get_desc_rw_size(this, &read_size, &write_size, 0, desc_idx) < 0) {
        virtio_set_needs_reset(this);
        return 0;
    }
    return write_size;
}

void VIRTIOConsoleDevice::Receive(const uint8_t *buf, int buf_len)
{
    int desc_idx;

    if (!virtio_queue_peek(this, 0, &desc_idx))
        return;
    memcpy_to_queue(this, 0, desc_idx, 0, buf, buf_len);
    virtio_consume_desc(this, 0, desc_idx, buf_len);
    virtio_queue_pop(this, 0);
}

/* send a resize event */
void VIRTIOConsoleDevice::Resize(int width, int height)
{
    VIRTIODevice *s = this;

    /* indicate the console size */
    put_le16(s->config_space + 0, width);
    put_le16(s->config_space + 2, height);

    virtio_config_change_notify(s);
}

VIRTIOConsoleDevice::VIRTIOConsoleDevice(DeviceContext *ctx):
    VIRTIODevice("virtio-console", VIRTIO_CONSOLE_DEVICE_ID,
                 0x0780 /* communication */, 4, 2),
    ctx(ctx)
{
    device_features = (1 << 0); /* VIRTIO_CONSOLE_F_SIZE */
    queue[0].manual_recv = true;
}

bool VIRTIOConsoleDevice::Realize()
{
    if (!VIRTIODevice::Realize())
        return false;
    cs = ctx->platform->Console();
    ctx->console_input = this;
    return true;
}


//#pragma mark - class

class VirtioConsoleClass final: public DeviceClass {
public:
    VirtioConsoleClass(): DeviceClass("virtio-console") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        (void)cfg;
        return new VIRTIOConsoleDevice(ctx);
    }
};

static const VirtioConsoleClass sVirtioConsoleClass;
