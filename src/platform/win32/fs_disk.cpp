/*
 * Host file system: a host directory, stored the way WSL stores Linux files
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

#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <windows.h>
#include <winioctl.h>
#include <winternl.h>

#include "cutils.h"
#include "platform_backends.h"

/* Reparse tags of the files WSL keeps for Linux; the system headers have
   IO_REPARSE_TAG_AF_UNIX. */
#define IO_REPARSE_TAG_LX_SYMLINK 0xA000001D
#define IO_REPARSE_TAG_LX_FIFO    0x80000024
#define IO_REPARSE_TAG_LX_CHR     0x80000025
#define IO_REPARSE_TAG_LX_BLK     0x80000026

/* Windows 10 classes the headers hide when building for older systems;
   where they are missing, the calls fail and the older class does. */
#define FILE_DISPOSITION_INFO_EX_CLASS ((FILE_INFO_BY_HANDLE_CLASS)21)
#define FILE_RENAME_INFO_EX_CLASS      ((FILE_INFO_BY_HANDLE_CLASS)22)
#define FILE_CASE_SENSITIVE_INFO_CLASS ((FILE_INFO_BY_HANDLE_CLASS)23)

#ifndef FILE_RENAME_FLAG_IGNORE_READONLY_ATTRIBUTE
#define FILE_RENAME_FLAG_IGNORE_READONLY_ATTRIBUTE 0x00000040
#endif

extern "C" {
NTSTATUS NTAPI NtQueryEaFile(HANDLE file, PIO_STATUS_BLOCK io, PVOID buffer,
                             ULONG length, BOOLEAN single, PVOID list,
                             ULONG list_length, PULONG index, BOOLEAN restart);
NTSTATUS NTAPI NtSetEaFile(HANDLE file, PIO_STATUS_BLOCK io, PVOID buffer,
                           ULONG length);
}

/* d_type of a directory entry */
#define DT_UNKNOWN 0
#define DT_FIFO    1
#define DT_CHR     2
#define DT_DIR     4
#define DT_BLK     6
#define DT_REG     8
#define DT_LNK     10
#define DT_SOCK    12

/* a reparse point's data, header included */
#define REPARSE_HEADER_SIZE 8
#define REPARSE_BUFFER_SIZE (16 * 1024)

/* SymbolicLinkReparseBuffer of REPARSE_DATA_BUFFER */
struct SymlinkReparse {
    USHORT substitute_offset;
    USHORT substitute_length;
    USHORT print_offset;
    USHORT print_length;
    ULONG flags;
    WCHAR path[1];
};
#define SYMLINK_FLAG_RELATIVE 1

/* WSL's symlink: this version, then the target with no NUL */
#define LX_SYMLINK_VERSION 2

/* the numeric user of an attach that names its user only by name */
#define P9_NONUNAME 0xFFFFFFFF

/* Files without metadata of their own get every permission, as in WSL. */
#define DEFAULT_PERMISSIONS 0777

/* FILETIME of 1970-01-01 */
#define UNIX_EPOCH_FILETIME 116444736000000000LL


struct DirEntry {
    std::string name;
    uint64_t ino;
    uint8_t qid_type;
    uint8_t d_type;
};

struct FileWin final: public FSFile {
    std::wstring path;
    uint32_t uid = 0;
    HANDLE handle = INVALID_HANDLE_VALUE;
    bool is_dir = false;
    /* listing taken when the directory is read from its start */
    std::vector<DirEntry> entries;

    ~FileWin() override;
    void Close();
};

/* What WSL keeps of a file in its extended attributes, each 32-bit
   little-endian: $LXUID, $LXGID, $LXMOD (the whole st_mode) and $LXDEV
   (major, then minor). */
struct LxMeta {
    bool has_uid = false;
    bool has_gid = false;
    bool has_mode = false;
    bool has_dev = false;
    uint32_t uid = 0;
    uint32_t gid = 0;
    uint32_t mode = 0;
    uint32_t major = 0;
    uint32_t minor = 0;
};

/* What one query of an open file tells. */
struct NodeInfo {
    uint32_t attributes;
    uint32_t tag; /* of a reparse point that was not followed, else 0 */
    uint64_t ino;
    uint64_t nlink;
    uint64_t size;
};

/* A byte range one owner has locked, its end included. */
struct LockRange {
    uint32_t proc_id;
    std::string client_id;
    FileWin *fid; /* through which it was taken */
    uint8_t type;
    uint64_t start;
    uint64_t end;
};

/* a file: its volume's serial number and its index there */
typedef std::pair<uint32_t, uint64_t> FileKey;

class FileSystemWin final: public HostFileSystem {
private:
    std::wstring fRoot;
    /* Every lock the guest holds. They are the guest's alone: programs on
       the host neither see nor honour them. */
    std::map<FileKey, std::vector<LockRange>> fLocks;
    /* every fid the guest holds, so that a rename can carry their paths */
    std::set<FileWin *> fFiles;

    int LoadDir(FileWin *f);
    std::wstring Parent(const std::wstring &path) const;
    int LockedFile(FileWin *f, FileKey *key);
    void DropLocks(FileWin *f);
    FileWin *NewFile(std::wstring path, uint32_t uid);
    void MovePaths(const std::wstring &from, const std::wstring &to);

public:
    explicit FileSystemWin(std::wstring root): fRoot(std::move(root)) {}

    void End() override {}
    void Delete(FSFile *f) override;
    void StatFS(FSStatFS *st) override;
    int Attach(FSFile **pf, FSQID *qid, uint32_t uid, const char *uname,
               const char *aname) override;
    int Walk(FSFile **pf, FSQID *qids, FSFile *f, int n,
             char **names) override;
    int Mkdir(FSQID *qid, FSFile *f, const char *name, uint32_t mode,
              uint32_t gid) override;
    int Open(FSQID *qid, FSFile *f, uint32_t flags,
             FSOpenCompletion *completion) override;
    int Create(FSQID *qid, FSFile *f, const char *name, uint32_t flags,
               uint32_t mode, uint32_t gid) override;
    int Stat(FSFile *f, FSStat *st) override;
    int SetAttr(FSFile *f, uint32_t mask, uint32_t mode, uint32_t uid,
                uint32_t gid, uint64_t size, uint64_t atime_sec,
                uint64_t atime_nsec, uint64_t mtime_sec,
                uint64_t mtime_nsec) override;
    void Close(FSFile *f) override;
    int ReadDir(FSFile *f, uint64_t offset, uint8_t *buf, int count) override;
    int Read(FSFile *f, uint64_t offset, uint8_t *buf, int count) override;
    int Write(FSFile *f, uint64_t offset, const uint8_t *buf,
              int count) override;
    int Link(FSFile *df, FSFile *f, const char *name) override;
    int Symlink(FSQID *qid, FSFile *f, const char *name, const char *symgt,
                uint32_t gid) override;
    int Mknod(FSQID *qid, FSFile *f, const char *name, uint32_t mode,
              uint32_t major, uint32_t minor, uint32_t gid) override;
    int ReadLink(char *buf, int buf_size, FSFile *f) override;
    int RenameAt(FSFile *f, const char *name, FSFile *new_f,
                 const char *new_name) override;
    int UnlinkAt(FSFile *f, const char *name) override;
    int Lock(FSFile *f, const FSLock *lock) override;
    int GetLock(FSFile *f, FSLock *lock) override;
};

