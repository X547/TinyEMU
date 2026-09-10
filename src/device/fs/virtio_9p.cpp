/*
 * VIRTIO driver
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
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <assert.h>
#include <stdarg.h>

#include "cutils.h"
#include "list.h"
#include "fs.h"
#include "virtio.h"
#include "virtio_priv.h"


typedef struct {
    struct list_head link;
    uint32_t fid;
    FSFile *fd;
} FIDDesc;

struct VIRTIO9PDevice: public VIRTIODevice {
    FSDevice *fs = nullptr;
    int msize = 0; /* maximum message size */
    struct list_head fid_list {}; /* list of FIDDesc */
    bool req_in_progress = false;

    int RecvRequest(int queue_idx, int desc_idx, int read_size,
                    int write_size) override;
};

static FIDDesc *fid_find1(VIRTIO9PDevice *s, uint32_t fid)
{
    struct list_head *el;
    FIDDesc *f;

    list_for_each(el, &s->fid_list) {
        f = list_entry(el, FIDDesc, link);
        if (f->fid == fid)
            return f;
    }
    return NULL;
}

static FSFile *fid_find(VIRTIO9PDevice *s, uint32_t fid)
{
    FIDDesc *f;

    f = fid_find1(s, fid);
    if (!f)
        return NULL;
    return f->fd;
}

static void fid_delete(VIRTIO9PDevice *s, uint32_t fid)
{
    FIDDesc *f;

    f = fid_find1(s, fid);
    if (f) {
        s->fs->Delete(f->fd);
        list_del(&f->link);
        free(f);
    }
}

static void fid_set(VIRTIO9PDevice *s, uint32_t fid, FSFile *fd)
{
    FIDDesc *f;

    f = fid_find1(s, fid);
    if (f) {
        s->fs->Delete(f->fd);
        f->fd = fd;
    } else {
        f = static_cast<FIDDesc *>(malloc(sizeof(*f)));
        f->fid = fid;
        f->fd = fd;
        list_add(&f->link, &s->fid_list);
    }
}

#ifdef DEBUG_VIRTIO

typedef struct {
    uint8_t tag;
    const char *name;
} Virtio9POPName;

static const Virtio9POPName virtio_9p_op_names[] = {
    { 8, "statfs" },
    { 12, "lopen" },
    { 14, "lcreate" },
    { 16, "symlink" },
    { 18, "mknod" },
    { 22, "readlink" },
    { 24, "getattr" },
    { 26, "setattr" },
    { 30, "xattrwalk" },
    { 40, "readdir" },
    { 50, "fsync" },
    { 52, "lock" },
    { 54, "getlock" },
    { 70, "link" },
    { 72, "mkdir" },
    { 74, "renameat" },
    { 76, "unlinkat" },
    { 100, "version" },
    { 104, "attach" },
    { 108, "flush" },
    { 110, "walk" },
    { 116, "read" },
    { 118, "write" },
    { 120, "clunk" },
    { 0, NULL },
};

static const char *get_9p_op_name(int tag)
{
    const Virtio9POPName *p;
    for(p = virtio_9p_op_names; p->name != NULL; p++) {
        if (p->tag == tag)
            return p->name;
    }
    return NULL;
}

#endif /* DEBUG_VIRTIO */

