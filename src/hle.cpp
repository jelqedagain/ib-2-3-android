#include "hle.h"
#include "macho.h"
#include <atomic>
#include <mutex>
#include <unordered_map>
#include <unordered_set>

namespace hle {

namespace {
struct Entry {
    cpu::Handler handler;
    GuestAddr addr = 0;  // stub or data address once materialized
    std::function<GuestAddr()> make_data;
    bool is_data = false;
};
std::recursive_mutex g_mutex;
std::unordered_map<std::string, Entry>& registry() {
    static std::unordered_map<std::string, Entry> r;
    return r;
}

GuestAddr unimplemented_stub(const std::string& sym) {
    auto count = std::make_shared<std::atomic<int>>(0);
    return cpu::make_stub(sym, [sym, count](cpu::Thread& t) {
        int n = (*count)++;
        if (n < 5 || logging::enabled(logging::Level::Debug))
            LOG_WARN("UNIMPLEMENTED %s(0x%llx, 0x%llx, 0x%llx) from %s", sym.c_str(), (unsigned long long)t.x(0),
                     (unsigned long long)t.x(1), (unsigned long long)t.x(2), cpu::symbolize(t.x(30)).c_str());
        t.set_x(0, 0);
    });
}
}  // namespace

void raw(const std::string& sym, cpu::Handler h) {
    std::lock_guard lock(g_mutex);
    auto& e = registry()[sym];
    if (e.addr) LOG_WARN("HLE symbol %s registered after it was bound", sym.c_str());
    e.handler = std::move(h);
}

void data(const std::string& sym, GuestAddr addr) {
    std::lock_guard lock(g_mutex);
    auto& e = registry()[sym];
    e.is_data = true;
    e.addr = addr;
}

GuestAddr native(const std::string& sym, std::initializer_list<u32> code) {
    auto* p = static_cast<u32*>(alloc_static(code.size() * 4, 16));
    std::copy(code.begin(), code.end(), p);
    data(sym, gaddr(p));
    return gaddr(p);
}

void data_lazy(const std::string& sym, std::function<GuestAddr()> make) {
    std::lock_guard lock(g_mutex);
    auto& e = registry()[sym];
    e.is_data = true;
    e.make_data = std::move(make);
}

std::vector<std::function<GuestAddr(const std::string&)>>& resolvers() {
    static std::vector<std::function<GuestAddr(const std::string&)>> r;
    return r;
}

void add_resolver(std::function<GuestAddr(const std::string&)> r) {
    std::lock_guard lock(g_mutex);
    resolvers().push_back(std::move(r));
}

GuestAddr lookup(const std::string& sym) {
    std::lock_guard lock(g_mutex);
    auto it = registry().find(sym);
    if (it == registry().end()) {
        if (auto* img = cpu::image()) {
            auto ex = img->exports.find(sym);
            if (ex != img->exports.end()) return ex->second;
        }
        for (auto& r : resolvers()) {
            if (GuestAddr a = r(sym)) {
                auto& e = registry()[sym];
                e.is_data = true;
                e.addr = a;
                return a;
            }
        }
        return 0;
    }
    auto& e = it->second;
    if (!e.addr) {
        if (e.is_data && e.make_data) e.addr = e.make_data();
        else if (e.handler) e.addr = cpu::make_stub(sym, e.handler);
    }
    return e.addr;
}

GuestAddr resolve(const std::string& sym) {
    std::lock_guard lock(g_mutex);
    if (GuestAddr a = lookup(sym)) return a;
    auto& e = registry()[sym];
    e.addr = unimplemented_stub(sym);
    return e.addr;
}

void bind_image(const macho::Image& img) {
    std::unordered_set<std::string> missing;
    for (auto& b : img.binds) {
        GuestAddr target;
        auto self = img.exports.find(b.symbol);
        if (b.dylib_ordinal <= 0 && self != img.exports.end()) {
            target = self->second;
        } else {
            std::lock_guard lock(g_mutex);
            target = lookup(b.symbol);
            if (!target) {
                missing.insert(b.symbol);
                target = resolve(b.symbol);
            }
        }
        *gptr<u64>(b.addr) = target + b.addend;
    }
    LOG_INFO("bound %zu locations; %zu imported symbols have no implementation yet", img.binds.size(), missing.size());
    if (logging::enabled(logging::Level::Debug))
        for (auto& m : missing) LOG_DEBUG("  missing: %s", m.c_str());
}

void* alloc_static(size_t size, size_t align) {
    static std::mutex m;
    static u8* cur = nullptr;
    static size_t left = 0;
    std::lock_guard lock(m);
    size_t pad = (align - (reinterpret_cast<uintptr_t>(cur) & (align - 1))) & (align - 1);
    if (!cur || pad + size > left) {
        size_t chunk = std::max<size_t>(size + align, 1 << 20);
        cur = static_cast<u8*>(std::calloc(1, chunk));
        left = chunk;
        pad = (align - (reinterpret_cast<uintptr_t>(cur) & (align - 1))) & (align - 1);
    }
    u8* p = cur + pad;
    cur += pad + size;
    left -= pad + size;
    return p;
}

const char* static_cstr(std::string_view s) {
    char* p = static_cast<char*>(alloc_static(s.size() + 1, 1));
    std::memcpy(p, s.data(), s.size());
    p[s.size()] = 0;
    return p;
}

}  // namespace hle
