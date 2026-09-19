/*
 * Event loop wait set: I/O completion port
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
#pragma once

#include <winsock2.h>
#include <windows.h>
#include <algorithm>
#include <vector>

#include "event_loop.h"


/* The handles one wait watches, however many. A handle should stay signalled
   until its source acts on it, like a manual-reset event or a console input
   buffer: a wait that its source stops watching may consume an auto-reset
   signal. */
class WaitSet {
private:
    friend class LoopWaker;

    std::vector<HANDLE> fWatched;
    std::vector<HANDLE> fSignaled;
    std::vector<OVERLAPPED_ENTRY> fCompletions;

public:
    /* Sockets, for libraries that fill descriptor sets themselves. After
       the wait they hold what is ready, as select() leaves them. */
    fd_set rfds, wfds, efds;
    /* unused: Winsock numbers no sockets, but the libraries fill it */
    int fd_max = -1;
    /* the select() result, valid in Dispatch() */
    int ready = 0;
    int timeout_ms;

    explicit WaitSet(int timeout): timeout_ms(timeout)
    {
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
    }

    void WatchHandle(HANDLE h)
    {
        if (std::find(fWatched.begin(), fWatched.end(), h) == fWatched.end())
            fWatched.push_back(h);
    }

    /* Valid in Dispatch(). */
    bool IsSignaled(HANDLE h) const
    {
        return std::find(fSignaled.begin(), fSignaled.end(), h) !=
               fSignaled.end();
    }

    /* Valid in Dispatch(): the overlapped I/O that finished during the
       wait, which each source picks its own out of by key. */
    const std::vector<OVERLAPPED_ENTRY> &Completions() const
    {
        return fCompletions;
    }

    void LimitTimeout(int ms)
    {
        if (ms < timeout_ms)
            timeout_ms = ms;
    }
};


/* Makes the overlapped I/O on 'file' finish through the loop: a request
   that does not finish at once shows up in Completions() with 'key'. One
   that finishes at once is not reported again. */
bool event_loop_attach(EventLoop &loop, HANDLE file, void *key);
