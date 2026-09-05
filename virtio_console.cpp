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


struct VIRTIOConsoleDevice: public VIRTIODevice {
    CharacterDevice *cs = nullptr;

    int RecvRequest(int queue_idx, int desc_idx, int read_size,
                    int write_size) override;
};

int VIRTIOConsoleDevice::RecvRequest(int queue_idx, int desc_idx, int read_size,
                                     int write_size)
{
    VIRTIODevice *s = this;
    VIRTIOConsoleDevice *s1 = (VIRTIOConsoleDevice *)s;
    CharacterDevice *cs = s1->cs;
    uint8_t *buf;

    if (queue_idx == 1) {
        /* send to console */
        buf = static_cast<uint8_t *>(malloc(read_size));
        memcpy_from_queue(s, buf, queue_idx, desc_idx, 0, read_size);
        cs->WriteData(buf, read_size);
        free(buf);
        virtio_consume_desc(s, queue_idx, desc_idx, 0);
    }
    return 0;
}

bool virtio_console_can_write_data(VIRTIODevice *s)
{
    QueueState *qs = &s->queue[0];
    uint16_t avail_idx;

    if (!qs->ready)
        return false;
    avail_idx = virtio_read16(s, qs->avail_addr + 2);
    return qs->last_avail_idx != avail_idx;
}

int virtio_console_get_write_len(VIRTIODevice *s)
{
    int queue_idx = 0;
    QueueState *qs = &s->queue[queue_idx];
    int desc_idx;
    int read_size, write_size;
    uint16_t avail_idx;

    if (!qs->ready)
        return 0;
    avail_idx = virtio_read16(s, qs->avail_addr + 2);
    if (qs->last_avail_idx == avail_idx)
        return 0;
    desc_idx = virtio_read16(s, qs->avail_addr + 4 + 
                             (qs->last_avail_idx & (qs->num - 1)) * 2);
    if (get_desc_rw_size(s, &read_size, &write_size, queue_idx, desc_idx))
        return 0;
    return write_size;
}

int virtio_console_write_data(VIRTIODevice *s, const uint8_t *buf, int buf_len)
{
    int queue_idx = 0;
    QueueState *qs = &s->queue[queue_idx];
    int desc_idx;
    uint16_t avail_idx;

    if (!qs->ready)
        return 0;
    avail_idx = virtio_read16(s, qs->avail_addr + 2);
    if (qs->last_avail_idx == avail_idx)
        return 0;
    desc_idx = virtio_read16(s, qs->avail_addr + 4 + 
                             (qs->last_avail_idx & (qs->num - 1)) * 2);
    memcpy_to_queue(s, queue_idx, desc_idx, 0, buf, buf_len);
    virtio_consume_desc(s, queue_idx, desc_idx, buf_len);
    qs->last_avail_idx++;
    return buf_len;
}

/* send a resize event */
void virtio_console_resize_event(VIRTIODevice *s, int width, int height)
{
    /* indicate the console size */
    put_le16(s->config_space + 0, width);
    put_le16(s->config_space + 2, height);

    virtio_config_change_notify(s);
}

VIRTIODevice *virtio_console_init(VIRTIOBusDef *bus, CharacterDevice *cs)
{
    VIRTIOConsoleDevice *s;

    s = new VIRTIOConsoleDevice();
    virtio_init(s, bus, 3, 4);
    s->device_features = (1 << 0); /* VIRTIO_CONSOLE_F_SIZE */
    s->queue[0].manual_recv = true;
    
    s->cs = cs;
    return s;
}
