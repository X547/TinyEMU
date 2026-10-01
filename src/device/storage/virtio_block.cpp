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
#include <algorithm>
#include <vector>

#include "cutils.h"
#include "virtio.h"
#include "virtio_priv.h"


struct VIRTIOBlockDevice;

/* One request the guest made. It is also the completion the back end answers
   through, which is what tells the requests in flight apart. */
struct BlockRequest final: public BlockCompletion {
    VIRTIOBlockDevice &dev;
    uint32_t type = 0;
    std::unique_ptr<uint8_t[]> buf;
    int write_size = 0;
    int queue_idx = 0;
    int desc_idx = 0;

    BlockRequest(VIRTIOBlockDevice &dev): dev(dev) {}

    void Complete(int ret) override;
};

struct VIRTIOBlockDevice: public VIRTIODevice {
    HostBlockDevice *bs = nullptr; /* owned by the node that created the device */
    bool read_only = false;

    /* The requests with the back end. They finish in whatever order it
       answers, which virtio allows. */
    std::vector<std::unique_ptr<BlockRequest>> in_flight;

    int RecvRequest(int queue_idx, int desc_idx, int read_size,
                    int write_size) override;
    void Reset() override;
};

typedef struct {
    uint32_t type;
    uint32_t ioprio;
    uint64_t sector_num;
} BlockRequestHeader;

#define VIRTIO_BLK_T_IN          0
#define VIRTIO_BLK_T_OUT         1
#define VIRTIO_BLK_T_FLUSH       4
#define VIRTIO_BLK_T_FLUSH_OUT   5
#define VIRTIO_BLK_T_GET_ID      8

#define VIRTIO_BLK_F_SIZE_MAX    (1 << 1)
#define VIRTIO_BLK_F_SEG_MAX     (1 << 2)
#define VIRTIO_BLK_F_RO          (1 << 5)

/* configuration space */
#define VIRTIO_BLK_CFG_CAPACITY  0
#define VIRTIO_BLK_CFG_SIZE_MAX  8
#define VIRTIO_BLK_CFG_SEG_MAX   12
#define VIRTIO_BLK_CFG_SIZE      16

/* Without these a driver puts one segment in each request, which is one
   page of a buffer that is not physically contiguous. A request has to fit
   the ring with its header and status, as no indirect descriptors are
   offered. */
#define VIRTIO_BLK_SEG_MAX       (MAX_QUEUE_NUM - 2)
#define VIRTIO_BLK_SIZE_MAX      (1 << 20)

/* length of the serial reported by VIRTIO_BLK_T_GET_ID */
#define VIRTIO_BLK_ID_BYTES     20

#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

#define SECTOR_SIZE 512

/* Complete a request that carries a data buffer: the status byte is the last
   byte the guest made writable. */
static void virtio_block_req_end_buf(VIRTIODevice *s, BlockRequest *req,
                                     uint8_t status)
{
    uint8_t *buf = req->buf.get();

    buf[req->write_size - 1] = status;
    memcpy_to_queue(s, req->queue_idx, req->desc_idx, 0, buf, req->write_size);
    virtio_consume_desc(s, req->queue_idx, req->desc_idx, req->write_size);
}

/* Complete a request whose only writable byte is the status. */
static void virtio_block_req_end_status(VIRTIODevice *s, BlockRequest *req,
                                        uint8_t status)
{
    memcpy_to_queue(s, req->queue_idx, req->desc_idx, 0, &status, 1);
    virtio_consume_desc(s, req->queue_idx, req->desc_idx, 1);
}

static void virtio_block_req_end(VIRTIODevice *s, BlockRequest *req, int ret)
{
    uint8_t status = ret < 0 ? VIRTIO_BLK_S_IOERR : VIRTIO_BLK_S_OK;

    switch(req->type) {
    case VIRTIO_BLK_T_IN:
    case VIRTIO_BLK_T_GET_ID:
        virtio_block_req_end_buf(s, req, status);
        break;
    default:
        /* OUT, FLUSH, and anything else: status byte only. Every request
           must be completed, whatever its type: leaving one unanswered
           stalls the queue forever because the guest waits for a used ring
           entry that never arrives. */
        virtio_block_req_end_status(s, req, status);
        break;
    }
}

