// Applies the launcher's graphics and frame-rate settings by overriding IB3's own config values
// right after the engine has loaded its (encrypted) coalesced .ini files, before anything reads
// them. The game runs with the iPhone 6 Plus profile, so device settings go into that section.
#include "game/game.h"
#include "game/unreal.h"
#include "hook.h"
#include "libc/vfs.h"
#include "macho.h"
#include "settings.h"
#include <filesystem>
#include <fstream>
#include <iterator>

namespace game {

namespace {

GuestAddr g_load_original = 0;  // FConfigCacheIni::LoadCoalescedFile
GuestAddr g_set_string = 0;     // FConfigCacheIni::SetString(Section, Key, Value, Filename)
GuestAddr g_engine_ini = 0;     // GEngineIni (wchar_t[])
GuestAddr g_system_ini = 0;     // GSystemSettingsIni

constexpr const char* kDevice = "SystemSettingsIPhone6Plus";

std::string read_wide(GuestAddr p) {
    std::string s;
    for (const u32* w = gptr<u32>(p); *w && s.size() < 260; w++) s += *w < 0x80 ? (char)*w : '?';
    return s;
}

void apply(cpu::Thread& t, GuestAddr config) {
    const settings::Settings& s = settings::get();
    auto set = [&](const char* section, const char* key, const std::string& value, GuestAddr file) {
        t.call(g_set_string, {config, gaddr(ue::wide(section)), gaddr(ue::wide(key)), gaddr(ue::wide(value)), file});
    };
    auto flag = [](bool b) { return std::string(b ? "True" : "False"); };

    // UE3's frame smoothing caps each frame at a slow running average of recent frame times,
    // clamped to [Min, Max]SmoothedFrameRate. Loading hitches drag the average down and the cap
    // cannot climb back (the limiter sleeps to hold it), so the game stayed at ~40 in 60 FPS mode.
    // Pin the range to the chosen rate.
    std::string fps = std::to_string(s.max_fps);
    set("Engine.Engine", "MinSmoothedFrameRate", fps, g_engine_ini);
    set("Engine.Engine", "MaxSmoothedFrameRate", fps, g_engine_ini);

    char scale[32];
    snprintf(scale, sizeof scale, "%.4f", s.render_height / 414.0);  // 414 points tall
    set(kDevice, "MobileContentScaleFactor", scale, g_system_ini);
    set(kDevice, "MobileEnableMSAA", flag(s.anti_aliasing == 2), g_system_ini);
    set(kDevice, "MobileFXAAQuality", s.anti_aliasing == 1 ? "1" : "0", g_system_ini);
    set(kDevice, "DynamicShadows", flag(s.dynamic_shadows), g_system_ini);
    set(kDevice, "MobileModShadows", flag(s.dynamic_shadows), g_system_ini);
    set(kDevice, "MaxShadowResolution", s.high_res_shadows ? "2048" : "1024", g_system_ini);
    set(kDevice, "MobileShadowTextureResolution", s.high_res_shadows ? "4096" : "2048", g_system_ini);
    set(kDevice, "bAllowLightShafts", flag(s.light_shafts), g_system_ini);
    set(kDevice, "Bloom", flag(s.bloom), g_system_ini);
    set(kDevice, "DepthOfField", flag(s.depth_of_field), g_system_ini);
    // The bloom / depth-of-field blur (GaussianBlurFilterBuffer) widens its kernel with the render width
    // (width / 1280) and keeps only the first MaxFilterBlurSampleCount taps, 4 on iOS: enough up to the 2048
    // pixels of iOS screens. Wider renders (1440p, 1080p on phones longer than 16:9) lost the taps on one
    // side, so the blur shifted and drew a jagged dark fringe beside everything in focus. 16 is the engine's
    // own default; renders up to 2048 wide never need more than 4, so they are unchanged.
    set(kDevice, "MaxFilterBlurSampleCount", "16", g_system_ini);
    set(kDevice, "MaxAnisotropy", std::to_string(s.anisotropy), g_system_ini);
    LOG_INFO("settings: %s, %d fps cap, AA %d, shadows %d%s, light shafts %d, bloom %d, DoF %d, aniso %d (%s)", scale,
             s.max_fps, s.anti_aliasing, s.dynamic_shadows, s.high_res_shadows ? " (high res)" : "", s.light_shafts,
             s.bloom, s.depth_of_field, s.anisotropy, read_wide(g_system_ini).c_str());
}

void on_load_coalesced(cpu::Thread& t) {
    GuestAddr config = t.x(0), language = t.x(1);
    t.call(g_load_original, {config, language});
    apply(t, config);
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), {});
}

// For games whose engine functions cannot be found by name (Infinity Blade II ships with them
// stripped), the settings become console commands the engine runs at startup instead: its command
// line (CookedIPhone/UE3CommandLine.txt) can run a file of commands, as the Community Patches do
// with Binaries/Commands.txt. The game is served copies of both with the settings added.
// Only the shadow resolution is changed: turning on effects the game's own settings leave off (bloom,
// depth of field...) broke Infinity Blade II's picture.
void apply_as_startup_commands() {
    const settings::Settings& s = settings::get();
    std::string commands = read_file(vfs::host_bundle() + "/Binaries/Commands.txt");
    commands += "\n; Added by the port: sharper character shadows at today's screen resolutions\n";
    commands += std::string("Scale Set MaxShadowResolution ") + (s.high_res_shadows ? "2048" : "1024") + "\n";
    // The Community Patch caps the frame rate at 62 (about 60); a later line wins.
    if (s.max_fps != 60)
        commands += "Set Engine MaxSmoothedFrameRate " + std::to_string(s.max_fps == 30 ? 30 : s.max_fps + 2) + "\n";

    std::string command_line = read_file(vfs::host_bundle() + "/CookedIPhone/UE3CommandLine.txt");
    if (command_line.find("-exec=") == std::string::npos) command_line += " -exec=\"Commands.txt\"";
    else if (command_line.find("Commands.txt") == std::string::npos) {
        LOG_WARN("settings: the game's command line runs another file; graphics settings not applied");
        return;
    }

    std::string dir = vfs::host_home() + "/Library/Caches/port";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    std::ofstream(dir + "/Commands.txt", std::ios::binary) << commands;
    std::ofstream(dir + "/UE3CommandLine.txt", std::ios::binary) << command_line;
    vfs::override_bundle_file("Binaries/Commands.txt", dir + "/Commands.txt");
    vfs::override_bundle_file("CookedIPhone/UE3CommandLine.txt", dir + "/UE3CommandLine.txt");
    LOG_INFO("settings: shadow resolution %s, %d fps cap set by startup commands", s.high_res_shadows ? "2048" : "1024",
             s.max_fps);
}

}  // namespace

void install_config(const macho::Image& img) {
    GuestAddr load = img.find("__ZN15FConfigCacheIni17LoadCoalescedFileEPKw");
    g_set_string = img.find("__ZN15FConfigCacheIni9SetStringEPKwS1_S1_S1_");
    g_engine_ini = img.find("_GEngineIni");
    g_system_ini = img.find("_GSystemSettingsIni");
    if (!load || !g_set_string || !g_engine_ini || !g_system_ini) {
        LOG_WARN("settings: engine config functions not found; the game keeps its own graphics settings");
        apply_as_startup_commands();
        return;
    }
    g_load_original = hook::install(load, "FConfigCacheIni::LoadCoalescedFile", on_load_coalesced);
}

}  // namespace game
