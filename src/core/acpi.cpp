/*
 * ACPI Machine Language encoding
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
#include "acpi.h"

#include <assert.h>
#include <string.h>


//#pragma mark - AcpiContext

AcpiContext::AcpiContext(AmlBytes *aml, const char *scope):
    fNames(std::make_shared<std::set<std::string>>()),
    aml(aml),
    scope(scope)
{
}


std::string AcpiContext::NewName(const char *prefix)
{
    static const char kDigits[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ";
    std::string name(prefix);

    if (name.size() == 4) {
        bool added = fNames->insert(scope + "." + name).second;
        assert(added);
        (void)added;
        return name;
    }
    assert(name.size() == 3);
    for (const char *d = kDigits; *d != '\0'; d++) {
        std::string candidate = name + *d;
        if (fNames->insert(scope + "." + candidate).second) {
            return candidate;
        }
    }
    assert(0);
    return name + "_";
}


AcpiContext AcpiContext::Nested(const std::string &name, AmlBytes *aml) const
{
    AcpiContext nested = *this;

    nested.aml = aml;
    nested.scope = scope + "." + name;
    return nested;
}


//#pragma mark - values

void aml_put8(AmlBytes &b, uint8_t v)
{
    b.push_back(v);
}


void aml_put16(AmlBytes &b, uint16_t v)
{
    aml_put8(b, v);
    aml_put8(b, v >> 8);
}


void aml_put32(AmlBytes &b, uint32_t v)
{
    aml_put16(b, v);
    aml_put16(b, v >> 16);
}


void aml_put64(AmlBytes &b, uint64_t v)
{
    aml_put32(b, v);
    aml_put32(b, v >> 32);
}


void aml_put_str(AmlBytes &b, const char *s, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        aml_put8(b, s[i]);
    }
}


void aml_append(AmlBytes &b, const AmlBytes &more)
{
    b.insert(b.end(), more.begin(), more.end());
}


//#pragma mark - terms

/* A PkgLength counts itself, so its size decides its value. */
static void aml_pkglength(AmlBytes &b, size_t body_len)
{
    if (body_len + 1 < 0x40) {
        aml_put8(b, body_len + 1);
        return;
    }
    for (int n = 2; n <= 4; n++) {
        size_t len = body_len + n;
        if (len < ((size_t)1 << (4 + 8 * (n - 1)))) {
            aml_put8(b, ((n - 1) << 6) | (len & 0xf));
            for (int i = 1; i < n; i++) {
                aml_put8(b, len >> (4 + 8 * (i - 1)));
            }
            return;
        }
    }
    assert(0);
}


AmlBytes aml_with_length(std::initializer_list<uint8_t> op,
                         const AmlBytes &body)
{
    AmlBytes b(op);

    aml_pkglength(b, body.size());
    aml_append(b, body);
    return b;
}


AmlBytes aml_int(uint32_t v)
{
    AmlBytes b;

    if (v <= 1) {
        aml_put8(b, v); /* ZeroOp, OneOp */
    } else if (v <= 0xff) {
        aml_put8(b, 0x0a);
        aml_put8(b, v);
    } else if (v <= 0xffff) {
        aml_put8(b, 0x0b);
        aml_put16(b, v);
    } else {
        aml_put8(b, 0x0c);
        aml_put32(b, v);
    }
    return b;
}


AmlBytes aml_name(const char *seg, const AmlBytes &value)
{
    AmlBytes b;

    aml_put8(b, 0x08); /* NameOp */
    aml_put_str(b, seg, 4);
    aml_append(b, value);
    return b;
}


AmlBytes aml_package(const std::vector<AmlBytes> &elements)
{
    AmlBytes body;

    aml_put8(body, elements.size());
    for (const AmlBytes &e : elements) {
        aml_append(body, e);
    }
    return aml_with_length({0x12}, body); /* PackageOp */
}


