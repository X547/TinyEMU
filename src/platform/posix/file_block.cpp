/*
 * Host block storage: an image file, read and written on a thread of its own
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
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <unistd.h>
#include <algorithm>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>
#include <vector>

#include "event_loop.h"
#include "host_thread.h"
#include "platform_backends.h"
#include "wait_set.h"

#define SECTOR_SIZE 512

//#define DUMP_BLOCK_READ

class BlockDeviceFile final: public HostBlockDevice, public PollSource {
private:
    struct Request {
        bool is_write;
        uint64_t sector_num;
        uint8_t *buf;
        int n;
        BlockCompletion *completion;
        int ret;
    };

    EventLoop &fLoop;
    int fFd;
    int64_t fSectorCount;
    BlockModeEnum fMode;
    /* snapshot mode: the sectors written so far, null where unchanged */
    std::vector<std::unique_ptr<uint8_t[]>> fSectorTable;

    /* Shared with the worker. Requests run one at a time in the order they
       came, so a read sees every write queued before it. */
    std::mutex fMutex;
    std::condition_variable fWorkCond;
    std::condition_variable fCurrentDoneCond;
    std::deque<Request> fPending;
    /* the request the worker is on, while fHasCurrent */
    Request fCurrent {};
    bool fHasCurrent = false;
    /* finished, waiting for the event loop */
    std::deque<Request> fDone;
    bool fStop = false;
    std::thread fWorker;

    int Queue(bool is_write, uint64_t sector_num, uint8_t *buf, int n,
              BlockCompletion *completion);
    void WorkerMain();
    int Perform(const Request &req);

public:
    BlockDeviceFile(EventLoop &loop, int fd, int64_t sectors,
                    BlockModeEnum mode);
    ~BlockDeviceFile() override;

    /* HostBlockDevice */
    int64_t SectorCount() override {return fSectorCount;}
    int ReadAsync(uint64_t sector_num, uint8_t *buf, int n,
                  BlockCompletion *completion) override;
    int WriteAsync(uint64_t sector_num, const uint8_t *buf, int n,
                   BlockCompletion *completion) override;
    void Cancel(BlockCompletion *completion) override;

    /* PollSource */
    void Prepare(WaitSet &ws) override;
    void Dispatch(WaitSet &ws) override;
    bool Busy() override;
};


BlockDeviceFile::BlockDeviceFile(EventLoop &loop, int fd, int64_t sectors,
                                 BlockModeEnum mode):
    fLoop(loop),
    fFd(fd),
    fSectorCount(sectors),
    fMode(mode)
{
    if (mode == BLOCK_MODE_SNAPSHOT)
        fSectorTable.resize(sectors);
    fLoop.Add(this);
    fWorker = std::thread([this]() {WorkerMain();});
}


BlockDeviceFile::~BlockDeviceFile()
{
    {
        std::lock_guard<std::mutex> lock(fMutex);
        fStop = true;
    }
    fWorkCond.notify_one();
    /* drops what is queued, after the request in progress */
    fWorker.join();
    fLoop.Remove(this);
    close(fFd);
}


void BlockDeviceFile::WorkerMain()
{
    host_set_thread_name("block file");
    std::unique_lock<std::mutex> lock(fMutex);
    for (;;) {
        fWorkCond.wait(lock, [this]() {return fStop || !fPending.empty();});
        if (fStop)
            return;
        fCurrent = fPending.front();
        fPending.pop_front();
        fHasCurrent = true;

        lock.unlock();
        int ret = Perform(fCurrent);
        lock.lock();

        fCurrent.ret = ret;
        fDone.push_back(fCurrent);
        fHasCurrent = false;
        fCurrentDoneCond.notify_all();
        fLoop.Wake();
    }
}


/* On the worker, without the device lock. */
int BlockDeviceFile::Perform(const Request &req)
{
    size_t len = (size_t)req.n * SECTOR_SIZE;
    off_t offset = (off_t)req.sector_num * SECTOR_SIZE;
    size_t done = 0;

    while (done < len) {
        ssize_t ret;
        if (req.is_write)
            ret = pwrite(fFd, req.buf + done, len - done, offset + done);
        else
            ret = pread(fFd, req.buf + done, len - done, offset + done);
        if (ret < 0) {
            if (errno == EINTR)
                continue;
            return -1;
        }
        if (ret == 0)
            break;
        done += ret;
    }
    if (done < len) {
        if (req.is_write)
            return -1;
        memset(req.buf + done, 0, len - done);
    }
    return 0;
}


