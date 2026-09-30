// Infinity Blade II on phones longer than 16:9. Its script (GameViewportClient.AdjustHUDRenderSize)
// lays the HUD out over the whole screen only for aspect ratios in (1.7, 1.8) and exactly 1.5; any
// other shape is taken for an iPad and gets a 3:2 HUD as wide as the screen, which on a 19.5:9 phone
// runs off the top and bottom (and the touch zones follow it). The game is served a copy of its
// script package with the 1.8 limit raised, so wider screens take the 16:9 branch all the way through.
// The menus' swirl backdrops (two flat meshes per menu camera in IB2_InventoryMenu) only cover a
// 16:9 view, and the camera widens its view with the screen: they are scaled up to match.
// The installed game files are not changed; patched copies go to Library/Caches/port.
#include "game/game.h"
#include "libc/vfs.h"
#include "uikit/uikit.h"
#include <cctype>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <vector>

namespace game {

namespace {

constexpr double kWidestKnown = 1.8;  // the game's own upper limit for its 16:9 layout
constexpr u8 kFloatConst = 0x1e;      // UnrealScript bytecode: EX_FloatConst

bool is_infinity_blade_2() {
    std::ifstream f(vfs::host_bundle() + "/Info.plist", std::ios::binary);
    std::string plist((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    static const std::string id = "com.chairentertainment.IB2";
    size_t at = plist.find(id);
    return at != std::string::npos && (at + id.size() == plist.size() || !std::isalnum((unsigned char)plist[at + id.size()]));
}

std::string bytes_of(const void* p, size_t n) { return std::string((const char*)p, n); }

std::string float_const(float v) { return (char)kFloatConst + bytes_of(&v, 4); }

std::vector<size_t> find_all(const std::string& data, const std::string& pattern) {
    std::vector<size_t> hits;
    for (size_t at = data.find(pattern); at != std::string::npos; at = data.find(pattern, at + 1)) hits.push_back(at);
    return hits;
}

s32 read_s32(const std::string& data, size_t at) {
    s32 v = 0;
    if (at + 4 <= data.size()) memcpy(&v, &data[at], 4);
    return v;
}

// Index of `name` in an Unreal package's name table, or -1.
s32 name_index(const std::string& pkg, const std::string& name) {
    size_t p = 12;  // Tag, FileVersion, TotalHeaderSize
    s32 folder = read_s32(pkg, p);
    p += 4 + (folder >= 0 ? (size_t)folder : (size_t)-(s64)folder * 2);
    s32 count = read_s32(pkg, p + 4);  // after PackageFlags
    p = (size_t)read_s32(pkg, p + 8);
    for (s32 i = 0; i < count && p + 4 <= pkg.size(); i++) {
        s32 len = read_s32(pkg, p);
        p += 4;
        size_t bytes = len >= 0 ? (size_t)len : (size_t)-(s64)len * 2;
        if (len > 0 && (size_t)len == name.size() + 1 && pkg.compare(p, name.size(), name) == 0) return i;
        p += bytes + 8;  // + object flags
    }
    return -1;
}

// SwordGame.xxx: "AspectRatio > 1.7 && AspectRatio < 1.8" in AdjustHUDRenderSize. The 1.8 is the
// only one in the package's script; the 1.7 comes a few bytes before it.
bool raise_hud_limit(std::string& pkg) {
    std::vector<size_t> hits = find_all(pkg, float_const(1.8f));
    if (hits.size() != 1) return false;
    size_t low = pkg.rfind(float_const(1.7f), hits[0]);
    if (low == std::string::npos || hits[0] - low > 32) return false;
    float widest = 100.0f;
    memcpy(&pkg[hits[0] + 1], &widest, 4);
    return true;
}

// IB2_InventoryMenu.xxx: the six backdrop meshes are the package's only "DrawScale = 1.2" tags.
bool scale_backdrops(std::string& pkg, float scale) {
    s32 draw_scale = name_index(pkg, "DrawScale"), float_property = name_index(pkg, "FloatProperty");
    if (draw_scale < 0 || float_property < 0) return false;
    const float original = 1.2f;
    const s32 tag[6] = {draw_scale, 0, float_property, 0, 4, 0};  // name, type, size, array index
    std::vector<size_t> hits = find_all(pkg, bytes_of(tag, sizeof tag) + bytes_of(&original, 4));
    if (hits.size() != 6) return false;
    float value = original * scale;
    for (size_t at : hits) memcpy(&pkg[at + sizeof tag], &value, 4);
    return true;
}

// Serves the game a patched copy of one of its bundle files; false if `patch` did not apply.
template <class Patch>
bool serve_patched(const std::string& bundle_relative, Patch patch) {
    std::ifstream in(vfs::host_bundle() + "/" + bundle_relative, std::ios::binary);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.empty() || !patch(data)) return false;
    std::string dir = vfs::host_home() + "/Library/Caches/port";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::string path = dir + "/" + std::filesystem::path(bundle_relative).filename().string();
    std::ofstream out(path, std::ios::binary);
    out << data;
    out.close();
    if (!out) return false;
    vfs::override_bundle_file(bundle_relative, path);
    return true;
}

}  // namespace

void install_widescreen() {
    double aspect = uikit::g_device.width_pt / uikit::g_device.height_pt;
    if (aspect <= kWidestKnown || !is_infinity_blade_2()) return;
    if (!serve_patched("CookedIPhone/SwordGame.xxx", raise_hud_limit)) {
        uikit::g_device.width_pt = std::round(uikit::g_device.height_pt * 16 / 9);
        LOG_WARN("widescreen: HUD layout not found in SwordGame.xxx; keeping the 16:9 screen");
        return;
    }
    LOG_INFO("widescreen: HUD laid out for the full %.0fx%.0f screen", uikit::g_device.width_pt, uikit::g_device.height_pt);
    // Measured on a 19.5:9 phone: the backdrops need about this much more width than the 16:9 view.
    float scale = (float)(aspect / 1.55);
    if (serve_patched("CookedIPhone/IB2_InventoryMenu.xxx", [&](std::string& pkg) { return scale_backdrops(pkg, scale); }))
        LOG_INFO("widescreen: menu backdrops scaled by %.2f", scale);
    else
        LOG_WARN("widescreen: menu backdrops not found in IB2_InventoryMenu.xxx; menus may show the world at the sides");
}

}  // namespace game
