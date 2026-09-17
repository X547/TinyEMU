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
#pragma once

#include <stdint.h>

#include <vector>

/* Usage pages, and the usages of them these devices name. */
#define HID_PAGE_GENERIC_DESKTOP 0x01
#define HID_PAGE_KEYBOARD        0x07
#define HID_PAGE_LED             0x08
#define HID_PAGE_BUTTON          0x09

#define HID_USAGE_POINTER        0x01
#define HID_USAGE_MOUSE          0x02
#define HID_USAGE_KEYBOARD       0x06
#define HID_USAGE_X              0x30
#define HID_USAGE_Y              0x31
#define HID_USAGE_WHEEL          0x38

/* What a collection collects. */
#define HID_COLLECTION_PHYSICAL    0x00
#define HID_COLLECTION_APPLICATION 0x01

/* The flags of an Input, Output or Feature item. The zero valued ones are
   named so that a field can say what it is rather than leaving it out. */
#define HID_DATA      0x00
#define HID_CONSTANT  0x01
#define HID_ARRAY     0x00
#define HID_VARIABLE  0x02
#define HID_ABSOLUTE  0x00
#define HID_RELATIVE  0x04


/* Assembles a report descriptor out of its items.
 *
 * A value is written in the fewest bytes that hold it: as it stands for a
 * value that is not negative, and in two's complement for one that is. That
 * is how these descriptors are written by hand, and a report descriptor is
 * read by a driver that takes a logical maximum of 0xff in one byte as 255
 * whatever the signedness rule says.
 */
class HIDReportBuilder {
private:
    /* The three kinds of item, as the type field of a prefix numbers them. */
    enum {
        MAIN,
        GLOBAL,
        LOCAL,
    };

    std::vector<uint8_t> fData;

    void Item(int type, int tag, int32_t value);
    void ItemEmpty(int type, int tag);

public:
    /* global items: they hold until changed */
    void UsagePage(uint32_t page) {Item(GLOBAL, 0x0, page);}
    void LogicalRange(int32_t min, int32_t max)
        {Item(GLOBAL, 0x1, min); Item(GLOBAL, 0x2, max);}
    void PhysicalRange(int32_t min, int32_t max)
        {Item(GLOBAL, 0x3, min); Item(GLOBAL, 0x4, max);}
    void ReportSize(uint32_t bits) {Item(GLOBAL, 0x7, bits);}
    void ReportId(uint32_t id) {Item(GLOBAL, 0x8, id);}
    void ReportCount(uint32_t count) {Item(GLOBAL, 0x9, count);}

    /* local items: they apply to the next main item only */
    void Usage(uint32_t usage) {Item(LOCAL, 0x0, usage);}
    void UsageRange(uint32_t min, uint32_t max)
        {Item(LOCAL, 0x1, min); Item(LOCAL, 0x2, max);}

    /* main items */
    void Input(uint32_t flags) {Item(MAIN, 0x8, flags);}
    void Output(uint32_t flags) {Item(MAIN, 0x9, flags);}
    void Feature(uint32_t flags) {Item(MAIN, 0xb, flags);}
    void BeginCollection(uint32_t kind) {Item(MAIN, 0xa, kind);}
    void EndCollection() {ItemEmpty(MAIN, 0xc);}

    /* Bits of padding, which is an input field with no usage. */
    void InputPadding(uint32_t bits)
        {ReportCount(1); ReportSize(bits); Input(HID_CONSTANT);}

    int Length() const {return (int)fData.size();}
    const uint8_t *Data() const {return fData.data();}

    /* Hands the bytes to whoever keeps them; the builder is empty after. */
    std::vector<uint8_t> Take() {return std::move(fData);}
};
