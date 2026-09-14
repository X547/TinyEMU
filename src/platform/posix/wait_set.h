/*
 * Event loop wait set: select()
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

#include <sys/select.h>

#include "event_loop.h"


class WaitSet {
public:
    /* Filled in by the sources; libraries that fill descriptor sets
       themselves are handed these directly. */
    fd_set rfds, wfds, efds;
    int fd_max = -1;
    int timeout_ms;
    /* The select() result, valid in Dispatch(). */
    int ready = 0;

    explicit WaitSet(int timeout): timeout_ms(timeout)
    {
        FD_ZERO(&rfds);
        FD_ZERO(&wfds);
        FD_ZERO(&efds);
    }

    void WatchRead(int fd)
    {
        FD_SET(fd, &rfds);
        if (fd > fd_max)
            fd_max = fd;
    }

    bool IsReadable(int fd) {return ready > 0 && FD_ISSET(fd, &rfds);}

    void LimitTimeout(int ms)
    {
        if (ms < timeout_ms)
            timeout_ms = ms;
    }
};
