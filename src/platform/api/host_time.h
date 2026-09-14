/*
 * Host clock
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

/* Implemented once per host; the build picks the file. */

struct HostDateTime {
    int year;    /* e.g. 2026 */
    int month;   /* 1..12 */
    int day;     /* 1..31 */
    int weekday; /* 0 is Sunday */
    int hour;
    int minute;
    int second;
    int usec;
};

/* Microseconds from an arbitrary origin; never goes backwards. */
uint64_t host_monotonic_us();

/* Microseconds since the Unix epoch. */
int64_t host_real_time_us();

/* The current calendar time, in local time or in UTC. */
void host_date_time(HostDateTime *dt, bool local);

/* Local time minus UTC. */
int host_utc_offset_minutes();
