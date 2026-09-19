/*
 * Host block storage: an image file
 *
 * Copyright (c) 2016-2018 Fabrice Bellard
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <algorithm>
#include <deque>
#include <vector>

#include "event_loop.h"
#include "platform_backends.h"
#include "wait_set.h"

#define SECTOR_SIZE 512

//#define DUMP_BLOCK_READ

/* Answer every request later, from the event loop, rather than at once. The
   file is still read and written synchronously; this only exercises the
   devices' asynchronous paths. */
//#define DEFER_BLOCK_COMPLETION

struct FileCloser {
    void operator()(FILE *f) const {fclose(f);}
};

class BlockDeviceFile final: public HostBlockDevice, public PollSource {
private:
    struct Request {
        bool is_write;
        uint64_t sector_num;
        uint8_t *buf;
        int n;
        BlockCompletion *completion;
    };

    EventLoop &fLoop;
    /* deferred requests, oldest first */
    std::deque<Request> fQueue;

    int Read(uint64_t sector_num, uint8_t *buf, int n);
    int Write(uint64_t sector_num, const uint8_t *buf, int n);
    int Defer(bool is_write, uint64_t sector_num, uint8_t *buf, int n,
              BlockCompletion *completion);

public:
    std::unique_ptr<FILE, FileCloser> f;
    int64_t nb_sectors = 0;
    BlockModeEnum mode {};
    /* snapshot mode: the sectors written so far, null where unchanged */
    std::vector<std::unique_ptr<uint8_t[]>> sector_table;

    BlockDeviceFile(EventLoop &loop): fLoop(loop) {fLoop.Add(this);}
    ~BlockDeviceFile() override {fLoop.Remove(this);}

    /* HostBlockDevice */
    int64_t SectorCount() override {return nb_sectors;}
    int ReadAsync(uint64_t sector_num, uint8_t *buf, int n,
                  BlockCompletion *completion) override;
    int WriteAsync(uint64_t sector_num, const uint8_t *buf, int n,
                   BlockCompletion *completion) override;
    void Cancel(BlockCompletion *completion) override;

    /* PollSource */
    void Prepare(WaitSet &ws) override;
    void Dispatch(WaitSet &ws) override;
    bool Busy() override {return !fQueue.empty();}
};

int BlockDeviceFile::Read(uint64_t sector_num, uint8_t *buf, int n)
{
    BlockDeviceFile *bf = this;
    //    printf("bf_read_async: sector_num=%" PRId64 " n=%d\n", sector_num, n);
#ifdef DUMP_BLOCK_READ
    {
        static FILE *f;
        if (!f)
            f = fopen("/tmp/read_sect.txt", "wb");
        fprintf(f, "%" PRId64 " %d\n", sector_num, n);
    }
#endif
    if (!bf->f)
        return -1;
    /* a guest probing past the last sector gets an error */
    if (sector_num + n > (uint64_t)bf->nb_sectors)
        return -1;
    if (bf->mode == BLOCK_MODE_SNAPSHOT) {
        int i;
        for(i = 0; i < n; i++) {
            if (!bf->sector_table[sector_num]) {
                fseek(bf->f.get(), sector_num * SECTOR_SIZE, SEEK_SET);
                if (fread(buf, 1, SECTOR_SIZE, bf->f.get()) != SECTOR_SIZE)
                    memset(buf, 0, SECTOR_SIZE);
            } else {
                memcpy(buf, bf->sector_table[sector_num].get(), SECTOR_SIZE);
            }
            sector_num++;
            buf += SECTOR_SIZE;
        }
    } else {
        size_t len = (size_t)n * SECTOR_SIZE;
        size_t got;
        fseek(bf->f.get(), sector_num * SECTOR_SIZE, SEEK_SET);
        got = fread(buf, 1, len, bf->f.get());
        if (got < len)
            memset(buf + got, 0, len - got);
    }
    return 0;
}

int BlockDeviceFile::Write(uint64_t sector_num, const uint8_t *buf, int n)
{
    BlockDeviceFile *bf = this;
    int ret;

    switch(bf->mode) {
    case BLOCK_MODE_RO:
        ret = -1; /* error */
        break;
    case BLOCK_MODE_RW:
        fseek(bf->f.get(), sector_num * SECTOR_SIZE, SEEK_SET);
        fwrite(buf, 1, n * SECTOR_SIZE, bf->f.get());
        ret = 0;
        break;
    case BLOCK_MODE_SNAPSHOT:
        {
            int i;
            if ((sector_num + n) > bf->nb_sectors)
                return -1;
            for(i = 0; i < n; i++) {
                if (!bf->sector_table[sector_num]) {
                    bf->sector_table[sector_num].reset(
                        new uint8_t[SECTOR_SIZE]);
                }
                memcpy(bf->sector_table[sector_num].get(), buf, SECTOR_SIZE);
                sector_num++;
                buf += SECTOR_SIZE;
            }
            ret = 0;
        }
        break;
    default:
        abort();
    }

    return ret;
}

int BlockDeviceFile::Defer(bool is_write, uint64_t sector_num, uint8_t *buf,
                           int n, BlockCompletion *completion)
{
    fQueue.push_back({is_write, sector_num, buf, n, completion});
    fLoop.Wake();
    return 1;
}

int BlockDeviceFile::ReadAsync(uint64_t sector_num, uint8_t *buf, int n,
                               BlockCompletion *completion)
{
#ifdef DEFER_BLOCK_COMPLETION
    return Defer(false, sector_num, buf, n, completion);
#else
    (void)completion;
    return Read(sector_num, buf, n);
#endif
}

int BlockDeviceFile::WriteAsync(uint64_t sector_num, const uint8_t *buf, int n,
                                BlockCompletion *completion)
{
#ifdef DEFER_BLOCK_COMPLETION
    return Defer(true, sector_num, const_cast<uint8_t *>(buf), n, completion);
#else
    (void)completion;
    return Write(sector_num, buf, n);
#endif
}

void BlockDeviceFile::Cancel(BlockCompletion *completion)
{
    fQueue.erase(std::remove_if(fQueue.begin(), fQueue.end(),
                                [completion](const Request &req) {
                                    return req.completion == completion;
                                }),
                 fQueue.end());
}

void BlockDeviceFile::Prepare(WaitSet &ws)
{
    if (!fQueue.empty())
        ws.LimitTimeout(0);
}

void BlockDeviceFile::Dispatch(WaitSet &ws)
{
    (void)ws;
    /* Only what was queued before: a completion may queue the next. */
    for (size_t count = fQueue.size(); count > 0 && !fQueue.empty(); count--) {
        Request req = fQueue.front();
        fQueue.pop_front();
        int ret = req.is_write ? Write(req.sector_num, req.buf, req.n)
                               : Read(req.sector_num, req.buf, req.n);
        req.completion->Complete(ret);
    }
}

std::unique_ptr<HostBlockDevice> file_block_open(EventLoop &loop,
                                                 const char *filename,
                                                 BlockModeEnum mode)
{
    int64_t file_size;
    FILE *f;
    const char *mode_str;

    if (mode == BLOCK_MODE_RW) {
        mode_str = "r+b";
    } else {
        mode_str = "rb";
    }

    f = fopen(filename, mode_str);
    if (!f) {
        perror(filename);
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    file_size = ftello(f);

    auto bf = std::make_unique<BlockDeviceFile>(loop);

    bf->mode = mode;
    bf->nb_sectors = file_size / 512;
    bf->f.reset(f);

    if (mode == BLOCK_MODE_SNAPSHOT) {
        bf->sector_table.resize(bf->nb_sectors);
    }

    return bf;
}
