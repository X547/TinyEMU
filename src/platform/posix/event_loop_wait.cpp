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

#include "wait_set.h"


void EventLoop::Wait(int timeout_ms)
{
    WaitSet ws(timeout_ms);
    struct timeval tv;

    /* a source may remove itself while being dispatched */
    std::vector<PollSource *> sources = fSources;

    for (PollSource *source : sources) {
        source->Prepare(ws);
    }
    if (ws.timeout_ms < 0)
        ws.timeout_ms = 0;
    tv.tv_sec = ws.timeout_ms / 1000;
    tv.tv_usec = (ws.timeout_ms % 1000) * 1000;
    ws.ready = select(ws.fd_max + 1, &ws.rfds, &ws.wfds, &ws.efds, &tv);
    for (PollSource *source : sources) {
        source->Dispatch(ws);
    }
}
