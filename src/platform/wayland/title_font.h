/*
 * Window title font
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

#include <unordered_map>
#include <vector>

struct _FcConfig;
struct FT_LibraryRec_;
struct FT_FaceRec_;


/* A system font, found through fontconfig and rendered by FreeType into
   anti-aliased glyphs, which are kept once rendered. */
class TitleFont {
public:
    struct Glyph {
        /* where the alpha starts relative to the pen on the baseline, y
           growing upwards */
        int left = 0;
        int top = 0;
        int width = 0;
        int height = 0;
        /* in 1/64 pixels */
        int advance = 0;
        std::vector<uint8_t> alpha;
    };

private:
    _FcConfig *fConfig = nullptr;
    FT_LibraryRec_ *fLibrary = nullptr;
    FT_FaceRec_ *fFace = nullptr;
    int fSize = 0;
    std::unordered_map<uint32_t, Glyph> fGlyphs;

public:
    TitleFont() = default;
    TitleFont(const TitleFont &) = delete;
    TitleFont &operator=(const TitleFont &) = delete;
    ~TitleFont();

    /* The best match for a fontconfig 'pattern' such as "sans-serif:bold",
       'size' pixels high; false when there is none. */
    bool Open(const char *pattern, int size);

    /* as a font reports its height: the design's, scaled, not rounded */
    float Ascent() const;
    float Descent() const;
    bool HasChar(uint32_t c) const;
    /* the font's missing glyph for a character it lacks */
    const Glyph &GetGlyph(uint32_t c);
};


/* The Unicode characters of a UTF-8 string; a malformed byte is taken as
   U+FFFD. */
std::vector<uint32_t> utf8_decode(const char *s);
