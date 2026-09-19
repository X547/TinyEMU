/*
 * Host clock: Windows
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
#include "host_time.h"

#include <windows.h>

/* 100 ns intervals from 1601 to 1970 */
#define FILETIME_UNIX_EPOCH 116444736000000000ULL


uint64_t host_monotonic_us()
{
    static LARGE_INTEGER freq;
    LARGE_INTEGER count;

    if (freq.QuadPart == 0)
        QueryPerformanceFrequency(&freq);
    QueryPerformanceCounter(&count);
    /* in two steps, so that the multiplication does not overflow */
    return (uint64_t)(count.QuadPart / freq.QuadPart) * 1000000 +
           (uint64_t)(count.QuadPart % freq.QuadPart) * 1000000 /
               freq.QuadPart;
}


/* 100 ns intervals since 1601, UTC */
static uint64_t filetime_now()
{
    FILETIME ft;

    GetSystemTimePreciseAsFileTime(&ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}


static uint64_t filetime_of(const SYSTEMTIME &st)
{
    FILETIME ft;

    SystemTimeToFileTime(&st, &ft);
    return ((uint64_t)ft.dwHighDateTime << 32) | ft.dwLowDateTime;
}


int64_t host_real_time_us()
{
    return (int64_t)(filetime_now() - FILETIME_UNIX_EPOCH) / 10;
}


void host_date_time(HostDateTime *dt, bool local)
{
    uint64_t now = filetime_now();
    FILETIME ft;
    SYSTEMTIME utc, st;

    ft.dwLowDateTime = (DWORD)now;
    ft.dwHighDateTime = (DWORD)(now >> 32);
    FileTimeToSystemTime(&ft, &utc);
    if (!local || !SystemTimeToTzSpecificLocalTime(nullptr, &utc, &st))
        st = utc;
    dt->year = st.wYear;
    dt->month = st.wMonth;
    dt->day = st.wDay;
    dt->weekday = st.wDayOfWeek;
    dt->hour = st.wHour;
    dt->minute = st.wMinute;
    dt->second = st.wSecond;
    dt->usec = (int)(now % 10000000) / 10;
}


int host_utc_offset_minutes()
{
    SYSTEMTIME utc, st;

    GetSystemTime(&utc);
    if (!SystemTimeToTzSpecificLocalTime(nullptr, &utc, &st))
        return 0;
    return (int)(((int64_t)filetime_of(st) - (int64_t)filetime_of(utc)) /
                 (10000000LL * 60));
}
