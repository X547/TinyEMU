/*
 * Event loop wait: select()
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
#include "event_loop.h"

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>

#include "device_lock.h"
#include "wait_set.h"


/* A pipe that a wait also watches. */
class LoopWaker {
public:
    int fds[2];

    LoopWaker()
    {
        if (pipe(fds) < 0) {
            perror("pipe");
            exit(1);
        }
        for (int fd : fds) {
            fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
            fcntl(fd, F_SETFD, FD_CLOEXEC);
        }
    }

    ~LoopWaker()
    {
        close(fds[0]);
        close(fds[1]);
    }

    void Drain()
    {
        char buf[16];
        while (read(fds[0], buf, sizeof(buf)) > 0) {
        }
    }
};


EventLoop::EventLoop():
    fWaker(std::make_unique<LoopWaker>())
{
}


EventLoop::~EventLoop()
{
    Stop();
}


void EventLoop::Wake()
{
    if (!fWakePending.exchange(true)) {
        char c = 0;
        if (write(fWaker->fds[1], &c, 1) < 0) {
            /* the pipe is full, so a wake is pending anyway */
        }
    }
}


void EventLoop::Wait(int timeout_ms)
{
    WaitSet ws(timeout_ms);
    struct timeval tv;

    /* a source may remove itself while being dispatched */
    std::vector<PollSource *> sources = fSources;

    ws.WatchRead(fWaker->fds[0]);
    if (fDeviceLock != nullptr)
        fDeviceLock->Lock();
    for (PollSource *source : sources) {
        source->Prepare(ws);
    }
    if (fDeviceLock != nullptr)
        fDeviceLock->Unlock();

    if (ws.timeout_ms < 0)
        ws.timeout_ms = 0;
    tv.tv_sec = ws.timeout_ms / 1000;
    tv.tv_usec = (ws.timeout_ms % 1000) * 1000;
    ws.ready = select(ws.fd_max + 1, &ws.rfds, &ws.wfds, &ws.efds, &tv);

    if (ws.IsReadable(fWaker->fds[0])) {
        /* Cleared after draining: a wake that finds it still set needs no
           byte of its own, because the sources are prepared again after
           this anyway. */
        fWaker->Drain();
        fWakePending.store(false);
    }

    if (fDeviceLock != nullptr)
        fDeviceLock->Lock();
    for (PollSource *source : sources) {
        source->Dispatch(ws);
    }
    if (fDeviceLock != nullptr)
        fDeviceLock->Unlock();
}