int BlockDeviceFile::Queue(bool is_write, uint64_t sector_num, uint8_t *buf,
                           int n, BlockCompletion *completion)
{
    {
        std::lock_guard<std::mutex> lock(fMutex);
        fPending.push_back({is_write, sector_num, buf, n, completion, 0});
    }
    fWorkCond.notify_one();
    return 1;
}


int BlockDeviceFile::ReadAsync(uint64_t sector_num, uint8_t *buf, int n,
                               BlockCompletion *completion)
{
#ifdef DUMP_BLOCK_READ
    {
        static FILE *f;
        if (!f)
            f = fopen("/tmp/read_sect.txt", "wb");
        fprintf(f, "%" PRId64 " %d\n", sector_num, n);
    }
#endif
    /* a guest probing past the last sector gets an error */
    if (n <= 0 || sector_num + n > (uint64_t)fSectorCount)
        return -1;
    return Queue(false, sector_num, buf, n, completion);
}


int BlockDeviceFile::WriteAsync(uint64_t sector_num, const uint8_t *buf, int n,
                                BlockCompletion *completion)
{
    if (n <= 0 || sector_num + n > (uint64_t)fSectorCount)
        return -1;

    switch (fMode) {
    case BLOCK_MODE_RO:
        return -1;
    case BLOCK_MODE_SNAPSHOT:
        for (int i = 0; i < n; i++) {
            auto &sector = fSectorTable[sector_num + i];
            if (!sector)
                sector.reset(new uint8_t[SECTOR_SIZE]);
            memcpy(sector.get(), buf + i * SECTOR_SIZE, SECTOR_SIZE);
        }
        return 0;
    case BLOCK_MODE_RW:
        break;
    }
    return Queue(true, sector_num, const_cast<uint8_t *>(buf), n, completion);
}


void BlockDeviceFile::Cancel(BlockCompletion *completion)
{
    auto matches = [completion](const Request &req) {
        return req.completion == completion;
    };
    std::unique_lock<std::mutex> lock(fMutex);
    /* the worker is using the buffer: let it finish */
    fCurrentDoneCond.wait(lock, [&]() {
        return !fHasCurrent || fCurrent.completion != completion;
    });
    fPending.erase(std::remove_if(fPending.begin(), fPending.end(), matches),
                   fPending.end());
    fDone.erase(std::remove_if(fDone.begin(), fDone.end(), matches),
                fDone.end());
}


void BlockDeviceFile::Prepare(WaitSet &ws)
{
    std::lock_guard<std::mutex> lock(fMutex);
    if (!fDone.empty())
        ws.LimitTimeout(0);
}


void BlockDeviceFile::Dispatch(WaitSet &ws)
{
    (void)ws;
    std::unique_lock<std::mutex> lock(fMutex);
    /* Only what finished before: a completion may queue the next. */
    for (size_t count = fDone.size(); count > 0 && !fDone.empty(); count--) {
        Request req = fDone.front();
        fDone.pop_front();
        lock.unlock();

        /* what the guest wrote since the image was opened */
        if (!req.is_write && req.ret == 0 && fMode == BLOCK_MODE_SNAPSHOT) {
            for (int i = 0; i < req.n; i++) {
                const uint8_t *sector =
                    fSectorTable[req.sector_num + i].get();
                if (sector != nullptr)
                    memcpy(req.buf + i * SECTOR_SIZE, sector, SECTOR_SIZE);
            }
        }
        req.completion->Complete(req.ret);

        lock.lock();
    }
}


bool BlockDeviceFile::Busy()
{
    std::lock_guard<std::mutex> lock(fMutex);
    return fHasCurrent || !fPending.empty() || !fDone.empty();
}


std::unique_ptr<HostBlockDevice> file_block_open(EventLoop &loop,
                                                 const char *filename,
                                                 BlockModeEnum mode)
{
    int flags = mode == BLOCK_MODE_RW ? O_RDWR : O_RDONLY;
    int fd = open(filename, flags | O_CLOEXEC);
    if (fd < 0) {
        perror(filename);
        return nullptr;
    }
    off_t file_size = lseek(fd, 0, SEEK_END);
    if (file_size < 0) {
        perror(filename);
        close(fd);
        return nullptr;
    }
    return std::make_unique<BlockDeviceFile>(loop, fd,
                                             file_size / SECTOR_SIZE, mode);
}
