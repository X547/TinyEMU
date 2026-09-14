/*
 * Host file system: a host directory, Haiku parts
 *
 * Copyright (c) 2016 Fabrice Bellard
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
#include "fs_disk_os.h"

#include <errno.h>

#include "host_fs.h"


/* directory entries carry no type */
void fs_disk_dirent_type(const char *dir_path, struct dirent *de,
                         int *qid_type, int *d_type)
{
    (void)dir_path;
    (void)de;
    *qid_type = P9_QTFILE;
    *d_type = 0;
}


int fs_disk_mknod(const char *path, uint32_t mode, uint32_t major,
                  uint32_t minor)
{
    (void)path;
    (void)mode;
    (void)major;
    (void)minor;
    return ENOTSUP;
}
