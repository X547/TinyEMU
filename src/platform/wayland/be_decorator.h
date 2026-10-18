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

    /* the frame's width around the content; the top, Top(), takes in the
       tab and follows the font */
    static const int kLeft = 5;
    static const int kRight = 5;
    static const int kBottom = 5;

private:
    struct PlacedChar {
        uint32_t c;
        /* the pen from the title's start, in 1/64 pixels */
        int x;
    };

    TitleFont *fFont = nullptr;
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

    void LayoutTitle();
    /* the tab's last row */
    int FrameTop() const {return fTabHeight - 1;}

public:
    BeDecorator();

    /* nullptr leaves the title out; the tab keeps its size */
    void SetFont(TitleFont *font);
    /* UTF-8 */
    void SetTitle(const char *title);
    /* the whole window, frame included */
    void SetSize(int width, int height);
    void SetActive(bool active) {fActive = active;}
    void SetClosePressed(bool pressed) {fClosePressed = pressed;}
    /* whether the resize corner is shown */
    void SetResizable(bool resizable) {fResizable = resizable;}

    bool ClosePressed() const {return fClosePressed;}
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
