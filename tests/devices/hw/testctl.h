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
#pragma once

#include "registry.h"

/* Lines the guest drives itself, which is what interrupt controllers are
   tested with: line i is the device's i-th interrupt in the tables. All
   are 0 when there is no test-control device. */
int tc_irq_count();
uint32_t tc_irq_levels();
void tc_irq_set_levels(uint32_t mask);
void tc_irq_raise(int i);
void tc_irq_lower(int i);

/* The disks of temu's "test" block driver. A machine gives each a size of
   its own, by which a test finds the one behind the device it drives:
   tc_block_find() selects it for the block registers, or gives -1. */
int tc_block_find(uint64_t sectors);
uint32_t tc_block_read(uint32_t reg);
void tc_block_write(uint32_t reg, uint32_t value);
/* Between tests: every disk finishes requests at once again, none is to
   fail, and what was held is let go. */
void tc_block_reset_all();

/* The streams of temu's "test" audio driver. A machine gives each a "tag"
   of its own, by which a test finds the one behind the device it drives:
   tc_audio_find() selects it for the audio registers, or gives -1. */
int tc_audio_find(uint32_t tag);
uint32_t tc_audio_read(uint32_t reg);
void tc_audio_write(uint32_t reg, uint32_t value);
/* The selected stream's clock moves on by 'frames', and its device moves
   what became due. */
void tc_audio_advance(uint32_t frames);
/* 'len' bytes the selected stream moved, from byte 'pos' of it on. */
void tc_audio_data(uint32_t pos, void *buf, uint32_t len);
/* The CRC-32 that TEST_CONTROL_AUDIO_CRC keeps, carried on from 'crc'. */
uint32_t tc_crc32(uint32_t crc, const void *buf, size_t len);

/* Resets of the machine: whether there is a device to count them, how many
   there were, the word they leave alone, and asking for one. */
bool tc_present();
uint32_t tc_resets();
uint32_t tc_keep();
void tc_set_keep(uint32_t value);
void tc_request_reset();
