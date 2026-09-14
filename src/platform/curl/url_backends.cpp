/*
 * Back ends reached through a URL
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
#include "event_loop.h"
#include "fs_net.h"
#include "fs_utils.h"
#include "fs_wget.h"
#include "platform_backends.h"

/* in KB */
#define BLOCK_CACHE_SIZE (128 * 1024)


void url_backend_init(EventLoop &loop)
{
    fs_wget_init(loop);
}


bool url_backend_matches(const char *path)
{
    return is_url(path);
}


class UrlStartCallback final: public StartCallback {
public:
    bool started = false;

    void Start() override {started = true;}
};


std::unique_ptr<HostBlockDevice> url_block_open(EventLoop &loop,
                                                const char *url)
{
    UrlStartCallback start;

    std::unique_ptr<HostBlockDevice> bs(
        block_device_init_http(url, BLOCK_CACHE_SIZE, &start));
    /* wait until the drive is initialized */
    loop.RunUntil([&start]() {return start.started;});
    return bs;
}


std::unique_ptr<HostFileSystem> url_fs_open(EventLoop &loop, const char *url,
                                            const char *preload_file)
{
    std::unique_ptr<HostFileSystem> fs(fs_net_init(url, nullptr));

    if (fs == nullptr) {
        return nullptr;
    }
    if (preload_file != nullptr) {
        fs_dump_cache_load(fs.get(), preload_file);
    }
    loop.RunUntilIdle();
    return fs;
}
