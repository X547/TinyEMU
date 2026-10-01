/*
 * Host block storage: an image file, with overlapped I/O
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
#include <string.h>

#include <algorithm>
#include <memory>
#include <vector>

#include <windows.h>

#include "event_loop.h"
#include "platform_backends.h"
#include "wait_set.h"

#define SECTOR_SIZE 512

/* Answer every request that finished at once later, from the event loop, as
   if it had not. Reads the system has cached finish at once, so this is what
   puts several requests in flight for the devices' queues to be tested. */
//#define DEFER_BLOCK_COMPLETION


/* One request the system has. It is read into, or written from, a buffer
   of its own, so that a request the device gives up on can be left to
   finish without touching the device's buffer. */
struct FileRequest {
    OVERLAPPED ov {};
    /* null once the device gave the request up */
    BlockCompletion *completion = nullptr;
    bool is_write = false;
    uint64_t sector_num = 0;
    int n = 0;
    /* where a read goes */
    uint8_t *buf = nullptr;
    std::unique_ptr<uint8_t[]> data;
};


class BlockDeviceFile final: public HostBlockDevice, public PollSource {
private:
    EventLoop &fLoop;
    HANDLE fFile;
    int64_t fSectorCount;
    BlockModeEnum fMode;
    /* snapshot mode: the sectors written so far, null where unchanged */
    std::vector<std::unique_ptr<uint8_t[]>> fSectorTable;
    std::vector<std::unique_ptr<FileRequest>> fInFlight;

