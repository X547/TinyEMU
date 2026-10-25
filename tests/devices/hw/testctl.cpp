/*
 * The test-control device's interrupt lines
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
#include "testctl.h"

#include "console.h"
#include "mmio.h"
#include "test_control.h"

namespace {

uint64_t base()
{
    const HwDevice *d = test_control_device();
    const HwRegion *r = d ? hw_region(d, HW_MMIO) : nullptr;
    if (r == nullptr ||
        mmio_read32(r->base + TEST_CONTROL_ID) != TEST_CONTROL_MAGIC)
        return 0;
    return r->base;
}

}


int tc_irq_count()
{
    uint64_t b = base();
    return b ? (int)mmio_read32(b + TEST_CONTROL_IRQ_COUNT) : 0;
}


uint32_t tc_irq_levels()
{
    uint64_t b = base();
    return b ? mmio_read32(b + TEST_CONTROL_IRQ_LEVEL) : 0;
}


void tc_irq_set_levels(uint32_t mask)
{
    uint64_t b = base();
    if (b)
        mmio_write32(b + TEST_CONTROL_IRQ_LEVEL, mask);
}


void tc_irq_raise(int i)
{
    tc_irq_set_levels(tc_irq_levels() | 1u << i);
}


void tc_irq_lower(int i)
{
    tc_irq_set_levels(tc_irq_levels() & ~(1u << i));
}


int tc_block_find(uint64_t sectors)
{
    uint64_t b = base();
    if (b == 0)
        return -1;
    int n = mmio_read32(b + TEST_CONTROL_BLOCK_COUNT);
    for (int i = 0; i < n; i++) {
        mmio_write32(b + TEST_CONTROL_BLOCK_SELECT, i);
        if (mmio_read32(b + TEST_CONTROL_BLOCK_SECTORS) == sectors)
            return i;
    }
    return -1;
}


uint32_t tc_block_read(uint32_t reg)
{
    uint64_t b = base();
    return b ? mmio_read32(b + reg) : 0;
}


void tc_block_write(uint32_t reg, uint32_t value)
{
    uint64_t b = base();
    if (b)
        mmio_write32(b + reg, value);
}


void tc_block_reset_all()
{
    uint64_t b = base();
    if (b == 0)
        return;
    int n = mmio_read32(b + TEST_CONTROL_BLOCK_COUNT);
    for (int i = 0; i < n; i++) {
        mmio_write32(b + TEST_CONTROL_BLOCK_SELECT, i);
        mmio_write32(b + TEST_CONTROL_BLOCK_MODE, TEST_BLOCK_MODE_SYNC);
        mmio_write32(b + TEST_CONTROL_BLOCK_FAIL, 0);
        mmio_write32(b + TEST_CONTROL_BLOCK_RELEASE, 0x7fffffff);
    }
}
