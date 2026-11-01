/*
 * Wayland window scale
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

#include <math.h>

#include <algorithm>
#include <numeric>

#include "screen_rect.h"


/* How many of the output's pixels make one unit of the surface, in 120ths
   as wp_fractional_scale_v1 gives it. The window is drawn in pixels; the
   compositor takes sizes and positions in units. */
class WindowScale {
private:
    int fScale = kOne;

public:
    static const int kOne = 120;

    WindowScale() = default;
    explicit WindowScale(int scale): fScale(std::max(scale, 1)) {}

    bool operator==(const WindowScale &other) const
        {return fScale == other.fScale;}
    bool operator!=(const WindowScale &other) const
        {return fScale != other.fScale;}

    int Value() const {return fScale;}

    /* a size in units as pixels, rounded halfway away from zero as the
       protocol has it */
    int ToPixels(int units) const
        {return (units * fScale + kOne / 2) / kOne;}
    /* a position in units, as wl_pointer gives it, as the pixel it is on */
    int ToPixel(double units) const
        {return (int)floor(units * fScale / kOne);}
    /* a length meant for a scale of 1, at least 'min' pixels */
    int Metric(int length, int min = 1) const
        {return std::max(ToPixels(length), min);}

    /* The fewest units that are at least 'pixels' pixels; 'hint' when it is
       exactly that many. */
    int FitUnits(int pixels, int hint = 0) const
    {
        if (hint > 0 && ToPixels(hint) == pixels)
            return hint;
        int units = std::max(pixels * kOne / fScale, 1);
        while (units > 1 && ToPixels(units - 1) >= pixels)
            units--;
        while (ToPixels(units) < pixels)
            units++;
        return units;
    }

    /* 'r' in units: the units it covers whole when 'inner', all those it
       touches otherwise */
    ScreenRect ToUnits(const ScreenRect &r, bool inner) const
    {
        auto down = [this](int v) {return v * kOne / fScale;};
        auto up = [this](int v) {return (v * kOne + fScale - 1) / fScale;};
        ScreenRect out;
        out.x0 = inner ? up(r.x0) : down(r.x0);
        out.y0 = inner ? up(r.y0) : down(r.y0);
        out.x1 = inner ? down(r.x1) : up(r.x1);
        out.y1 = inner ? down(r.y1) : up(r.y1);
        return out;
    }

    /* Where an image 'size' pixels long with its hot spot at 'hot' goes in
       a cursor buffer for the compositor to show it pixel for pixel:
       'offset' pixels into a buffer 'pixels' long, which is 'units' long,
       the hot spot 'hot_units' into it. Both are on whole pixels and whole
       units when the scale has a short enough step, near them otherwise. */
    void PlaceCursor(int size, int hot, int &offset, int &pixels, int &units,
                     int &hot_units) const
    {
        static const int kMaxStep = 8;
        /* the fewest pixels that are a whole number of units */
        int g = std::gcd(fScale, kOne);
        int step_pixels = fScale / g;
        int step_units = kOne / g;

        hot = std::max(hot, 0);
        if (step_pixels > kMaxStep) {
            offset = 0;
            units = FitUnits(size);
            pixels = ToPixels(units);
            hot_units = hot * kOne / fScale;
            return;
        }
        int hot_steps = (hot + step_pixels - 1) / step_pixels;
        offset = hot_steps * step_pixels - hot;
        int steps = (offset + size + step_pixels - 1) / step_pixels;
        pixels = steps * step_pixels;
        units = steps * step_units;
        hot_units = hot_steps * step_units;
    }
};
