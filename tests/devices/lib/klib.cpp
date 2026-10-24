/*
 * Freestanding basics
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
#include "klib.h"

/* Byte loops: the compiler must not turn them back into calls to
   themselves. */
#define NO_BUILTIN __attribute__((no_builtin))

extern "C" {

NO_BUILTIN void *memcpy(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--)
        *d++ = *s++;
    return dst;
}

NO_BUILTIN void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d < s) {
        while (n--)
            *d++ = *s++;
    } else {
        d += n;
        s += n;
        while (n--)
            *--d = *--s;
    }
    return dst;
}

NO_BUILTIN void *memset(void *dst, int c, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    while (n--)
        *d++ = (uint8_t)c;
    return dst;
}

NO_BUILTIN int memcmp(const void *a, const void *b, size_t n)
{
    const uint8_t *x = (const uint8_t *)a, *y = (const uint8_t *)b;
    for (; n; n--, x++, y++) {
        if (*x != *y)
            return *x - *y;
    }
    return 0;
}

NO_BUILTIN size_t strlen(const char *s)
{
    size_t n = 0;
    while (s[n])
        n++;
    return n;
}

NO_BUILTIN size_t strnlen(const char *s, size_t max)
{
    size_t n = 0;
    while (n < max && s[n])
        n++;
    return n;
}

NO_BUILTIN int strcmp(const char *a, const char *b)
{
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (uint8_t)*a - (uint8_t)*b;
}

NO_BUILTIN int strncmp(const char *a, const char *b, size_t n)
{
    for (; n; n--, a++, b++) {
        if (*a != *b)
            return (uint8_t)*a - (uint8_t)*b;
        if (!*a)
            break;
    }
    return 0;
}

NO_BUILTIN char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c)
            return (char *)s;
        if (!*s)
            return nullptr;
    }
}

}


bool glob_match(const char *pattern, const char *name)
{
    /* the classic backtracking matcher: only the last '*' needs to be
       remembered */
    const char *star = nullptr, *resume = nullptr;
    while (*name) {
        if (*pattern == '*') {
            star = pattern++;
            resume = name;
        } else if (*pattern == '?' || *pattern == *name) {
            pattern++;
            name++;
        } else if (star) {
            pattern = star + 1;
            name = ++resume;
        } else {
            return false;
        }
    }
    while (*pattern == '*')
        pattern++;
    return !*pattern;
}


bool parse_u64(const char *s, int len, uint64_t *out)
{
    size_t n = len < 0 ? strlen(s) : (size_t)len;
    uint64_t v = 0;
    unsigned base = 10;
    size_t i = 0;

    if (n > 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        i = 2;
    }
    if (i == n)
        return false;
    for (; i < n; i++) {
        char c = s[i];
        unsigned d;
        if (c >= '0' && c <= '9')
            d = c - '0';
        else if (base == 16 && c >= 'a' && c <= 'f')
            d = c - 'a' + 10;
        else if (base == 16 && c >= 'A' && c <= 'F')
            d = c - 'A' + 10;
        else
            return false;
        v = v * base + d;
    }
    *out = v;
    return true;
}
