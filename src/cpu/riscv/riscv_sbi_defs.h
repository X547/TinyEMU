/*
 * RISC-V SBI numbers
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

#define SBI_SUCCESS               0
#define SBI_ERR_FAILED            -1
#define SBI_ERR_NOT_SUPPORTED     -2
#define SBI_ERR_INVALID_PARAM     -3
#define SBI_ERR_ALREADY_AVAILABLE -6

#define SBI_EXT_LEGACY_LAST 0x08
#define SBI_EXT_BASE 0x10
#define SBI_EXT_TIME 0x54494d45
#define SBI_EXT_IPI  0x735049
#define SBI_EXT_RFNC 0x52464e43
#define SBI_EXT_HSM  0x48534d
#define SBI_EXT_SRST 0x53525354
#define SBI_EXT_DBCN 0x4442434e

#define SBI_LEGACY_SET_TIMER   0
#define SBI_LEGACY_PUTCHAR     1
#define SBI_LEGACY_GETCHAR     2
#define SBI_LEGACY_CLEAR_IPI   3
#define SBI_LEGACY_SEND_IPI    4
#define SBI_LEGACY_FENCE_I     5
#define SBI_LEGACY_SFENCE_VMA  6
#define SBI_LEGACY_SFENCE_ASID 7
#define SBI_LEGACY_SHUTDOWN    8

#define SBI_HSM_SUSPEND_RETENTIVE     0x00000000
#define SBI_HSM_SUSPEND_NON_RETENTIVE 0x80000000

#define SBI_SRST_SHUTDOWN    0
#define SBI_SRST_COLD_REBOOT 1
#define SBI_SRST_WARM_REBOOT 2
#define SBI_SRST_REASON_NONE    0
#define SBI_SRST_REASON_FAILURE 1
