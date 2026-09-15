/*
 * The device lock
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

#include <assert.h>
#include <mutex>

/* Serializes the device tree and the host back ends the devices call. A
   thread takes it where it enters device code; device code never takes it,
   so it is not reentrant. */
class DeviceLock {
private:
    std::mutex fMutex;
    static inline thread_local bool sHeld = false;

public:
    void Lock()
    {
        assert(!sHeld);
        fMutex.lock();
        sHeld = true;
    }

    void Unlock()
    {
        assert(sHeld);
        sHeld = false;
        fMutex.unlock();
    }

    /* Whether the calling thread holds it. */
    static bool IsHeld() {return sHeld;}
};


class DeviceLocker {
private:
    DeviceLock &fLock;

public:
    explicit DeviceLocker(DeviceLock &lock): fLock(lock) {fLock.Lock();}
    ~DeviceLocker() {fLock.Unlock();}

    DeviceLocker(const DeviceLocker &) = delete;
    DeviceLocker &operator=(const DeviceLocker &) = delete;
};
