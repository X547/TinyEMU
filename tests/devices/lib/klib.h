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
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

extern "C" {
void *memcpy(void *dst, const void *src, size_t n);
void *memmove(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
size_t strnlen(const char *s, size_t max);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strchr(const char *s, int c);
}

inline void *operator new(size_t, void *p) noexcept {return p;}
inline void *operator new[](size_t, void *p) noexcept {return p;}

template <typename T> constexpr T min(T a, T b) {return a < b ? a : b;}
template <typename T> constexpr T max(T a, T b) {return a > b ? a : b;}

constexpr uint64_t align_up(uint64_t v, uint64_t a) {return (v + a - 1) & ~(a - 1);}
constexpr uint64_t align_down(uint64_t v, uint64_t a) {return v & ~(a - 1);}

#define ARRAY_SIZE(a) (sizeof(a) / sizeof((a)[0]))

/* Whether 'name' matches 'pattern', in which '*' stands for any run of
   characters and '?' for one. */
bool glob_match(const char *pattern, const char *name);

/* Parse an unsigned number, decimal or 0x hex, of at most 'len' characters
   (all of them when len < 0). False when there are none or junk follows. */
bool parse_u64(const char *s, int len, uint64_t *out);

/* Big endian loads, for the device tree and SCSI. */
inline uint16_t be16(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return b[0] << 8 | b[1];
}
inline uint32_t be32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return (uint32_t)b[0] << 24 | (uint32_t)b[1] << 16 |
           (uint32_t)b[2] << 8 | b[3];
}
inline uint64_t be64(const void *p)
{
    return (uint64_t)be32(p) << 32 | be32((const uint8_t *)p + 4);
}

/* And the stores. */
inline void put_be16(void *p, uint16_t v)
{
    uint8_t *b = (uint8_t *)p;
    b[0] = v >> 8;
    b[1] = v;
}
inline void put_be32(void *p, uint32_t v)
{
    uint8_t *b = (uint8_t *)p;
    b[0] = v >> 24;
    b[1] = v >> 16;
    b[2] = v >> 8;
    b[3] = v;
}

/* Little endian loads of unaligned data, for ACPI tables. */
inline uint16_t le16(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return b[0] | b[1] << 8;
}
inline uint32_t le32(const void *p)
{
    const uint8_t *b = (const uint8_t *)p;
    return b[0] | b[1] << 8 | b[2] << 16 | (uint32_t)b[3] << 24;
}
inline uint64_t le64(const void *p)
{
    return le32(p) | (uint64_t)le32((const uint8_t *)p + 4) << 32;
}
inline void put_le16(void *p, uint16_t v)
{
    uint8_t *b = (uint8_t *)p;
    b[0] = v;
    b[1] = v >> 8;
}
inline void put_le32(void *p, uint32_t v)
{
    uint8_t *b = (uint8_t *)p;
    b[0] = v;
    b[1] = v >> 8;
    b[2] = v >> 16;
    b[3] = v >> 24;
}

/* Stop for good: a state no test can be reported from. The message goes to
   whichever output is up. */
[[noreturn]] void panic(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
