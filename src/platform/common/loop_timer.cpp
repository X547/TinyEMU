/*
 * Timers run by the event loop
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
#include "host_time.h"
#include "platform_backends.h"
#include "wait_set.h"


/* The loop waits in whole milliseconds, so a deadline is met to within one. */
class LoopTimer final: public HostTimer, public PollSource {
private:
    EventLoop &fLoop;
    bool fArmed = false;
    bool fDispatching = false;
    uint64_t fDeadlineUs = 0;

public:
    explicit LoopTimer(EventLoop &loop): fLoop(loop) {fLoop.Add(this);}
    ~LoopTimer() override {fLoop.Remove(this);}

    /* HostTimer */
    void Arm(uint64_t deadline_us) override;
    void Disarm() override {fArmed = false;}

    /* PollSource */
    void Prepare(WaitSet &ws) override;
    void Dispatch(WaitSet &ws) override;
};


void LoopTimer::Arm(uint64_t deadline_us)
{
    bool sooner = !fArmed || deadline_us < fDeadlineUs;

    fArmed = true;
    fDeadlineUs = deadline_us;
    /* The loop may be in a wait that knows nothing of this deadline. One
       that is dispatching prepares again anyway. */
    if (sooner && !fDispatching) {
        fLoop.Wake();
    }
}


void LoopTimer::Prepare(WaitSet &ws)
{
    if (!fArmed) {
        return;
    }
    uint64_t now = host_monotonic_us();
    if (now >= fDeadlineUs) {
        ws.LimitTimeout(0);
        return;
    }
    uint64_t ms = (fDeadlineUs - now + 999) / 1000;
    ws.LimitTimeout(ms > 1000 ? 1000 : (int)ms);
}


void LoopTimer::Dispatch(WaitSet &ws)
{
    (void)ws;
    if (!fArmed || host_monotonic_us() < fDeadlineUs || target == nullptr) {
        return;
    }
    fArmed = false;
    fDispatching = true;
    target->TimerExpired();
    fDispatching = false;
}


std::unique_ptr<HostTimer> loop_timer_open(EventLoop &loop)
{
    return std::make_unique<LoopTimer>(loop);
}
