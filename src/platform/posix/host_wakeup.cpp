/*
 * Waking a sleeping thread: POSIX
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
#include "host_wakeup.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <mutex>

#ifdef __linux__
#include <sys/prctl.h>
#endif


struct HostWakeup::Impl {
    std::mutex mutex;
    std::condition_variable cond;
    std::atomic<bool> kicked {false};
    std::atomic<bool> sleeping {false};
};


HostWakeup::HostWakeup():
    fImpl(std::make_unique<Impl>())
{
}


HostWakeup::~HostWakeup() = default;


/* kicked is set before sleeping is read here, and sleeping before kicked is
   read in Wait(), so one of the two sides always sees the other. */
void HostWakeup::Kick()
{
    Impl &s = *fImpl;

    s.kicked.store(true);
    if (s.sleeping.load()) {
        std::lock_guard<std::mutex> locker(s.mutex);
        s.cond.notify_one();
    }
}


void HostWakeup::Wait(int64_t timeout_us)
{
    Impl &s = *fImpl;
#ifdef __linux__
    /* Linux lets a sleep run 50 us late by default. */
    static thread_local bool precise = false;
    if (!precise) {
        prctl(PR_SET_TIMERSLACK, 1UL, 0UL, 0UL, 0UL);
        precise = true;
    }
#endif
    std::unique_lock<std::mutex> locker(s.mutex);

    s.sleeping.store(true);
    if (!s.kicked.load()) {
        auto kicked = [&s]() {return s.kicked.load();};
        if (timeout_us < 0)
            s.cond.wait(locker, kicked);
        else
            s.cond.wait_for(locker, std::chrono::microseconds(timeout_us),
                            kicked);
    }
    s.kicked.store(false);
    s.sleeping.store(false);
}
