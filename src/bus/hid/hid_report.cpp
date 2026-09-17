/*
 * HID report descriptor builder
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
#include "hid_report.h"

#include "bits.h"

/* How many data bytes an item needs. Only 0, 1, 2 and 4 can be encoded, and a
   value of zero still takes one: an item with no data at all means something
   else for some tags, and every descriptor here spells such a value out. */
static int item_bytes(int32_t value)
{
    if (value >= 0) {
        if (value <= 0xff) {
            return 1;
        }
        return value <= 0xffff ? 2 : 4;
    }
    if (value >= -0x80) {
        return 1;
    }
    return value >= -0x8000 ? 2 : 4;
}


/* One short item: the tag, the type and the size of the data in a prefix
   byte, then that many bytes of the value, least significant first. */
void HIDReportBuilder::Item(int type, int tag, int32_t value)
{
    int len = item_bytes(value);

    fData.push_back(set_bits(set_bits((uint32_t)0, 4, 4, tag), 2, 2, type) |
                    (len == 4 ? 3 : len));
    for (int i = 0; i < len; i++) {
        fData.push_back(get_bits(value, i * 8, 8));
    }
}


void HIDReportBuilder::ItemEmpty(int type, int tag)
{
    fData.push_back(set_bits(set_bits((uint32_t)0, 4, 4, tag), 2, 2, type));
}
