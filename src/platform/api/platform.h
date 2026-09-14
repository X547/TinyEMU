/*
 * Host services available to the machine
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

#include <memory>

#include "host_block.h"
#include "host_console.h"
#include "host_ethernet.h"
#include "host_fs.h"
#include "host_input.h"
#include "host_screen.h"

/* The host objects a machine's devices connect to. Each may be null when
   the host has none. */
class Platform {
public:
    virtual ~Platform() = default;

    virtual HostConsole *Console() = 0;
    virtual HostScreen *Screen() = 0;
    virtual HostKeyboard *Keyboard() = 0;
    virtual HostPointer *Pointer() = 0;

    /* The back ends one device takes over. They report their own errors and
       return nullptr. */
    virtual std::unique_ptr<HostBlockDevice> OpenBlockDevice(
        const char *path) = 0;
    /* 'ifname' is the host interface, for a driver that attaches to one */
    virtual std::unique_ptr<HostEthernet> OpenEthernet(const char *driver,
                                                       const char *ifname) = 0;
    virtual std::unique_ptr<HostFileSystem> OpenFileSystem(
        const char *path) = 0;
};