static FileWin *F(FSFile *f) {return static_cast<FileWin *>(f);}


//#pragma mark - errors and names

static int error_to_p9(DWORD err)
{
    switch (err) {
    case ERROR_FILE_NOT_FOUND:
    case ERROR_PATH_NOT_FOUND:
    case ERROR_INVALID_NAME:
    case ERROR_BAD_NETPATH:
    case ERROR_BAD_NET_NAME:
        return P9_ENOENT;
    case ERROR_ACCESS_DENIED:
    case ERROR_PRIVILEGE_NOT_HELD:
        return P9_EACCES;
    case ERROR_SHARING_VIOLATION:
    case ERROR_LOCK_VIOLATION:
        return P9_EBUSY;
    case ERROR_FILE_EXISTS:
    case ERROR_ALREADY_EXISTS:
        return P9_EEXIST;
    case ERROR_NOT_SAME_DEVICE:
        return P9_EXDEV;
    case ERROR_DIRECTORY:
        return P9_ENOTDIR;
    case ERROR_DIR_NOT_EMPTY:
        return P9_ENOTEMPTY;
    case ERROR_DISK_FULL:
    case ERROR_HANDLE_DISK_FULL:
        return P9_ENOSPC;
    case ERROR_WRITE_PROTECT:
        return P9_EROFS;
    case ERROR_FILENAME_EXCED_RANGE:
    case ERROR_BUFFER_OVERFLOW:
        return P9_ENAMETOOLONG;
    case ERROR_NOT_SUPPORTED:
    case ERROR_INVALID_FUNCTION:
        return P9_ENOTSUP;
    case ERROR_INVALID_PARAMETER:
        return P9_EINVAL;
    default:
        return P9_EIO;
    }
}

static int last_error()
{
    return -error_to_p9(GetLastError());
}

/* Characters a Linux name may hold but a Windows one may not. WSL stores each
   as U+F000 plus the character. */
static bool is_escaped(uint32_t c)
{
    return c < 0x20 || (c != 0 && c < 0x80 && strchr("\"*:<>?\\|", c));
}

/* A guest name as the host stores it. */
static int host_name(std::wstring *out, const char *name)
{
    if (name[0] == '\0' || strchr(name, '/'))
        return -P9_ENOENT;
    int n = MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1,
                                nullptr, 0);
    if (n == 0)
        return -P9_EINVAL;
    out->resize(n);
    MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, name, -1, &(*out)[0],
                        n);
    out->resize(n - 1);
    for (wchar_t &c: *out) {
        if (is_escaped(c))
            c = 0xF000 + c;
    }
    return 0;
}

/* A host name as the guest sees it. */
static std::string guest_name(const wchar_t *name, size_t len)
{
    std::wstring w(name, len);
    for (wchar_t &c: w) {
        if (c > 0xF000 && c < 0xF080 && is_escaped(c - 0xF000))
            c -= 0xF000;
    }
    std::string out;
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), w.size(), nullptr, 0,
                                nullptr, nullptr);
    out.resize(n);
    WideCharToMultiByte(CP_UTF8, 0, w.data(), w.size(), &out[0], n, nullptr,
                        nullptr);
    return out;
}

static std::wstring compose_path(const std::wstring &dir,
                                 const std::wstring &name)
{
    std::wstring path = dir;
    if (path.back() != L'\\')
        path += L'\\';
    return path + name;
}

/* The path of a new or removed entry 'name' of 'dir'. */
static int child_path(std::wstring *path, const std::wstring &dir,
                      const char *name)
{
    if (strcmp(name, ".") == 0 || strcmp(name, "..") == 0)
        return -P9_EINVAL;
    std::wstring host;
    int err = host_name(&host, name);
    if (err)
        return err;
    *path = compose_path(dir, host);
    return 0;
}


//#pragma mark - host files

static bool is_lx_tag(DWORD tag)
{
    switch (tag) {
    case IO_REPARSE_TAG_LX_SYMLINK:
    case IO_REPARSE_TAG_AF_UNIX:
    case IO_REPARSE_TAG_LX_FIFO:
    case IO_REPARSE_TAG_LX_CHR:
    case IO_REPARSE_TAG_LX_BLK:
        return true;
    default:
        return false;
    }
}

/* Files stay open to the guest's deletes and renames, as in Linux. */
#define SHARE_ALL (FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE)

/* The data of the reparse point 'h', header included. */
static bool read_reparse(HANDLE h, std::vector<uint8_t> *buf)
{
    DWORD n;
    buf->resize(REPARSE_BUFFER_SIZE);
    if (!DeviceIoControl(h, FSCTL_GET_REPARSE_POINT, nullptr, 0, buf->data(),
                         buf->size(), &n, nullptr))
        return false;
    buf->resize(n);
    return n >= REPARSE_HEADER_SIZE;
}

static const SymlinkReparse *symlink_reparse(const std::vector<uint8_t> &buf)
{
    if (buf.size() < REPARSE_HEADER_SIZE + offsetof(SymlinkReparse, path) ||
        get_le32(buf.data()) != IO_REPARSE_TAG_SYMLINK)
        return nullptr;
    return reinterpret_cast<const SymlinkReparse *>(
        buf.data() + REPARSE_HEADER_SIZE);
}

/* Whether a reparse point is shown as it is rather than followed: the
   files WSL makes, and a Windows symlink to a relative path, which reads
   the same in the guest. */
static bool keep_reparse(HANDLE h, DWORD tag)
{
    if (is_lx_tag(tag))
        return true;
    if (tag != IO_REPARSE_TAG_SYMLINK)
        return false;
    std::vector<uint8_t> buf;
    const SymlinkReparse *sl = read_reparse(h, &buf) ? symlink_reparse(buf)
                                                     : nullptr;
    return sl && (sl->flags & SYMLINK_FLAG_RELATIVE);
}

/* Opens the file 'path' names. A symlink or another Linux special file is
   opened itself; any other reparse point, such as a junction, is followed,
   unless its target is missing. */
static HANDLE open_node(const std::wstring &path, DWORD access)
{
    access |= FILE_READ_ATTRIBUTES | FILE_READ_EA;
    HANDLE h = CreateFileW(path.c_str(), access, SHARE_ALL, nullptr,
                           OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS |
                           FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return h;
    FILE_ATTRIBUTE_TAG_INFO tag;
    if (GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag,
                                     sizeof(tag)) &&
        (tag.FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT) &&
        !keep_reparse(h, tag.ReparseTag)) {
        HANDLE target = CreateFileW(path.c_str(), access, SHARE_ALL, nullptr,
                                    OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS,
                                    nullptr);
        if (target != INVALID_HANDLE_VALUE) {
            CloseHandle(h);
            h = target;
        }
    }
    return h;
}

static bool query_node(HANDLE h, NodeInfo *info)
{
    BY_HANDLE_FILE_INFORMATION bh;
    if (!GetFileInformationByHandle(h, &bh))
        return false;
    info->attributes = bh.dwFileAttributes;
    info->tag = 0;
    info->ino = ((uint64_t)bh.nFileIndexHigh << 32) | bh.nFileIndexLow;
    info->nlink = bh.nNumberOfLinks;
    info->size = ((uint64_t)bh.nFileSizeHigh << 32) | bh.nFileSizeLow;
    if (info->attributes & FILE_ATTRIBUTE_REPARSE_POINT) {
        FILE_ATTRIBUTE_TAG_INFO tag;
        if (!GetFileInformationByHandleEx(h, FileAttributeTagInfo, &tag,
                                          sizeof(tag)))
            return false;
        info->tag = tag.ReparseTag;
    }
    return true;
}

