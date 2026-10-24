/*
 * printf style formatting
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
#include "format.h"

namespace {

struct Spec {
    bool left = false;
    bool zero = false;
    bool alt = false;
    int width = 0;
    int precision = -1;
};

void pad(FormatSink sink, void *ctx, int n, char c)
{
    while (n-- > 0)
        sink(ctx, c);
}

void put_str(FormatSink sink, void *ctx, const char *s, const Spec &spec)
{
    if (s == nullptr)
        s = "(null)";
    int len = spec.precision >= 0 ? (int)strnlen(s, spec.precision)
                                  : (int)strlen(s);
    if (!spec.left)
        pad(sink, ctx, spec.width - len, ' ');
    for (int i = 0; i < len; i++)
        sink(ctx, s[i]);
    if (spec.left)
        pad(sink, ctx, spec.width - len, ' ');
}

void put_num(FormatSink sink, void *ctx, uint64_t v, bool negative,
             unsigned base, bool upper, const char *prefix, const Spec &spec)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    char buf[24];
    int n = 0;

    do {
        buf[n++] = digits[v % base];
        v /= base;
    } while (v);

    int prefix_len = (negative ? 1 : 0) + (int)strlen(prefix);
    int fill = spec.width - n - prefix_len;
    if (!spec.left && !spec.zero)
        pad(sink, ctx, fill, ' ');
    if (negative)
        sink(ctx, '-');
    for (; *prefix; prefix++)
        sink(ctx, *prefix);
    if (!spec.left && spec.zero)
        pad(sink, ctx, fill, '0');
    while (n)
        sink(ctx, buf[--n]);
    if (spec.left)
        pad(sink, ctx, fill, ' ');
}

}


void vformat(FormatSink sink, void *ctx, const char *fmt, va_list ap)
{
    for (; *fmt; fmt++) {
        if (*fmt != '%') {
            sink(ctx, *fmt);
            continue;
        }
        fmt++;

        Spec spec;
        for (;; fmt++) {
            if (*fmt == '-')
                spec.left = true;
            else if (*fmt == '0')
                spec.zero = true;
            else if (*fmt == '#')
                spec.alt = true;
            else
                break;
        }
        if (*fmt == '*') {
            spec.width = va_arg(ap, int);
            if (spec.width < 0) {
                spec.left = true;
                spec.width = -spec.width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9')
                spec.width = spec.width * 10 + (*fmt++ - '0');
        }
        if (*fmt == '.') {
            fmt++;
            spec.precision = 0;
            if (*fmt == '*') {
                spec.precision = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9')
                    spec.precision = spec.precision * 10 + (*fmt++ - '0');
            }
        }

        /* 0 int, 1 long, 2 long long, 3 size_t/ptrdiff_t/intmax_t */
        int length = 0;
        if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h')
                fmt++;
        } else if (*fmt == 'l') {
            fmt++;
            length = 1;
            if (*fmt == 'l') {
                fmt++;
                length = 2;
            }
        } else if (*fmt == 'z' || *fmt == 't') {
            fmt++;
            length = 3;
        } else if (*fmt == 'j') {
            fmt++;
            length = 2;
        }

        char conv = *fmt;
        if (conv == '\0')
            break;
        switch (conv) {
        case 'd':
        case 'i': {
            int64_t v;
            if (length == 0)
                v = va_arg(ap, int);
            else if (length == 1)
                v = va_arg(ap, long);
            else if (length == 2)
                v = va_arg(ap, long long);
            else
                v = va_arg(ap, ptrdiff_t);
            uint64_t mag = v < 0 ? -(uint64_t)v : (uint64_t)v;
            put_num(sink, ctx, mag, v < 0, 10, false, "", spec);
            break;
        }
        case 'u':
        case 'x':
        case 'X':
        case 'o': {
            uint64_t v;
            if (length == 0)
                v = va_arg(ap, unsigned int);
            else if (length == 1)
                v = va_arg(ap, unsigned long);
            else if (length == 2)
                v = va_arg(ap, unsigned long long);
            else
                v = va_arg(ap, size_t);
            unsigned base = conv == 'u' ? 10 : conv == 'o' ? 8 : 16;
            /* as in C: no prefix for zero */
            const char *prefix = "";
            if (spec.alt && v != 0)
                prefix = conv == 'x' ? "0x" : conv == 'X' ? "0X"
                       : conv == 'o' ? "0" : "";
            put_num(sink, ctx, v, false, base, conv == 'X', prefix, spec);
            break;
        }
        case 'p':
            put_num(sink, ctx, (uintptr_t)va_arg(ap, void *), false, 16,
                    false, "0x", spec);
            break;
        case 'c': {
            char c = (char)va_arg(ap, int);
            if (!spec.left)
                pad(sink, ctx, spec.width - 1, ' ');
            sink(ctx, c);
            if (spec.left)
                pad(sink, ctx, spec.width - 1, ' ');
            break;
        }
        case 's':
            put_str(sink, ctx, va_arg(ap, const char *), spec);
            break;
        default:
            sink(ctx, '%');
            sink(ctx, conv);
            break;
        }
    }
}


void format(FormatSink sink, void *ctx, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vformat(sink, ctx, fmt, ap);
    va_end(ap);
}


namespace {

struct BufCtx {
    char *buf;
    size_t size;
    size_t len;
};

void buf_sink(void *p, char c)
{
    BufCtx *b = (BufCtx *)p;
    if (b->len + 1 < b->size)
        b->buf[b->len] = c;
    b->len++;
}

}


int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    BufCtx b {buf, size, 0};
    vformat(buf_sink, &b, fmt, ap);
    if (size > 0)
        buf[min(b.len, size - 1)] = '\0';
    return (int)b.len;
}


int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
