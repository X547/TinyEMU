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

#include "config_props.h"
#include "cutils.h"
#include "devices.h"
#include "virtio.h"


#define VIRTIO_NET_DEVICE_ID 1

#define VIRTIO_NET_F_MRG_RXBUF (1ULL << 15)

struct VIRTIONetDevice final: public VIRTIODevice, public EthernetTarget {
    DeviceContext *ctx;
    std::unique_ptr<HostEthernet> es;

    VIRTIONetDevice(DeviceContext *ctx, std::unique_ptr<HostEthernet> es);

    bool Realize() override;

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
    int desc_idx;

    return virtio_queue_peek(this, 0, &desc_idx);
}

void VIRTIONetDevice::WritePacket(const uint8_t *buf, int buf_len)
{
    VIRTIODevice *s = this;
    int queue_idx = 0;
    int desc_idx;
    VIRTIONetHeader h;
    int header_size = HeaderSize();
    int len, read_size, write_size;

    if (!virtio_queue_peek(s, queue_idx, &desc_idx))
        return;
    if (get_desc_rw_size(s, &read_size, &write_size, queue_idx, desc_idx)) {
        virtio_set_needs_reset(s);
        return;
    }
    len = header_size + buf_len;
    if (len > write_size)
        return;
    memset(&h, 0, sizeof(h));
    /* the whole packet is in this one buffer */
    h.num_buffers = 1;
    memcpy_to_queue(s, queue_idx, desc_idx, 0, &h, header_size);
    memcpy_to_queue(s, queue_idx, desc_idx, header_size, buf, buf_len);
    virtio_consume_desc(s, queue_idx, desc_idx, len);
    virtio_queue_pop(s, queue_idx);
}

void VIRTIONetDevice::SetCarrier(bool carrier_state)
{
    /* TODO: report link state to the guest; this needs the config-change
       interrupt, which is not implemented yet. */
    (void)carrier_state;
}

VIRTIONetDevice::VIRTIONetDevice(DeviceContext *ctx,
                                 std::unique_ptr<HostEthernet> es):
    VIRTIODevice("virtio-net", VIRTIO_NET_DEVICE_ID, 0x0200 /* Ethernet */,
                 6 + 2, 2),
    ctx(ctx),
    es(std::move(es))
{
    /* VIRTIO_NET_F_MAC, VIRTIO_NET_F_STATUS */
    device_features = (1 << 5) /* | (1 << 16) */;
    queue[0].manual_recv = true;
    memcpy(config_space, this->es->mac_addr, 6);
    /* status */
    config_space[6] = 0;
    config_space[7] = 0;
}

bool VIRTIONetDevice::Realize()
{
    if (!VIRTIODevice::Realize())
        return false;
    es->target = this;
    ctx->ethernet.push_back(es.get());
    return true;
}


//#pragma mark - class

class VirtioNetClass final: public DeviceClass {
public:
    VirtioNetClass(): DeviceClass("virtio-net") {}

    Device *Create(const DeviceConfig &cfg, DeviceContext *ctx) const override
    {
        auto net = config_open_ethernet(cfg, ctx);
        if (net == nullptr) {
            return nullptr;
        }
        return new VIRTIONetDevice(ctx, std::move(net));
    }
};

static const VirtioNetClass sVirtioNetClass;
