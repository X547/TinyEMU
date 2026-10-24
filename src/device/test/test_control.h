/*
 * Test control device
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

/* Registers, 32 bits wide. The guest finds the device by its FDT
   compatible or its ACPI _HID. */
#define TEST_CONTROL_COMPATIBLE "tinyemu,test-control"
#define TEST_CONTROL_ACPI_HID   "TEMU0001"

#define TEST_CONTROL_ID       0x00 /* RO: TEST_CONTROL_MAGIC */
#define TEST_CONTROL_VERSION  0x04 /* RO */
#define TEST_CONTROL_OUTPUT   0x08 /* WO: low byte appended to the output */
#define TEST_CONTROL_EXIT     0x0c /* WO: stops the emulator with this status */

#define TEST_CONTROL_MAGIC    0x54455354 /* "TEST" */
#define TEST_CONTROL_REVISION 1
