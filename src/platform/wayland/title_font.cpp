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
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H

#include "title_font.h"


TitleFont::~TitleFont()
{
    if (fFace != nullptr)
        FT_Done_Face(fFace);
    if (fLibrary != nullptr)
        FT_Done_FreeType(fLibrary);
    if (fConfig != nullptr)
        FcConfigDestroy(fConfig);
}


bool TitleFont::Open(const char *pattern, int size)
{
    fConfig = FcInitLoadConfigAndFonts();
    if (fConfig == nullptr)
        return false;
    FcPattern *pat = FcNameParse(reinterpret_cast<const FcChar8 *>(pattern));
    if (pat == nullptr)
        return false;
    FcPatternAddDouble(pat, FC_PIXEL_SIZE, size);
    FcConfigSubstitute(fConfig, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);
    FcResult result;
    FcPattern *match = FcFontMatch(fConfig, pat, &result);
    FcPatternDestroy(pat);
    if (match == nullptr)
        return false;

    FcChar8 *file = nullptr;
    int index = 0;
    bool found = FcPatternGetString(match, FC_FILE, 0, &file) ==
                 FcResultMatch;
    FcPatternGetInteger(match, FC_INDEX, 0, &index);
    if (found) {
        found = FT_Init_FreeType(&fLibrary) == 0 &&
                FT_New_Face(fLibrary, reinterpret_cast<const char *>(file),
                            index, &fFace) == 0 &&
                FT_Set_Pixel_Sizes(fFace, 0, size) == 0;
    }
    FcPatternDestroy(match);
    if (!found)
        return false;
    fSize = size;
    return true;
}


float TitleFont::Ascent() const
{
    return (float)fFace->ascender * fSize / fFace->units_per_EM;
}


float TitleFont::Descent() const
{
    return -(float)fFace->descender * fSize / fFace->units_per_EM;
}


bool TitleFont::HasChar(uint32_t c) const
{
    return FT_Get_Char_Index(fFace, c) != 0;
}


const TitleFont::Glyph &TitleFont::GetGlyph(uint32_t c)
{
    auto it = fGlyphs.find(c);
    if (it != fGlyphs.end())
        return it->second;

    Glyph &glyph = fGlyphs[c];
    FT_UInt index = FT_Get_Char_Index(fFace, c);
    if (FT_Load_Glyph(fFace, index, FT_LOAD_RENDER | FT_LOAD_TARGET_LIGHT) != 0)
        return glyph;
    FT_GlyphSlot slot = fFace->glyph;
    const FT_Bitmap &bitmap = slot->bitmap;
    glyph.left = slot->bitmap_left;
    glyph.top = slot->bitmap_top;
    glyph.advance = slot->advance.x;
    if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY)
        return glyph;
    glyph.width = bitmap.width;
    glyph.height = bitmap.rows;
    glyph.alpha.resize((size_t)glyph.width * glyph.height);
    for (int y = 0; y < glyph.height; y++) {
        for (int x = 0; x < glyph.width; x++)
            glyph.alpha[y * glyph.width + x] =
                bitmap.buffer[y * bitmap.pitch + x];
    }
    return glyph;
}


std::vector<uint32_t> utf8_decode(const char *s)
{
    std::vector<uint32_t> chars;
    const auto *p = reinterpret_cast<const uint8_t *>(s);

    while (*p != 0) {
        uint32_t c = *p++;
        int more = c >= 0xf0 ? 3 : c >= 0xe0 ? 2 : c >= 0xc0 ? 1 : 0;
        if (c >= 0x80 && (more == 0 || c >= 0xf8)) {
            chars.push_back(0xfffd);
            continue;
        }
        static const uint8_t kLeadBits[] = {0x7f, 0x1f, 0x0f, 0x07};
        c &= kLeadBits[more];
        for (; more > 0; more--) {
            if ((*p & 0xc0) != 0x80) {
                c = 0xfffd;
                break;
            }
            c = (c << 6) | (*p++ & 0x3f);
        }
        chars.push_back(c);
    }
    return chars;
}
