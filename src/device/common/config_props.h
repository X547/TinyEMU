/*
 * Properties several device classes read the same way
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
#include <memory>

#include "device_class.h"
#include "host_block.h"

/* The device's "file", relative to the configuration file, opened as a disk
   image. Reports and returns nullptr on failure. */
std::unique_ptr<HostBlockDevice> config_open_block(const DeviceConfig &cfg,
                                                   DeviceContext *ctx,
                                                   bool read_only = false);

/* The device's "quirks": deviations a guest needs, asked for by name so that
   a conformant guest gets a conformant device. 'from_name' gives the bits for
   one name, or 0 for a name it does not know. Reports and returns false on
   anything it cannot read. */
bool config_get_quirks(const DeviceConfig &cfg,
                       uint32_t (*from_name)(const char *name),
                       uint32_t *out);