/* 0 reads as an empty slot in a directory listing. */
static uint64_t valid_ino(uint64_t ino)
{
    return ino != 0 ? ino : 1;
}

/* The st_mode type bits of a file with these attributes and reparse tag. */
static uint32_t node_type(uint32_t attributes, uint32_t tag)
{
    switch (tag) {
    case IO_REPARSE_TAG_LX_SYMLINK:
    case IO_REPARSE_TAG_SYMLINK:
        return P9_S_IFLNK;
    case IO_REPARSE_TAG_LX_FIFO:
        return P9_S_IFIFO;
    case IO_REPARSE_TAG_LX_CHR:
        return P9_S_IFCHR;
    case IO_REPARSE_TAG_LX_BLK:
        return P9_S_IFBLK;
    case IO_REPARSE_TAG_AF_UNIX:
        return P9_S_IFSOCK;
    default:
        return (attributes & FILE_ATTRIBUTE_DIRECTORY) ? P9_S_IFDIR
                                                       : P9_S_IFREG;
    }
}

static uint32_t node_type(const NodeInfo &info)
{
    return node_type(info.attributes, info.tag);
}

static uint8_t mode_qid_type(uint32_t type)
{
    switch (type) {
    case P9_S_IFDIR:
        return P9_QTDIR;
    case P9_S_IFLNK:
        return P9_QTSYMLINK;
    default:
        return P9_QTFILE;
    }
}

static uint8_t mode_d_type(uint32_t type)
{
    switch (type) {
    case P9_S_IFIFO:
        return DT_FIFO;
    case P9_S_IFCHR:
        return DT_CHR;
    case P9_S_IFDIR:
        return DT_DIR;
    case P9_S_IFBLK:
        return DT_BLK;
    case P9_S_IFLNK:
        return DT_LNK;
    case P9_S_IFSOCK:
        return DT_SOCK;
    default:
        return DT_REG;
    }
}

static void node_qid(FSQID *qid, const NodeInfo &info)
{
    qid->type = mode_qid_type(node_type(info));
    qid->version = 0; /* no caching on client */
    qid->path = valid_ino(info.ino);
}

static int stat_path(const std::wstring &path, NodeInfo *info)
{
    HANDLE h = open_node(path, 0);
    if (h == INVALID_HANDLE_VALUE)
        return last_error();
    bool ok = query_node(h, info);
    int err = ok ? 0 : last_error();
    CloseHandle(h);
    return err;
}

static void filetime_to_unix(LARGE_INTEGER t, uint64_t *sec, uint32_t *nsec)
{
    int64_t ticks = t.QuadPart - UNIX_EPOCH_FILETIME;
    int64_t s = ticks / 10000000;
    int64_t rem = ticks % 10000000;
    if (rem < 0) {
        s--;
        rem += 10000000;
    }
    *sec = s;
    *nsec = rem * 100;
}

static LARGE_INTEGER unix_to_filetime(uint64_t sec, uint64_t nsec)
{
    LARGE_INTEGER t;
    t.QuadPart = (int64_t)sec * 10000000 + nsec / 100 + UNIX_EPOCH_FILETIME;
    return t;
}

/* Opens the entry 'path' itself, never what a reparse point names, for an
   operation on the name. */
static HANDLE open_entry(const std::wstring &path, DWORD access)
{
    return CreateFileW(path.c_str(), access | FILE_READ_ATTRIBUTES, SHARE_ALL,
                       nullptr, OPEN_EXISTING, FILE_FLAG_BACKUP_SEMANTICS |
                       FILE_FLAG_OPEN_REPARSE_POINT, nullptr);
}

static DWORD open_access(uint32_t flags)
{
    switch (flags & P9_O_NOACCESS) {
    case P9_O_RDONLY:
        return GENERIC_READ;
    case P9_O_WRONLY:
        return GENERIC_WRITE;
    case P9_O_RDWR:
        return GENERIC_READ | GENERIC_WRITE;
    default:
        return 0;
    }
}

/* Whether the volume lacks a newer class of information, so that the older
   one has to do. */
static bool unsupported_class()
{
    DWORD err = GetLastError();
    return err == ERROR_INVALID_PARAMETER || err == ERROR_NOT_SUPPORTED ||
           err == ERROR_INVALID_FUNCTION;
}

/* Renames the open entry 'h' to 'to', replacing what is there, and with
   Linux semantics where the volume has them: a file still open under the
   old target goes on working. */
static bool rename_entry(HANDLE h, const std::wstring &to)
{
    /* the system takes a native path here: \??\C:\... */
    std::wstring native = L"\\??\\" + to.substr(4);
    DWORD bytes = native.size() * sizeof(WCHAR);
    std::vector<uint8_t> buf(sizeof(FILE_RENAME_INFO) + bytes);
    auto *ri = reinterpret_cast<FILE_RENAME_INFO *>(buf.data());
    ri->Flags = FILE_RENAME_FLAG_REPLACE_IF_EXISTS |
                FILE_RENAME_FLAG_POSIX_SEMANTICS |
                FILE_RENAME_FLAG_IGNORE_READONLY_ATTRIBUTE;
    ri->RootDirectory = nullptr;
    ri->FileNameLength = bytes;
    memcpy(ri->FileName, native.data(), bytes);
    if (SetFileInformationByHandle(h, FILE_RENAME_INFO_EX_CLASS, ri,
                                   buf.size()))
        return true;
    if (!unsupported_class())
        return false;
    ri->Flags = 0;
    ri->ReplaceIfExists = TRUE;
    return SetFileInformationByHandle(h, FileRenameInfo, ri, buf.size());
}

/* Removes the open entry 'h' with Linux semantics where the volume has
   them: the name goes at once, though the file is still open. */
static bool delete_entry(HANDLE h)
{
    FILE_DISPOSITION_INFO_EX ex;
    ex.Flags = FILE_DISPOSITION_FLAG_DELETE |
               FILE_DISPOSITION_FLAG_POSIX_SEMANTICS |
               FILE_DISPOSITION_FLAG_IGNORE_READONLY_ATTRIBUTE;
    if (SetFileInformationByHandle(h, FILE_DISPOSITION_INFO_EX_CLASS, &ex,
                                   sizeof(ex)))
        return true;
    if (!unsupported_class())
        return false;
    FILE_DISPOSITION_INFO legacy;
    legacy.DeleteFile = TRUE;
    return SetFileInformationByHandle(h, FileDispositionInfo, &legacy,
                                      sizeof(legacy));
}

/* FILE_GET_EA_INFORMATION, for a name of six characters */
struct LxEaName {
    ULONG next;
    UCHAR name_length;
    CHAR name[7];
};

static const char *const lx_ea_names[] = {"$LXUID", "$LXGID", "$LXMOD",
                                          "$LXDEV"};

