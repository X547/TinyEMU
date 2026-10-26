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
    fUids(std::make_shared<std::map<std::string, int>>()),
    fEcams(std::make_shared<std::vector<AcpiEcam>>()),
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


int AcpiContext::NewUid(const char *hid)
{
    return (*fUids)[hid]++;
}


void AcpiContext::AddInterrupt(AmlBytes &r, uint64_t line) const
{
    aml_res_interrupt(r, line, true, false);
}


int AcpiContext::AddEcam(uint64_t base, int bus_count)
{
    int segment = fEcams->size();

    assert(bus_count >= 1 && bus_count <= 256);
    fEcams->push_back({base, (uint16_t)segment, (uint8_t)(bus_count - 1)});
    return segment;
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


AmlBytes aml_string(const char *s)
{
    AmlBytes b;

    aml_put8(b, 0x0d); /* StringPrefix */
    aml_put_str(b, s, strlen(s) + 1);
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


/* The first three fields are stored little endian, the last two as they are
   written. */
AmlBytes aml_uuid(const char *uuid)
{
    static const int kOrder[16] = {
        3, 2, 1, 0, 5, 4, 7, 6, 8, 9, 10, 11, 12, 13, 14, 15,
    };
    uint8_t bytes[16];
    int n = 0;

    for (const char *p = uuid; *p != '\0' && n < 32; p++) {
        int c = *p;
        int digit;
        if (c >= '0' && c <= '9') {
            digit = c - '0';
        } else if (c >= 'a' && c <= 'f') {
            digit = c - 'a' + 10;
        } else if (c >= 'A' && c <= 'F') {
            digit = c - 'A' + 10;
        } else {
            continue;
        }
        if (n % 2 == 0) {
            bytes[n / 2] = digit << 4;
        } else {
            bytes[n / 2] |= digit;
        }
        n++;
    }
    assert(n == 32);

    AmlBytes data;
    for (int i = 0; i < 16; i++) {
        aml_put8(data, bytes[kOrder[i]]);
    }
    return aml_buffer(data);
}


AmlBytes aml_method(const char *seg, int arg_count, const AmlBytes &body)
{
    AmlBytes b;

    aml_put_str(b, seg, 4);
    aml_put8(b, arg_count); /* not serialized, sync level 0 */
    aml_append(b, body);
    return aml_with_length({0x14}, b); /* MethodOp */
}


AmlBytes aml_if(const AmlBytes &predicate, const AmlBytes &body)
{
    AmlBytes b = predicate;

    aml_append(b, body);
    return aml_with_length({0xa0}, b); /* IfOp */
}


AmlBytes aml_lequal(const AmlBytes &a, const AmlBytes &b)
{
    AmlBytes t {0x93}; /* LEqualOp */

    aml_append(t, a);
    aml_append(t, b);
    return t;
}


AmlBytes aml_return(const AmlBytes &value)
{
    AmlBytes b {0xa4}; /* ReturnOp */

    aml_append(b, value);
    return b;
}


AmlBytes aml_arg(int index)
{
    assert(index >= 0 && index < 7);
    return AmlBytes {(uint8_t)(0x68 + index)}; /* Arg0Op */
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


void aml_res_interrupt(AmlBytes &r, uint32_t gsi, bool level,
                       bool active_low)
{
    aml_put8(r, 0x89); /* extended interrupt descriptor */
    aml_put16(r, 6);
    /* consumer, exclusive */
    aml_put8(r, 1 | (level ? 0 : 2) | (active_low ? 4 : 0));
    aml_put8(r, 1);
    aml_put32(r, gsi);
}


void aml_res_memory32_fixed(AmlBytes &r, uint32_t base, uint32_t size)
{
    aml_put8(r, 0x86); /* 32 bit fixed memory range descriptor */
    aml_put16(r, 9);
    aml_put8(r, 1); /* read/write */
    aml_put32(r, base);
    aml_put32(r, size);
}


void aml_res_i2c_serial_bus(AmlBytes &r, uint16_t address, uint32_t speed,
                            const char *source)
{
    size_t source_len = strlen(source) + 1;

    aml_put8(r, 0x8e); /* generic serial bus connection descriptor */
    aml_put16(r, 15 + source_len);
    aml_put8(r, 2); /* revision: V2 */
    aml_put8(r, 0); /* resource source index */
    aml_put8(r, 1); /* I2C */
    aml_put8(r, 2); /* consumer, controller initiated, exclusive */
    aml_put16(r, 0); /* 7 bit addressing */
    aml_put8(r, 1); /* type specific revision */
    aml_put16(r, 6); /* type data length */
    aml_put32(r, speed);
    aml_put16(r, address);
    aml_put_str(r, source, source_len);
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
