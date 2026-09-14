/*
 * Network file system
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
#pragma once

#include "host_fs.h"
#include "machine.h"

HostFileSystem *fs_mem_init(void);
HostFileSystem *fs_net_init(const char *url, StartCallback *start);
void fs_net_set_pwd(HostFileSystem *fs, const char *pwd);
void fs_export_file(const char *filename,
                    const uint8_t *buf, int buf_len);
void fs_end(HostFileSystem *fs);
void fs_dump_cache_load(HostFileSystem *fs1, const char *filename);

FSFile *fs_dup(HostFileSystem *fs, FSFile *f);
FSFile *fs_walk_path1(HostFileSystem *fs, FSFile *f, const char *path,
                      char **pname);
FSFile *fs_walk_path(HostFileSystem *fs, FSFile *f, const char *path);