/* Reads what 'h' has of the metadata, which may be nothing. */
static void query_lx(HANDLE h, LxMeta *m)
{
    LxEaName list[4];
    for (int i = 0; i < 4; i++) {
        list[i].next = i < 3 ? sizeof(LxEaName) : 0;
        list[i].name_length = 6;
        memcpy(list[i].name, lx_ea_names[i], 7);
    }
    alignas(8) uint8_t buf[256];
    IO_STATUS_BLOCK io;
    *m = LxMeta();
    /* fails on a file without extended attributes, or a volume without */
    if (!NT_SUCCESS(NtQueryEaFile(h, &io, buf, sizeof(buf), FALSE, list,
                                  sizeof(list), nullptr, TRUE)))
        return;
    for (uint8_t *p = buf;;) {
        auto *e = reinterpret_cast<FILE_FULL_EA_INFORMATION *>(p);
        const uint8_t *value =
            reinterpret_cast<uint8_t *>(e->EaName) + e->EaNameLength + 1;
        if (e->EaValueLength >= 4) {
            uint32_t v = get_le32(value);
            if (strcmp(e->EaName, "$LXUID") == 0) {
                m->has_uid = true;
                m->uid = v;
            } else if (strcmp(e->EaName, "$LXGID") == 0) {
                m->has_gid = true;
                m->gid = v;
            } else if (strcmp(e->EaName, "$LXMOD") == 0) {
                m->has_mode = true;
                m->mode = v;
            } else if (strcmp(e->EaName, "$LXDEV") == 0 &&
                       e->EaValueLength >= 8) {
                m->has_dev = true;
                m->major = v;
                m->minor = get_le32(value + 4);
            }
        }
        if (e->NextEntryOffset == 0)
            break;
        p += e->NextEntryOffset;
    }
}

/* Writes the fields 'm' has. */
static int set_lx(HANDLE h, const LxMeta &m)
{
    alignas(8) uint8_t buf[128] {};
    size_t pos = 0;
    FILE_FULL_EA_INFORMATION *last = nullptr;

    auto add = [&](const char *name, uint32_t a, uint32_t b, int len) {
        auto *e = reinterpret_cast<FILE_FULL_EA_INFORMATION *>(buf + pos);
        if (last)
            last->NextEntryOffset = (uint8_t *)e - (uint8_t *)last;
        e->EaNameLength = 6;
        e->EaValueLength = len;
        memcpy(e->EaName, name, 7);
        uint8_t *value = reinterpret_cast<uint8_t *>(e->EaName) + 7;
        put_le32(value, a);
        if (len == 8)
            put_le32(value + 4, b);
        pos += (offsetof(FILE_FULL_EA_INFORMATION, EaName) + 7 + len + 3) &
               ~3;
        last = e;
    };
    if (m.has_uid)
        add("$LXUID", m.uid, 0, 4);
    if (m.has_gid)
        add("$LXGID", m.gid, 0, 4);
    if (m.has_mode)
        add("$LXMOD", m.mode, 0, 4);
    if (m.has_dev)
        add("$LXDEV", m.major, m.minor, 8);
    if (pos == 0)
        return 0;

    IO_STATUS_BLOCK io;
    NTSTATUS status = NtSetEaFile(h, &io, buf, pos);
    if (!NT_SUCCESS(status))
        return -error_to_p9(RtlNtStatusToDosError(status));
    return 0;
}

/* The target of the symlink 'h' as the guest reads it. */
static int link_target(HANDLE h, std::string *target)
{
    std::vector<uint8_t> buf;
    if (!read_reparse(h, &buf))
        return last_error();
    size_t len = get_le16(buf.data() + 4);
    if (REPARSE_HEADER_SIZE + len > buf.size())
        return -P9_EIO;
    if (get_le32(buf.data()) == IO_REPARSE_TAG_LX_SYMLINK) {
        if (len < 4)
            return -P9_EIO;
        target->assign(reinterpret_cast<const char *>(buf.data()) +
                       REPARSE_HEADER_SIZE + 4, len - 4);
        return 0;
    }
    const SymlinkReparse *sl = symlink_reparse(buf);
    if (!sl)
        return -P9_EINVAL;
    size_t off = sl->print_offset;
    size_t n = sl->print_length;
    if (n == 0) {
        off = sl->substitute_offset;
        n = sl->substitute_length;
    }
    const uint8_t *names = reinterpret_cast<const uint8_t *>(sl->path);
    if (names - buf.data() + off + n > buf.size())
        return -P9_EIO;
    std::wstring w(reinterpret_cast<const wchar_t *>(names + off),
                   n / sizeof(WCHAR));
    for (wchar_t &c: w) {
        if (c == L'\\')
            c = L'/';
    }
    *target = guest_name(w.data(), w.size());
    return 0;
}

/* Creates 'path' as a file of the kind WSL marks with reparse tag 'tag',
   with 'data' after the reparse header. */
static HANDLE create_special(const std::wstring &path, DWORD tag,
                             const std::vector<uint8_t> &data, int *err)
{
    if (REPARSE_HEADER_SIZE + data.size() > REPARSE_BUFFER_SIZE) {
        *err = -P9_ENAMETOOLONG;
        return INVALID_HANDLE_VALUE;
    }
    HANDLE h = CreateFileW(path.c_str(), GENERIC_WRITE | DELETE |
                           FILE_READ_ATTRIBUTES | FILE_READ_EA | FILE_WRITE_EA,
                           SHARE_ALL, nullptr, CREATE_NEW,
                           FILE_ATTRIBUTE_NORMAL | FILE_FLAG_OPEN_REPARSE_POINT,
                           nullptr);
    if (h == INVALID_HANDLE_VALUE) {
        *err = last_error();
        return h;
    }
    std::vector<uint8_t> buf(REPARSE_HEADER_SIZE + data.size());
    put_le32(buf.data(), tag);
    put_le16(buf.data() + 4, data.size());
    put_le16(buf.data() + 6, 0);
    if (!data.empty())
        memcpy(buf.data() + REPARSE_HEADER_SIZE, data.data(), data.size());
    DWORD n;
    if (!DeviceIoControl(h, FSCTL_SET_REPARSE_POINT, buf.data(), buf.size(),
                         nullptr, 0, &n, nullptr)) {
        *err = last_error();
        delete_entry(h);
        CloseHandle(h);
        return INVALID_HANDLE_VALUE;
    }
    return h;
}

/* The qid of a file just made; closes it. */
static int new_node_qid(HANDLE h, FSQID *qid)
{
    NodeInfo info;
    bool ok = query_node(h, &info);
    int err = ok ? 0 : last_error();
    CloseHandle(h);
    if (!err)
        node_qid(qid, info);
    return err;
}

/* st_rdev as Linux encodes a device number */
static uint64_t encode_dev(uint32_t major, uint32_t minor)
{
    return (minor & 0xff) | ((uint64_t)(major & 0xfff) << 8) |
           ((uint64_t)(minor & ~0xffu) << 12);
}

/* The metadata of a new file. */
static LxMeta new_lx(uint32_t uid, uint32_t gid, uint32_t mode)
{
    LxMeta m;
    m.has_uid = m.has_gid = m.has_mode = true;
    m.uid = uid;
    m.gid = gid;
    m.mode = mode;
    return m;
}


//#pragma mark - FileWin

FileWin::~FileWin()
{
    Close();
}

void FileWin::Close()
{
    if (handle != INVALID_HANDLE_VALUE) {
        CloseHandle(handle);
        handle = INVALID_HANDLE_VALUE;
    }
    entries.clear();
}


//#pragma mark - FileSystemWin

