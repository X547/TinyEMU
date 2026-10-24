/*
 * Device register access
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

/* Every access is exactly one load or store of the given width, in program
   order with the others. */
#define MMIO_BARRIER() __asm__ volatile("" ::: "memory")

inline uint8_t mmio_read8(uint64_t a)
{
    MMIO_BARRIER();
    return *(volatile uint8_t *)(uintptr_t)a;
}
inline uint16_t mmio_read16(uint64_t a)
{
    MMIO_BARRIER();
    return *(volatile uint16_t *)(uintptr_t)a;
}
inline uint32_t mmio_read32(uint64_t a)
{
    MMIO_BARRIER();
    return *(volatile uint32_t *)(uintptr_t)a;
}
inline void mmio_write8(uint64_t a, uint8_t v)
{
    *(volatile uint8_t *)(uintptr_t)a = v;
    MMIO_BARRIER();
}
inline void mmio_write16(uint64_t a, uint16_t v)
{
    *(volatile uint16_t *)(uintptr_t)a = v;
    MMIO_BARRIER();
}
inline void mmio_write32(uint64_t a, uint32_t v)
{
    *(volatile uint32_t *)(uintptr_t)a = v;
    MMIO_BARRIER();
}

#if defined(__i386__) || defined(__x86_64__)
inline uint8_t inb(uint16_t port)
{
    uint8_t v;
    __asm__ volatile("inb %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
inline uint16_t inw(uint16_t port)
{
    uint16_t v;
    __asm__ volatile("inw %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
inline uint32_t inl(uint16_t port)
{
    uint32_t v;
    __asm__ volatile("inl %1, %0" : "=a"(v) : "Nd"(port) : "memory");
    return v;
}
inline void outb(uint16_t port, uint8_t v)
{
    __asm__ volatile("outb %0, %1" : : "a"(v), "Nd"(port) : "memory");
}
inline void outw(uint16_t port, uint16_t v)
{
    __asm__ volatile("outw %0, %1" : : "a"(v), "Nd"(port) : "memory");
}
inline void outl(uint16_t port, uint32_t v)
{
    __asm__ volatile("outl %0, %1" : : "a"(v), "Nd"(port) : "memory");
}
#endif
