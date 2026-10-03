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
#include "config_props.h"

#include <stdlib.h>

#include "devices.h"
#include "machine.h"


std::unique_ptr<HostBlockDevice> config_open_block(const DeviceConfig &cfg,
                                                   DeviceContext *ctx,
                                                   bool read_only)
{
    std::unique_ptr<HostBlockDevice> bs;
    const char *file;
    char *fname;

    if (!cfg.GetStrOpt("file", &file)) {
        return nullptr;
    }
    if (file == nullptr) {
        vm_error("%s: expecting a 'file' property\n", cfg.Type());
        return nullptr;
    }
    fname = cfg.ResolvePath(file);
    bs = ctx->platform->OpenBlockDevice(fname, read_only);
    free(fname);
    if (bs == nullptr) {
        vm_error("%s: could not open\n", file);
    }
    return bs;
}


bool config_get_quirks(const DeviceConfig &cfg,
                       uint32_t (*from_name)(const char *name),
                       uint32_t *out)
{
    const char *type = cfg.Type();
    uint32_t quirks = 0;

    JSONValue list = cfg.Get("quirks");
    if (!json_is_undefined(list)) {
        if (list.type != JSON_ARRAY) {
            vm_error("%s: 'quirks' must be an array of names\n", type);
            return false;
        }
        for (int i = 0; i < list.u.array->Length(); i++) {
            JSONValue item = json_array_get(list, i);
            if (item.type != JSON_STR) {
                vm_error("%s: 'quirks' must be an array of names\n", type);
                return false;
            }
            uint32_t bits = from_name(item.u.str->data);
            if (bits == 0) {
                vm_error("%s: unknown quirk '%s'\n", type, item.u.str->data);
                return false;
            }
            quirks |= bits;
        }
    }
    *out = quirks;
    return true;
}
