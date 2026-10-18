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
#include <algorithm>

#include "be_decorator.h"
#include "title_font.h"

/* Haiku's BeOS decorator's metrics. The tab is padded kTabPadding around
   the close button, kTabPaddingBottom under it, and kTitleSpacing on either
   side of the title; the button is as tall as the title's font. */
static const int kBorder = 5;
static const int kTabPadding = 5;
static const int kTabPaddingBottom = 4;
static const int kTitleSpacing = 18;
/* how far back from the content's corner the resize corner reaches */
static const int kResizeKnob = 18;
/* the height of DejaVu Sans Bold at 12 pixels, for when there is no font */
static const float kDefaultAscent = 11.1387f;
static const float kDefaultDescent = 2.8301f;

static_assert(BeDecorator::kLeft == kBorder &&
              BeDecorator::kRight == kBorder &&
              BeDecorator::kBottom == kBorder);

/* every colour is opaque */
struct Palette {
    uint32_t outer_tl, outer_br, light, fill, shadow;
};

static const Palette kActive = {
    0xffcb9833, 0xff606060, 0xffffff66, 0xffffcb00, 0xffff9800,
};
static const Palette kInactive = {
    0xff989898, 0xff303030, 0xffffffff, 0xffe8e8e8, 0xffd8d8d8,
};
static const uint32_t kContentLine = 0xff989898;
static const uint32_t kTitleColor = 0xff000000;


namespace {

/* Draws into the area being painted, never into the content. */
struct Painter {
    uint32_t *pixels;
    int pitch;
    ScreenRect area;
    ScreenRect content;

    void Fill(const ScreenRect &r, uint32_t color) const
    {
        ScreenRect c = r & area;
        if (c.Empty())
            return;
        for (int y = c.y0; y < c.y1; y++) {
            uint32_t *row = pixels + y * pitch;
            if (y >= content.y0 && y < content.y1) {
                for (int x = c.x0; x < std::min(c.x1, content.x0); x++)
                    row[x] = color;
                for (int x = std::max(c.x0, content.x1); x < c.x1; x++)
                    row[x] = color;
            } else {
                std::fill(row + c.x0, row + c.x1, color);
            }
        }
    }

    void HLine(int x0, int x1, int y, uint32_t color) const
    {
        Fill(ScreenRect(x0, y, x1 - x0 + 1, 1), color);
    }

    void VLine(int x, int y0, int y1, uint32_t color) const
    {
        Fill(ScreenRect(x, y0, 1, y1 - y0 + 1), color);
    }

    /* A one pixel bevel; the bottom and right edges get the corners they
       share with the others. */
    void Bevel(int x0, int y0, int x1, int y1, uint32_t tl, uint32_t br) const
    {
        HLine(x0, x1, y0, tl);
        VLine(x0, y0, y1, tl);
        HLine(x0, x1, y1, br);
        VLine(x1, y0, y1, br);
    }

    /* 'alpha' over an opaque pixel. */
    void Blend(int x, int y, uint32_t color, unsigned alpha) const
    {
        if (!area.Contains(x, y))
            return;
        uint32_t &p = pixels[y * pitch + x];
        uint32_t out = 0xff000000;
        for (int shift = 0; shift < 24; shift += 8) {
            unsigned s = (color >> shift) & 0xff;
            unsigned d = (p >> shift) & 0xff;
            out |= ((s * alpha + d * (255 - alpha) + 127) / 255) << shift;
        }
        p = out;
    }
};

}


BeDecorator::BeDecorator()
{
    SetFont(nullptr);
}


void BeDecorator::SetFont(TitleFont *font)
{
    float ascent = font != nullptr ? font->Ascent() : kDefaultAscent;
    float descent = font != nullptr ? font->Descent() : kDefaultDescent;

    fFont = font;
    fButtonSize = (int)(ascent + descent + 0.5f) - 1;
    /* the tab's last row is the frame's top edge */
    fTabHeight = kTabPadding + fButtonSize + kTabPaddingBottom;
    fTitleX = kTabPadding + fButtonSize + kTitleSpacing;
    fTitleBaseline = kTabPadding + (int)ascent;
    LayoutTitle();
}


void BeDecorator::SetTitle(const char *title)
{
    fTitle = utf8_decode(title);
    LayoutTitle();
}


