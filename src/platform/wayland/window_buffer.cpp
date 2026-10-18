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
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#include <algorithm>

#include <wayland-client.h>

#include "window_buffer.h"


namespace {

struct ShmBuffer final: WindowBuffer {
    size_t size = 0;

    ~ShmBuffer() override
    {
        if (pixels != nullptr)
            munmap(pixels, size);
    }
};

} // namespace


/* An unnamed file of 'size' bytes to share with the compositor. */
static int create_shm_file(size_t size)
{
    int fd;

#ifdef MFD_CLOEXEC
    fd = memfd_create("tinyemu-shm", MFD_CLOEXEC);
#else
    fd = -1;
    for (int i = 0; i < 100 && fd < 0; i++) {
        char name[64];
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        snprintf(name, sizeof(name), "/tinyemu-%d-%ld", (int)getpid(),
                 (long)ts.tv_nsec);
        fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (fd >= 0)
            shm_unlink(name);
        else if (errno != EEXIST)
            break;
    }
#endif
    if (fd < 0)
        return -1;
    int res;
    do {
        res = ftruncate(fd, size);
    } while (res < 0 && errno == EINTR);
    if (res < 0) {
        close(fd);
        return -1;
    }
    return fd;
}


WindowBuffer::~WindowBuffer()
{
    if (buffer != nullptr)
        wl_buffer_destroy(buffer);
}


WindowBuffer *ShmAllocator::Create(int width, int height)
{
    size_t stride = (size_t)width * 4;
    size_t size = stride * height;

    int fd = create_shm_file(size);
    if (fd < 0) {
        fprintf(stderr, "Could not allocate a %zu byte window buffer: %s\n",
                size, strerror(errno));
        return nullptr;
    }
    void *pixels = mmap(nullptr, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd,
                        0);
    if (pixels == MAP_FAILED) {
        fprintf(stderr, "Could not map a window buffer: %s\n",
                strerror(errno));
        close(fd);
        return nullptr;
    }
    wl_shm_pool *pool = wl_shm_create_pool(fShm, fd, size);
    wl_buffer *wl_buffer = wl_shm_pool_create_buffer(pool, 0, width, height,
                                                     stride, fFormat);
    wl_shm_pool_destroy(pool);
    close(fd);

    auto *buffer = new ShmBuffer;
    buffer->buffer = wl_buffer;
    buffer->pixels = static_cast<uint32_t *>(pixels);
    buffer->size = size;
    buffer->pitch = width;
    buffer->width = width;
    buffer->height = height;
    return buffer;
}


BufferRing::~BufferRing()
{
    for (WindowBuffer *buffer : fBuffers)
        delete buffer;
    for (WindowBuffer *buffer : fDropped)
        delete buffer;
}


void BufferRing::HandleRelease(void *data, wl_buffer *wl_buffer)
{
    (void)wl_buffer;
    auto *buffer = static_cast<WindowBuffer *>(data);
    buffer->ring->Released(buffer);
}


void BufferRing::Released(WindowBuffer *buffer)
{
    buffer->busy = false;
    auto it = std::find(fDropped.begin(), fDropped.end(), buffer);
    if (it != fDropped.end()) {
        fDropped.erase(it);
        delete buffer;
    }
    if (fReleased)
        fReleased();
}


WindowBuffer *BufferRing::Acquire(int width, int height)
{
    static const wl_buffer_listener listener = {
        .release = HandleRelease,
    };

    fCreateFailed = false;
    if (!fBuffers.empty() && (fBuffers[0]->width != width ||
                              fBuffers[0]->height != height)) {
        for (WindowBuffer *buffer : fBuffers) {
            if (buffer->busy)
                fDropped.push_back(buffer);
            else
                delete buffer;
        }
        fBuffers.clear();
    }

    for (WindowBuffer *buffer : fBuffers) {
        if (!buffer->busy)
            return buffer;
    }
    if (fBuffers.size() >= fMaxBuffers || width <= 0 || height <= 0)
        return nullptr;
    WindowBuffer *buffer = fAllocator.Create(width, height);
    if (buffer == nullptr) {
        fCreateFailed = true;
        return nullptr;
    }
    buffer->ring = this;
    buffer->stale = ScreenRect(0, 0, width, height);
    wl_buffer_add_listener(buffer->buffer, &listener, buffer);
    fBuffers.push_back(buffer);
    return buffer;
}


void BufferRing::Attached(WindowBuffer *buffer)
{
    buffer->busy = true;
    buffer->stale = ScreenRect();
}


void BufferRing::Invalidate(const ScreenRect &r)
{
    for (WindowBuffer *buffer : fBuffers)
        buffer->stale |= r & ScreenRect(0, 0, buffer->width, buffer->height);
}
