/*
 * Filesystem abstraction
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

#include <memory>

/* FSQID.type */
#define P9_QTDIR 0x80
#define P9_QTAPPEND 0x40
#define P9_QTEXCL 0x20
#define P9_QTMOUNT 0x10
#define P9_QTAUTH 0x08
#define P9_QTTMP 0x04
#define P9_QTSYMLINK 0x02
#define P9_QTLINK 0x01
#define P9_QTFILE 0x00

/* mode bits */
#define P9_S_IRWXUGO 0x01FF
#define P9_S_ISVTX   0x0200
#define P9_S_ISGID   0x0400
#define P9_S_ISUID   0x0800

#define P9_S_IFMT   0xF000
#define P9_S_IFIFO  0x1000
#define P9_S_IFCHR  0x2000
#define P9_S_IFDIR  0x4000
#define P9_S_IFBLK  0x6000
#define P9_S_IFREG  0x8000
#define P9_S_IFLNK  0xA000
#define P9_S_IFSOCK 0xC000

/* flags for lopen()/lcreate() */
#define P9_O_RDONLY        0x00000000
#define P9_O_WRONLY        0x00000001
#define P9_O_RDWR          0x00000002
#define P9_O_NOACCESS      0x00000003
#define P9_O_CREAT         0x00000040
#define P9_O_EXCL          0x00000080
#define P9_O_NOCTTY        0x00000100
#define P9_O_TRUNC         0x00000200
#define P9_O_APPEND        0x00000400
#define P9_O_NONBLOCK      0x00000800
#define P9_O_DSYNC         0x00001000
#define P9_O_FASYNC        0x00002000
#define P9_O_DIRECT        0x00004000
#define P9_O_LARGEFILE     0x00008000
#define P9_O_DIRECTORY     0x00010000
#define P9_O_NOFOLLOW      0x00020000
#define P9_O_NOATIME       0x00040000
#define P9_O_CLOEXEC       0x00080000
#define P9_O_SYNC          0x00100000

/* for fs_setattr() */
#define P9_SETATTR_MODE      0x00000001
#define P9_SETATTR_UID       0x00000002
#define P9_SETATTR_GID       0x00000004
#define P9_SETATTR_SIZE      0x00000008
#define P9_SETATTR_ATIME     0x00000010
#define P9_SETATTR_MTIME     0x00000020
#define P9_SETATTR_CTIME     0x00000040
#define P9_SETATTR_ATIME_SET 0x00000080
#define P9_SETATTR_MTIME_SET 0x00000100

#define P9_EPERM     1
#define P9_ENOENT    2
#define P9_EIO       5
#define	P9_EEXIST    17
#define	P9_ENOTDIR   20
#define P9_EINVAL    22
#define	P9_ENOSPC    28
#define P9_ENOTEMPTY 39
#define P9_EPROTO    71
#define P9_ENOTSUP   524

class FSDevice;

/* Opaque to callers; fs_disk and fs_net each derive their own file state. */
class FSFile {
public:
    virtual ~FSFile() = default;
};

typedef struct {
    uint32_t f_bsize;
    uint64_t f_blocks;
    uint64_t f_bfree;
    uint64_t f_bavail;
    uint64_t f_files;
    uint64_t f_ffree;
} FSStatFS;

typedef struct {
    uint8_t type; /* P9_IFx */
    uint32_t version;
    uint64_t path;
} FSQID;

typedef struct {
    FSQID qid;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint64_t st_nlink;
    uint64_t st_rdev;
    uint64_t st_size;
    uint64_t st_blksize;
    uint64_t st_blocks;
    uint64_t st_atime_sec;
    uint32_t st_atime_nsec;
    uint64_t st_mtime_sec;
    uint32_t st_mtime_nsec;
    uint64_t st_ctime_sec;
    uint32_t st_ctime_nsec;
} FSStat;

#define P9_LOCK_TYPE_RDLCK 0
#define P9_LOCK_TYPE_WRLCK 1
#define P9_LOCK_TYPE_UNLCK 2

#define P9_LOCK_FLAGS_BLOCK 1
#define P9_LOCK_FLAGS_RECLAIM 2

