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


HostPlatform::HostPlatform(DeviceLock &lock, RunControl &run_control,
                           const PlatformOptions &options):
    fDeviceLock(lock),
    fOptions(options)
{
    url_backend_init(fLoop);
    fConsole = host_console_create(fLoop, run_control, fOptions.allow_ctrlc);
    fDisplay = host_display_create(fDeviceLock, run_control);
}


HostPlatform::~HostPlatform()
{
    fLoop.Stop();
}


void HostPlatform::StartIo()
{
    fLoop.Start(fDeviceLock);
}


void HostPlatform::StopIo()
{
    fLoop.Stop();
}


void HostPlatform::RunGui()
{
    if (fDisplay != nullptr) {
        fDisplay->Run();
        return;
    }
    std::unique_lock<std::mutex> locker(fGuiMutex);
    fGuiCond.wait(locker, [this]() {return fGuiQuit;});
}


void HostPlatform::QuitGui()
{
    if (fDisplay != nullptr) {
        fDisplay->Quit();
        return;
    }
    std::lock_guard<std::mutex> locker(fGuiMutex);
    fGuiQuit = true;
    fGuiCond.notify_all();
}


std::unique_ptr<HostBlockDevice> HostPlatform::OpenBlockDevice(const char *path,
                                                              bool read_only)
{
    if (url_backend_matches(path)) {
        return url_block_open(fLoop, path);
    }
    return file_block_open(fLoop, path,
                           read_only ? BLOCK_MODE_RO : fOptions.block_mode);
}


std::unique_ptr<HostEthernet> HostPlatform::OpenEthernet(
    const char *driver, const char *ifname,
    const std::vector<EthernetForward> &forwards)
{
    if (strcmp(driver, "user") == 0) {
        return slirp_ethernet_open(fLoop, forwards);
    }
    if (strcmp(driver, "tap") == 0) {
        if (ifname == nullptr) {
            fprintf(stderr, "tap: expecting an 'ifname' property\n");
            return nullptr;
        }
        /* A tap interface puts the guest on the host's own network, where it
           has an address of its own and nothing needs forwarding. */
        if (!forwards.empty()) {
            fprintf(stderr, "tap: 'forward' is for a driver that translates "
                    "addresses\n");
            return nullptr;
        }
        return tap_ethernet_open(fLoop, ifname);
    }
    fprintf(stderr, "Unsupported network driver '%s'\n", driver);
    return nullptr;
}


std::unique_ptr<HostFileSystem> HostPlatform::OpenFileSystem(const char *path)
{
    if (url_backend_matches(path)) {
        return url_fs_open(fLoop, path, fOptions.build_preload_file);
    }
    return disk_fs_open(path);
}


std::unique_ptr<HostAudio> HostPlatform::OpenAudio(
    const AudioSettings &settings)
{
    if (strcmp(settings.driver, "host") == 0) {
        auto audio = host_audio_open(fLoop, settings);
        if (audio != nullptr) {
            return audio;
        }
        /* A configuration is meant to run on any host, so one without an
           audio system still runs the guest, silently. */
        fprintf(stderr, "audio: no host audio, continuing without sound\n");
        return null_audio_open(fLoop, settings);
    }
    if (strcmp(settings.driver, "wav") == 0) {
        return wav_audio_open(fLoop, settings);
    }
    if (strcmp(settings.driver, "none") == 0) {
        return null_audio_open(fLoop, settings);
    }
    fprintf(stderr, "Unsupported audio driver '%s'\n", settings.driver);
    return nullptr;
}
