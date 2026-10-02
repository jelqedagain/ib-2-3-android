// Cache of the ETC2 re-encodings of the games' PVRTC textures (gl.cpp), so a texture is decoded and
// encoded once instead of every time a level loads it: on slower phones the re-encoding added seconds
// to every area change. One append-only file in the app's internal storage (not in userdata, so not in
// save backups; removed with the app), indexed when it is first opened.
#ifdef __ANDROID__
#include "gles/gl.h"
#include "settings.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/system_properties.h>
#include <unistd.h>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace gles {

namespace {

// Bump kVersion when the encoder's output changes: older caches are then started over.
constexpr u32 kFileMagic = 0x43544249;  // "IBTC"
constexpr u32 kVersion = 1;
constexpr u32 kRecordMagic = 0x31434552;  // "REC1"
constexpr u64 kMaxBytes = 1ull << 30;     // no new entries past 1 GB

struct Header {
    u32 magic, version;
};
struct Record {
    u32 magic, size;
    u64 key;
};
struct Entry {
    u64 offset;  // of the data
    u32 size;
};

std::mutex g_mutex;
std::string g_dir;
int g_fd = -1;
bool g_tried = false;
u64 g_end = 0;
std::unordered_map<u64, Entry> g_index;

bool disabled_for_tests() {
    char v[PROP_VALUE_MAX] = "";
    return __system_property_get("debug.ibport.texcache", v) > 0 && v[0] == '0';
}

// Opens (or starts) the cache file and indexes its records; a record cut short (the app was closed
// while writing it) and everything after it are dropped.
bool open_locked() {
    if (g_tried) return g_fd >= 0;
    g_tried = true;
    if (g_dir.empty()) return false;
    std::string path = g_dir + "/etc2-cache.bin";
    if (!settings::get().texture_cache || disabled_for_tests()) {
        if (unlink(path.c_str()) == 0) LOG_INFO("texture cache: off, deleted");
        return false;
    }
    g_fd = ::open(path.c_str(), O_RDWR | O_CREAT | O_CLOEXEC, 0600);
    if (g_fd < 0) {
        LOG_WARN("texture cache: cannot open %s", path.c_str());
        return false;
    }
    struct stat st;
    fstat(g_fd, &st);
    u64 size = (u64)st.st_size;
    Header h{};
    if (size < sizeof h || pread(g_fd, &h, sizeof h, 0) != (ssize_t)sizeof h || h.magic != kFileMagic || h.version != kVersion) {
        if (size) LOG_INFO("texture cache: starting over (old format)");
        h = {kFileMagic, kVersion};
        if (ftruncate(g_fd, 0) != 0 || pwrite(g_fd, &h, sizeof h, 0) != (ssize_t)sizeof h) {
            close(g_fd);
            g_fd = -1;
            return false;
        }
        size = sizeof h;
    }
    u64 at = sizeof h;
    while (at + sizeof(Record) <= size) {
        Record r{};
        if (pread(g_fd, &r, sizeof r, at) != (ssize_t)sizeof r || r.magic != kRecordMagic || at + sizeof r + r.size > size) break;
        g_index[r.key] = {at + sizeof r, r.size};
        at += sizeof r + r.size;
    }
    if (at < size && ftruncate(g_fd, at) != 0) LOG_WARN("texture cache: cannot drop a cut-short entry");
    g_end = at;
    LOG_INFO("texture cache: %zu textures, %llu MB", g_index.size(), (unsigned long long)(g_end >> 20));
    return true;
}

}  // namespace

void set_texture_cache_dir(const char* dir) {
    std::lock_guard lock(g_mutex);
    g_dir = dir ? dir : "";
}

u64 texture_cache_key(const void* data, size_t size, u32 format, int w, int h) {
    u64 x = 0x9E3779B97F4A7C15ull ^ ((u64)format << 40) ^ ((u64)(u32)w << 20) ^ (u64)(u32)h ^ ((u64)size << 48);
    auto mix = [&](u64 v) {
        x ^= v;
        x *= 0xBF58476D1CE4E5B9ull;
        x ^= x >> 31;
    };
    const u8* p = static_cast<const u8*>(data);
    size_t i = 0;
    for (; i + 8 <= size; i += 8) {
        u64 v;
        std::memcpy(&v, p + i, 8);
        mix(v);
    }
    u64 tail = 0;
    std::memcpy(&tail, p + i, size - i);
    mix(tail ^ size);
    x *= 0x94D049BB133111EBull;
    return x ^ (x >> 29);
}

bool texture_cache_get(u64 key, std::vector<u64>& blocks) {
    std::lock_guard lock(g_mutex);
    if (!open_locked()) return false;
    auto it = g_index.find(key);
    if (it == g_index.end()) return false;
    blocks.resize(it->second.size / 8);
    if (pread(g_fd, blocks.data(), it->second.size, (off_t)it->second.offset) != (ssize_t)it->second.size) {
        g_index.erase(it);
        return false;
    }
    return true;
}

void texture_cache_put(u64 key, const std::vector<u64>& blocks) {
    std::lock_guard lock(g_mutex);
    if (!open_locked() || g_index.count(key) || g_end > kMaxBytes) return;
    Record r{kRecordMagic, (u32)(blocks.size() * 8), key};
    if (pwrite(g_fd, &r, sizeof r, (off_t)g_end) != (ssize_t)sizeof r ||
        pwrite(g_fd, blocks.data(), r.size, (off_t)(g_end + sizeof r)) != (ssize_t)r.size) {
        if (ftruncate(g_fd, (off_t)g_end) != 0) {}  // storage full: keep what is there
        return;
    }
    g_index[key] = {g_end + sizeof r, r.size};
    g_end += sizeof r + r.size;
}

}  // namespace gles
#endif
