/*
 * BeOS-style window frame
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

#include <vector>

#include "screen_rect.h"
#include "window_scale.h"

class TitleFont;


/* A frame around the content with a title tab on its top left edge, as
   BeOS draws one: a close button in the tab, and a resize corner on the
   bottom right. It knows nothing of the window system; it paints premultiplied
   ARGB pixels and says which part a point is on. */
class BeDecorator {
public:
    enum Part {
        kNowhere,   /* see-through, beside the tab */
        kContent,
        kClose,
        kMove,      /* the tab and the frame */
        kResize,
    };

private:
    struct PlacedChar {
        uint32_t c;
        /* the pen from the title's start, in 1/64 pixels */
        int x;
    };

    TitleFont *fFont = nullptr;
    WindowScale fScale;
    /* from the scale; the bevels stay one pixel wide */
    int fBorder = 0;
    int fTabPadding = 0;
    int fTabPaddingBottom = 0;
    int fTitleSpacing = 0;
    int fResizeKnob = 0;
    /* added to the right and the bottom border */
    int fPadRight = 0;
    int fPadBottom = 0;
    /* from the font's height */
    int fButtonSize = 0;
    int fTabHeight = 0;
    int fTitleX = 0;
    int fTitleBaseline = 0;
    std::vector<uint32_t> fTitle;
    /* the title's whole width, in pixels */
    int fTitleWidth = 0;
    /* what is drawn of it: all, or its start and end around an ellipsis */
    std::vector<PlacedChar> fShownTitle;
    int fWidth = 0;
    int fHeight = 0;
    bool fActive = false;
    bool fClosePressed = false;
    bool fResizable = false;

    void UpdateMetrics();
    void LayoutTitle();
    /* the tab's last row */
    int FrameTop() const {return fTabHeight - 1;}

public:
    BeDecorator();

    /* nullptr leaves the title out; the tab keeps its size */
    void SetFont(TitleFont *font);
    /* the metrics are Haiku's times 'scale'; the font is sized by the
       caller, then set again */
    void SetScale(WindowScale scale);
    /* more border on the right and the bottom than the scale gives */
    void SetPadding(int right, int bottom);
    /* UTF-8 */
    void SetTitle(const char *title);
    /* the whole window, frame included */
    void SetSize(int width, int height);
    void SetActive(bool active) {fActive = active;}
    void SetClosePressed(bool pressed) {fClosePressed = pressed;}
    /* whether the resize corner is shown */
    void SetResizable(bool resizable) {fResizable = resizable;}

    bool ClosePressed() const {return fClosePressed;}
    /* the frame's width around the content, padding left out; the top
       takes in the tab and follows the font */
    int Left() const {return fBorder;}
    int Right() const {return fBorder;}
    int Bottom() const {return fBorder;}
    int Top() const;
    ScreenRect ContentRect() const;
    ScreenRect TabRect() const;
    /* everything but the see-through part, as at most two rectangles */
    int SolidRects(ScreenRect rects[2]) const;
    Part HitTest(int x, int y) const;

    /* Paints the frame within 'area', leaving the content alone. */
    void Paint(uint32_t *pixels, int pitch, const ScreenRect &area) const;
};


/* The host cursor shown over the frame, the BeOS hand: premultiplied
   ARGB. */
struct FrameCursor {
    std::vector<uint32_t> pixels;
    int width = 0;
    int height = 0;
    int hot_x = 0;
    int hot_y = 0;
};

FrameCursor frame_cursor();