static int marshall(VIRTIO9PDevice *s, 
                    uint8_t *buf1, int max_len, const char *fmt, ...)
{
    va_list ap;
    int c;
    uint32_t val;
    uint64_t val64;
    uint8_t *buf, *buf_end;

#ifdef DEBUG_VIRTIO
    if (s->debug & VIRTIO_DEBUG_9P)
        printf(" ->");
#endif
    va_start(ap, fmt);
    buf = buf1;
    buf_end = buf1 + max_len;
    for(;;) {
        c = *fmt++;
        if (c == '\0')
            break;
        switch(c) {
        case 'b':
            assert(buf + 1 <= buf_end);
            val = va_arg(ap, int);
#ifdef DEBUG_VIRTIO
            if (s->debug & VIRTIO_DEBUG_9P)
                printf(" b=%d", val);
#endif
            buf[0] = val;
            buf += 1;
            break;
        case 'h':
            assert(buf + 2 <= buf_end);
            val = va_arg(ap, int);
#ifdef DEBUG_VIRTIO
            if (s->debug & VIRTIO_DEBUG_9P)
                printf(" h=%d", val);
#endif
            put_le16(buf, val);
            buf += 2;
            break;
        case 'w':
            assert(buf + 4 <= buf_end);
            val = va_arg(ap, int);
#ifdef DEBUG_VIRTIO
            if (s->debug & VIRTIO_DEBUG_9P)
                printf(" w=%d", val);
#endif
            put_le32(buf, val);
            buf += 4;
            break;
        case 'd':
            assert(buf + 8 <= buf_end);
            val64 = va_arg(ap, uint64_t);
#ifdef DEBUG_VIRTIO
            if (s->debug & VIRTIO_DEBUG_9P)
                printf(" d=%" PRId64, val64);
#endif
            put_le64(buf, val64);
            buf += 8;
            break;
        case 's':
            {
                char *str;
                int len;
                str = va_arg(ap, char *);
#ifdef DEBUG_VIRTIO
                if (s->debug & VIRTIO_DEBUG_9P)
                    printf(" s=\"%s\"", str);
#endif
                len = strlen(str);
                assert(len <= 65535);
                assert(buf + 2 + len <= buf_end);
                put_le16(buf, len);
                buf += 2;
                memcpy(buf, str, len);
                buf += len;
            }
            break;
        case 'Q':
            {
                FSQID *qid;
                assert(buf + 13 <= buf_end);
                qid = va_arg(ap, FSQID *);
#ifdef DEBUG_VIRTIO
                if (s->debug & VIRTIO_DEBUG_9P)
                    printf(" Q=%d:%d:%" PRIu64, qid->type, qid->version, qid->path);
#endif
                buf[0] = qid->type;
                put_le32(buf + 1, qid->version);
                put_le64(buf + 5, qid->path);
                buf += 13;
            }
            break;
        default:
            abort();
        }
    }
    va_end(ap);
    return buf - buf1;
}

/* return < 0 if error */
/* XXX: free allocated strings in case of error */
static int unmarshall(VIRTIO9PDevice *s, int queue_idx,
                      int desc_idx, int *poffset, const char *fmt, ...)
{
    VIRTIODevice *s1 = (VIRTIODevice *)s;
    va_list ap;
    int offset, c;
    uint8_t buf[16];

    offset = *poffset;
    va_start(ap, fmt);
    for(;;) {
        c = *fmt++;
        if (c == '\0')
            break;
        switch(c) {
        case 'b':
            {
                uint8_t *ptr;
                if (memcpy_from_queue(s1, buf, queue_idx, desc_idx, offset, 1))
                    return -1;
                ptr = va_arg(ap, uint8_t *);
                *ptr = buf[0];
                offset += 1;
#ifdef DEBUG_VIRTIO
                if (s->debug & VIRTIO_DEBUG_9P)
                    printf(" b=%d", *ptr);
#endif
            }
            break;
        case 'h':
            {
                uint16_t *ptr;
                if (memcpy_from_queue(s1, buf, queue_idx, desc_idx, offset, 2))
                    return -1;
                ptr = va_arg(ap, uint16_t *);
                *ptr = get_le16(buf);
                offset += 2;
#ifdef DEBUG_VIRTIO
                if (s->debug & VIRTIO_DEBUG_9P)
                    printf(" h=%d", *ptr);
#endif
            }
            break;
        case 'w':
            {
                uint32_t *ptr;
                if (memcpy_from_queue(s1, buf, queue_idx, desc_idx, offset, 4))
                    return -1;
                ptr = va_arg(ap, uint32_t *);
                *ptr = get_le32(buf);
                offset += 4;
#ifdef DEBUG_VIRTIO
                if (s->debug & VIRTIO_DEBUG_9P)
                    printf(" w=%d", *ptr);
#endif
            }
            break;
        case 'd':
            {
                uint64_t *ptr;
                if (memcpy_from_queue(s1, buf, queue_idx, desc_idx, offset, 8))
                    return -1;
                ptr = va_arg(ap, uint64_t *);
                *ptr = get_le64(buf);
                offset += 8;
#ifdef DEBUG_VIRTIO
                if (s->debug & VIRTIO_DEBUG_9P)
                    printf(" d=%" PRId64, *ptr);
#endif
            }
            break;
        case 's':
            {
                char *str, **ptr;
                int len;

                if (memcpy_from_queue(s1, buf, queue_idx, desc_idx, offset, 2))
                    return -1;
                len = get_le16(buf);
                offset += 2;
                str = static_cast<char *>(malloc(len + 1));
                if (memcpy_from_queue(s1, str, queue_idx, desc_idx, offset, len))
                    return -1;
                str[len] = '\0';
                offset += len;
                ptr = va_arg(ap, char **);
                *ptr = str;
#ifdef DEBUG_VIRTIO
                if (s->debug & VIRTIO_DEBUG_9P)
                    printf(" s=\"%s\"", *ptr);
#endif
            }
            break;
        default:
            abort();
        }
    }
    va_end(ap);
    *poffset = offset;
    return 0;
}

