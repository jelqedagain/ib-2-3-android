// IB3 "Dev Mod + Community Patch" .ipa: its SwordGame.xxx carries the mod's own
// SwordUserOptionsList.Init (the Options list with the developer rows) as an orphaned block, while the
// package's export table still points Init at the Community Patch's copy, so no developer rows ever
// showed. When the package has that shape (Init followed by nothing, and an unused block between two
// exports that starts like Init's function header), the game is served a copy whose Init points at the
// mod's block. The installed files are not changed; the copy goes to Library/Caches/port.
#include "game/game.h"
#include "game/unreal.h"
#include "libc/vfs.h"
#include <windows.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

namespace game {

namespace {

constexpr size_t kHeaderMatch = 24;  // UStruct header bytes before the script sizes (Next, Super, Children...)

struct Reader {
    const std::string& d;
    size_t p = 0;
    bool ok = true;
    s32 s32v() {
        if (p + 4 > d.size()) {
            ok = false;
            return 0;
        }
        s32 v;
        memcpy(&v, &d[p], 4);
        p += 4;
        return v;
    }
    std::string fstr() {
        s32 n = s32v();
        size_t bytes = n >= 0 ? (size_t)n : (size_t)(-(s64)n) * 2;
        if (!ok || p + bytes > d.size()) {
            ok = false;
            return {};
        }
        std::string s = n > 0 ? d.substr(p, (size_t)n - 1) : std::string();
        p += bytes;
        return s;
    }
};

struct Export {
    s32 outer, name;
    s32 size, off;
    size_t size_at, off_at;
};

// Exports of a cooked UE3 package (IB3's version 868 layout), or empty if it doesn't parse.
std::vector<Export> read_exports(const std::string& d, std::vector<std::string>& names) {
    Reader r{d};
    r.p = 12;   // tag, version, header size
    r.fstr();   // folder
    r.s32v();   // package flags
    s32 name_count = r.s32v(), name_off = r.s32v(), export_count = r.s32v(), export_off = r.s32v();
    if (!r.ok || name_count <= 0 || export_count <= 0) return {};
    r.p = (size_t)name_off;
    for (s32 i = 0; i < name_count && r.ok; i++) {
        names.push_back(r.fstr());
        r.p += 8;  // flags
    }
    r.p = (size_t)export_off;
    std::vector<Export> out;
    for (s32 i = 0; i < export_count && r.ok; i++) {
        Export e{};
        r.s32v(), r.s32v();  // class, super
        e.outer = r.s32v();
        e.name = r.s32v();
        r.s32v();   // name number
        r.s32v();   // archetype
        r.p += 8;   // object flags
        e.size_at = r.p;
        e.size = r.s32v();
        e.off_at = r.p;
        e.off = r.s32v();
        r.s32v();   // export flags
        s32 gens = r.s32v();
        r.p += 4 * (size_t)std::max(gens, 0) + 16 + 4;  // generation counts, GUID, package flags
        out.push_back(e);
    }
    return r.ok ? out : std::vector<Export>{};
}

bool repoint_options_init(std::string& pkg) {
    std::vector<std::string> names;
    std::vector<Export> ex = read_exports(pkg, names);
    if (ex.empty()) return false;
    auto name_of = [&](s32 i) { return i >= 0 && (size_t)i < names.size() ? names[(size_t)i] : std::string(); };
    const Export* init = nullptr;
    for (const Export& e : ex)
        if (name_of(e.name) == "Init" && e.outer > 0 && (size_t)e.outer <= ex.size() &&
            name_of(ex[(size_t)e.outer - 1].name) == "SwordUserOptionsList")
            init = &e;
    if (!init || init->off <= 0 || (size_t)init->off + kHeaderMatch > pkg.size()) return false;
    // Unused stretches between exports, sorted by offset.
    std::vector<const Export*> sorted;
    for (const Export& e : ex)
        if (e.size > 0) sorted.push_back(&e);
    std::sort(sorted.begin(), sorted.end(), [](const Export* a, const Export* b) { return a->off < b->off; });
    for (size_t i = 0; i + 1 < sorted.size(); i++) {
        size_t gap = (size_t)sorted[i]->off + (size_t)sorted[i]->size, next = (size_t)sorted[i + 1]->off;
        if (next <= gap || next - gap <= (size_t)init->size || next - gap > 64 * 1024) continue;
        if (pkg.compare(gap, kHeaderMatch, pkg, (size_t)init->off, kHeaderMatch) != 0) continue;
        // The Community Patch also leaves the game's own older Init behind (smaller than its own): only
        // the dev mod's block, which has the developer rows, is taken.
        if (pkg.find("Developer", gap) >= next) continue;
        s32 size = (s32)(next - gap), off = (s32)gap;
        memcpy(&pkg[init->size_at], &size, 4);
        memcpy(&pkg[init->off_at], &off, 4);
        LOG_INFO("devipa: the Options list's Init points at the mod's own copy (%d bytes at %d, was %d at %d)", size, off,
                 init->size, init->off);
        return true;
    }
    return false;
}

GuestAddr options_scene(cpu::Thread& t) {
    GuestAddr input = ue::player_input(t);
    ue::TArray<u64> stack{};
    if (!input || !ue::read_property(t, input, "MobileMenuStack", stack)) return 0;
    for (int i = stack.num - 1; i >= 0; i--)
        if (GuestAddr scene = stack.at(i); scene && ue::is_a(t, scene, "SwordUserOptionsScene")) return scene;
    return 0;
}

// Loads (or frees) every item cache of the engine, as the menus do (Load / Free count requests).
void item_caches(cpu::Thread& t, bool load) {
    GuestAddr engine = ue::engine();
    ue::TArray<GuestAddr> caches{}, classes{};
    if (!ue::read_property(t, engine, "ItemCaches", caches) || !ue::read_property(t, engine, "ItemCacheClasses", classes)) return;
    for (int i = 0; i < caches.num; i++) {
        GuestAddr cache = caches.at(i);
        int off = cache ? ue::property_offset(t, cache, "CurrentClass") : -1;
        if (off < 0) continue;
        if (!*gptr<GuestAddr>(cache + off) && i < classes.num) *gptr<GuestAddr>(cache + off) = classes.at(i);
        alignas(16) u8 params[256] = {};
        ue::call_event(t, cache, load ? "Load" : "Free", params);
    }
    LOG_INFO("devipa: item caches %s while the Options list is open", load ? "loaded" : "freed");
}

}  // namespace

// IB2's give-all cheat (SwordPlayer.GiveAllItems, the Options list's "Give All Items" row of the dev
// mod .ipa and of developer mode) hands out what the engine's item caches hold, and those are loaded
// only by the inventory menus, so it gave nothing. They are kept loaded while the Options list is open.
void options_item_caches_tick(cpu::Thread& t) {
    static bool loaded = false;
    static u64 last = 0;
    if (!is_ib2()) return;
    u64 now = GetTickCount64();
    if (now - last < 250) return;
    last = now;
    bool open = options_scene(t) != 0;
    if (open == loaded) return;
    item_caches(t, open);
    loaded = open;
}

void install_dev_ipa_fix() {
    if (is_ib2()) return;
    const std::string rel = "CookedIPhone/SwordGame.xxx";
    std::ifstream in(vfs::host_bundle() + "/" + rel, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    std::string dir = vfs::host_home() + "/Library/Caches/port", path = dir + "/SwordGame.xxx";
    std::error_code ec;
    if (data.empty() || !repoint_options_init(data)) {
        std::filesystem::remove(path, ec);  // a copy from a dev mod .ipa installed before
        return;
    }
    std::filesystem::create_directories(dir, ec);
    std::ofstream out(path, std::ios::binary);
    out << data;
    out.close();
    if (out) vfs::override_bundle_file(rel, path);
}

}  // namespace game
