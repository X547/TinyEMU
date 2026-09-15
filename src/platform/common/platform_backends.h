/*
 * Host back ends selected by the build
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
#include "host_fs.h"
#include "host_input.h"
#include "host_screen.h"

class DeviceLock;
class EventLoop;
class HostEthernet;
class RunControl;

/* A window with the keyboard and the pointer that come with it. It owns the
   window system's event loop. */
class HostDisplay: public HostScreen, public HostKeyboard, public HostPointer {
public:
    /* On the main thread: runs the window system until Quit(). */
    virtual void Run() = 0;
    /* Any thread; also before Run(), which then returns at once. */
    virtual void Quit() = 0;
};

/* Each is implemented by exactly one file the build picks. A back end the
   build leaves out reports that and returns nullptr. */

/* C-a x and the end of input request a shutdown from 'run_control'. */
std::unique_ptr<HostConsole> host_console_create(EventLoop &loop,
                                                 RunControl &run_control,
                                                 bool allow_ctrlc);

/* nullptr when the build has no display. Input reaches the devices with
   'lock' held; closing the window requests a shutdown. */
std::unique_ptr<HostDisplay> host_display_create(DeviceLock &lock,
                                                 RunControl &run_control);

typedef enum {
    BLOCK_MODE_RO,
    BLOCK_MODE_RW,
    /* writes are kept in memory and dropped on exit */
    BLOCK_MODE_SNAPSHOT,
} BlockModeEnum;

std::unique_ptr<HostBlockDevice> file_block_open(const char *filename,
                                                 BlockModeEnum mode);

/* HTTP back ends; transfers progress from the event loop */
void url_backend_init(EventLoop &loop);
bool url_backend_matches(const char *path);
/* returns once the image description is loaded */
std::unique_ptr<HostBlockDevice> url_block_open(EventLoop &loop,
                                                const char *url);
/* returns once the root is loaded; 'preload_file' may be null */
std::unique_ptr<HostFileSystem> url_fs_open(EventLoop &loop, const char *url,
                                            const char *preload_file);

/* a host directory; nullptr if 'path' is not one */
std::unique_ptr<HostFileSystem> disk_fs_open(const char *path);

std::unique_ptr<HostEthernet> tap_ethernet_open(EventLoop &loop,
                                                const char *ifname);
std::unique_ptr<HostEthernet> slirp_ethernet_open(EventLoop &loop);