void BeDecorator::SetSize(int width, int height)
{
    fWidth = width;
    fHeight = height;
    LayoutTitle();
}


/* The title as it fits the tab: whole, or as much of its start and end as
   fits around an ellipsis, taken from either end in turn. */
void BeDecorator::LayoutTitle()
{
    fShownTitle.clear();
    fTitleWidth = 0;
    if (fFont == nullptr)
        return;

    int total = 0;
    std::vector<int> advances;
    for (uint32_t c : fTitle) {
        advances.push_back(fFont->GetGlyph(c).advance);
        total += advances.back();
    }
    fTitleWidth = (total + 63) / 64;

    int room = (TabRect().Width() - fTitleX - kTitleSpacing) * 64;
    if (total <= room) {
        int x = 0;
        for (size_t i = 0; i < fTitle.size(); i++) {
            fShownTitle.push_back({fTitle[i], x});
            x += advances[i];
        }
        return;
    }

    std::vector<uint32_t> ellipsis;
    if (fFont->HasChar(0x2026))
        ellipsis = {0x2026};
    else
        ellipsis = {'.', '.', '.'};
    int ellipsis_width = 0;
    for (uint32_t c : ellipsis)
        ellipsis_width += fFont->GetGlyph(c).advance;

    size_t count = fTitle.size();
    size_t head = 0, tail = 0;
    int used = ellipsis_width;
    if (used > room)
        return;
    for (bool grew = true; grew && head + tail < count;) {
        grew = false;
        if (used + advances[head] <= room) {
            used += advances[head++];
            grew = true;
        }
        if (head + tail < count &&
            used + advances[count - 1 - tail] <= room) {
            used += advances[count - 1 - tail++];
            grew = true;
        }
    }

    int x = 0;
    for (size_t i = 0; i < head; i++) {
        fShownTitle.push_back({fTitle[i], x});
        x += advances[i];
    }
    for (uint32_t c : ellipsis) {
        fShownTitle.push_back({c, x});
        x += fFont->GetGlyph(c).advance;
    }
    for (size_t i = count - tail; i < count; i++) {
        fShownTitle.push_back({fTitle[i], x});
        x += advances[i];
    }
}


int BeDecorator::Top() const
{
    return fTabHeight - 1 + kBorder;
}


ScreenRect BeDecorator::ContentRect() const
{
    return ScreenRect(kLeft, Top(), fWidth - kLeft - kRight,
                      fHeight - Top() - kBottom);
}


ScreenRect BeDecorator::TabRect() const
{
    int width = std::min(fTitleX + fTitleWidth + kTitleSpacing, fWidth);
    return ScreenRect(0, 0, width, fTabHeight);
}


int BeDecorator::SolidRects(ScreenRect rects[2]) const
{
    rects[0] = TabRect();
    rects[1] = ScreenRect(0, FrameTop(), fWidth, fHeight - FrameTop());
    return 2;
}


BeDecorator::Part BeDecorator::HitTest(int x, int y) const
{
    ScreenRect content = ContentRect();

    if (ScreenRect(kTabPadding, kTabPadding, fButtonSize,
                   fButtonSize).Contains(x, y))
        return kClose;
    if (content.Contains(x, y))
        return kContent;
    if (fResizable && x >= content.x1 - kResizeKnob &&
        y >= content.y1 - kResizeKnob && x < fWidth && y < fHeight)
        return kResize;
    if (TabRect().Contains(x, y) ||
        ScreenRect(0, FrameTop(), fWidth, fHeight - FrameTop()).Contains(x, y))
        return kMove;
    return kNowhere;
}


