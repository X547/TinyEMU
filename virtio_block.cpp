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

#define VIRTIO_BLK_S_OK     0
#define VIRTIO_BLK_S_IOERR  1
#define VIRTIO_BLK_S_UNSUPP 2

#define SECTOR_SIZE 512

static void virtio_block_req_end(VIRTIODevice *s, int ret)
{
    VIRTIOBlockDevice *s1 = (VIRTIOBlockDevice *)s;
    int write_size;
    int queue_idx = s1->req.queue_idx;
    int desc_idx = s1->req.desc_idx;
    uint8_t *buf, buf1[1];

    switch(s1->req.type) {
    case VIRTIO_BLK_T_IN:
        write_size = s1->req.write_size;
        buf = s1->req.buf;
        if (ret < 0) {
            buf[write_size - 1] = VIRTIO_BLK_S_IOERR;
        } else {
            buf[write_size - 1] = VIRTIO_BLK_S_OK;
        }
        memcpy_to_queue(s, queue_idx, desc_idx, 0, buf, write_size);
        free(buf);
        virtio_consume_desc(s, queue_idx, desc_idx, write_size);
        break;
    case VIRTIO_BLK_T_OUT:
        if (ret < 0)
            buf1[0] = VIRTIO_BLK_S_IOERR;
        else
            buf1[0] = VIRTIO_BLK_S_OK;
        memcpy_to_queue(s, queue_idx, desc_idx, 0, buf1, sizeof(buf1));
        virtio_consume_desc(s, queue_idx, desc_idx, 1);
        break;
    default:
        abort();
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
    default:
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
