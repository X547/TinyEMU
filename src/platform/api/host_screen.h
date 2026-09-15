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

/* Implemented by a display device. A frame buffer the guest writes behind
   the device's back is polled by the machine instead (FBDevice); a device
   that is told what changed reports it to the screen as it happens. */
class ScreenSource {
public:
    virtual ~ScreenSource() = default;

    /* The screen this source is shown on; nullptr detaches it. */
    virtual void SetScreen(HostScreen *screen) {(void)screen;}
    /* Whether the source can follow a resize of the screen. */
    virtual bool Resizable() {return false;}
    /* The host resized the screen, e.g. the user resized the window. */
    virtual void ScreenResized(int width, int height)
        {(void)width; (void)height;}
};


/* A window, or whatever else shows the guest's display. It is called from
   whichever thread runs the source, and hands the work to its own. */
class HostScreen {
public:
    virtual ~HostScreen() = default;

    /* Show 'source' in a width x height area. nullptr detaches it. */
    virtual void SetSource(ScreenSource *source, int width, int height) = 0;
    /* 32 bit xRGB pixels, 'stride' bytes per row; nullptr blanks the
       screen. The pixels are copied here when the frame buffer changes and
       in Update(), and not read at any other time. */
    virtual void SetFramebuffer(uint8_t *data, int width, int height,
                                int stride) = 0;
    virtual void Update(int x, int y, int w, int h) = 0;
    /* A cursor image of 32 bit ARGB pixels, copied; nullptr hides the
       cursor. (hot_x, hot_y) is the pixel of the image that points. A
       screen may show it as the host's own cursor, which follows the host
       pointer rather than MoveCursor(). */
    virtual void SetCursor(const uint32_t *pixels, int width, int height,
                           int hot_x, int hot_y) = 0;
    /* Where the cursor image's top left corner is, in frame buffer
       pixels. */
    virtual void MoveCursor(int x, int y) = 0;
};