std::wstring FileSystemWin::Parent(const std::wstring &path) const
{
    if (path.size() <= fRoot.size())
        return fRoot;
    size_t sep = path.rfind(L'\\');
    if (sep < fRoot.size())
        return fRoot;
    return path.substr(0, sep);
}

FileWin *FileSystemWin::NewFile(std::wstring path, uint32_t uid)
{
    FileWin *f = new FileWin();
    f->path = std::move(path);
    f->uid = uid;
    fFiles.insert(f);
    return f;
}

/* A fid names a file, not a path: those on the renamed entry, or below it,
   follow it to its new name. */
void FileSystemWin::MovePaths(const std::wstring &from, const std::wstring &to)
{
    for (FileWin *f: fFiles) {
        if (f->path.compare(0, from.size(), from) != 0)
            continue;
        if (f->path.size() == from.size())
            f->path = to;
        else if (f->path[from.size()] == L'\\')
            f->path = to + f->path.substr(from.size());
    }
}

void FileSystemWin::Delete(FSFile *f)
{
    DropLocks(F(f));
    fFiles.erase(F(f));
    delete F(f);
}

void FileSystemWin::StatFS(FSStatFS *st)
{
    ULARGE_INTEGER avail {}, total {}, free {};
    GetDiskFreeSpaceExW(fRoot.c_str(), &avail, &total, &free);
    st->f_bsize = 4096;
    st->f_blocks = total.QuadPart / st->f_bsize;
    st->f_bfree = free.QuadPart / st->f_bsize;
    st->f_bavail = avail.QuadPart / st->f_bsize;
    /* the host sets no limit on the number of files */
    st->f_files = 0;
    st->f_ffree = 0;
}

int FileSystemWin::Attach(FSFile **pf, FSQID *qid, uint32_t uid,
                          const char *uname, const char *aname)
{
    NodeInfo info;
    int err = stat_path(fRoot, &info);
    if (err) {
        *pf = nullptr;
        return err;
    }
    /* a client that gives only a user name owns what files without an
       owner of their own have */
    FileWin *f = NewFile(fRoot, (uid == P9_NONUNAME) ? 0 : uid);
    node_qid(qid, info);
    *pf = f;
    return 0;
}

int FileSystemWin::Walk(FSFile **pf, FSQID *qids, FSFile *f1, int n,
                        char **names)
{
    FileWin *f = F(f1);
    std::wstring path = f->path;
    int i;

    for (i = 0; i < n; i++) {
        std::wstring next;
        if (strcmp(names[i], ".") == 0) {
            next = path;
        } else if (strcmp(names[i], "..") == 0) {
            next = Parent(path);
        } else {
            std::wstring name;
            if (host_name(&name, names[i]) != 0)
                break;
            next = compose_path(path, name);
        }
        NodeInfo info;
        if (stat_path(next, &info) != 0)
            break;
        path = std::move(next);
        node_qid(&qids[i], info);
    }
    *pf = NewFile(std::move(path), f->uid);
    return i;
}

int FileSystemWin::Mkdir(FSQID *qid, FSFile *f, const char *name,
                         uint32_t mode, uint32_t gid)
{
    std::wstring path;
    int err = child_path(&path, F(f)->path, name);
    if (err)
        return err;
    if (!CreateDirectoryW(path.c_str(), nullptr))
        return last_error();
    HANDLE h = open_entry(path, FILE_WRITE_ATTRIBUTES | FILE_WRITE_EA);
    if (h == INVALID_HANDLE_VALUE)
        return last_error();
    /* a volume without extended attributes keeps no owner */
    set_lx(h, new_lx(F(f)->uid, gid, P9_S_IFDIR | (mode & 07777)));
    /* Names that differ in case only are different files in a directory
       the guest makes, as with WSL's case=dir. A system without the WSL
       feature refuses, and the directory stays case-insensitive. */
    FILE_CASE_SENSITIVE_INFO cs;
    cs.Flags = FILE_CS_FLAG_CASE_SENSITIVE_DIR;
    SetFileInformationByHandle(h, FILE_CASE_SENSITIVE_INFO_CLASS, &cs,
                               sizeof(cs));
    return new_node_qid(h, qid);
}

int FileSystemWin::Open(FSQID *qid, FSFile *f1, uint32_t flags,
                        FSOpenCompletion *completion)
{
    FileWin *f = F(f1);
    DWORD access = open_access(flags);

    f->Close();
    HANDLE h = open_node(f->path, access);
    if (h == INVALID_HANDLE_VALUE)
        return last_error();
    NodeInfo info;
    if (!query_node(h, &info)) {
        int err = last_error();
        CloseHandle(h);
        return err;
    }
    bool is_dir = info.attributes & FILE_ATTRIBUTE_DIRECTORY;
    if (is_dir && (access & GENERIC_WRITE)) {
        CloseHandle(h);
        return -P9_EISDIR;
    }
    if (!is_dir && (flags & P9_O_DIRECTORY)) {
        CloseHandle(h);
        return -P9_ENOTDIR;
    }
    if (!is_dir && (flags & P9_O_TRUNC)) {
        FILE_END_OF_FILE_INFO eof {};
        if (!SetFileInformationByHandle(h, FileEndOfFileInfo, &eof,
                                        sizeof(eof))) {
            int err = last_error();
            CloseHandle(h);
            return err;
        }
    }
    f->handle = h;
    f->is_dir = is_dir;
    node_qid(qid, info);
    return 0;
}

