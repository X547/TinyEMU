/*
 * syscon-poweroff tests
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
#include "mmio.h"
#include "test.h"

/* The Linux bindings power/reset/syscon-poweroff.yaml (a "regmap"
   phandle, "offset" and "value", or "mask") and the SiFive test device
   ("sifive,test0") the register belongs to, as riscv-isa-sim and QEMU's
   sifive_test implement it: 0x5555 passes, 0x3333 fails with the status
   in the upper 16 bits, 0x7777 resets. */

#define TEST_PASS  0x5555
#define TEST_FAIL  0x3333
#define TEST_RESET 0x7777

namespace {

uint64_t sReg;
uint32_t sValue;

void require_syscon()
{
    const HwDevice *d = t_require_device("syscon-poweroff");
    ASSERT_TRUE(d->fdt != nullptr);
    uint32_t regmap = 0, offset = 0;
    ASSERT_TRUE(fdt_prop_u32(d->fdt, "regmap", &regmap));
    ASSERT_TRUE(fdt_prop_u32(d->fdt, "value", &sValue));
    fdt_prop_u32(d->fdt, "offset", &offset);
    const FdtNode *syscon = fdt_find_phandle(regmap);
    ASSERT_TRUE(syscon != nullptr);
    uint64_t base, size;
    ASSERT_TRUE(fdt_reg(syscon, 0, &base, &size));
    sReg = base + offset;
}

}


/* The node and the block it names, as the bindings have them. */
TEST(syscon, nodes)
{
    require_syscon();
    const HwDevice *d = registry_find("syscon-poweroff");
    uint32_t regmap = 0;
    fdt_prop_u32(d->fdt, "regmap", &regmap);
    const FdtNode *syscon = fdt_find_phandle(regmap);
    EXPECT_TRUE(fdt_is_compatible(syscon, "syscon"));
    EXPECT_TRUE(fdt_is_compatible(syscon, "sifive,test0"));
    EXPECT_EQ(sValue, (uint32_t)TEST_PASS);
    uint64_t base, size;
    fdt_reg(syscon, 0, &base, &size);
    SNAP("size", "%#llx", T_FMT_VAL(size));
    SNAP("read", "%#x", mmio_read32(sReg));
}

/* Other values, and other offsets, leave the machine running. Reset is
   not offered, and is ignored. */
TEST(syscon, ignored_writes)
{
    require_syscon();
    mmio_write32(sReg, 0x1234);
    mmio_write32(sReg + 4, TEST_PASS);
    mmio_write32(sReg + 8, TEST_FAIL | 2 << 16);
    mmio_write32(sReg, TEST_RESET);
    mmio_write32(sReg, TEST_RESET);
    /* still here */
    EXPECT_EQ(mmio_read32(sReg), 0u);
}

/* The value the node gives stops the machine, with status 0. */
TEST(syscon, power_off)
{
    require_syscon();
    t_expect_exit(0);
    mmio_write32(sReg, sValue);
    arch_halt();
}

TEST(syscon, fail_status)
{
    require_syscon();
    t_expect_exit(42);
    mmio_write32(sReg, 42 << 16 | TEST_FAIL);
    arch_halt();
}

/* A failure is never a status of 0. */
TEST(syscon, fail_status_zero)
{
    require_syscon();
    t_expect_exit(1);
    mmio_write32(sReg, TEST_FAIL);
    arch_halt();
}

/* Only 8 bits survive on the host; more is a failure, 255. */
TEST(syscon, fail_status_too_large)
{
    require_syscon();
    t_expect_exit(255);
    mmio_write32(sReg, 0x1234u << 16 | TEST_FAIL);
    arch_halt();
}
