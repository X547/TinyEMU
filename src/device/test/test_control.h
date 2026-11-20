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
#define TEST_CONTROL_IRQ_COUNT 0x10 /* RO: the interrupt lines it drives */
#define TEST_CONTROL_IRQ_LEVEL 0x14 /* RW: bit n is the level of line n */

/* The disks of the "test" block driver, in the order they were opened. The
   registers after SELECT act on the disk it names. */
#define TEST_CONTROL_BLOCK_COUNT   0x18 /* RO */
#define TEST_CONTROL_BLOCK_SELECT  0x1c /* RW */
#define TEST_CONTROL_BLOCK_SECTORS 0x20 /* RO: its size in 512 byte sectors */
#define TEST_CONTROL_BLOCK_MODE    0x24 /* RW: TEST_BLOCK_MODE_* */
#define TEST_CONTROL_BLOCK_FAIL    0x28 /* RW: the next requests that fail */
#define TEST_CONTROL_BLOCK_HELD    0x2c /* RO: requests waiting for RELEASE */
#define TEST_CONTROL_BLOCK_RELEASE 0x30 /* WO: finish this many held requests */
#define TEST_CONTROL_BLOCK_READS   0x34 /* RO: read requests so far */
#define TEST_CONTROL_BLOCK_WRITES  0x38 /* RO: write requests so far */
#define TEST_CONTROL_BLOCK_CANCELS 0x3c /* RO: requests the device gave up */

/* Resets of the machine. KEEP is the one register a reset leaves, for the
   guest to tell itself what it was doing. */
#define TEST_CONTROL_RESETS   0x40 /* RO: machine resets so far */
#define TEST_CONTROL_KEEP     0x44 /* RW: survives a reset */
#define TEST_CONTROL_RESET    0x48 /* WO: resets the machine */

/* The streams of the "test" audio driver, in the order they were opened. The
   registers after SELECT act on the stream it names. Its clock runs only
   when ADVANCE moves it, which also has the device move the frames that
   became due. Counts start again with each start of the stream. */
#define TEST_CONTROL_AUDIO_COUNT     0x80 /* RO */
#define TEST_CONTROL_AUDIO_SELECT    0x84 /* RW */
#define TEST_CONTROL_AUDIO_TAG       0x88 /* RO: its "tag" in the machine */
#define TEST_CONTROL_AUDIO_DIRECTION 0x8c /* RO: TEST_AUDIO_DIR_* */
#define TEST_CONTROL_AUDIO_RUNNING   0x90 /* RO: 1 while started */
#define TEST_CONTROL_AUDIO_STARTS    0x94 /* RO: starts so far */
#define TEST_CONTROL_AUDIO_RATE      0x98 /* RO: the format of the last start */
#define TEST_CONTROL_AUDIO_CHANNELS  0x9c /* RO */
#define TEST_CONTROL_AUDIO_BITS      0xa0 /* RO */
#define TEST_CONTROL_AUDIO_ADVANCE   0xa4 /* WO: frames the clock moves on */
#define TEST_CONTROL_AUDIO_CLOCK     0xa8 /* RO: frames due since the start */
#define TEST_CONTROL_AUDIO_FRAMES    0xac /* RO: frames moved since the start */
#define TEST_CONTROL_AUDIO_CRC       0xb0 /* RO: CRC-32 of the bytes moved */
#define TEST_CONTROL_AUDIO_DATA_POS  0xb4 /* RW: byte offset into the stream */
#define TEST_CONTROL_AUDIO_DATA      0xb8 /* RO: 4 bytes moved at DATA_POS,
                                             which moves on by 4 */

#define TEST_CONTROL_MAX_IRQS 8

#define TEST_CONTROL_MAGIC    0x54455354 /* "TEST" */
#define TEST_CONTROL_REVISION 4

/* How a test disk finishes requests: in the call, soon after from the event
   loop, or once RELEASE says so. A failing request returns an error in the
   call, or finishes with one. */
#define TEST_BLOCK_MODE_SYNC  0
#define TEST_BLOCK_MODE_ASYNC 1
#define TEST_BLOCK_MODE_HOLD  2

/* RELEASE finishes the oldest held requests, or with this bit the newest
   first. */
#define TEST_BLOCK_RELEASE_NEWEST 0x80000000u

#define TEST_AUDIO_DIR_RENDER  0 /* the device plays to it */
#define TEST_AUDIO_DIR_CAPTURE 1 /* the device records from it */

/* The bytes of what was moved, either way, that DATA reaches back over;
   older ones read as 0. */
#define TEST_AUDIO_HISTORY 65536

/* What a capture stream records: sample 'ch' of frame 'f' since the start,
   left justified, of which a container of n bytes takes the top n. */
#define TEST_AUDIO_SAMPLE(f, ch) \
    ((uint32_t)(((uint32_t)(f) << 4 | (uint32_t)(ch)) * 0x9e3779b1u))

/* What byte 'i' of sector 's' of a test disk holds until it is written. */
#define TEST_BLOCK_PATTERN(s, i) \
    ((uint8_t)(((s) ^ ((s) >> 8)) * 0x9d + (i) + ((i) >> 8) * 0x35))