int FileSystemWin::Create(FSQID *qid, FSFile *f1, const char *name,
                          uint32_t flags, uint32_t mode, uint32_t gid)
{
    FileWin *f = F(f1);
    std::wstring path;
    int err = child_path(&path, f->path, name);
    if (err)
        return err;

    f->Close();
    /* an existing file is opened as it is, keeping what WSL stored on it */
    DWORD disposition = (flags & P9_O_EXCL) ? CREATE_NEW : OPEN_ALWAYS;
    HANDLE h = CreateFileW(path.c_str(),
                           open_access(flags) | FILE_READ_ATTRIBUTES |
                           FILE_READ_EA | FILE_WRITE_EA,
                           SHARE_ALL, nullptr, disposition,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        return last_error();
    bool created = GetLastError() != ERROR_ALREADY_EXISTS;
    if (created)
        set_lx(h, new_lx(f->uid, gid, P9_S_IFREG | (mode & 07777)));
    NodeInfo info;
    bool ok = query_node(h, &info);
    if (ok && (flags & P9_O_TRUNC)) {
        FILE_END_OF_FILE_INFO eof {};
        ok = SetFileInformationByHandle(h, FileEndOfFileInfo, &eof,
                                        sizeof(eof));
    }
    if (!ok) {
        err = last_error();
        CloseHandle(h);
        return err;
    }
    f->path = std::move(path);
    f->handle = h;
    f->is_dir = false;
    node_qid(qid, info);
    return 0;
}

int FileSystemWin::Stat(FSFile *f1, FSStat *st)
{
    FileWin *f = F(f1);
    HANDLE h = f->handle;
    bool own = false;

    /* an open file is asked directly, so it answers after being unlinked */
    if (h == INVALID_HANDLE_VALUE) {
        h = open_node(f->path, 0);
        if (h == INVALID_HANDLE_VALUE)
            return last_error();
        own = true;
    }
    NodeInfo info;
    FILE_BASIC_INFO basic;
    FILE_STANDARD_INFO standard;
    LxMeta lx;
    bool ok = query_node(h, &info) &&
              GetFileInformationByHandleEx(h, FileBasicInfo, &basic,
                                           sizeof(basic)) &&
              GetFileInformationByHandleEx(h, FileStandardInfo, &standard,
                                           sizeof(standard));
    int err = ok ? 0 : last_error();
    uint64_t size = 0;
    if (ok) {
        size = info.size;
        query_lx(h, &lx);
        /* a symlink's size is its target's length */
        std::string target;
        if (node_type(info) == P9_S_IFLNK && link_target(h, &target) == 0)
            size = target.size();
    }
    if (own)
        CloseHandle(h);
    if (err)
        return err;

    node_qid(&st->qid, info);
    /* the host decides the type; the guest, when it set them, the rest */
    st->st_mode = node_type(info);
    if (lx.has_mode) {
        st->st_mode |= lx.mode & 07777;
    } else {
        st->st_mode |= DEFAULT_PERMISSIONS;
        if ((info.attributes & FILE_ATTRIBUTE_READONLY) &&
            !(info.attributes & FILE_ATTRIBUTE_DIRECTORY))
            st->st_mode &= ~0222;
    }
    st->st_uid = lx.uid;
    st->st_gid = lx.gid;
    st->st_nlink = info.nlink;
    st->st_rdev = lx.has_dev ? encode_dev(lx.major, lx.minor) : 0;
    /* a directory's size is its index's, which nothing in Linux expects */
    st->st_size = (node_type(info) == P9_S_IFDIR) ? 4096 : size;
    st->st_blksize = 4096;
    st->st_blocks = standard.AllocationSize.QuadPart / 512;
    filetime_to_unix(basic.LastAccessTime, &st->st_atime_sec,
                     &st->st_atime_nsec);
    filetime_to_unix(basic.LastWriteTime, &st->st_mtime_sec,
                     &st->st_mtime_nsec);
    filetime_to_unix(basic.ChangeTime, &st->st_ctime_sec, &st->st_ctime_nsec);
    return 0;
}

int FileSystemWin::SetAttr(FSFile *f1, uint32_t mask, uint32_t mode,
                           uint32_t uid, uint32_t gid, uint64_t size,
                           uint64_t atime_sec, uint64_t atime_nsec,
                           uint64_t mtime_sec, uint64_t mtime_nsec)
{
    FileWin *f = F(f1);
    DWORD access = 0;

    if (mask & (P9_SETATTR_MODE | P9_SETATTR_UID | P9_SETATTR_GID))
        access |= FILE_WRITE_EA | FILE_WRITE_ATTRIBUTES;
    if (mask & P9_SETATTR_SIZE)
        access |= GENERIC_WRITE;
    if (mask & (P9_SETATTR_ATIME | P9_SETATTR_MTIME))
        access |= FILE_WRITE_ATTRIBUTES;
    /* a change time of its own is not kept */
    if (access == 0)
        return 0;

    /* a file unlinked while open has only its handle left */
    HANDLE h = open_node(f->path, access);
    bool own = h != INVALID_HANDLE_VALUE;
    if (!own) {
        if (f->handle == INVALID_HANDLE_VALUE)
            return last_error();
        h = f->handle;
    }
    NodeInfo info;
    bool ok = query_node(h, &info);
    int err = ok ? 0 : last_error();
    if (!err && (mask & (P9_SETATTR_MODE | P9_SETATTR_UID |
                         P9_SETATTR_GID))) {
        LxMeta lx;
        lx.has_uid = mask & P9_SETATTR_UID;
        lx.uid = uid;
        lx.has_gid = mask & P9_SETATTR_GID;
        lx.gid = gid;
        lx.has_mode = mask & P9_SETATTR_MODE;
        lx.mode = node_type(info) | (mode & 07777);
        err = set_lx(h, lx);
        /* Once the guest lets the owner write, the host's read-only
           attribute must not stop it. */
        bool dir = info.attributes & FILE_ATTRIBUTE_DIRECTORY;
        if (!err && lx.has_mode && (mode & 0200) && !dir &&
            (info.attributes & FILE_ATTRIBUTE_READONLY)) {
            FILE_BASIC_INFO basic {};
            basic.FileAttributes = info.attributes &
                (FILE_ATTRIBUTE_ARCHIVE | FILE_ATTRIBUTE_HIDDEN |
                 FILE_ATTRIBUTE_SYSTEM | FILE_ATTRIBUTE_NOT_CONTENT_INDEXED);
            if (basic.FileAttributes == 0)
                basic.FileAttributes = FILE_ATTRIBUTE_NORMAL;
            if (!SetFileInformationByHandle(h, FileBasicInfo, &basic,
                                            sizeof(basic)))
                err = last_error();
        }
    }
    if (!err && (mask & P9_SETATTR_SIZE)) {
        if (info.attributes & FILE_ATTRIBUTE_DIRECTORY) {
            err = -P9_EISDIR;
        } else {
            FILE_END_OF_FILE_INFO eof;
            eof.EndOfFile.QuadPart = size;
            if (!SetFileInformationByHandle(h, FileEndOfFileInfo, &eof,
                                            sizeof(eof)))
                err = last_error();
        }
    }
    if (!err && (mask & (P9_SETATTR_ATIME | P9_SETATTR_MTIME))) {
        FILE_BASIC_INFO basic {}; /* zero leaves a field as it is */
        LARGE_INTEGER now;
        GetSystemTimePreciseAsFileTime(reinterpret_cast<FILETIME *>(&now));
        if (mask & P9_SETATTR_ATIME) {
            basic.LastAccessTime = (mask & P9_SETATTR_ATIME_SET)
                ? unix_to_filetime(atime_sec, atime_nsec) : now;
        }
        if (mask & P9_SETATTR_MTIME) {
            basic.LastWriteTime = (mask & P9_SETATTR_MTIME_SET)
                ? unix_to_filetime(mtime_sec, mtime_nsec) : now;
        }
        if (!SetFileInformationByHandle(h, FileBasicInfo, &basic,
                                        sizeof(basic)))
            err = last_error();
    }
    if (own)
        CloseHandle(h);
    return err;
}

void FileSystemWin::Close(FSFile *f)
{
    F(f)->Close();
}

/* Lists the directory once, so that an offset is an index into the list. */
int FileSystemWin::LoadDir(FileWin *f)
{
    NodeInfo self, parent;
    f->entries.clear();
    if (!query_node(f->handle, &self))
        return last_error();
    if (stat_path(Parent(f->path), &parent) != 0)
        parent = self;
    f->entries.push_back({".", valid_ino(self.ino), P9_QTDIR, DT_DIR});
    f->entries.push_back({"..", valid_ino(parent.ino), P9_QTDIR, DT_DIR});

    std::unique_ptr<uint8_t[]> buf(new uint8_t[65536]);
    FILE_INFO_BY_HANDLE_CLASS cls = FileIdBothDirectoryRestartInfo;
    for (;;) {
        if (!GetFileInformationByHandleEx(f->handle, cls, buf.get(), 65536)) {
            if (GetLastError() == ERROR_NO_MORE_FILES)
                break;
            return last_error();
        }
        cls = FileIdBothDirectoryInfo;
        uint8_t *p = buf.get();
        for (;;) {
            auto *e = reinterpret_cast<FILE_ID_BOTH_DIR_INFO *>(p);
            size_t len = e->FileNameLength / sizeof(WCHAR);
            bool dot = (len == 1 && e->FileName[0] == L'.') ||
                       (len == 2 && e->FileName[0] == L'.' &&
                        e->FileName[1] == L'.');
            if (!dot) {
                /* a reparse point's tag is where its EA size would be */
                DWORD tag = (e->FileAttributes & FILE_ATTRIBUTE_REPARSE_POINT)
                            ? e->EaSize : 0;
                uint32_t type = node_type(e->FileAttributes, tag);
                /* whether a Windows symlink is followed is not known yet */
                uint8_t d_type = (tag == IO_REPARSE_TAG_SYMLINK)
                                 ? DT_UNKNOWN : mode_d_type(type);
                f->entries.push_back({guest_name(e->FileName, len),
                                      valid_ino(e->FileId.QuadPart),
                                      mode_qid_type(type), d_type});
            }
            if (e->NextEntryOffset == 0)
                break;
            p += e->NextEntryOffset;
        }
    }
    return 0;
}

int FileSystemWin::ReadDir(FSFile *f1, uint64_t offset, uint8_t *buf,
                           int count)
{
    FileWin *f = F(f1);
    int pos = 0;

    if (f->handle == INVALID_HANDLE_VALUE || !f->is_dir)
        return -P9_EPROTO;
    if (offset == 0 || f->entries.empty()) {
        int err = LoadDir(f);
        if (err)
            return err;
    }
    for (uint64_t i = offset; i < f->entries.size(); i++) {
        const DirEntry &e = f->entries[i];
        int name_len = e.name.size();
        int len = 13 + 8 + 1 + 2 + name_len;
        if (pos + len > count)
            break;
        buf[pos++] = e.qid_type;
        put_le32(buf + pos, 0); /* version */
        pos += 4;
        put_le64(buf + pos, e.ino);
        pos += 8;
        put_le64(buf + pos, i + 1); /* offset of the next entry */
        pos += 8;
        buf[pos++] = e.d_type;
        put_le16(buf + pos, name_len);
        pos += 2;
        memcpy(buf + pos, e.name.data(), name_len);
        pos += name_len;
    }
    return pos;
}

int FileSystemWin::Read(FSFile *f1, uint64_t offset, uint8_t *buf, int count)
{
    FileWin *f = F(f1);
    OVERLAPPED ov {};
    DWORD n;

    if (f->handle == INVALID_HANDLE_VALUE || f->is_dir)
        return -P9_EPROTO;
    ov.Offset = (DWORD)offset;
    ov.OffsetHigh = (DWORD)(offset >> 32);
    if (!ReadFile(f->handle, buf, count, &n, &ov)) {
        if (GetLastError() == ERROR_HANDLE_EOF)
            return 0;
        return last_error();
    }
    return n;
}

int FileSystemWin::Write(FSFile *f1, uint64_t offset, const uint8_t *buf,
                         int count)
{
    FileWin *f = F(f1);
    OVERLAPPED ov {};
    DWORD n;

    if (f->handle == INVALID_HANDLE_VALUE || f->is_dir)
        return -P9_EPROTO;
    ov.Offset = (DWORD)offset;
    ov.OffsetHigh = (DWORD)(offset >> 32);
    if (!WriteFile(f->handle, buf, count, &n, &ov))
        return last_error();
    return n;
}

int FileSystemWin::Link(FSFile *df, FSFile *f, const char *name)
{
    std::wstring path;
    int err = child_path(&path, F(df)->path, name);
    if (err)
        return err;
    if (!CreateHardLinkW(path.c_str(), F(f)->path.c_str(), nullptr)) {
        err = last_error();
        NodeInfo info;
        if (stat_path(F(f)->path, &info) == 0 &&
            (info.attributes & FILE_ATTRIBUTE_DIRECTORY))
            return -P9_EPERM;
        return err;
    }
    return 0;
}

int FileSystemWin::Symlink(FSQID *qid, FSFile *f, const char *name,
                           const char *symgt, uint32_t gid)
{
    std::wstring path;
    int err = child_path(&path, F(f)->path, name);
    if (err)
        return err;
    size_t len = strlen(symgt);
    std::vector<uint8_t> data(4 + len);
    put_le32(data.data(), LX_SYMLINK_VERSION);
    memcpy(data.data() + 4, symgt, len);
    HANDLE h = create_special(path, IO_REPARSE_TAG_LX_SYMLINK, data, &err);
    if (h == INVALID_HANDLE_VALUE)
        return err;
    set_lx(h, new_lx(F(f)->uid, gid, P9_S_IFLNK | 0777));
    return new_node_qid(h, qid);
}

int FileSystemWin::Mknod(FSQID *qid, FSFile *f, const char *name,
                         uint32_t mode, uint32_t major, uint32_t minor,
                         uint32_t gid)
{
    uint32_t type = mode & P9_S_IFMT;
    DWORD tag;
    switch (type) {
    case P9_S_IFIFO:
        tag = IO_REPARSE_TAG_LX_FIFO;
        break;
    case P9_S_IFCHR:
        tag = IO_REPARSE_TAG_LX_CHR;
        break;
    case P9_S_IFBLK:
        tag = IO_REPARSE_TAG_LX_BLK;
        break;
    case P9_S_IFSOCK:
        tag = IO_REPARSE_TAG_AF_UNIX;
        break;
    case 0:
    case P9_S_IFREG:
        type = P9_S_IFREG;
        tag = 0;
        break;
    default:
        return -P9_EINVAL;
    }

    std::wstring path;
    int err = child_path(&path, F(f)->path, name);
    if (err)
        return err;
    HANDLE h;
    if (tag != 0) {
        h = create_special(path, tag, {}, &err);
    } else {
        h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES | FILE_WRITE_EA,
                        SHARE_ALL, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL,
                        nullptr);
        if (h == INVALID_HANDLE_VALUE)
            err = last_error();
    }
    if (h == INVALID_HANDLE_VALUE)
        return err;
    LxMeta lx = new_lx(F(f)->uid, gid, type | (mode & 07777));
    if (type == P9_S_IFCHR || type == P9_S_IFBLK) {
        lx.has_dev = true;
        lx.major = major;
        lx.minor = minor;
    }
    set_lx(h, lx);
    return new_node_qid(h, qid);
}