#define P9_LOCK_SUCCESS 0
#define P9_LOCK_BLOCKED 1
#define P9_LOCK_ERROR   2
#define P9_LOCK_GRACE   3

#define FSCMD_NAME ".fscmd"

typedef struct {
    uint8_t type;
    uint32_t flags;
    uint64_t start;
    uint64_t length;
    uint32_t proc_id;
    char *client_id;
} FSLock;

/* Notified when a deferred open completes. */
class FSOpenCompletion {
public:
    virtual ~FSOpenCompletion() = default;

    virtual void Complete(FSDevice *fs, FSQID *qid, int err) = 0;
};


/* Run once a network filesystem has finished loading its root. */
class StartCallback {
public:
    virtual ~StartCallback() = default;

    virtual void Start() = 0;
};


class FSDevice {
public:
    virtual ~FSDevice() = default;

    virtual void End() = 0;
    virtual void Delete(FSFile *f) = 0;
    virtual void StatFS(FSStatFS *st) = 0;
    virtual int Attach(FSFile **pf, FSQID *qid, uint32_t uid,
                       const char *uname, const char *aname) = 0;
    virtual int Walk(FSFile **pf, FSQID *qids, FSFile *f, int n,
                     char **names) = 0;
    virtual int Mkdir(FSQID *qid, FSFile *f, const char *name, uint32_t mode,
                      uint32_t gid) = 0;
    virtual int Open(FSQID *qid, FSFile *f, uint32_t flags,
                     FSOpenCompletion *completion) = 0;
    virtual int Create(FSQID *qid, FSFile *f, const char *name,
                       uint32_t flags, uint32_t mode, uint32_t gid) = 0;
    virtual int Stat(FSFile *f, FSStat *st) = 0;
    virtual int SetAttr(FSFile *f, uint32_t mask,
                        uint32_t mode, uint32_t uid, uint32_t gid,
                        uint64_t size, uint64_t atime_sec, uint64_t atime_nsec,
                        uint64_t mtime_sec, uint64_t mtime_nsec) = 0;
    virtual void Close(FSFile *f) = 0;
    virtual int ReadDir(FSFile *f, uint64_t offset, uint8_t *buf,
                        int count) = 0;
    virtual int Read(FSFile *f, uint64_t offset, uint8_t *buf, int count) = 0;
    virtual int Write(FSFile *f, uint64_t offset, const uint8_t *buf,
                      int count) = 0;
    virtual int Link(FSFile *df, FSFile *f, const char *name) = 0;
    virtual int Symlink(FSQID *qid, FSFile *f, const char *name,
                        const char *symgt, uint32_t gid) = 0;
    virtual int Mknod(FSQID *qid, FSFile *f, const char *name, uint32_t mode,
                      uint32_t major, uint32_t minor, uint32_t gid) = 0;
    virtual int ReadLink(char *buf, int buf_size, FSFile *f) = 0;
    virtual int RenameAt(FSFile *f, const char *name, FSFile *new_f,
                         const char *new_name) = 0;
    virtual int UnlinkAt(FSFile *f, const char *name) = 0;
    virtual int Lock(FSFile *f, const FSLock *lock) = 0;
    virtual int GetLock(FSFile *f, FSLock *lock) = 0;

    /* Replaces the upstream identity test that compared function pointers. */
    virtual bool IsNet() const {return false;}
};

std::unique_ptr<FSDevice> fs_disk_init(const char *root_path);
FSDevice *fs_mem_init(void);
FSDevice *fs_net_init(const char *url, StartCallback *start);
void fs_net_set_pwd(FSDevice *fs, const char *pwd);
void fs_export_file(const char *filename,
                    const uint8_t *buf, int buf_len);
void fs_end(FSDevice *fs);
void fs_dump_cache_load(FSDevice *fs1, const char *filename);

FSFile *fs_dup(FSDevice *fs, FSFile *f);
FSFile *fs_walk_path1(FSDevice *fs, FSFile *f, const char *path,
                      char **pname);
FSFile *fs_walk_path(FSDevice *fs, FSFile *f, const char *path);
