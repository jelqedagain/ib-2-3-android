// User settings (settings.ini next to the executable), shared by the launcher and the game.
#pragma once
#include "common.h"
#include <string>

namespace settings {

struct Settings {
    // Display
    bool fullscreen = false;
    int window_width = 1280, window_height = 720;
    int render_height = 1080;  // 720, 1080, 1440, 2160 (16:9)
#ifdef __ANDROID__
    int max_fps = 60;          // 30 (original) or 60
#else
    int max_fps = 30;          // 30 (original) or 60
#endif
    bool show_fps = false;
    // Graphics (IB3's own mobile renderer options)
    int anti_aliasing = 1;     // 0 off, 1 FXAA, 2 MSAA 4x
    bool dynamic_shadows = true;
    bool high_res_shadows = false;
    bool light_shafts = true;
    bool bloom = true;
    bool depth_of_field = true;
    int anisotropy = 4;        // 1, 2, 4, 8, 16
    // Audio (percent)
    int music_volume = 100;
    int effects_volume = 100;
    // Controller (percent multipliers of the defaults)
    bool controller = true;
    int cursor_speed = 100, camera_speed = 100, swipe_size = 100;
};

// Directory of the executable (with trailing backslash); on Android, the app's files folder.
std::wstring exe_dir();
std::wstring path();  // settings.ini

Settings& get();  // loaded on first use
void load();
void save();

// Key bindings: Unreal key name ("S", "LeftShift", ...) for each action id.
std::string key_for(const char* action, const char* default_key);
void set_key(const char* action, const std::string& key);

}  // namespace settings