int FileSystemWin::ReadLink(char *buf, int buf_size, FSFile *f)
{
    HANDLE h = open_entry(F(f)->path, 0);
    if (h == INVALID_HANDLE_VALUE)
        return last_error();
    std::string target;
    int err = link_target(h, &target);
    CloseHandle(h);
    if (err)
        return err;
    if ((int)target.size() >= buf_size)
        return -P9_ENAMETOOLONG;
    memcpy(buf, target.c_str(), target.size() + 1);
    return 0;
}

int FileSystemWin::RenameAt(FSFile *f, const char *name, FSFile *new_f,
                            const char *new_name)
{
    std::wstring from, to;
    int err = child_path(&from, F(f)->path, name);
    if (!err)
        err = child_path(&to, F(new_f)->path, new_name);
    if (err)
        return err;

    HANDLE h = open_entry(from, DELETE);
    if (h == INVALID_HANDLE_VALUE)
        return last_error();
    if (rename_entry(h, to)) {
        CloseHandle(h);
        MovePaths(from, to);
        return 0;
    }
    err = last_error();

    /* The system replaces no directory; Linux replaces an empty one with
       another directory. */
    NodeInfo src, dst;
    if (query_node(h, &src) && stat_path(to, &dst) == 0) {
        bool src_dir = src.attributes & FILE_ATTRIBUTE_DIRECTORY;
        bool dst_dir = dst.attributes & FILE_ATTRIBUTE_DIRECTORY;
        if (dst_dir && !src_dir) {
            err = -P9_EISDIR;
        } else if (src_dir && !dst_dir) {
            err = -P9_ENOTDIR;
        } else if (src_dir && dst_dir) {
            HANDLE dh = open_entry(to, DELETE);
            if (dh == INVALID_HANDLE_VALUE) {
                err = last_error();
            } else {
                bool removed = delete_entry(dh);
                err = removed ? 0 : last_error();
                CloseHandle(dh);
                if (removed && !rename_entry(h, to))
                    err = last_error();
            }
        }
    }
    CloseHandle(h);
    if (err == 0)
        MovePaths(from, to);
    return err;
}

