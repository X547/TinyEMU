/*
 * The platform the emulator runs on
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
#include "host_platform.h"

#include <stdio.h>
#include <string.h>


HostPlatform::HostPlatform(EventLoop &loop, const PlatformOptions &options):
    fLoop(loop),
    fOptions(options)
{
    url_backend_init(fLoop);
    fConsole = host_console_create(fLoop, fOptions.allow_ctrlc);
    fDisplay = host_display_create(fLoop);
}


HostPlatform::~HostPlatform() = default;


std::unique_ptr<HostBlockDevice> HostPlatform::OpenBlockDevice(const char *path)
{
    if (url_backend_matches(path)) {
        return url_block_open(fLoop, path);
    }
    return file_block_open(path, fOptions.block_mode);
}


std::unique_ptr<HostEthernet> HostPlatform::OpenEthernet(const char *driver,
                                                         const char *ifname)
{
    if (strcmp(driver, "user") == 0) {
        return slirp_ethernet_open(fLoop);
    }
    if (strcmp(driver, "tap") == 0) {
        if (ifname == nullptr) {
            fprintf(stderr, "tap: expecting an 'ifname' property\n");
            return nullptr;
        }
        return tap_ethernet_open(fLoop, ifname);
    }
    fprintf(stderr, "Unsupported network driver '%s'\n", driver);
    return nullptr;
}


std::unique_ptr<HostFileSystem> HostPlatform::OpenFileSystem(const char *path)
{
    std::unique_ptr<HostFileSystem> fs;

    if (url_backend_matches(path)) {
        return url_fs_open(fLoop, path, fOptions.build_preload_file);
    }
    fs = disk_fs_open(path);
    if (fs == nullptr) {
        fprintf(stderr, "%s: must be a directory\n", path);
    }
    return fs;
}
