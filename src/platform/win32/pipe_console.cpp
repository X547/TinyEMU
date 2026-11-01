/*
 * Host console: a named pipe
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
#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include <algorithm>
#include <atomic>
#include <string>
#include <vector>

#include <windows.h>

#include "platform_backends.h"
#include "run_control.h"
#include "wait_set.h"

/* How often a wait for the client looks for a shutdown. */
#define PIPE_RETRY_MS 100
#define PIPE_BUFFER_SIZE 4096


/* As a server it takes one client at a time, and waits for the next once
   that one goes; as a client it is done when the server goes. What the
   guest sends while nothing is connected is dropped. */
class PipeConsole final: public HostConsole, public PollSource {
private:
    enum Op {
        OP_NONE,
        OP_CONNECT,
        OP_READ,
    };

    EventLoop &fLoop;
    RunControl &fRunControl;
    ConsoleTarget *fTarget = nullptr;
    std::string fPath;
    bool fServer = false;
    HANDLE fPipe = INVALID_HANDLE_VALUE;
    std::atomic<bool> fConnected {false};
    /* a write found the other end gone, which the loop has to act on */
    std::atomic<bool> fBroken {false};

    /* the connect or read in flight, which signals fEvent when it ends */
    Op fOp = OP_NONE;
    OVERLAPPED fOv {};
    HANDLE fEvent = nullptr;
    uint8_t fReadBuf[PIPE_BUFFER_SIZE];
    /* the event the last Prepare() watched, if any */
    bool fWatching = false;

    /* input read but not yet taken by the target */
    std::vector<uint8_t> fPending;
    size_t fPendingPos = 0;
    /* the target had no room at the last Prepare() */
    bool fBlocked = false;

    HANDLE fWriteEvent = nullptr;

    bool StartConnect();
    void StartRead();
    void Finish();
    void Disconnected();
    void Deliver();

public:
    PipeConsole(EventLoop &loop, RunControl &run_control, const char *path);
    ~PipeConsole() override;

    bool Open(bool server);

    /* HostConsole */
    void WriteData(const uint8_t *buf, int len) override;
    void SetTarget(ConsoleTarget *target) override {fTarget = target;}
    void TargetReady() override;
    bool WaitConnected() override;

    /* PollSource */
    void Prepare(WaitSet &ws) override;
    void Dispatch(WaitSet &ws) override;
};


PipeConsole::PipeConsole(EventLoop &loop, RunControl &run_control,
                         const char *path):
    fLoop(loop),
    fRunControl(run_control),
    fPath(path)
{
    fEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    fWriteEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    fLoop.Add(this);
}


PipeConsole::~PipeConsole()
{
    fLoop.Remove(this);
    if (fPipe != INVALID_HANDLE_VALUE) {
        if (fOp != OP_NONE) {
            DWORD n;
            CancelIoEx(fPipe, &fOv);
            GetOverlappedResult(fPipe, &fOv, &n, TRUE);
        }
        CloseHandle(fPipe);
    }
    CloseHandle(fEvent);
    CloseHandle(fWriteEvent);
}


/* "\\.\pipe\name", or with forward slashes. */
static bool is_pipe_name(const std::string &path)
{
    static const char prefix[] = "\\\\.\\pipe\\";
    size_t n = sizeof(prefix) - 1;

    if (path.size() <= n)
        return false;
    for (size_t i = 0; i < n; i++) {
        char c = path[i] == '/' ? '\\' : path[i];
        if (tolower((unsigned char)c) != prefix[i])
            return false;
    }
    return true;
}


bool PipeConsole::Open(bool server)
{
    if (!is_pipe_name(fPath)) {
        fprintf(stderr, "%s: a pipe is named \\\\.\\pipe\\<name>\n",
                fPath.c_str());
        return false;
    }
    int n = MultiByteToWideChar(CP_UTF8, 0, fPath.c_str(), -1, nullptr, 0);
    std::wstring wpath(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, fPath.c_str(), -1, &wpath[0], n);

    fServer = server;
    if (server) {
        fPipe = CreateNamedPipeW(wpath.c_str(),
                                 PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED |
                                     FILE_FLAG_FIRST_PIPE_INSTANCE,
                                 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE |
                                     PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
                                 1, PIPE_BUFFER_SIZE, PIPE_BUFFER_SIZE, 0,
                                 nullptr);
        if (fPipe == INVALID_HANDLE_VALUE) {
            fprintf(stderr, "%s: could not create the pipe (error %lu)\n",
                    fPath.c_str(), GetLastError());
            return false;
        }
        return StartConnect();
    }

    for (;;) {
        fPipe = CreateFileW(wpath.c_str(), GENERIC_READ | GENERIC_WRITE, 0,
                            nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
                            nullptr);
        if (fPipe != INVALID_HANDLE_VALUE)
            break;
        /* the server is between clients */
        if (GetLastError() == ERROR_PIPE_BUSY &&
            WaitNamedPipeW(wpath.c_str(), NMPWAIT_USE_DEFAULT_WAIT))
            continue;
        fprintf(stderr, "%s: could not connect to the pipe (error %lu)\n",
                fPath.c_str(), GetLastError());
        return false;
    }
    fConnected = true;
    return true;
}


