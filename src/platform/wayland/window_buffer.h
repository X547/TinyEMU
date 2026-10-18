/*
 * Wayland window buffers
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

#include <stddef.h>
#include <stdint.h>

#include <functional>
#include <vector>

#include "screen_rect.h"

struct wl_buffer;
struct wl_shm;
class BufferRing;


/* 32 bit pixels a wl_buffer shows, 'pitch' pixels per row. */
struct WindowBuffer {
    BufferRing *ring = nullptr;
    wl_buffer *buffer = nullptr;
    uint32_t *pixels = nullptr;
    int pitch = 0;
    int width = 0;
    int height = 0;
    /* attached, and not yet released by the compositor */
    bool busy = false;
    /* what changed since the pixels were last drawn */
    ScreenRect stale;

    virtual ~WindowBuffer();
};


/* Makes the buffers of a ring. */
class BufferAllocator {
public:
    virtual ~BufferAllocator() = default;

    /* A buffer of that size, with its wl_buffer and pixels; nullptr when
       none could be made. */
    virtual WindowBuffer *Create(int width, int height) = 0;
};


/* Buffers in memory shared with the compositor. */
class ShmAllocator final: public BufferAllocator {
private:
    wl_shm *fShm;
    uint32_t fFormat;

public:
    /* 'format' is a WL_SHM_FORMAT_ value. */
    ShmAllocator(wl_shm *shm, uint32_t format): fShm(shm), fFormat(format) {}

    WindowBuffer *Create(int width, int height) override;
};


/* The buffers one surface takes turns on, all of one size. One the
   compositor still reads is not drawn into; a buffer that is reused is
   drawn again only where it is out of date. */
class BufferRing {
private:
    BufferAllocator &fAllocator;
    size_t fMaxBuffers;
    /* a buffer came back from the compositor */
    std::function<void()> fReleased;
    std::vector<WindowBuffer *> fBuffers;
    /* of an older size, destroyed once released */
    std::vector<WindowBuffer *> fDropped;
    bool fCreateFailed = false;

    void Released(WindowBuffer *buffer);
    static void HandleRelease(void *data, wl_buffer *wl_buffer);

public:
    /* 'released' is called from the compositor's events whenever a buffer
       stops being busy. The allocator outlives the ring. */
    BufferRing(BufferAllocator &allocator, size_t max_buffers,
               std::function<void()> released):
        fAllocator(allocator), fMaxBuffers(max_buffers),
        fReleased(std::move(released)) {}
    ~BufferRing();

    /* A buffer of that size that is not busy, its 'stale' area to be
       drawn; nullptr when all are busy or none could be made. A size
       other than the last one drops the old buffers. */
    WindowBuffer *Acquire(int width, int height);
    /* the last Acquire() returned nullptr because no buffer could be made */
    bool CreateFailed() const {return fCreateFailed;}
    /* 'buffer', drawn, is attached and committed. */
    void Attached(WindowBuffer *buffer);
    /* 'r' changed, in every buffer. */
    void Invalidate(const ScreenRect &r);
};