static void virtio_9p_send_reply(VIRTIO9PDevice *s, int queue_idx,
                                 int desc_idx, uint8_t id, uint16_t tag, 
                                 uint8_t *buf, int buf_len)
{
    uint8_t *buf1;
    int len;

#ifdef DEBUG_VIRTIO
    if (s->debug & VIRTIO_DEBUG_9P) {
        if (id == 6)
            printf(" (error)");
        printf("\n");
    }
#endif
    len = buf_len + 7;
    buf1 = static_cast<uint8_t *>(malloc(len));
    put_le32(buf1, len);
    buf1[4] = id + 1;
    put_le16(buf1 + 5, tag);
    memcpy(buf1 + 7, buf, buf_len);
    memcpy_to_queue((VIRTIODevice *)s, queue_idx, desc_idx, 0, buf1, len);
    virtio_consume_desc((VIRTIODevice *)s, queue_idx, desc_idx, len);
    free(buf1);
}

static void virtio_9p_send_error(VIRTIO9PDevice *s, int queue_idx,
                                 int desc_idx, uint16_t tag, uint32_t error)
{
    uint8_t buf[4];
    int buf_len;

    buf_len = marshall(s, buf, sizeof(buf), "w", -error);
    virtio_9p_send_reply(s, queue_idx, desc_idx, 6, tag, buf, buf_len);
}

/* One per in-flight lopen: it carries the reply context and is itself the
   completion the filesystem calls back into. */
struct P9OpenInfo: public FSOpenCompletion {
    VIRTIO9PDevice *dev;
    int queue_idx;
    int desc_idx;
    uint16_t tag;

    void Complete(FSDevice *fs, FSQID *qid, int err) override;
};

static void virtio_9p_open_reply(FSDevice *fs, FSQID *qid, int err,
                                 P9OpenInfo *oi)
{
    VIRTIO9PDevice *s = oi->dev;
    uint8_t buf[32];
    int buf_len;
    
    if (err < 0) {
        virtio_9p_send_error(s, oi->queue_idx, oi->desc_idx, oi->tag, err);
    } else {
        buf_len = marshall(s, buf, sizeof(buf),
                           "Qw", qid, s->msize - 24);
        virtio_9p_send_reply(s, oi->queue_idx, oi->desc_idx, 12, oi->tag,
                             buf, buf_len);
    }
    delete oi;
}

void P9OpenInfo::Complete(FSDevice *fs, FSQID *qid, int err)
{
    P9OpenInfo *oi = this;
    VIRTIO9PDevice *s = oi->dev;
    int queue_idx = oi->queue_idx;
    
    virtio_9p_open_reply(fs, qid, err, oi);

    s->req_in_progress = false;

    /* handle next requests */
    queue_notify((VIRTIODevice *)s, queue_idx);
}