void BeDecorator::Paint(uint32_t *pixels, int pitch,
                        const ScreenRect &area) const
{
    const Palette &c = fActive ? kActive : kInactive;
    ScreenRect content = ContentRect();
    Painter p = {pixels, pitch, area & ScreenRect(0, 0, fWidth, fHeight),
                 content};
    int right = fWidth - 1;
    int bottom = fHeight - 1;
    ScreenRect tab = TabRect();
    int tab_right = tab.x1 - 1;

    if (p.area.Empty())
        return;

    /* see-through beside the tab */
    p.Fill(ScreenRect(tab.x1, 0, fWidth - tab.x1, FrameTop()), 0);

    /* the frame: raised outside, sunken inside, a line around the content */
    p.Fill(ScreenRect(0, FrameTop(), fWidth, fHeight - FrameTop()), c.fill);
    p.Bevel(0, FrameTop(), right, bottom, c.outer_tl, c.outer_br);
    p.Bevel(1, FrameTop() + 1, right - 1, bottom - 1, c.light, c.shadow);
    p.Bevel(content.x0 - 2, content.y0 - 2, content.x1 + 1, content.y1 + 1,
            c.shadow, c.light);
    p.Bevel(content.x0 - 1, content.y0 - 1, content.x1, content.y1,
            kContentLine, kContentLine);

    /* the tab, open at the bottom where it joins the frame */
    p.Fill(ScreenRect(0, 0, tab.x1, fTabHeight), c.fill);
    p.HLine(0, tab_right, 0, c.outer_tl);
    p.VLine(0, 0, FrameTop(), c.outer_tl);
    p.VLine(tab_right, 0, FrameTop() - 1, c.outer_br);
    p.HLine(1, tab_right - 1, 1, c.light);
    p.VLine(1, 1, FrameTop(), c.light);
    p.VLine(tab_right - 1, 1, FrameTop(), c.shadow);
    p.Fill(ScreenRect(2, FrameTop(), tab_right - 3, 2), c.fill);

    /* the close button, flat: a raised square in a groove, sunken two
       pixels deep while pressed */
    int x0 = kTabPadding, y0 = kTabPadding;
    int x1 = x0 + fButtonSize - 1, y1 = y0 + fButtonSize - 1;
    p.Bevel(x0, y0, x1, y1, c.shadow, c.light);
    if (fClosePressed)
        p.Bevel(x0 + 1, y0 + 1, x1 - 1, y1 - 1, c.shadow, c.light);
    else
        p.Bevel(x0 + 1, y0 + 1, x1 - 1, y1 - 1, c.light, c.shadow);
    p.Fill(ScreenRect(x0 + 2, y0 + 2, fButtonSize - 4, fButtonSize - 4),
           c.fill);

    /* the title, kept inside the tab */
    Painter text = p;
    text.area = p.area & ScreenRect(2, 2, tab_right - 3, fTabHeight - 2);
    for (const PlacedChar &placed : fShownTitle) {
        const TitleFont::Glyph &g = fFont->GetGlyph(placed.c);
        int pen = fTitleX + (placed.x + 32) / 64;
        for (int y = 0; y < g.height; y++) {
            for (int x = 0; x < g.width; x++) {
                unsigned a = g.alpha[y * g.width + x];
                if (a != 0) {
                    text.Blend(pen + g.left + x, fTitleBaseline - g.top + y,
                               kTitleColor, a);
                }
            }
        }
    }

    /* a line across the right and the bottom edge marks off the resize
       corner */
    if (fResizable) {
        int knob = kResizeKnob - 1;
        p.HLine(content.x1, right - 1, content.y1 - 1 - knob, c.outer_tl);
        p.VLine(content.x1 - 1 - knob, content.y1, bottom - 1, c.outer_tl);
    }
}


//#pragma mark - cursor

