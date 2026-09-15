/*
 * Event loop
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

#include <assert.h>
#include <algorithm>

/* How long a wait for setup work may block before looking again. */
#define RUN_UNTIL_TIMEOUT 10000 /* ms */
/* How long the thread waits when no source asks for less. */
#define IDLE_TIMEOUT 1000 /* ms */


void EventLoop::Add(PollSource *source)
{
    assert(!fThread.joinable());
    fSources.push_back(source);
}


void EventLoop::Remove(PollSource *source)
{
    assert(!fThread.joinable());
    fSources.erase(std::remove(fSources.begin(), fSources.end(), source),
                   fSources.end());
}


void EventLoop::RunUntil(const std::function<bool()> &done)
{
    assert(!fThread.joinable());
    while (!done()) {
        Wait(RUN_UNTIL_TIMEOUT);
    }
}


void EventLoop::RunUntilIdle()
{
    RunUntil([this]() {
        return std::none_of(fSources.begin(), fSources.end(),
                            [](PollSource *source) {return source->Busy();});
    });
}


void EventLoop::Start(DeviceLock &lock)
{
    assert(!fThread.joinable());
    fDeviceLock = &lock;
    fStopRequested.store(false);
    fThread = std::thread([this]() {ThreadLoop();});
}


void EventLoop::Stop()
{
    if (!fThread.joinable())
        return;
    fStopRequested.store(true);
    Wake();
    fThread.join();
    fDeviceLock = nullptr;
}


void EventLoop::ThreadLoop()
{
    while (!fStopRequested.load()) {
        Wait(IDLE_TIMEOUT);
    }
}
