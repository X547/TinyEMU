/*
 * Waking a sleeping thread: Win32
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

#include <stdio.h>
#include <stdlib.h>

#include <windows.h>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif


/* An auto-reset event holds a kick until the next wait takes it. The timeout
   is a waitable timer: std::condition_variable waits in whole system ticks,
   about 15.6 ms, whatever timeBeginPeriod() says. */
struct HostWakeup::Impl {
    HANDLE event;
    HANDLE timer;
};


HostWakeup::HostWakeup():
    fImpl(std::make_unique<Impl>())
{
    fImpl->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    /* Before Windows 10 1803 there is only the ordinary kind, which keeps to
       timeBeginPeriod(). */
    fImpl->timer = CreateWaitableTimerExW(
        nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
        TIMER_ALL_ACCESS);
    if (fImpl->timer == nullptr)
        fImpl->timer = CreateWaitableTimerW(nullptr, FALSE, nullptr);
    if (fImpl->event == nullptr || fImpl->timer == nullptr) {
        fprintf(stderr, "cannot create a wakeup: error %lu\n",
                GetLastError());
        exit(1);
    }
}


HostWakeup::~HostWakeup()
{
    CloseHandle(fImpl->timer);
    CloseHandle(fImpl->event);
}


void HostWakeup::Kick()
{
    SetEvent(fImpl->event);
}


void HostWakeup::Wait(int64_t timeout_us)
{
    HANDLE handles[2] = {fImpl->event, fImpl->timer};
    LARGE_INTEGER due;

    /* relative, in 100 ns units */
    due.QuadPart = -(timeout_us * 10);
    SetWaitableTimer(fImpl->timer, &due, 0, nullptr, nullptr, FALSE);
    WaitForMultipleObjects(2, handles, FALSE, INFINITE);
    CancelWaitableTimer(fImpl->timer);
}