    int Start(std::unique_ptr<FileRequest> req);
    int Finish(FileRequest *req, bool ok, DWORD bytes);

public:
    BlockDeviceFile(EventLoop &loop, HANDLE file, int64_t sectors,
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
    void Prepare(WaitSet &ws) override {(void)ws;}
    void Dispatch(WaitSet &ws) override;
    bool Busy() override {return !fInFlight.empty();}
};


BlockDeviceFile::BlockDeviceFile(EventLoop &loop, HANDLE file,
                                 int64_t sectors, BlockModeEnum mode):
    fLoop(loop),
    fFile(file),
    fSectorCount(sectors),
    fMode(mode)
{
    if (mode == BLOCK_MODE_SNAPSHOT)
        fSectorTable.resize(sectors);
    fLoop.Add(this);
}


BlockDeviceFile::~BlockDeviceFile()
{
    fLoop.Remove(this);
    /* Nothing takes the packets off the port any more, but the requests
       still have to finish before their memory goes. */
    for (auto &req : fInFlight) {
        while (!HasOverlappedIoCompleted(&req->ov))
            Sleep(1);
    }
    fInFlight.clear();
    CloseHandle(fFile);
}


/* Returns 0 if the request finished at once, 1 if it is in flight and -1
   if it failed. */
int BlockDeviceFile::Start(std::unique_ptr<FileRequest> req)
{
    uint64_t offset = req->sector_num * SECTOR_SIZE;
    DWORD len = req->n * SECTOR_SIZE;
    BOOL ok;

    req->ov.Offset = (DWORD)offset;
    req->ov.OffsetHigh = (DWORD)(offset >> 32);
    if (req->is_write)
        ok = WriteFile(fFile, req->data.get(), len, nullptr, &req->ov);
    else
        ok = ReadFile(fFile, req->data.get(), len, nullptr, &req->ov);

    if (!ok && GetLastError() == ERROR_IO_PENDING) {
        fInFlight.push_back(std::move(req));
        return 1;
    }
#ifdef DEFER_BLOCK_COMPLETION
    if (ok && event_loop_post(fLoop, this, &req->ov)) {
        fInFlight.push_back(std::move(req));
        return 1;
    }
#endif
    DWORD bytes = 0;
    if (ok)
        ok = GetOverlappedResult(fFile, &req->ov, &bytes, FALSE);
    return Finish(req.get(), ok, bytes);
}


/* The system is done with 'req'. */
int BlockDeviceFile::Finish(FileRequest *req, bool ok, DWORD bytes)
{
    if (!ok)
        return -1;
    if (req->is_write)
        return 0;

    size_t len = (size_t)req->n * SECTOR_SIZE;
    if (bytes < len)
        memset(req->data.get() + bytes, 0, len - bytes);
    memcpy(req->buf, req->data.get(), len);
    /* what the guest wrote since the image was opened */
    if (fMode == BLOCK_MODE_SNAPSHOT) {
        for (int i = 0; i < req->n; i++) {
            const uint8_t *sector = fSectorTable[req->sector_num + i].get();
            if (sector != nullptr)
                memcpy(req->buf + i * SECTOR_SIZE, sector, SECTOR_SIZE);
        }
    }
    return 0;
}


int BlockDeviceFile::ReadAsync(uint64_t sector_num, uint8_t *buf, int n,
                               BlockCompletion *completion)
{
    /* a guest probing past the last sector gets an error */
    if (n <= 0 || sector_num + n > (uint64_t)fSectorCount)
        return -1;

    auto req = std::make_unique<FileRequest>();
    req->completion = completion;
    req->sector_num = sector_num;
    req->n = n;
    req->buf = buf;
    req->data.reset(new uint8_t[(size_t)n * SECTOR_SIZE]);
    return Start(std::move(req));
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

    auto req = std::make_unique<FileRequest>();
    req->completion = completion;
    req->is_write = true;
    req->sector_num = sector_num;
    req->n = n;
    req->data.reset(new uint8_t[(size_t)n * SECTOR_SIZE]);
    memcpy(req->data.get(), buf, (size_t)n * SECTOR_SIZE);
    return Start(std::move(req));
}


/* The request goes on in the system, and is dropped when it finishes. */
void BlockDeviceFile::Cancel(BlockCompletion *completion)
{
    for (auto &req : fInFlight) {
        if (req->completion == completion)
            req->completion = nullptr;
    }
}


void BlockDeviceFile::Dispatch(WaitSet &ws)
{
    for (const OVERLAPPED_ENTRY &e : ws.Completions()) {
        if (e.lpCompletionKey != reinterpret_cast<ULONG_PTR>(this))
            continue;
        auto it = std::find_if(fInFlight.begin(), fInFlight.end(),
                               [&e](const std::unique_ptr<FileRequest> &r) {
                                   return &r->ov == e.lpOverlapped;
                               });
        if (it == fInFlight.end())
            continue;
        /* forgotten before the device hears of it, which may start the
           next request */
        std::unique_ptr<FileRequest> req = std::move(*it);
        fInFlight.erase(it);
        if (req->completion == nullptr)
            continue;
        DWORD bytes = 0;
        bool ok = GetOverlappedResult(fFile, &req->ov, &bytes, FALSE);
        int ret = Finish(req.get(), ok, bytes);
        req->completion->Complete(ret);
    }
}


std::unique_ptr<HostBlockDevice> file_block_open(EventLoop &loop,
                                                 const char *filename,
                                                 BlockModeEnum mode)
{
    /* Paths in a configuration file are UTF-8. */
    int wlen = MultiByteToWideChar(CP_UTF8, 0, filename, -1, nullptr, 0);
    std::vector<wchar_t> wname(wlen > 0 ? wlen : 1);
    MultiByteToWideChar(CP_UTF8, 0, filename, -1, wname.data(), wname.size());

    DWORD access = GENERIC_READ;
    DWORD share = FILE_SHARE_READ | FILE_SHARE_WRITE;
    if (mode == BLOCK_MODE_RW) {
        access |= GENERIC_WRITE;
        share = FILE_SHARE_READ;
    }
    HANDLE file = CreateFileW(wname.data(), access, share, nullptr,
                              OPEN_EXISTING,
                              FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OVERLAPPED,
                              nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        fprintf(stderr, "%s: cannot open (error %lu)\n", filename,
                GetLastError());
        return nullptr;
    }

    LARGE_INTEGER size;
    if (!GetFileSizeEx(file, &size)) {
        fprintf(stderr, "%s: cannot get the size (error %lu)\n", filename,
                GetLastError());
        CloseHandle(file);
        return nullptr;
    }

    auto bf = std::make_unique<BlockDeviceFile>(loop, file,
                                                size.QuadPart / SECTOR_SIZE,
                                                mode);
    if (!event_loop_attach(loop, file, bf.get())) {
        fprintf(stderr, "%s: cannot wait for I/O (error %lu)\n", filename,
                GetLastError());
        return nullptr;
    }
    return bf;
}
