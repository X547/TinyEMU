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

#include <functional>
#include <vector>

/* What one wait watches. Defined per host in wait_set.h. */
class WaitSet;


/* Something the event loop waits on: a descriptor, a window system's event
   queue, a library with sockets of its own. */
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
    bool fQuitRequested = false;
    int fExitCode = 0;

public:
    void Add(PollSource *source);
    void Remove(PollSource *source);

    /* Wait up to timeout_ms for a source, then dispatch every source.
       Implemented per host. */
    void Wait(int timeout_ms);

    void RunUntil(const std::function<bool()> &done);
    /* Until no source is busy. */
    void RunUntilIdle();

    /* The first request decides the exit code. */
    void RequestQuit(int exit_code);
    bool QuitRequested() const {return fQuitRequested;}
    int ExitCode() const {return fExitCode;}
};
