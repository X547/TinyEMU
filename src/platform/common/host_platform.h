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
#pragma once

#include <memory>

#include "event_loop.h"
#include "platform.h"
#include "platform_backends.h"

struct PlatformOptions {
    /* C-c stops the emulator rather than reaching the guest */
    bool allow_ctrlc = false;
    BlockModeEnum block_mode = BLOCK_MODE_SNAPSHOT;
    /* network file systems record the files they fetch here */
    const char *build_preload_file = nullptr;
};


/* Composes the back ends the build selected. Must outlive the machine,
   whose devices own some of them. */
class HostPlatform final: public Platform {
private:
    EventLoop &fLoop;
    PlatformOptions fOptions;
    std::unique_ptr<HostConsole> fConsole;
    std::unique_ptr<HostDisplay> fDisplay;

public:
    HostPlatform(EventLoop &loop, const PlatformOptions &options);
    ~HostPlatform() override;

    EventLoop &Loop() {return fLoop;}

    /* Platform */
    HostConsole *Console() override {return fConsole.get();}
    HostScreen *Screen() override {return fDisplay.get();}
    HostKeyboard *Keyboard() override {return fDisplay.get();}
    HostPointer *Pointer() override {return fDisplay.get();}
    std::unique_ptr<HostBlockDevice> OpenBlockDevice(
        const char *path) override;
    std::unique_ptr<HostEthernet> OpenEthernet(const char *driver,
                                               const char *ifname) override;
    std::unique_ptr<HostFileSystem> OpenFileSystem(const char *path) override;
};
