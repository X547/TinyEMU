/*
 * Wayland shared memory buffers
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

#include "shm_buffer.h"


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


ShmBufferRing::~ShmBufferRing()
{
    for (ShmBuffer *buffer : fBuffers)
        Destroy(buffer);
    for (ShmBuffer *buffer : fDropped)
        Destroy(buffer);
}


void ShmBufferRing::HandleRelease(void *data, wl_buffer *wl_buffer)
{
    (void)wl_buffer;
    auto *buffer = static_cast<ShmBuffer *>(data);
    buffer->ring->Released(buffer);
}


void ShmBufferRing::Released(ShmBuffer *buffer)
{
    buffer->busy = false;
    auto it = std::find(fDropped.begin(), fDropped.end(), buffer);
    if (it != fDropped.end()) {
        fDropped.erase(it);
        Destroy(buffer);
    }
    if (fReleased)
        fReleased();
}


void ShmBufferRing::Destroy(ShmBuffer *buffer)
{
    if (buffer->buffer != nullptr)
        wl_buffer_destroy(buffer->buffer);
    if (buffer->pixels != nullptr)
        munmap(buffer->pixels, buffer->size);
    delete buffer;
}


ShmBuffer *ShmBufferRing::Create(int width, int height)
{
    static const wl_buffer_listener listener = {
        .release = HandleRelease,
    };
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
    buffer->ring = this;
    buffer->buffer = wl_buffer;
    buffer->pixels = static_cast<uint32_t *>(pixels);
    buffer->size = size;
    buffer->width = width;
    buffer->height = height;
    buffer->stale = ScreenRect(0, 0, width, height);
    wl_buffer_add_listener(wl_buffer, &listener, buffer);
    return buffer;
}


ShmBuffer *ShmBufferRing::Acquire(int width, int height)
{
    if (!fBuffers.empty() && (fBuffers[0]->width != width ||
                              fBuffers[0]->height != height)) {
        for (ShmBuffer *buffer : fBuffers) {
            if (buffer->busy)
                fDropped.push_back(buffer);
            else
                Destroy(buffer);
        }
        fBuffers.clear();
    }

    for (ShmBuffer *buffer : fBuffers) {
        if (!buffer->busy)
            return buffer;
    }
    if (fBuffers.size() >= fMaxBuffers || width <= 0 || height <= 0)
        return nullptr;
    ShmBuffer *buffer = Create(width, height);
    if (buffer != nullptr)
        fBuffers.push_back(buffer);
    return buffer;
}


void ShmBufferRing::Attached(ShmBuffer *buffer)
{
    buffer->busy = true;
    buffer->stale = ScreenRect();
}


void ShmBufferRing::Invalidate(const ScreenRect &r)
{
    for (ShmBuffer *buffer : fBuffers)
        buffer->stale |= r & ScreenRect(0, 0, buffer->width, buffer->height);
}
