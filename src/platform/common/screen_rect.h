/*
 * Rectangles and cursor blending for the display back ends
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

#include <algorithm>


/* A rectangle in pixels, right and bottom edges excluded. */
struct ScreenRect {
    int x0 = 0, y0 = 0, x1 = 0, y1 = 0;

    ScreenRect() = default;
    ScreenRect(int x, int y, int w, int h):
        x0(x), y0(y), x1(x + w), y1(y + h) {}

    bool Empty() const {return x0 >= x1 || y0 >= y1;}
    int Width() const {return x1 - x0;}
    int Height() const {return y1 - y0;}

    ScreenRect operator&(const ScreenRect &r) const
    {
        ScreenRect res;
        res.x0 = std::max(x0, r.x0);
        res.y0 = std::max(y0, r.y0);
        res.x1 = std::min(x1, r.x1);
        res.y1 = std::min(y1, r.y1);
        return res;
    }

    ScreenRect &operator|=(const ScreenRect &r)
    {
        if (r.Empty())
            return *this;
        if (Empty()) {
            *this = r;
        } else {
            x0 = std::min(x0, r.x0);
            y0 = std::min(y0, r.y0);
            x1 = std::max(x1, r.x1);
            y1 = std::max(y1, r.y1);
        }
        return *this;
    }
};


/* The parts of 'r' right of and below a width x height area at the origin;
   returns how many of the two are not empty. */
static inline int screen_rect_outside(const ScreenRect &r, int width,
                                      int height, ScreenRect out[2])
{
    int count = 0;
    ScreenRect right = r;
    ScreenRect below = r;

    right.x0 = std::max(r.x0, width);
    below.x1 = std::min(r.x1, width);
    below.y0 = std::max(r.y0, height);

    if (!right.Empty())
        out[count++] = right;
    if (!below.Empty())
        out[count++] = below;
    return count;
}


/* An ARGB cursor pixel over an xRGB one. */
static inline uint32_t cursor_blend(uint32_t dst, uint32_t src)
{
    uint32_t a = src >> 24;
    uint32_t out = 0;

    for (int shift = 0; shift < 24; shift += 8) {
        uint32_t s = (src >> shift) & 0xff;
        uint32_t d = (dst >> shift) & 0xff;
        out |= ((s * a + d * (255 - a) + 127) / 255) << shift;
    }
    return out;
}
