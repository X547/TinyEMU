/*
 * Back ends reached through a URL, when the build leaves them out
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
#include "platform_backends.h"


void url_backend_init(EventLoop &loop)
{
    (void)loop;
}


/* a URL is then just a file name that does not exist */
bool url_backend_matches(const char *path)
{
    (void)path;
    return false;
}


std::unique_ptr<HostBlockDevice> url_block_open(EventLoop &loop,
                                                const char *url)
{
    (void)loop;
    (void)url;
    return nullptr;
}


std::unique_ptr<HostFileSystem> url_fs_open(EventLoop &loop, const char *url,
                                            const char *preload_file)
{
    (void)loop;
    (void)url;
    (void)preload_file;
    return nullptr;
}
