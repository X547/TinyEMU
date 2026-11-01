/*
 * Host console: a named pipe, as a pair of FIFOs
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
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>

#include <mutex>
#include <string>
#include <thread>

#include "cutils.h"
#include "host_thread.h"
#include "platform_backends.h"
#include "run_control.h"
#include "wait_set.h"

/* How often a wait for the reader looks again. */
#define PIPE_RETRY_US 100000


/* "<path>.in" carries what the guest receives and "<path>.out" what it
   sends. The output is open only while something reads it; what the guest
   sends meanwhile is dropped.

   The output is opened by a blocking open() on a thread of its own, which
   returns once a reader is there. A non-blocking one would fail while there
   is none, and on Haiku every such failure leaves a writer behind, so that
   readers never see the end of the output. */
class PipeConsole final: public HostConsole, public PollSource {
private:
    EventLoop &fLoop;
    RunControl &fRunControl;
    ConsoleTarget *fTarget = nullptr;
    std::string fInPath;
    std::string fOutPath;
    int fIn = -1;
    /* the output, used with the device lock held */
    int fOut = -1;
    bool fWatching = false;
    /* the target had no room at the last Prepare() */
    bool fBlocked = false;

    std::thread fOpener;
    std::mutex fOpenerMutex;
    /* the opener runs */
    bool fOpening = false;
    /* what it opened, not yet taken */
    int fOpened = -1;

    void StartOpener();
    bool TakeOpened();
    void StopOpener();

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
    fInPath(std::string(path) + ".in"),
    fOutPath(std::string(path) + ".out")
{
    fLoop.Add(this);
}


PipeConsole::~PipeConsole()
{
    fLoop.Remove(this);
    StopOpener();
    if (fIn >= 0)
        close(fIn);
    if (fOut >= 0)
        close(fOut);
}


static bool make_fifo(const char *path, bool server)
{
    struct stat st;

    if (stat(path, &st) == 0) {
        if (S_ISFIFO(st.st_mode))
            return true;
        fprintf(stderr, "%s: not a FIFO\n", path);
        return false;
    }
    if (errno == ENOENT && server && mkfifo(path, 0600) == 0)
        return true;
    fprintf(stderr, "%s: %s\n", path, strerror(errno));
    return false;
}


bool PipeConsole::Open(bool server)
{
    if (!make_fifo(fInPath.c_str(), server) ||
        !make_fifo(fOutPath.c_str(), server))
        return false;

    /* Open for writing as well, so that a writer going away is not the end
       of the input. */
    fIn = open(fInPath.c_str(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
    if (fIn < 0) {
        fprintf(stderr, "%s: %s\n", fInPath.c_str(), strerror(errno));
        return false;
    }
    /* a reader going away fails the write instead of ending the emulator */
    signal(SIGPIPE, SIG_IGN);
    return true;
}


/* Open the output once the next reader comes. */
void PipeConsole::StartOpener()
{
    std::lock_guard<std::mutex> locker(fOpenerMutex);
    if (fOpening || fOpened >= 0)
        return;
    if (fOpener.joinable())
        fOpener.join();
    fOpening = true;
    fOpener = std::thread([this]() {
        host_set_thread_name("fifo opener");
        int fd;
        do {
            /* the guest waits for a slow reader, as it does for the
               terminal */
            fd = open(fOutPath.c_str(), O_WRONLY | O_CLOEXEC);
        } while (fd < 0 && errno == EINTR);
        if (fd < 0)
            fprintf(stderr, "%s: %s\n", fOutPath.c_str(), strerror(errno));
        std::lock_guard<std::mutex> locker(fOpenerMutex);
        fOpened = fd;
        fOpening = false;
    });
}


/* Whether the opener has the output open, which is then fOut. */
bool PipeConsole::TakeOpened()
{
    std::lock_guard<std::mutex> locker(fOpenerMutex);
    if (fOpened < 0)
        return false;
    fOut = fOpened;
    fOpened = -1;
    return true;
}


void PipeConsole::StopOpener()
{
    bool opening;
    {
        std::lock_guard<std::mutex> locker(fOpenerMutex);
        opening = fOpening;
    }
    /* a reader of our own lets its open() return */
    int fd = opening ? open(fOutPath.c_str(), O_RDONLY | O_NONBLOCK |
                                                  O_CLOEXEC)
                     : -1;
    if (fOpener.joinable())
        fOpener.join();
    if (fd >= 0)
        close(fd);
    if (fOpened >= 0)
        close(fOpened);
    fOpened = -1;
}


bool PipeConsole::WaitConnected()
{
    bool told = false;

    StartOpener();
    while (!TakeOpened()) {
        {
            /* the opener gave up, and has said why */
            std::lock_guard<std::mutex> locker(fOpenerMutex);
            if (!fOpening && fOpened < 0)
                return false;
        }
        if (fRunControl.ShutdownRequested())
            return false;
        usleep(PIPE_RETRY_US);
        if (!told) {
            fprintf(stderr, "waiting for a reader on %s\n",
                    fOutPath.c_str());
            told = true;
        }
    }
    return true;
}


void PipeConsole::WriteData(const uint8_t *buf, int len)
{
    if (fOut < 0 && !TakeOpened())
        return;
    while (len > 0) {
        ssize_t n = write(fOut, buf, len);
        if (n < 0) {
            if (errno == EINTR)
                continue;
            /* the reader went away; the next one gets the output */
            close(fOut);
            fOut = -1;
            StartOpener();
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


void PipeConsole::Prepare(WaitSet &ws)
{
    fWatching = false;
    if (fTarget == nullptr || fIn < 0)
        return;
    if (fTarget->ReceiveRoom() <= 0) {
        fBlocked = true;
        return;
    }
    ws.WatchRead(fIn);
    fWatching = true;
}


void PipeConsole::Dispatch(WaitSet &ws)
{
    uint8_t buf[128];

    if (!fWatching || fTarget == nullptr || !ws.IsReadable(fIn))
        return;
    int len = min_int(fTarget->ReceiveRoom(), sizeof(buf));
    if (len <= 0)
        return;
    ssize_t n = read(fIn, buf, len);
    if (n > 0)
        fTarget->Receive(buf, n);
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