FrameCursor frame_cursor()
{
    /* straight alpha ARGB */
    static const uint32_t kHand[] = {
        0x00000000, 0xa5000000, 0xfd000000, 0xb4000000, 0x1a000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x06000000,
        0xff000000, 0xfff7f7f7, 0xffb6b6b6, 0xd6000000, 0x0f000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0xcb000000,
        0xffd5d5d5, 0xffffffff, 0xffa2a2a2, 0xa2000000, 0x00000000, 0x02000000,
        0x02000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x56000000, 0xff7a7a7a,
        0xfffefefe, 0xfffafafa, 0xff696969, 0x63000000, 0xdb000000, 0xeb000000,
        0x79000000, 0xad000000, 0xfa000000, 0x9a000000, 0x07000000, 0x02000000,
        0x00000000, 0x00000000, 0x00000000, 0x02000000, 0xc3000000, 0xffcacaca,
        0xffffffff, 0xffe1e1e1, 0xff272727, 0xff767676, 0xffececec, 0xff878787,
        0xff000000, 0xfff3f3f3, 0xff909090, 0xff000000, 0xe2000000, 0x3f000000,
        0x00000000, 0x00000000, 0x00000000, 0x2f000000, 0xff4a4a4a, 0xfff3f3f3,
        0xfffefefe, 0xffd5d5d5, 0xff404040, 0xffe2e2e2, 0xfffbfbfb, 0xffa1a1a1,
        0xffd5d5d5, 0xfff9f9f9, 0xff8c8c8c, 0xffd9d9d9, 0xff353535, 0x2e000000,
        0x00000000, 0x00000000, 0x00000000, 0x88000000, 0xff9d9d9d, 0xfffefefe,
        0xfffefefe, 0xfff1f1f1, 0xffdfdfdf, 0xfffefefe, 0xfffefefe, 0xfff7f7f7,
        0xfffefefe, 0xffeaeaea, 0xffe5e5e5, 0xffc8c8c8, 0xbe000000, 0x00000000,
        0x00000000, 0x00000000, 0x0b000000, 0xde000000, 0xffdbdbdb, 0xfffefefe,
        0xfffefefe, 0xfffefefe, 0xfffefefe, 0xfffefefe, 0xfffefefe, 0xfffefefe,
        0xfffefefe, 0xfffefefe, 0xfffbfbfb, 0xfe000000, 0x52000000, 0xd9000000,
        0xfd000000, 0xd2000000, 0xc1000000, 0xff636363, 0xfff8f8f8, 0xfffdfdfd,
        0xfffdfdfd, 0xfffdfdfd, 0xfffdfdfd, 0xfffdfdfd, 0xfffdfdfd, 0xfffdfdfd,
        0xfffdfdfd, 0xfffdfdfd, 0xff000000, 0xf2000000, 0xffdbdbdb, 0xfff9f9f9,
        0xffdfdfdf, 0xffa5a5a5, 0xff383838, 0xffb0b0b0, 0xfffcfcfc, 0xfffcfcfc,
        0xfffcfcfc, 0xfffcfcfc, 0xfffcfcfc, 0xfffcfcfc, 0xfffcfcfc, 0xfffcfcfc,
        0xfffcfcfc, 0xff000000, 0xce000000, 0xff9b9b9b, 0xffd9d9d9, 0xfffbfbfb,
        0xfffcfcfc, 0xfff2f2f2, 0xffb8b8b8, 0xffececec, 0xfffcfcfc, 0xfffcfcfc,
        0xfffcfcfc, 0xfffcfcfc, 0xfffcfcfc, 0xfffcfcfc, 0xfffcfcfc, 0xfffcfcfc,
        0xff000000, 0x12000000, 0x73000000, 0xce000000, 0xff666666, 0xffcacaca,
        0xfff9f9f9, 0xfffcfcfc, 0xfffcfcfc, 0xfffcfcfc, 0xfffcfcfc, 0xfffcfcfc,
        0xfffcfcfc, 0xfffcfcfc, 0xfffcfcfc, 0xfffcfcfc, 0xfffcfcfc, 0xff000000,
        0x00000000, 0x00000000, 0x02000000, 0x3a000000, 0xb0000000, 0xff666666,
        0xffe1e1e1, 0xfffbfbfb, 0xfffbfbfb, 0xfffbfbfb, 0xfffbfbfb, 0xfffbfbfb,
        0xfffbfbfb, 0xfffbfbfb, 0xfffbfbfb, 0xfffbfbfb, 0xff000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x4c000000, 0xfb3b3b3b,
        0xffd9d9d9, 0xfffbfbfb, 0xfffbfbfb, 0xfffbfbfb, 0xfffbfbfb, 0xfffbfbfb,
        0xfdf9f9f9, 0xb6d7d7d7, 0x488c8c8c, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x40000000, 0xe0525252,
        0xc2c1c1c1, 0xa6cecece, 0xb5d6d6d6, 0xacd1d1d1, 0x7cb4b4b4, 0x1e5d5d5d,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x02000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000, 0x00000000,
        0x00000000, 0x00000000,
    };
    FrameCursor cursor;
    cursor.width = 17;
    cursor.height = 16;
    cursor.hot_x = 1;
    cursor.hot_y = 0;
    for (uint32_t p : kHand) {
        uint32_t a = p >> 24;
        uint32_t out = a << 24;
        for (int shift = 0; shift < 24; shift += 8)
            out |= ((((p >> shift) & 0xff) * a + 127) / 255) << shift;
        cursor.pixels.push_back(out);
    }
    return cursor;
}