void BlockRequest::Complete(int ret)
{
    VIRTIOBlockDevice *s = &dev;

    virtio_block_req_end(s, this, ret);

    /* the last thing done with the request: it goes with its entry */
    auto it = std::find_if(s->in_flight.begin(), s->in_flight.end(),
                           [this](const std::unique_ptr<BlockRequest> &r) {
                               return r.get() == this;
                           });
    std::unique_ptr<BlockRequest> self = std::move(*it);
    s->in_flight.erase(it);
}

void VIRTIOBlockDevice::Reset()
{
    /* the queues the requests came from are gone */
    for (auto &req : in_flight)
        bs->Cancel(req.get());
    in_flight.clear();
}

int VIRTIOBlockDevice::RecvRequest(int queue_idx, int desc_idx, int read_size,
                                   int write_size)
{
    VIRTIODevice *s = this;
    BlockRequestHeader h;
    int len, ret;

    if (memcpy_from_queue(s, &h, queue_idx, desc_idx, 0, sizeof(h)) < 0)
        return 0;
    auto req = std::make_unique<BlockRequest>(*this);
    req->type = h.type;
    req->queue_idx = queue_idx;
    req->desc_idx = desc_idx;
    switch(h.type) {
    case VIRTIO_BLK_T_IN:
        /* not zeroed, the read fills it */
        req->buf.reset(new uint8_t[write_size]);
        req->write_size = write_size;
        ret = bs->ReadAsync(h.sector_num, req->buf.get(),
                            (write_size - 1) / SECTOR_SIZE, req.get());
        break;
    case VIRTIO_BLK_T_OUT:
        assert(write_size >= 1);
        if (read_only) {
            ret = -1;
            break;
        }
        len = read_size - sizeof(h);
        /* kept until the write finishes */
        req->buf.reset(new uint8_t[len]);
        memcpy_from_queue(s, req->buf.get(), queue_idx, desc_idx,
                          sizeof(h), len);
        ret = bs->WriteAsync(h.sector_num, req->buf.get(),
                             len / SECTOR_SIZE, req.get());
        break;
    case VIRTIO_BLK_T_FLUSH:
    case VIRTIO_BLK_T_FLUSH_OUT:
        /* a write is answered only once the back end has it, so there is
           nothing to flush */
        ret = 0;
        break;
    case VIRTIO_BLK_T_GET_ID:
        req->buf = std::make_unique<uint8_t[]>(write_size);
        req->write_size = write_size;
        if (write_size > 1) {
            int id_len = write_size - 1;
            if (id_len > VIRTIO_BLK_ID_BYTES) {
                id_len = VIRTIO_BLK_ID_BYTES;
            }
            strncpy((char *)req->buf.get(), "tinyemu-blk", id_len);
        }
        ret = 0;
        break;
    default:
        /* Report the request as unsupported rather than dropping it: an
           unanswered request stalls the queue forever. */
        virtio_block_req_end_status(s, req.get(), VIRTIO_BLK_S_UNSUPP);
        return 0;
    }

    if (ret > 0) {
        /* the completion finishes it */
        in_flight.push_back(std::move(req));
    } else {
        virtio_block_req_end(s, req.get(), ret);
    }
    return 0;
}

std::unique_ptr<VIRTIODevice> virtio_block_init(VIRTIOBusDef *bus,
                                                HostBlockDevice *bs,
                                                bool read_only)
{
    uint64_t nb_sectors;

    auto s = std::make_unique<VIRTIOBlockDevice>();
    virtio_init(s.get(), bus, 2, VIRTIO_BLK_CFG_SIZE);
    s->bs = bs;
    s->read_only = read_only;
    s->device_features = VIRTIO_BLK_F_SIZE_MAX | VIRTIO_BLK_F_SEG_MAX;
    if (read_only)
        s->device_features |= VIRTIO_BLK_F_RO;

    nb_sectors = bs->SectorCount();
    put_le32(s->config_space + VIRTIO_BLK_CFG_CAPACITY, nb_sectors);
    put_le32(s->config_space + VIRTIO_BLK_CFG_CAPACITY + 4, nb_sectors >> 32);
    put_le32(s->config_space + VIRTIO_BLK_CFG_SIZE_MAX, VIRTIO_BLK_SIZE_MAX);
    put_le32(s->config_space + VIRTIO_BLK_CFG_SEG_MAX, VIRTIO_BLK_SEG_MAX);

    return s;
}
