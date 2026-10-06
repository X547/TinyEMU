/*
 * Host timer
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

#include <stdint.h>

/* The device half of a timer. */
class TimerTarget {
public:
    virtual ~TimerTarget() = default;

    /* The deadline passed. Called from the event loop with the device lock
       held, possibly a millisecond or more late. */
    virtual void TimerExpired() = 0;
};


/* A one shot timer on host_monotonic_us(), for a device whose work follows
   wall clock time rather than the guest's accesses. */
class HostTimer {
public:
    TimerTarget *target = nullptr; /* set by the device */

    virtual ~HostTimer() = default;

    /* Fires once at or after 'deadline_us'. Arming again replaces the
       deadline. */
    virtual void Arm(uint64_t deadline_us) = 0;
    virtual void Disarm() = 0;
};
