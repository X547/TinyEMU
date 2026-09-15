/*
 * Host block storage: an image file
 *
 * Copyright (c) 2016-2018 Fabrice Bellard
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
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <inttypes.h>
#include <vector>

#include "platform_backends.h"

#define SECTOR_SIZE 512

struct FileCloser {
    void operator()(FILE *f) const {fclose(f);}
};

class BlockDeviceFile final: public HostBlockDevice {
public:
    std::unique_ptr<FILE, FileCloser> f;
    int64_t nb_sectors = 0;
    BlockModeEnum mode {};
    /* snapshot mode: the sectors written so far, null where unchanged */
    std::vector<std::unique_ptr<uint8_t[]>> sector_table;

    int64_t SectorCount() override {return nb_sectors;}
    int ReadAsync(uint64_t sector_num, uint8_t *buf, int n,
                  BlockCompletion *completion) override;
    int WriteAsync(uint64_t sector_num, const uint8_t *buf, int n,
                   BlockCompletion *completion) override;
};

//#define DUMP_BLOCK_READ

int BlockDeviceFile::ReadAsync(uint64_t sector_num, uint8_t *buf, int n,
                               BlockCompletion *completion)
{
    (void)completion;
    BlockDeviceFile *bf = this;
    //    printf("bf_read_async: sector_num=%" PRId64 " n=%d\n", sector_num, n);
#ifdef DUMP_BLOCK_READ
    {
        static FILE *f;
        if (!f)
            f = fopen("/tmp/read_sect.txt", "wb");
        fprintf(f, "%" PRId64 " %d\n", sector_num, n);
    }
#endif
    if (!bf->f)
        return -1;
    /* a guest probing past the last sector gets an error */
    if (sector_num + n > (uint64_t)bf->nb_sectors)
        return -1;
    if (bf->mode == BLOCK_MODE_SNAPSHOT) {
        int i;
        for(i = 0; i < n; i++) {
            if (!bf->sector_table[sector_num]) {
                fseek(bf->f.get(), sector_num * SECTOR_SIZE, SEEK_SET);
                if (fread(buf, 1, SECTOR_SIZE, bf->f.get()) != SECTOR_SIZE)
                    memset(buf, 0, SECTOR_SIZE);
            } else {
                memcpy(buf, bf->sector_table[sector_num].get(), SECTOR_SIZE);
            }
            sector_num++;
            buf += SECTOR_SIZE;
        }
    } else {
        size_t len = (size_t)n * SECTOR_SIZE;
        size_t got;
        fseek(bf->f.get(), sector_num * SECTOR_SIZE, SEEK_SET);
        got = fread(buf, 1, len, bf->f.get());
        if (got < len)
            memset(buf + got, 0, len - got);
    }
    /* synchronous read */
    return 0;
}

int BlockDeviceFile::WriteAsync(uint64_t sector_num, const uint8_t *buf, int n,
                                BlockCompletion *completion)
{
    (void)completion;
    BlockDeviceFile *bf = this;
    int ret;

    switch(bf->mode) {
    case BLOCK_MODE_RO:
        ret = -1; /* error */
        break;
    case BLOCK_MODE_RW:
        fseek(bf->f.get(), sector_num * SECTOR_SIZE, SEEK_SET);
        fwrite(buf, 1, n * SECTOR_SIZE, bf->f.get());
        ret = 0;
        break;
    case BLOCK_MODE_SNAPSHOT:
        {
            int i;
            if ((sector_num + n) > bf->nb_sectors)
                return -1;
            for(i = 0; i < n; i++) {
                if (!bf->sector_table[sector_num]) {
                    bf->sector_table[sector_num].reset(
                        new uint8_t[SECTOR_SIZE]);
                }
                memcpy(bf->sector_table[sector_num].get(), buf, SECTOR_SIZE);
                sector_num++;
                buf += SECTOR_SIZE;
            }
            ret = 0;
        }
        break;
    default:
        abort();
    }

    return ret;
}

std::unique_ptr<HostBlockDevice> file_block_open(const char *filename,
                                                 BlockModeEnum mode)
{
    int64_t file_size;
    FILE *f;
    const char *mode_str;

    if (mode == BLOCK_MODE_RW) {
        mode_str = "r+b";
    } else {
        mode_str = "rb";
    }

    f = fopen(filename, mode_str);
    if (!f) {
        perror(filename);
        return nullptr;
    }
    fseek(f, 0, SEEK_END);
    file_size = ftello(f);

    auto bf = std::make_unique<BlockDeviceFile>();

    bf->mode = mode;
    bf->nb_sectors = file_size / 512;
    bf->f.reset(f);

    if (mode == BLOCK_MODE_SNAPSHOT) {
        bf->sector_table.resize(bf->nb_sectors);
    }

    return bf;
}
