/*
 * 64 bit division for 32 bit targets
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
#include <stdint.h>

/* The compiler calls these for 64 bit '/' and '%' where the processor has
   no such instruction; no runtime library is linked to provide them. */
#if defined(__i386__) || (defined(__riscv) && __riscv_xlen == 32)

static uint64_t udivmod(uint64_t n, uint64_t d, uint64_t *rem)
{
    if (d == 0) {
        /* what a hardware divide would do is trap; a wrong answer is
           easier to report from */
        *rem = n;
        return UINT64_MAX;
    }
    uint64_t q = 0, r = 0;
    for (int i = 63; i >= 0; i--) {
        r = r << 1 | ((n >> i) & 1);
        if (r >= d) {
            r -= d;
            q |= (uint64_t)1 << i;
        }
    }
    *rem = r;
    return q;
}

extern "C" {

uint64_t __udivdi3(uint64_t n, uint64_t d)
{
    uint64_t r;
    return udivmod(n, d, &r);
}

uint64_t __umoddi3(uint64_t n, uint64_t d)
{
    uint64_t r;
    udivmod(n, d, &r);
    return r;
}

uint64_t __udivmoddi4(uint64_t n, uint64_t d, uint64_t *rem)
{
    uint64_t r;
    uint64_t q = udivmod(n, d, &r);
    if (rem)
        *rem = r;
    return q;
}

int64_t __divdi3(int64_t n, int64_t d)
{
    uint64_t r;
    uint64_t q = udivmod(n < 0 ? -(uint64_t)n : n, d < 0 ? -(uint64_t)d : d,
                         &r);
    return (n < 0) != (d < 0) ? -(int64_t)q : (int64_t)q;
}

int64_t __moddi3(int64_t n, int64_t d)
{
    uint64_t r;
    udivmod(n < 0 ? -(uint64_t)n : n, d < 0 ? -(uint64_t)d : d, &r);
    return n < 0 ? -(int64_t)r : (int64_t)r;
}

}

#endif
