/*
 * The little of the C++ runtime the suite uses
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
#include "heap.h"

/* new takes from the heap, so what a test allocates goes back when the
   runner releases to its mark; delete has nothing to do. */
void *operator new(size_t size)
{
    void *p = heap_alloc(size);
    if (p == nullptr)
        panic("out of memory allocating %zu bytes", size);
    return p;
}

void *operator new[](size_t size)
{
    return operator new(size);
}

void operator delete(void *) noexcept {}
void operator delete[](void *) noexcept {}
void operator delete(void *, size_t) noexcept {}
void operator delete[](void *, size_t) noexcept {}

extern "C" void __cxa_pure_virtual()
{
    panic("pure virtual function called");
}

typedef void (*InitFn)();
extern "C" InitFn __init_array_start[], __init_array_end[];

/* Static constructors, once the image is relocated and the BSS clear. */
void run_constructors()
{
    for (InitFn *f = __init_array_start; f != __init_array_end; f++)
        (*f)();
}
