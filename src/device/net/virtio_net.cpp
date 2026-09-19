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
#include <stddef.h>
#include <inttypes.h>
#include <assert.h>

#include "cutils.h"
#include "virtio.h"
#include "virtio_priv.h"


#define VIRTIO_NET_F_MRG_RXBUF (1ULL << 15)

struct VIRTIONetDevice: public VIRTIODevice, public EthernetTarget {
    HostEthernet *es = nullptr;

    int HeaderSize() const;
    int RecvRequest(int queue_idx, int desc_idx, int read_size,
                    int write_size) override;
    void ManualQueueNotify(int queue_idx) override
    {
        if (queue_idx == 0)
            es->TargetReady();
    }

    /* EthernetTarget */
    bool CanWritePacket() override;
    void WritePacket(const uint8_t *buf, int buf_len) override;
    void SetCarrier(bool carrier_state) override;
};

typedef struct {
    uint8_t flags;
    uint8_t gso_type;
    uint16_t hdr_len;
    uint16_t gso_size;
    uint16_t csum_start;
    uint16_t csum_offset;
    uint16_t num_buffers;
} VIRTIONetHeader;

/* A driver that accepted virtio 1.0 or merged receive buffers puts a buffer
   count in the header; a legacy one ends it before. */
int VIRTIONetDevice::HeaderSize() const
{
    if (driver_features & (VIRTIO_F_VERSION_1 | VIRTIO_NET_F_MRG_RXBUF))
        return sizeof(VIRTIONetHeader);
    return offsetof(VIRTIONetHeader, num_buffers);
}

int VIRTIONetDevice::RecvRequest(int queue_idx, int desc_idx, int read_size,
                                 int write_size)
{
    VIRTIODevice *s = this;
    VIRTIONetDevice *s1 = (VIRTIONetDevice *)s;
    HostEthernet *es = s1->es;
    VIRTIONetHeader h;
    int header_size = HeaderSize();
    uint8_t *buf;
    int len;

    if (queue_idx == 1) {
        /* send to network */
        if (memcpy_from_queue(s, &h, queue_idx, desc_idx, 0, header_size) < 0)
            return 0;
        len = read_size - header_size;
        buf = static_cast<uint8_t *>(malloc(len));
        memcpy_from_queue(s, buf, queue_idx, desc_idx, header_size, len);
        es->WritePacket(buf, len);
        free(buf);
        virtio_consume_desc(s, queue_idx, desc_idx, 0);
    }
    return 0;
}

bool VIRTIONetDevice::CanWritePacket()
{
    VIRTIODevice *s = this;
    QueueState *qs = &s->queue[0];
    uint16_t avail_idx;

    if (!qs->ready)
        return false;
    avail_idx = virtio_read16(s, qs->avail_addr + 2);
    return qs->last_avail_idx != avail_idx;
}

void VIRTIONetDevice::WritePacket(const uint8_t *buf, int buf_len)
{
    VIRTIODevice *s = this;
    int queue_idx = 0;
    QueueState *qs = &s->queue[queue_idx];
    int desc_idx;
    VIRTIONetHeader h;
    int header_size = HeaderSize();
    int len, read_size, write_size;
    uint16_t avail_idx;

    if (!qs->ready)
        return;
    avail_idx = virtio_read16(s, qs->avail_addr + 2);
    if (qs->last_avail_idx == avail_idx)
        return;
    desc_idx = virtio_read16(s, qs->avail_addr + 4 + 
                             (qs->last_avail_idx & (qs->num - 1)) * 2);
    if (get_desc_rw_size(s, &read_size, &write_size, queue_idx, desc_idx))
        return;
    len = header_size + buf_len; 
    if (len > write_size)
        return;
    memset(&h, 0, sizeof(h));
    /* the whole packet is in this one buffer */
    h.num_buffers = 1;
    memcpy_to_queue(s, queue_idx, desc_idx, 0, &h, header_size);
    memcpy_to_queue(s, queue_idx, desc_idx, header_size, buf, buf_len);
    virtio_consume_desc(s, queue_idx, desc_idx, len);
    qs->last_avail_idx++;
}

void VIRTIONetDevice::SetCarrier(bool carrier_state)
{
    /* TODO: report link state to the guest; this needs the config-change
       interrupt, which is not implemented yet. */
    (void)carrier_state;
}

std::unique_ptr<VIRTIODevice> virtio_net_init(VIRTIOBusDef *bus,
                                              HostEthernet *es)
{
    auto s = std::make_unique<VIRTIONetDevice>();
    virtio_init(s.get(), bus, 1, 6 + 2);
    /* VIRTIO_NET_F_MAC, VIRTIO_NET_F_STATUS */
    s->device_features = (1 << 5) /* | (1 << 16) */;
    s->queue[0].manual_recv = true;
    s->es = es;
    memcpy(s->config_space, es->mac_addr, 6);
    /* status */
    s->config_space[6] = 0;
    s->config_space[7] = 0;

    es->target = s.get();
    return s;
}
