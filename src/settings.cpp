#include "settings.h"
#include <filesystem>
#include <windows.h>

namespace settings {

namespace {

Settings g_settings;
bool g_loaded = false;

std::wstring widen(const char* s) {
    std::wstring w;
    for (; *s; s++) w += (wchar_t)(u8)*s;
    return w;
}

int read_int(const wchar_t* section, const wchar_t* key, int def) {
    return (int)GetPrivateProfileIntW(section, key, def, path().c_str());
}
void write_int(const wchar_t* section, const wchar_t* key, int v) {
    WritePrivateProfileStringW(section, key, std::to_wstring(v).c_str(), path().c_str());
}

int clamp(int v, int lo, int hi) { return v < lo ? lo : v > hi ? hi : v; }

}  // namespace

std::wstring exe_dir() {
#ifdef __ANDROID__
    // The working directory: the app's files folder (port/android/app.cpp), or where the
    // command-line build was started.
    std::error_code ec;
    return std::filesystem::current_path(ec).wstring() + L"/";
#endif
    wchar_t buf[MAX_PATH * 2];
    DWORD n = GetModuleFileNameW(nullptr, buf, (DWORD)std::size(buf));
    std::wstring p(buf, n);
    return p.substr(0, p.find_last_of(L"\\/") + 1);
}

std::wstring path() { return exe_dir() + L"settings.ini"; }

void load() {
    Settings d;  // defaults
    Settings& s = g_settings;
    s.fullscreen = read_int(L"Display", L"Fullscreen", d.fullscreen) != 0;
    s.window_width = clamp(read_int(L"Display", L"WindowWidth", d.window_width), 640, 7680);
    s.window_height = clamp(read_int(L"Display", L"WindowHeight", d.window_height), 360, 4320);
    s.render_height = read_int(L"Display", L"RenderResolution", d.render_height);
    if (s.render_height != 720 && s.render_height != 1080 && s.render_height != 1440 && s.render_height != 2160)
        s.render_height = d.render_height;
    int fps = read_int(L"Display", L"MaxFPS", d.max_fps);
#ifdef __ANDROID__
    s.max_fps = fps >= 120 ? 120 : fps >= 60 ? 60 : 30;
    s.widescreen = read_int(L"Display", L"Widescreen", d.widescreen) != 0;
#else
    s.max_fps = fps >= 60 ? 60 : 30;
#endif
    s.show_fps = read_int(L"Display", L"ShowFPS", d.show_fps) != 0;
    s.anti_aliasing = clamp(read_int(L"Graphics", L"AntiAliasing", d.anti_aliasing), 0, 2);
    s.dynamic_shadows = read_int(L"Graphics", L"DynamicShadows", d.dynamic_shadows) != 0;
    s.high_res_shadows = read_int(L"Graphics", L"HighResShadows", d.high_res_shadows) != 0;
    s.light_shafts = read_int(L"Graphics", L"LightShafts", d.light_shafts) != 0;
    s.bloom = read_int(L"Graphics", L"Bloom", d.bloom) != 0;
    s.depth_of_field = read_int(L"Graphics", L"DepthOfField", d.depth_of_field) != 0;
    s.anisotropy = clamp(read_int(L"Graphics", L"Anisotropy", d.anisotropy), 1, 16);
    s.music_volume = clamp(read_int(L"Audio", L"MusicVolume", d.music_volume), 0, 100);
    s.effects_volume = clamp(read_int(L"Audio", L"EffectsVolume", d.effects_volume), 0, 100);
    s.controller = read_int(L"Controller", L"Enabled", d.controller) != 0;
    s.cursor_speed = clamp(read_int(L"Controller", L"CursorSpeed", d.cursor_speed), 20, 400);
    s.camera_speed = clamp(read_int(L"Controller", L"CameraSpeed", d.camera_speed), 20, 400);
    s.swipe_size = clamp(read_int(L"Controller", L"SwipeSize", d.swipe_size), 30, 300);
    g_loaded = true;
}

void save() {
    const Settings& s = get();
    write_int(L"Display", L"Fullscreen", s.fullscreen);
    write_int(L"Display", L"WindowWidth", s.window_width);
    write_int(L"Display", L"WindowHeight", s.window_height);
    write_int(L"Display", L"RenderResolution", s.render_height);
    write_int(L"Display", L"MaxFPS", s.max_fps);
#ifdef __ANDROID__
    write_int(L"Display", L"Widescreen", s.widescreen);
#endif
    write_int(L"Display", L"ShowFPS", s.show_fps);
    write_int(L"Graphics", L"AntiAliasing", s.anti_aliasing);
    write_int(L"Graphics", L"DynamicShadows", s.dynamic_shadows);
    write_int(L"Graphics", L"HighResShadows", s.high_res_shadows);
    write_int(L"Graphics", L"LightShafts", s.light_shafts);
    write_int(L"Graphics", L"Bloom", s.bloom);
    write_int(L"Graphics", L"DepthOfField", s.depth_of_field);
    write_int(L"Graphics", L"Anisotropy", s.anisotropy);
    write_int(L"Audio", L"MusicVolume", s.music_volume);
    write_int(L"Audio", L"EffectsVolume", s.effects_volume);
    write_int(L"Controller", L"Enabled", s.controller);
    write_int(L"Controller", L"CursorSpeed", s.cursor_speed);
    write_int(L"Controller", L"CameraSpeed", s.camera_speed);
    write_int(L"Controller", L"SwipeSize", s.swipe_size);
}

Settings& get() {
    if (!g_loaded) load();
    return g_settings;
}

std::string key_for(const char* action, const char* default_key) {
    wchar_t buf[64] = {};
    GetPrivateProfileStringW(L"Controls", widen(action).c_str(), widen(default_key).c_str(), buf, (DWORD)std::size(buf),
                             path().c_str());
    std::string out;
    for (const wchar_t* p = buf; *p; p++) out += (char)*p;
    return out;
}

void set_key(const char* action, const std::string& key) {
    WritePrivateProfileStringW(L"Controls", widen(action).c_str(), widen(key.c_str()).c_str(), path().c_str());
}

}  // namespace settings