/* Wait for the next client. */
bool PipeConsole::StartConnect()
{
    ResetEvent(fEvent);
    fOv = OVERLAPPED {};
    fOv.hEvent = fEvent;
    if (!ConnectNamedPipe(fPipe, &fOv)) {
        switch (GetLastError()) {
        case ERROR_IO_PENDING:
            fOp = OP_CONNECT;
            return true;
        case ERROR_PIPE_CONNECTED:
            /* it came between creating the pipe and this */
            break;
        default:
            fprintf(stderr, "%s: could not wait for a client (error %lu)\n",
                    fPath.c_str(), GetLastError());
            return false;
        }
    }
    fConnected = true;
    return true;
}


void PipeConsole::StartRead()
{
    ResetEvent(fEvent);
    fOv = OVERLAPPED {};
    fOv.hEvent = fEvent;
    /* one that finishes at once signals the event as well */
    if (ReadFile(fPipe, fReadBuf, sizeof(fReadBuf), nullptr, &fOv) ||
        GetLastError() == ERROR_IO_PENDING) {
        fOp = OP_READ;
        return;
    }
    Disconnected();
}


/* The connect or read that signalled fEvent. */
void PipeConsole::Finish()
{
    DWORD n = 0;
    BOOL ok = GetOverlappedResult(fPipe, &fOv, &n, FALSE);
    Op op = fOp;

    fOp = OP_NONE;
    if (op == OP_CONNECT) {
        if (ok)
            fConnected = true;
        else
            StartConnect();
    } else if (op == OP_READ) {
        if (ok)
            fPending.insert(fPending.end(), fReadBuf, fReadBuf + n);
        else
            Disconnected();
    }
}


void PipeConsole::Disconnected()
{
    fConnected = false;
    fBroken = false;
    if (fServer) {
        DisconnectNamedPipe(fPipe);
        StartConnect();
    }
}


bool PipeConsole::WaitConnected()
{
    if (fConnected || fOp != OP_CONNECT)
        return fConnected;
    fprintf(stderr, "waiting for a client on %s\n", fPath.c_str());
    while (WaitForSingleObject(fEvent, PIPE_RETRY_MS) == WAIT_TIMEOUT) {
        if (fRunControl.ShutdownRequested())
            return false;
    }
    Finish();
    return fConnected;
}


/* With the device lock held, like the loop's side; a failure is left for
   the loop to act on, since a read of its own may still be in flight. */
void PipeConsole::WriteData(const uint8_t *buf, int len)
{
    if (!fConnected || fBroken)
        return;
    while (len > 0) {
        OVERLAPPED ov {};
        DWORD n = 0;

        ResetEvent(fWriteEvent);
        ov.hEvent = fWriteEvent;
        if (!WriteFile(fPipe, buf, len, nullptr, &ov) &&
            GetLastError() != ERROR_IO_PENDING) {
            fBroken = true;
            fLoop.Wake();
            return;
        }
        /* the guest waits for a slow client, as it does for the terminal */
        if (!GetOverlappedResult(fPipe, &ov, &n, TRUE)) {
            fBroken = true;
            fLoop.Wake();
            return;
        }
        buf += n;
        len -= n;
    }
}


void PipeConsole::TargetReady()
{
    if (fBlocked) {
        fBlocked = false;
        fLoop.Wake();
    }
}


/* As much of what was read as the target takes. */
void PipeConsole::Deliver()
{
    int len = std::min<int>(fTarget->ReceiveRoom(),
                            fPending.size() - fPendingPos);
    if (len <= 0)
        return;
    fTarget->Receive(fPending.data() + fPendingPos, len);
    fPendingPos += len;
    if (fPendingPos == fPending.size()) {
        fPending.clear();
        fPendingPos = 0;
    }
}


void PipeConsole::Prepare(WaitSet &ws)
{
    fWatching = false;
    if (fPipe == INVALID_HANDLE_VALUE)
        return;
    /* with a read in flight, its failure says the same */
    if (fBroken && fOp == OP_NONE)
        Disconnected();
    if (fTarget != nullptr && !fPending.empty()) {
        if (fTarget->ReceiveRoom() <= 0)
            fBlocked = true;
        else
            ws.LimitTimeout(0);
    }
    /* A read stays in flight, which is also how a client that went is
       noticed, until what was read has gone. */
    if (fOp == OP_NONE && fConnected && fPending.empty())
        StartRead();
    if (fOp != OP_NONE) {
        ws.WatchHandle(fEvent);
        fWatching = true;
    }
}


void PipeConsole::Dispatch(WaitSet &ws)
{
    if (fWatching && ws.IsSignaled(fEvent))
        Finish();
    if (fTarget != nullptr)
        Deliver();
}


std::unique_ptr<HostConsole> pipe_console_open(EventLoop &loop,
                                               RunControl &run_control,
                                               const char *path, bool server)
{
    auto s = std::make_unique<PipeConsole>(loop, run_control, path);
    if (!s->Open(server))
        return nullptr;
    return s;
}
