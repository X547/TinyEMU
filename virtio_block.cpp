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


typedef struct {
    uint32_t type;
    uint8_t *buf;
    int write_size;
    int queue_idx;
    int desc_idx;
} BlockRequest;

struct VIRTIOBlockDevice: public VIRTIODevice {
    class Completion final: public BlockDeviceCompletion {
    private:
        VIRTIOBlockDevice &fDev;

    public:
        Completion(VIRTIOBlockDevice &dev): fDev(dev) {}

        void Complete(int ret) override;
    };

    BlockDevice *bs = nullptr;

    bool req_in_progress = false;
    BlockRequest req {}; /* request in progress */
    Completion fCompletion {*this};

    int RecvRequest(int queue_idx, int desc_idx, int read_size,
                    int write_size) override;
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

/* length of the serial reported by VIRTIO_BLK_T_GET_ID */
#define VIRTIO_BLK_ID_BYTES     20

#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

#define SECTOR_SIZE 512

/* Complete a request that carries a data buffer: the status byte is the last
   byte the guest made writable. */
static void virtio_block_req_end_buf(VIRTIODevice *s, uint8_t status)
{
    VIRTIOBlockDevice *s1 = (VIRTIOBlockDevice *)s;
    int queue_idx = s1->req.queue_idx;
    int desc_idx = s1->req.desc_idx;
    int write_size = s1->req.write_size;
    uint8_t *buf = s1->req.buf;

    buf[write_size - 1] = status;
    memcpy_to_queue(s, queue_idx, desc_idx, 0, buf, write_size);
    free(buf);
    s1->req.buf = nullptr;
    virtio_consume_desc(s, queue_idx, desc_idx, write_size);
}

/* Complete a request whose only writable byte is the status. */
static void virtio_block_req_end_status(VIRTIODevice *s, uint8_t status)
{
    VIRTIOBlockDevice *s1 = (VIRTIOBlockDevice *)s;

    memcpy_to_queue(s, s1->req.queue_idx, s1->req.desc_idx, 0, &status, 1);
    virtio_consume_desc(s, s1->req.queue_idx, s1->req.desc_idx, 1);
}

static void virtio_block_req_end(VIRTIODevice *s, int ret)
{
    VIRTIOBlockDevice *s1 = (VIRTIOBlockDevice *)s;
    uint8_t status = ret < 0 ? VIRTIO_BLK_S_IOERR : VIRTIO_BLK_S_OK;

    switch(s1->req.type) {
    case VIRTIO_BLK_T_IN:
    case VIRTIO_BLK_T_GET_ID:
        virtio_block_req_end_buf(s, status);
        break;
    default:
        /* OUT, FLUSH, and anything else: status byte only. Every request
           must be completed, whatever its type: leaving one unanswered
           stalls the queue forever because the guest waits for a used ring
           entry that never arrives. */
        virtio_block_req_end_status(s, status);
        break;
    }
}

void VIRTIOBlockDevice::Completion::Complete(int ret)
{
    VIRTIOBlockDevice *s1 = &fDev;

    virtio_block_req_end(s1, ret);

    s1->req_in_progress = false;

    /* handle next requests */
    queue_notify(s1, s1->req.queue_idx);
}

/* XXX: handle async I/O */
int VIRTIOBlockDevice::RecvRequest(int queue_idx, int desc_idx, int read_size,
                                   int write_size)
{
    VIRTIODevice *s = this;
    VIRTIOBlockDevice *s1 = this;
    BlockDevice *bs = s1->bs;
    BlockRequestHeader h;
    uint8_t *buf;
    int len, ret;

    if (s1->req_in_progress)
        return -1;
    
    if (memcpy_from_queue(s, &h, queue_idx, desc_idx, 0, sizeof(h)) < 0)
        return 0;
    s1->req.type = h.type;
    s1->req.queue_idx = queue_idx;
    s1->req.desc_idx = desc_idx;
    switch(h.type) {
    case VIRTIO_BLK_T_IN:
        s1->req.buf = static_cast<uint8_t *>(malloc(write_size));
        s1->req.write_size = write_size;
        ret = bs->ReadAsync(h.sector_num, s1->req.buf,
                            (write_size - 1) / SECTOR_SIZE, &s1->fCompletion);
        if (ret > 0) {
            /* asyncronous read */
            s1->req_in_progress = true;
        } else {
            virtio_block_req_end(s, ret);
        }
        break;
    case VIRTIO_BLK_T_OUT:
        assert(write_size >= 1);
        len = read_size - sizeof(h);
        buf = static_cast<uint8_t *>(malloc(len));
        memcpy_from_queue(s, buf, queue_idx, desc_idx, sizeof(h), len);
        ret = bs->WriteAsync(h.sector_num, buf, len / SECTOR_SIZE,
                             &s1->fCompletion);
        free(buf);
        if (ret > 0) {
            /* asyncronous write */
            s1->req_in_progress = true;
        } else {
            virtio_block_req_end(s, ret);
        }
        break;
    case VIRTIO_BLK_T_FLUSH:
    case VIRTIO_BLK_T_FLUSH_OUT:
        /* writes reach the backing file synchronously, so there is nothing
           to flush */
        virtio_block_req_end(s, 0);
        break;
    case VIRTIO_BLK_T_GET_ID:
        s1->req.buf = mallocz_t<uint8_t>(write_size);
        s1->req.write_size = write_size;
        if (write_size > 1) {
            int id_len = write_size - 1;
            if (id_len > VIRTIO_BLK_ID_BYTES) {
                id_len = VIRTIO_BLK_ID_BYTES;
            }
            strncpy((char *)s1->req.buf, "tinyemu-blk", id_len);
        }
        virtio_block_req_end(s, 0);
        break;
    default:
        /* Report the request as unsupported rather than dropping it: an
           unanswered request stalls the queue forever. */
        virtio_block_req_end_status(s, VIRTIO_BLK_S_UNSUPP);
        break;
    }
    return 0;
}

VIRTIODevice *virtio_block_init(VIRTIOBusDef *bus, BlockDevice *bs)
{
    VIRTIOBlockDevice *s;
    uint64_t nb_sectors;

    s = new VIRTIOBlockDevice();
    virtio_init(s, bus, 2, 8);
    s->bs = bs;
    
    nb_sectors = bs->SectorCount();
    put_le32(s->config_space, nb_sectors);
    put_le32(s->config_space + 4, nb_sectors >> 32);

    return s;
}