int FileSystemWin::UnlinkAt(FSFile *f, const char *name)
{
    std::wstring path;
    int err = child_path(&path, F(f)->path, name);
    if (err)
        return err;
    HANDLE h = open_entry(path, DELETE);
    if (h == INVALID_HANDLE_VALUE)
        return last_error();
    err = delete_entry(h) ? 0 : last_error();
    CloseHandle(h);
    return err;
}

/* The range a lock request covers; a length of 0 runs to the end. */
static bool lock_range(const FSLock *lock, uint64_t *start, uint64_t *end)
{
    *start = lock->start;
    if (lock->length == 0) {
        *end = UINT64_MAX;
        return true;
    }
    if (lock->length - 1 > UINT64_MAX - lock->start)
        return false;
    *end = lock->start + lock->length - 1;
    return true;
}

static bool same_owner(const LockRange &r, const FSLock *lock)
{
    return r.proc_id == lock->proc_id &&
           r.client_id == (lock->client_id ? lock->client_id : "");
}

/* Whether another owner's range keeps 'lock' from being taken. */
static bool lock_conflicts(const LockRange &r, const FSLock *lock,
                           uint64_t start, uint64_t end)
{
    return !same_owner(r, lock) && r.start <= end && start <= r.end &&
           (r.type == P9_LOCK_TYPE_WRLCK || lock->type == P9_LOCK_TYPE_WRLCK);
}

int FileSystemWin::LockedFile(FileWin *f, FileKey *key)
{
    HANDLE h = f->handle;
    bool own = false;
    if (h == INVALID_HANDLE_VALUE) {
        h = open_node(f->path, 0);
        if (h == INVALID_HANDLE_VALUE)
            return last_error();
        own = true;
    }
    BY_HANDLE_FILE_INFORMATION bh;
    bool ok = GetFileInformationByHandle(h, &bh);
    int err = ok ? 0 : last_error();
    if (own)
        CloseHandle(h);
    if (err)
        return err;
    *key = FileKey(bh.dwVolumeSerialNumber,
                   ((uint64_t)bh.nFileIndexHigh << 32) | bh.nFileIndexLow);
    return 0;
}

/* Drops the locks taken through 'f', which the guest no longer has. */
void FileSystemWin::DropLocks(FileWin *f)
{
    for (auto it = fLocks.begin(); it != fLocks.end();) {
        std::vector<LockRange> &ranges = it->second;
        for (size_t i = 0; i < ranges.size();) {
            if (ranges[i].fid == f)
                ranges.erase(ranges.begin() + i);
            else
                i++;
        }
        if (ranges.empty())
            it = fLocks.erase(it);
        else
            ++it;
    }
}

int FileSystemWin::Lock(FSFile *f, const FSLock *lock)
{
    uint64_t start, end;
    if (lock->type > P9_LOCK_TYPE_UNLCK || !lock_range(lock, &start, &end))
        return -P9_EINVAL;
    FileKey key;
    int err = LockedFile(F(f), &key);
    if (err)
        return err;

    std::vector<LockRange> &ranges = fLocks[key];
    if (lock->type != P9_LOCK_TYPE_UNLCK) {
        for (const LockRange &r: ranges) {
            if (lock_conflicts(r, lock, start, end))
                return P9_LOCK_BLOCKED; /* the client retries a blocking one */
        }
    }
    /* what the owner held of the range gives way to the request */
    std::vector<LockRange> kept;
    for (const LockRange &r: ranges) {
        if (!same_owner(r, lock) || r.end < start || r.start > end) {
            kept.push_back(r);
            continue;
        }
        if (r.start < start) {
            kept.push_back(r);
            kept.back().end = start - 1;
        }
        if (r.end > end) {
            kept.push_back(r);
            kept.back().start = end + 1;
        }
    }
    if (lock->type != P9_LOCK_TYPE_UNLCK) {
        kept.push_back({lock->proc_id,
                        lock->client_id ? lock->client_id : "", F(f),
                        lock->type, start, end});
    }
    if (kept.empty())
        fLocks.erase(key);
    else
        ranges = std::move(kept);
    return P9_LOCK_SUCCESS;
}

int FileSystemWin::GetLock(FSFile *f, FSLock *lock)
{
    uint64_t start, end;
    if (lock->type > P9_LOCK_TYPE_UNLCK || !lock_range(lock, &start, &end))
        return -P9_EINVAL;
    FileKey key;
    int err = LockedFile(F(f), &key);
    if (err)
        return err;

    auto it = fLocks.find(key);
    if (it != fLocks.end() && lock->type != P9_LOCK_TYPE_UNLCK) {
        for (const LockRange &r: it->second) {
            if (!lock_conflicts(r, lock, start, end))
                continue;
            lock->type = r.type;
            lock->start = r.start;
            lock->length = (r.end == UINT64_MAX) ? 0 : r.end - r.start + 1;
            lock->proc_id = r.proc_id;
            free(lock->client_id);
            lock->client_id = strdup(r.client_id.c_str());
            return 0;
        }
    }
    lock->type = P9_LOCK_TYPE_UNLCK;
    return 0;
}


std::unique_ptr<HostFileSystem> disk_fs_open(const char *root_path)
{
    int n = MultiByteToWideChar(CP_UTF8, 0, root_path, -1, nullptr, 0);
    std::wstring wpath(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, root_path, -1, &wpath[0], n);

    /* Absolute, with the prefix that keeps names such as "CON" or "a." as
       they are. */
    DWORD len = GetFullPathNameW(wpath.c_str(), 0, nullptr, nullptr);
    std::wstring full(len, L'\0');
    len = GetFullPathNameW(wpath.c_str(), len, &full[0], nullptr);
    full.resize(len);
    std::wstring root;
    if (full.compare(0, 4, L"\\\\?\\") == 0)
        root = full;
    else if (full.compare(0, 2, L"\\\\") == 0)
        root = L"\\\\?\\UNC\\" + full.substr(2);
    else
        root = L"\\\\?\\" + full;
    /* "C:\" keeps its separator; it names the drive's root directory. */
    if (root.size() > 7 && root.back() == L'\\')
        root.pop_back();

    NodeInfo info;
    if (len == 0 || stat_path(root, &info) != 0 ||
        !(info.attributes & FILE_ATTRIBUTE_DIRECTORY)) {
        fprintf(stderr, "%s: must be a directory\n", root_path);
        return nullptr;
    }
    return std::make_unique<FileSystemWin>(std::move(root));
}