AmlBytes aml_buffer(const AmlBytes &data)
{
    AmlBytes body = aml_int(data.size());

    aml_append(body, data);
    return aml_with_length({0x11}, body); /* BufferOp */
}


AmlBytes aml_device(const char *seg, const AmlBytes &contents)
{
    AmlBytes body;

    aml_put_str(body, seg, 4);
    aml_append(body, contents);
    return aml_with_length({0x5b, 0x82}, body); /* DeviceOp */
}


AmlBytes aml_scope(const char *path, const AmlBytes &contents)
{
    AmlBytes body;

    aml_put_str(body, path, strlen(path));
    aml_append(body, contents);
    return aml_with_length({0x10}, body); /* ScopeOp */
}


/* Three letters in five bits each and four hex digits, stored big
   endian. */
AmlBytes aml_eisaid(const char *id)
{
    uint32_t v = ((id[0] - 0x40) << 26) | ((id[1] - 0x40) << 21) |
        ((id[2] - 0x40) << 16);

    for (int i = 3; i < 7; i++) {
        int c = id[i];
        v |= (c <= '9' ? c - '0' : c - 'A' + 10) << ((6 - i) * 4);
    }
    return aml_int(__builtin_bswap32(v));
}


//#pragma mark - resource templates

void aml_res_io(AmlBytes &r, uint16_t port, uint8_t len)
{
    aml_put8(r, 0x47); /* I/O port descriptor */
    aml_put8(r, 1); /* 16 bit decode */
    aml_put16(r, port);
    aml_put16(r, port);
    aml_put8(r, 1); /* alignment */
    aml_put8(r, len);
}


void aml_res_irq(AmlBytes &r, int irq)
{
    aml_put8(r, 0x22); /* IRQ descriptor */
    aml_put16(r, 1 << irq);
}


void aml_res_memory32_fixed(AmlBytes &r, uint32_t base, uint32_t size)
{
    aml_put8(r, 0x86); /* 32 bit fixed memory range descriptor */
    aml_put16(r, 9);
    aml_put8(r, 1); /* read/write */
    aml_put32(r, base);
    aml_put32(r, size);
}


void aml_res_word_space(AmlBytes &r, int type, int type_flags, uint16_t min,
                        uint16_t max)
{
    aml_put8(r, 0x88); /* word address space descriptor */
    aml_put16(r, 13);
    aml_put8(r, type);
    aml_put8(r, 0x0c);
    aml_put8(r, type_flags);
    aml_put16(r, 0); /* granularity */
    aml_put16(r, min);
    aml_put16(r, max);
    aml_put16(r, 0); /* translation */
    aml_put16(r, max - min + 1);
}


void aml_res_dword_memory(AmlBytes &r, uint32_t min, uint32_t max)
{
    aml_put8(r, 0x87); /* dword address space descriptor */
    aml_put16(r, 23);
    aml_put8(r, 0); /* memory */
    aml_put8(r, 0x0c);
    aml_put8(r, 1); /* read/write, not cacheable */
    aml_put32(r, 0); /* granularity */
    aml_put32(r, min);
    aml_put32(r, max);
    aml_put32(r, 0); /* translation */
    aml_put32(r, max - min + 1);
}


void aml_res_qword_memory(AmlBytes &r, uint64_t min, uint64_t max)
{
    aml_put8(r, 0x8a); /* qword address space descriptor */
    aml_put16(r, 43);
    aml_put8(r, 0); /* memory */
    aml_put8(r, 0x0c);
    aml_put8(r, 1); /* read/write, not cacheable */
    aml_put64(r, 0); /* granularity */
    aml_put64(r, min);
    aml_put64(r, max);
    aml_put64(r, 0); /* translation */
    aml_put64(r, max - min + 1);
}


AmlBytes aml_resource_template(AmlBytes r)
{
    aml_put8(r, 0x79); /* end tag */
    aml_put8(r, 0);
    return aml_buffer(r);
}
