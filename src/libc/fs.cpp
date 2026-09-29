// POSIX file APIs over the sandbox mapping.
#include "hle.h"
#include "libc/format.h"
#include "libc/vfs.h"
#include "modules.h"
#include <cerrno>
#include <fcntl.h>
#include <filesystem>
#include <io.h>
#include <mutex>
#include <sys/stat.h>
#include <unordered_map>
#include <windows.h>

namespace vfs {

const char* const kBundlePath = "/var/containers/Bundle/Application/5E1B3000-0000-4000-8000-1B3000000001/SwordGame.app";
const char* const kHomePath = "/var/mobile/Containers/Data/Application/5E1B3000-0000-4000-8000-1B3000000002";

namespace {
std::string g_host_bundle, g_host_home;
std::unordered_map<std::string, std::string> g_overrides;  // lowercase bundle-relative path ("/binaries/x") -> host path

std::string lower(std::string s) {
    for (char& c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}
std::string g_cwd = "/";
std::mutex g_cwd_mutex;

std::string normalize(const std::string& p) {
    // Collapse "//", "/./" and "/../" components.
    std::vector<std::string> parts;
    size_t i = 0;
    while (i < p.size()) {
        size_t j = p.find('/', i);
        if (j == std::string::npos) j = p.size();
        std::string c = p.substr(i, j - i);
        if (c == "..") {
            if (!parts.empty()) parts.pop_back();
        } else if (!c.empty() && c != ".") {
            parts.push_back(c);
        }
        i = j + 1;
    }
    std::string out;
    for (auto& c : parts) out += "/" + c;
    return out.empty() ? "/" : out;
}
bool starts_with(const std::string& s, const std::string& prefix) {
    return s.size() >= prefix.size() && _strnicmp(s.c_str(), prefix.c_str(), prefix.size()) == 0 &&
           (s.size() == prefix.size() || s[prefix.size()] == '/');
}
thread_local int* t_errno = nullptr;
}  // namespace

void set_roots(const std::string& host_bundle, const std::string& host_home) {
    g_host_bundle = std::filesystem::absolute(host_bundle).generic_string();
    g_host_home = std::filesystem::absolute(host_home).generic_string();
    for (const char* d : {"", "/Documents", "/Library", "/Library/Caches", "/Library/Preferences", "/tmp"})
        std::filesystem::create_directories(g_host_home + d);
}

const std::string& host_home() { return g_host_home; }
const std::string& host_bundle() { return g_host_bundle; }

void override_bundle_file(const std::string& bundle_relative, const std::string& host_path) {
    g_overrides[lower(normalize("/" + bundle_relative))] = host_path;
}

void set_cwd(const std::string& p) {
    std::lock_guard lock(g_cwd_mutex);
    g_cwd = p;
}

std::string to_host(const char* guest_path) {
    if (!guest_path) return {};
    std::string p = guest_path;
    if (p.empty()) return {};
    if (p[0] != '/') {
        std::lock_guard lock(g_cwd_mutex);
        p = g_cwd + "/" + p;
    }
    p = normalize(p);
    // /private/var is the same as /var on iOS.
    if (p.rfind("/private/var/", 0) == 0) p = p.substr(8);
    if (starts_with(p, kBundlePath)) {
        std::string rel = p.substr(strlen(kBundlePath));
        if (!g_overrides.empty()) {
            auto it = g_overrides.find(lower(rel));
            if (it != g_overrides.end()) {
                static std::unordered_map<std::string, bool> logged;
                if (!logged[it->first]) LOG_INFO("vfs: serving %s in place of %s", it->second.c_str(), rel.c_str());
                logged[it->first] = true;
                return it->second;
            }
        }
        return g_host_bundle + rel;
    }
    if (starts_with(p, kHomePath)) return g_host_home + p.substr(strlen(kHomePath));
    return {};
}

std::wstring to_host_w(const char* guest_path) {
    std::string h = to_host(guest_path);
    return h.empty() ? std::wstring() : libc::utf8_to_wide(h);
}

int darwin_errno_from_crt(int e) {
    switch (e) {
    case EAGAIN: return 35;
    case EDEADLK: return 11;
    case ENAMETOOLONG: return 63;
    case ENOTEMPTY: return 66;
    case ENOSYS: return 78;
    default: return e;  // 1..34 agree
    }
}

GuestAddr errno_location() {
    if (!t_errno) t_errno = static_cast<int*>(std::calloc(1, 16));
    return gaddr(t_errno);
}
void set_errno(int e) { *gptr<int>(errno_location()) = e; }

}  // namespace vfs

namespace libc {

namespace {

constexpr int D_ENOENT = 2, D_EBADF = 9, D_EACCES = 13, D_EINVAL = 22;

// Darwin arm64 struct stat (64-bit inodes), 144 bytes.
struct DarwinStat {
    s32 st_dev;
    u16 st_mode;
    u16 st_nlink;
    u64 st_ino;
    u32 st_uid, st_gid;
    s32 st_rdev;
    s32 pad0;
    s64 st_atime_, st_atime_ns, st_mtime_, st_mtime_ns, st_ctime_, st_ctime_ns, st_birth, st_birth_ns;
    s64 st_size;
    s64 st_blocks;
    s32 st_blksize;
    u32 st_flags, st_gen;
    s32 st_lspare;
    s64 st_qspare[2];
};
static_assert(sizeof(DarwinStat) == 144);

void fill_stat(DarwinStat* out, const struct _stat64& s) {
    std::memset(out, 0, sizeof(*out));
    bool dir = (s.st_mode & _S_IFMT) == _S_IFDIR;
    out->st_mode = dir ? (0040000 | 0755) : (0100000 | 0644);
    out->st_nlink = 1;
    out->st_ino = (u64)s.st_ino;
    out->st_uid = out->st_gid = 501;
    out->st_atime_ = s.st_atime;
    out->st_mtime_ = s.st_mtime;
    out->st_ctime_ = s.st_ctime;
    out->st_birth = s.st_ctime;
    out->st_size = s.st_size;
    out->st_blocks = (s.st_size + 511) / 512;
    out->st_blksize = 4096;
}

int fail_crt() {
    vfs::set_errno(vfs::darwin_errno_from_crt(errno));
    return -1;
}

int host_open_flags(int f) {
    int h = _O_BINARY | _O_NOINHERIT;
    switch (f & 3) {
    case 0: h |= _O_RDONLY; break;
    case 1: h |= _O_WRONLY; break;
    default: h |= _O_RDWR; break;
    }
    if (f & 0x8) h |= _O_APPEND;
    if (f & 0x200) h |= _O_CREAT;
    if (f & 0x400) h |= _O_TRUNC;
    if (f & 0x800) h |= _O_EXCL;
    return h;
}

// Darwin arm64 struct dirent: d_ino(8) d_seekoff(8) d_reclen(2) d_namlen(2) d_type(1) d_name[1024].
struct DarwinDirent {
    u64 d_ino;
    u64 d_seekoff;
    u16 d_reclen;
    u16 d_namlen;
    u8 d_type;
    char d_name[1024];
};
struct Dir {
    HANDLE find = INVALID_HANDLE_VALUE;
    WIN32_FIND_DATAW data;
    bool first = true;
    DarwinDirent ent;
};

}  // namespace

void install_fs() {
    using hle::fn;
    hle::fn("___error", []() { return vfs::errno_location(); });

    hle::raw("_open", [](cpu::Thread& t) {
        const char* path = gptr<char>(t.x(0));
        int flags = (int)t.x(1);
        int mode = (flags & 0x200) ? (int)t.stack_slot(0) : 0;
        (void)mode;
        std::wstring h = vfs::to_host_w(path);
        int fd = -1;
        if (h.empty()) {
            vfs::set_errno((flags & 0x200) ? D_EACCES : D_ENOENT);
        } else {
            fd = _wopen(h.c_str(), host_open_flags(flags), _S_IREAD | _S_IWRITE);
            if (fd < 0) fail_crt();
        }
        LOG_DEBUG("open(%s, 0x%x) = %d", path, flags, fd);
        t.set_x(0, (u64)(s64)fd);
    });
    fn("_close", [](int fd) { return _close(fd) < 0 ? fail_crt() : 0; });
    fn("_read", [](int fd, void* buf, u64 n) -> s64 {
        int r = _read(fd, buf, (unsigned)std::min<u64>(n, 0x7fffffff));
        return r < 0 ? fail_crt() : r;
    });
    fn("_write", [](int fd, const void* buf, u64 n) -> s64 {
        if (fd == 1 || fd == 2) {
            std::string s(static_cast<const char*>(buf), n);
            while (!s.empty() && s.back() == '\n') s.pop_back();
            LOG_INFO("[guest fd%d] %s", fd, s.c_str());
            return (s64)n;
        }
        int r = _write(fd, buf, (unsigned)std::min<u64>(n, 0x7fffffff));
        return r < 0 ? fail_crt() : r;
    });
    fn("_lseek", [](int fd, s64 off, int whence) -> s64 {
        s64 r = _lseeki64(fd, off, whence);
        return r < 0 ? fail_crt() : r;
    });
    fn("_fsync", [](int fd) { return _commit(fd) < 0 ? fail_crt() : 0; });
    fn("_fstat", [](int fd, DarwinStat* st) {
        struct _stat64 s;
        if (_fstat64(fd, &s) < 0) return fail_crt();
        fill_stat(st, s);
        return 0;
    });
    fn("_stat", [](const char* path, DarwinStat* st) {
        std::wstring h = vfs::to_host_w(path);
        struct _stat64 s;
        if (h.empty()) {
            vfs::set_errno(D_ENOENT);
            return -1;
        }
        while (h.size() > 3 && (h.back() == L'/' || h.back() == L'\\')) h.pop_back();
        if (_wstat64(h.c_str(), &s) < 0) return fail_crt();
        fill_stat(st, s);
        return 0;
    });
    fn("_access", [](const char* path, int mode) {
        std::wstring h = vfs::to_host_w(path);
        if (h.empty() || _waccess(h.c_str(), 0) < 0) {
            vfs::set_errno(D_ENOENT);
            return -1;
        }
        return 0;
    });
    fn("_mkdir", [](const char* path, u32) {
        std::wstring h = vfs::to_host_w(path);
        if (h.empty()) {
            vfs::set_errno(D_EACCES);
            return -1;
        }
        return _wmkdir(h.c_str()) < 0 ? fail_crt() : 0;
    });
    fn("_rmdir", [](const char* path) {
        std::wstring h = vfs::to_host_w(path);
        return h.empty() || _wrmdir(h.c_str()) < 0 ? fail_crt() : 0;
    });
    fn("_unlink", [](const char* path) {
        std::wstring h = vfs::to_host_w(path);
        return h.empty() || _wunlink(h.c_str()) < 0 ? fail_crt() : 0;
    });
    fn("_rename", [](const char* a, const char* b) {
        std::wstring ha = vfs::to_host_w(a), hb = vfs::to_host_w(b);
        if (ha.empty() || hb.empty()) return fail_crt();
        return MoveFileExW(ha.c_str(), hb.c_str(), MOVEFILE_REPLACE_EXISTING) ? 0 : (vfs::set_errno(D_ENOENT), -1);
    });
    fn("_chmod", [](const char*, u32) { return 0; });
    fn("_utimes", [](const char*, void*) { return 0; });
    fn("_setxattr", [](const char*, const char*, void*, u64, u32, int) { return 0; });
    fn("_symlink", [](const char*, const char*) {
        vfs::set_errno(D_EACCES);
        return -1;
    });
    fn("_chdir", [](const char* path) {
        vfs::set_cwd(path);
        return 0;
    });
    fn("_mkstemp", [](char* tmpl) {
        size_t n = std::strlen(tmpl);
        for (int attempt = 0; attempt < 100; attempt++) {
            for (size_t i = n; i > 0 && tmpl[i - 1] == 'X'; i--) tmpl[i - 1] = "abcdefghijklmnopqrstuvwxyz0123456789"[rand() % 36];
            std::wstring h = vfs::to_host_w(tmpl);
            if (h.empty()) break;
            int fd = _wopen(h.c_str(), _O_BINARY | _O_RDWR | _O_CREAT | _O_EXCL, _S_IREAD | _S_IWRITE);
            if (fd >= 0) return fd;
        }
        return fail_crt();
    });
    hle::raw("_fcntl", [](cpu::Thread& t) {
        int cmd = (int)t.x(1);
        LOG_DEBUG("fcntl(%d, %d)", (int)t.x(0), cmd);
        t.set_x(0, 0);  // F_NOCACHE, F_RDAHEAD, F_FULLFSYNC etc.: accept
    });

    fn("_opendir", [](const char* path) -> void* {
        std::wstring h = vfs::to_host_w(path);
        if (h.empty()) {
            vfs::set_errno(D_ENOENT);
            return nullptr;
        }
        auto* d = new Dir;
        d->find = FindFirstFileW((h + L"\\*").c_str(), &d->data);
        if (d->find == INVALID_HANDLE_VALUE) {
            delete d;
            vfs::set_errno(D_ENOENT);
            return nullptr;
        }
        return d;
    });
    fn("_readdir", [](Dir* d) -> DarwinDirent* {
        if (!d) return nullptr;
        if (!d->first && !FindNextFileW(d->find, &d->data)) return nullptr;
        d->first = false;
        std::string name = wide_to_utf8(d->data.cFileName);
        std::memset(&d->ent, 0, sizeof(d->ent));
        d->ent.d_ino = 1;
        d->ent.d_reclen = sizeof(DarwinDirent);
        d->ent.d_namlen = (u16)std::min<size_t>(name.size(), 1023);
        d->ent.d_type = (d->data.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) ? 4 : 8;
        std::memcpy(d->ent.d_name, name.c_str(), d->ent.d_namlen);
        return &d->ent;
    });
    fn("_closedir", [](Dir* d) {
        if (!d) return -1;
        FindClose(d->find);
        delete d;
        return 0;
    });
}

}  // namespace libc
