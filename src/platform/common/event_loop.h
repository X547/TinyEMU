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
#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

class DeviceLock;

/* What one wait watches. Defined per host in wait_set.h. */
class WaitSet;
/* How another thread interrupts a wait. Defined per host. */
class LoopWaker;


/* Something the event loop waits on: a descriptor, a library with sockets
   of its own. Once the loop runs on its thread, Prepare() and Dispatch() are
   called with the device lock held. */
class PollSource {
public:
    virtual ~PollSource() = default;

    /* Before waiting: watch what is needed and lower the timeout. A source
       that has already done work sets the timeout to 0. */
    virtual void Prepare(WaitSet &ws) = 0;
    /* After every wait, whether or not anything became ready. */
    virtual void Dispatch(WaitSet &ws) = 0;
    /* True while work is in flight that RunUntilIdle() waits for. */
    virtual bool Busy() {return false;}
};


class EventLoop {
private:
    std::vector<PollSource *> fSources;
    std::unique_ptr<LoopWaker> fWaker;
    std::atomic<bool> fWakePending {false};

    std::thread fThread;
    std::atomic<bool> fStopRequested {false};
    /* held around the sources while the thread runs */
    DeviceLock *fDeviceLock = nullptr;

    void ThreadLoop();

public:
    EventLoop();
    ~EventLoop();

    /* Only while the thread is not running. */
    void Add(PollSource *source);
    void Remove(PollSource *source);

    /* Wait up to timeout_ms for a source, then dispatch every source.
       Implemented per host. */
    void Wait(int timeout_ms);

    /* Only before Start(), on the thread that set things up. */
    void RunUntil(const std::function<bool()> &done);
    /* Until no source is busy. */
    void RunUntilIdle();

    /* Runs the sources on a thread of their own until Stop(). */
    void Start(DeviceLock &lock);
    void Stop();

    /* Makes the current or next wait return at once, so that the sources
       are prepared again. Any thread, and signal handlers. */
    void Wake();
};