int VIRTIO9PDevice::RecvRequest(int queue_idx, int desc_idx, int read_size,
                                int write_size)
{
    VIRTIODevice *s1 = this;
    VIRTIO9PDevice *s = (VIRTIO9PDevice *)s1;
    int offset, header_len;
    uint8_t id;
    uint16_t tag;
    uint8_t buf[1024];
    int buf_len, err;
    FSDevice *fs = s->fs;

    if (queue_idx != 0)
        return 0;
    
    if (s->req_in_progress)
        return -1;
    
    offset = 0;
    header_len = 4 + 1 + 2;
    if (memcpy_from_queue(s1, buf, queue_idx, desc_idx, offset, header_len)) {
        tag = 0;
        goto protocol_error;
    }
    //size = get_le32(buf);
    id = buf[4];
    tag = get_le16(buf + 5);
    offset += header_len;
    
#ifdef DEBUG_VIRTIO
    if (s1->debug & VIRTIO_DEBUG_9P) {
        const char *name;
        name = get_9p_op_name(id);
        printf("9p: op=");
        if (name)
            printf("%s", name);
        else
            printf("%d", id);
    }
#endif
    /* Note: same subset as JOR1K */
    switch(id) {
    case 8: /* statfs */
        {
            FSStatFS st;

            fs->StatFS(&st);
            buf_len = marshall(s, buf, sizeof(buf),
                               "wwddddddw", 
                               0,
                               st.f_bsize,
                               st.f_blocks,
                               st.f_bfree,
                               st.f_bavail,
                               st.f_files,
                               st.f_ffree,
                               0, /* id */
                               256 /* max filename length */
                               );
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 12: /* lopen */
        {
            uint32_t fid, flags;
            FSFile *f;
            FSQID qid;
            P9OpenInfo *oi;
            
            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "ww", &fid, &flags))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            oi = new P9OpenInfo();
            oi->dev = s;
            oi->queue_idx = queue_idx;
            oi->desc_idx = desc_idx;
            oi->tag = tag;
            err = fs->Open(&qid, f, flags, oi);
            if (err <= 0) {
                virtio_9p_open_reply(fs, &qid, err, oi);
            } else {
                s->req_in_progress = true;
            }
        }
        break;
    case 14: /* lcreate */
        {
            uint32_t fid, flags, mode, gid;
            char *name;
            FSFile *f;
            FSQID qid;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wswww", &fid, &name, &flags, &mode, &gid))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f) {
                err = -P9_EPROTO;
            } else {
                err = fs->Create(&qid, f, name, flags, mode, gid);
            }
            free(name);
            if (err) 
                goto error;
            buf_len = marshall(s, buf, sizeof(buf),
                               "Qw", &qid, s->msize - 24);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 16: /* symlink */
        {
            uint32_t fid, gid;
            char *name, *symgt;
            FSFile *f;
            FSQID qid;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wssw", &fid, &name, &symgt, &gid))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f) {
                err = -P9_EPROTO;
            } else {
                err = fs->Symlink(&qid, f, name, symgt, gid);
            }
            free(name);
            free(symgt);
            if (err)
                goto error;
            buf_len = marshall(s, buf, sizeof(buf),
                               "Q", &qid);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 18: /* mknod */
        {
            uint32_t fid, mode, major, minor, gid;
            char *name;
            FSFile *f;
            FSQID qid;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wswwww", &fid, &name, &mode, &major, &minor, &gid))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f) {
                err = -P9_EPROTO;
            } else {
                err = fs->Mknod(&qid, f, name, mode, major, minor, gid);
            }
            free(name);
            if (err)
                goto error;
            buf_len = marshall(s, buf, sizeof(buf),
                               "Q", &qid);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 22: /* readlink */
        {
            uint32_t fid;
            char buf1[1024];
            FSFile *f;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "w", &fid))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f) {
                err = -P9_EPROTO;
            } else {
                err = fs->ReadLink(buf1, sizeof(buf1), f);
            }
            if (err)
                goto error;
            buf_len = marshall(s, buf, sizeof(buf), "s", buf1);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 24: /* getattr */
        {
            uint32_t fid;
            uint64_t mask;
            FSFile *f;
            FSStat st;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wd", &fid, &mask))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            err = fs->Stat(f, &st);
            if (err)
                goto error;

            buf_len = marshall(s, buf, sizeof(buf),
                               "dQwwwddddddddddddddd", 
                               mask, &st.qid,
                               st.st_mode, st.st_uid, st.st_gid,
                               st.st_nlink, st.st_rdev, st.st_size,
                               st.st_blksize, st.st_blocks,
                               st.st_atime_sec, (uint64_t)st.st_atime_nsec,
                               st.st_mtime_sec, (uint64_t)st.st_mtime_nsec,
                               st.st_ctime_sec, (uint64_t)st.st_ctime_nsec,
                               (uint64_t)0, (uint64_t)0,
                               (uint64_t)0, (uint64_t)0);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 26: /* setattr */
        {
            uint32_t fid, mask, mode, uid, gid;
            uint64_t size, atime_sec, atime_nsec, mtime_sec, mtime_nsec;
            FSFile *f;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wwwwwddddd", &fid, &mask, &mode, &uid, &gid,
                           &size, &atime_sec, &atime_nsec, 
                           &mtime_sec, &mtime_nsec))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            err = fs->SetAttr(f, mask, mode, uid, gid, size, atime_sec,
                                 atime_nsec, mtime_sec, mtime_nsec);
            if (err)
                goto error;
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, NULL, 0);
        }
        break;
    case 30: /* xattrwalk */
        {
            /* not supported yet */
            err = -P9_ENOTSUP;
            goto error;
        }
        break;
    case 40: /* readdir */
        {
            uint32_t fid, count;
            uint64_t offs;
            uint8_t *buf;
            int n;
            FSFile *f;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wdw", &fid, &offs, &count))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            buf = static_cast<uint8_t *>(malloc(count + 4));
            n = fs->ReadDir(f, offs, buf + 4, count);
            if (n < 0) {
                err = n;
                goto error;
            }
            put_le32(buf, n);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, n + 4);
            free(buf);
        }
        break;
    case 50: /* fsync */
        {
            uint32_t fid;
            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "w", &fid))
                goto protocol_error;
            /* ignored */
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, NULL, 0);
        }
        break;
    case 52: /* lock */
        {
            uint32_t fid;
            FSFile *f;
            FSLock lock;
            
            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wbwddws", &fid, &lock.type, &lock.flags,
                           &lock.start, &lock.length,
                           &lock.proc_id, &lock.client_id))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                err = -P9_EPROTO;
            else
                err = fs->Lock(f, &lock);
            free(lock.client_id);
            if (err < 0)
                goto error;
            buf_len = marshall(s, buf, sizeof(buf), "b", err);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 54: /* getlock */
        {
            uint32_t fid;
            FSFile *f;
            FSLock lock;
            
            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wbddws", &fid, &lock.type,
                           &lock.start, &lock.length,
                           &lock.proc_id, &lock.client_id))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                err = -P9_EPROTO;
            else
                err = fs->GetLock(f, &lock);
            if (err < 0) {
                free(lock.client_id);
                goto error;
            }
            buf_len = marshall(s, buf, sizeof(buf), "bddws",
                               &lock.type,
                               &lock.start, &lock.length,
                               &lock.proc_id, &lock.client_id);
            free(lock.client_id);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 70: /* link */
        {
            uint32_t dfid, fid;
            char *name;
            FSFile *f, *df;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wws", &dfid, &fid, &name))
                goto protocol_error;
            df = fid_find(s, dfid);
            f = fid_find(s, fid);
            if (!df || !f) {
                err = -P9_EPROTO;
            } else {
                err = fs->Link(df, f, name);
            }
            free(name);
            if (err)
                goto error;
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, NULL, 0);
        }
        break;
    case 72: /* mkdir */
        {
            uint32_t fid, mode, gid;
            char *name;
            FSFile *f;
            FSQID qid;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wsww", &fid, &name, &mode, &gid))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            err = fs->Mkdir(&qid, f, name, mode, gid);
            if (err != 0)
                goto error;
            buf_len = marshall(s, buf, sizeof(buf), "Q", &qid);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 74: /* renameat */
        {
            uint32_t fid, new_fid;
            char *name, *new_name;
            FSFile *f, *new_f;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wsws", &fid, &name, &new_fid, &new_name))
                goto protocol_error;
            f = fid_find(s, fid);
            new_f = fid_find(s, new_fid);
            if (!f || !new_f) {
                err = -P9_EPROTO;
            } else {
                err = fs->RenameAt(f, name, new_f, new_name);
            }
            free(name);
            free(new_name);
            if (err != 0)
                goto error;
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, NULL, 0);
        }
        break;
    case 76: /* unlinkat */
        {
            uint32_t fid, flags;
            char *name;
            FSFile *f;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wsw", &fid, &name, &flags))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f) {
                err = -P9_EPROTO;
            } else {
                err = fs->UnlinkAt(f, name);
            }
            free(name);
            if (err != 0)
                goto error;
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, NULL, 0);
        }
        break;
    case 100: /* version */
        {
            uint32_t msize;
            char *version;
            if (unmarshall(s, queue_idx, desc_idx, &offset, 
                           "ws", &msize, &version))
                goto protocol_error;
            s->msize = msize;
            //            printf("version: msize=%d version=%s\n", msize, version);
            free(version);
            buf_len = marshall(s, buf, sizeof(buf), "ws", s->msize, "9P2000.L");
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 104: /* attach */
        {
            uint32_t fid, afid, uid;
            char *uname, *aname;
            FSQID qid;
            FSFile *f;
            
            if (unmarshall(s, queue_idx, desc_idx, &offset, 
                           "wwssw", &fid, &afid, &uname, &aname, &uid))
                goto protocol_error;
            err = fs->Attach(&f, &qid, uid, uname, aname);
            if (err != 0)
                goto error;
            fid_set(s, fid, f);
            free(uname);
            free(aname);
            buf_len = marshall(s, buf, sizeof(buf), "Q", &qid);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 108: /* flush */
        {
            uint16_t oldtag;
            if (unmarshall(s, queue_idx, desc_idx, &offset, 
                           "h", &oldtag))
                goto protocol_error;
            /* ignored */
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, NULL, 0);
        }
        break;
    case 110: /* walk */
        {
            uint32_t fid, newfid;
            uint16_t nwname;
            FSQID *qids;
            char **names;
            FSFile *f;
            int i;

            if (unmarshall(s, queue_idx, desc_idx, &offset, 
                           "wwh", &fid, &newfid, &nwname))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            names = static_cast<char **>(mallocz(sizeof(names[0]) * nwname));
            qids = static_cast<FSQID *>(malloc(sizeof(qids[0]) * nwname));
            for(i = 0; i < nwname; i++) {
                if (unmarshall(s, queue_idx, desc_idx, &offset, 
                               "s", &names[i])) {
                    err = -P9_EPROTO;
                    goto walk_done;
                }
            }
            err = fs->Walk(&f, qids, f, nwname, names);
        walk_done:
            for(i = 0; i < nwname; i++) {
                free(names[i]);
            }
            free(names);
            if (err < 0) {
                free(qids);
                goto error;
            }
            buf_len = marshall(s, buf, sizeof(buf), "h", err);
            for(i = 0; i < err; i++) {
                buf_len += marshall(s, buf + buf_len, sizeof(buf) - buf_len,
                                    "Q", &qids[i]);
            }
            free(qids);
            fid_set(s, newfid, f);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 116: /* read */
        {
            uint32_t fid, count;
            uint64_t offs;
            uint8_t *buf;
            int n;
            FSFile *f;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wdw", &fid, &offs, &count))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            buf = static_cast<uint8_t *>(malloc(count + 4));
            n = fs->Read(f, offs, buf + 4, count);
            if (n < 0) {
                err = n;
                free(buf);
                goto error;
            }
            put_le32(buf, n);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, n + 4);
            free(buf);
        }
        break;
    case 118: /* write */
        {
            uint32_t fid, count;
            uint64_t offs;
            uint8_t *buf1;
            int n;
            FSFile *f;

            if (unmarshall(s, queue_idx, desc_idx, &offset,
                           "wdw", &fid, &offs, &count))
                goto protocol_error;
            f = fid_find(s, fid);
            if (!f)
                goto fid_not_found;
            buf1 = static_cast<uint8_t *>(malloc(count));
            if (memcpy_from_queue(s1, buf1, queue_idx, desc_idx, offset,
                                  count)) {
                free(buf1);
                goto protocol_error;
            }
            n = fs->Write(f, offs, buf1, count);
            free(buf1);
            if (n < 0) {
                err = n;
                goto error;
            }
            buf_len = marshall(s, buf, sizeof(buf), "w", n);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, buf, buf_len);
        }
        break;
    case 120: /* clunk */
        {
            uint32_t fid;
            
            if (unmarshall(s, queue_idx, desc_idx, &offset, 
                           "w", &fid))
                goto protocol_error;
            fid_delete(s, fid);
            virtio_9p_send_reply(s, queue_idx, desc_idx, id, tag, NULL, 0);
        }
        break;
    default:
        printf("9p: unsupported operation id=%d\n", id);
        goto protocol_error;
    }
    return 0;
 error:
    virtio_9p_send_error(s, queue_idx, desc_idx, tag, err);
    return 0;
 protocol_error:
 fid_not_found:
    err = -P9_EPROTO;
    goto error;
}

VIRTIODevice *virtio_9p_init(VIRTIOBusDef *bus, FSDevice *fs,
                             const char *mount_tag)

{
    VIRTIO9PDevice *s;
    int len;
    uint8_t *cfg;

    len = strlen(mount_tag);
    s = new VIRTIO9PDevice();
    virtio_init(s, bus, 9, 2 + len);
    s->device_features = 1 << 0;

    /* set the mount tag */
    cfg = s->config_space;
    cfg[0] = len;
    cfg[1] = len >> 8;
    memcpy(cfg + 2, mount_tag, len);

    s->fs = fs;
    s->msize = 8192;
    init_list_head(&s->fid_list);
    
    return s;
}
