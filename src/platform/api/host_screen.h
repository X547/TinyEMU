/*
 * Host screen
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

class HostScreen;

/* Implemented by a display device. */
class ScreenSource {
public:
    virtual ~ScreenSource() = default;

    /* Report the frame buffer with SetFramebuffer(), then each rectangle
       that changed with Update(). */
    virtual void Refresh(HostScreen *screen) = 0;
};


/* A window, or whatever else shows the guest's display. */
class HostScreen {
public:
    virtual ~HostScreen() = default;

    /* Show 'source' in a width x height area; it is refreshed from the
       event loop. nullptr detaches it. */
    virtual void SetSource(ScreenSource *source, int width, int height) = 0;
    /* 32 bit xRGB pixels, 'stride' bytes per row. */
    virtual void SetFramebuffer(uint8_t *data, int width, int height,
                                int stride) = 0;
    virtual void Update(int x, int y, int w, int h) = 0;
};
