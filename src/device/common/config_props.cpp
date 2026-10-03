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
#include <string.h>

#include <vector>

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


std::unique_ptr<HostAudio> config_open_audio(const DeviceConfig &cfg,
                                             DeviceContext *ctx,
                                             AudioDirectionEnum direction)
{
    const char *type = cfg.Type();
    AudioSettings settings;
    const char *driver = nullptr, *device = nullptr, *file = nullptr;
    int loop = 0, latency = settings.latency_ms;

    settings.direction = direction;
    JSONValue host = cfg.Get("host");
    if (!json_is_undefined(host)) {
        if (host.type != JSON_OBJ) {
            vm_error("%s: 'host' must be an object\n", type);
            return nullptr;
        }
        if (vm_get_str_opt(host, "driver", &driver) < 0 ||
            vm_get_str_opt(host, "device", &device) < 0 ||
            vm_get_str_opt(host, "file", &file) < 0 ||
            vm_get_int_opt(host, "loop", &loop, 0) < 0 ||
            vm_get_int_opt(host, "latency", &latency, latency) < 0) {
            return nullptr;
        }
    }
    if (latency < 1 || latency > 1000) {
        vm_error("%s: 'latency' must be between 1 and 1000 ms\n", type);
        return nullptr;
    }
    if (driver != nullptr) {
        settings.driver = driver;
    }
    settings.device = device;
    settings.loop = loop != 0;
    settings.latency_ms = latency;

    char *path = nullptr;
    if (file != nullptr) {
        path = cfg.ResolvePath(file);
        settings.file = path;
    }
    auto audio = ctx->platform->OpenAudio(settings);
    free(path);
    return audio;
}


/* A dotted quad, as a host byte order value. Parsed here rather than by the
   host's resolver so that the configuration loader needs no socket
   headers. */
static bool parse_ipv4(const char *str, uint32_t *out)
{
    uint32_t addr = 0;

    for (int i = 0; i < 4; i++) {
        if (i > 0 && *str++ != '.') {
            return false;
        }
        uint32_t octet = 0;
        int digits = 0;
        while (*str >= '0' && *str <= '9') {
            octet = octet * 10 + (uint32_t)(*str++ - '0');
            if (++digits > 3 || octet > 255) {
                return false;
            }
        }
        if (digits == 0) {
            return false;
        }
        addr = (addr << 8) | octet;
    }
    if (*str != '\0') {
        return false;
    }
    *out = addr;
    return true;
}


/* One "forward" entry: a host port, and the guest port a connection to it
   reaches. Reports and returns false on anything it cannot read. */
static bool parse_forward(const char *type, JSONValue item,
                          EthernetForward *out)
{
    const char *proto, *host_addr, *guest_addr;
    int host_port, guest_port;

    if (item.type != JSON_OBJ) {
        vm_error("%s: every 'forward' entry must be an object\n", type);
        return false;
    }
    if (vm_get_str_opt(item, "proto", &proto) < 0 ||
        vm_get_str_opt(item, "host_addr", &host_addr) < 0 ||
        vm_get_str_opt(item, "guest_addr", &guest_addr) < 0 ||
        vm_get_int(item, "host_port", &host_port) < 0 ||
        vm_get_int(item, "guest_port", &guest_port) < 0) {
        return false;
    }

    if (proto != nullptr && strcmp(proto, "udp") == 0) {
        out->is_udp = true;
    } else if (proto != nullptr && strcmp(proto, "tcp") != 0) {
        vm_error("%s: 'proto' must be \"tcp\" or \"udp\"\n", type);
        return false;
    }

    /* Only the host itself reaches the guest unless the configuration says
       otherwise: a forward is a hole through to a guest, and one opened on
       every address of the host offers it to the whole network. */
    out->host_addr = 0x7f000001;
    if (host_addr != nullptr && !parse_ipv4(host_addr, &out->host_addr)) {
        vm_error("%s: 'host_addr' is not an address\n", type);
        return false;
    }
    /* Left at zero, the back end uses whichever address it gave the guest. */
    if (guest_addr != nullptr && !parse_ipv4(guest_addr, &out->guest_addr)) {
        vm_error("%s: 'guest_addr' is not an address\n", type);
        return false;
    }

    if (host_port < 1 || host_port > 65535 ||
        guest_port < 1 || guest_port > 65535) {
        vm_error("%s: 'host_port' and 'guest_port' must be between 1 and "
                 "65535\n", type);
        return false;
    }
    out->host_port = host_port;
    out->guest_port = guest_port;
    return true;
}


std::unique_ptr<HostEthernet> config_open_ethernet(const DeviceConfig &cfg,
                                                   DeviceContext *ctx)
{
    const char *type = cfg.Type();
    const char *driver, *ifname;
    std::vector<EthernetForward> forwards;

    if (!cfg.GetStr("driver", &driver) ||
        !cfg.GetStrOpt("ifname", &ifname)) {
        return nullptr;
    }

    JSONValue list = cfg.Get("forward");
    if (!json_is_undefined(list)) {
        if (list.type != JSON_ARRAY) {
            vm_error("%s: 'forward' must be an array of entries\n", type);
            return nullptr;
        }
        for (int i = 0; i < list.u.array->Length(); i++) {
            EthernetForward fwd;
            if (!parse_forward(type, json_array_get(list, i), &fwd)) {
                return nullptr;
            }
            forwards.push_back(fwd);
        }
    }

    return ctx->platform->OpenEthernet(driver, ifname, forwards);
}


std::unique_ptr<HostFileSystem> config_open_fs(const DeviceConfig &cfg,
                                               DeviceContext *ctx)
{
    std::unique_ptr<HostFileSystem> fs;
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
    fs = ctx->platform->OpenFileSystem(fname);
    free(fname);
    return fs;
}
