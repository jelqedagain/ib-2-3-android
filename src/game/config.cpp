// Applies the launcher's graphics and frame-rate settings by overriding IB3's own config values
// right after the engine has loaded its (encrypted) coalesced .ini files, before anything reads
// them. The game runs with the iPhone 6 Plus profile, so device settings go into that section.
#include "game/game.h"
#include "game/unreal.h"
#include "hook.h"
#include "macho.h"
#include "settings.h"

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
    std::string fps = s.max_fps >= 60 ? "60" : "30";
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

}  // namespace

void install_config(const macho::Image& img) {
    GuestAddr load = img.find("__ZN15FConfigCacheIni17LoadCoalescedFileEPKw");
    g_set_string = img.find("__ZN15FConfigCacheIni9SetStringEPKwS1_S1_S1_");
    g_engine_ini = img.find("_GEngineIni");
    g_system_ini = img.find("_GSystemSettingsIni");
    if (!load || !g_set_string || !g_engine_ini || !g_system_ini) {
        LOG_WARN("settings: engine config functions not found; graphics settings not applied");
        return;
    }
    g_load_original = hook::install(load, "FConfigCacheIni::LoadCoalescedFile", on_load_coalesced);
}

}  // namespace game
