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


struct VIRTIOConsoleDevice: public VIRTIODevice, public ConsoleTarget {
    HostConsole *cs = nullptr;

    int RecvRequest(int queue_idx, int desc_idx, int read_size,
                    int write_size) override;

    /* ConsoleTarget */
    int ReceiveRoom() override;
    void Receive(const uint8_t *buf, int len) override;
    void Resize(int width, int height) override;
};

int VIRTIOConsoleDevice::RecvRequest(int queue_idx, int desc_idx, int read_size,
                                     int write_size)
{
    VIRTIODevice *s = this;
    VIRTIOConsoleDevice *s1 = (VIRTIOConsoleDevice *)s;
    HostConsole *cs = s1->cs;
    uint8_t *buf;

    if (queue_idx == 1) {
        /* send to console */
        buf = static_cast<uint8_t *>(malloc(read_size));
        memcpy_from_queue(s, buf, queue_idx, desc_idx, 0, read_size);
        if (cs != nullptr)
            cs->WriteData(buf, read_size);
        free(buf);
        virtio_consume_desc(s, queue_idx, desc_idx, 0);
    }
    return 0;
}

/* the size of the receive buffer the guest has queued, 0 if none */
int VIRTIOConsoleDevice::ReceiveRoom()
{
    VIRTIODevice *s = this;
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

void VIRTIOConsoleDevice::Receive(const uint8_t *buf, int buf_len)
{
    VIRTIODevice *s = this;
    int queue_idx = 0;
    QueueState *qs = &s->queue[queue_idx];
    int desc_idx;
    uint16_t avail_idx;

    if (!qs->ready)
        return;
    avail_idx = virtio_read16(s, qs->avail_addr + 2);
    if (qs->last_avail_idx == avail_idx)
        return;
    desc_idx = virtio_read16(s, qs->avail_addr + 4 +
                             (qs->last_avail_idx & (qs->num - 1)) * 2);
    memcpy_to_queue(s, queue_idx, desc_idx, 0, buf, buf_len);
    virtio_consume_desc(s, queue_idx, desc_idx, buf_len);
    qs->last_avail_idx++;
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

std::unique_ptr<VIRTIODevice> virtio_console_init(VIRTIOBusDef *bus,
                                                  HostConsole *cs)
{
    auto s = std::make_unique<VIRTIOConsoleDevice>();
    virtio_init(s.get(), bus, 3, 4);
    s->device_features = (1 << 0); /* VIRTIO_CONSOLE_F_SIZE */
    s->queue[0].manual_recv = true;
    
    s->cs = cs;
    return s;
}

ConsoleTarget *virtio_console_target(VIRTIODevice *s)
{
    return static_cast<VIRTIOConsoleDevice *>(s);
}
