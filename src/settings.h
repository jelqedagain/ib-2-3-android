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
    int max_fps = 60;          // 30 (original), 60 or 120
    bool widescreen = true;    // IB2 on phones longer than 16:9: fill the screen (false: 16:9 with black bars)
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
#ifdef __ANDROID__
    bool texture_cache = true;  // keep the ETC2 re-encodings of the textures (gles/texcache.cpp)
#endif
    // Audio (percent)
    int music_volume = 100;
    int effects_volume = 100;
    // Controller (percent multipliers of the defaults)
    bool controller = true;
    int cursor_speed = 100, camera_speed = 100, swipe_size = 100;
    // Game language: the suffix of the game's text files (INT, FRA, DEU...). [Game] Language is the
    // player's choice; empty means the phone's language ([Game] PhoneLanguage, written by the launcher).
    std::string language = "INT";
    bool developer_mode = false;  // the game's developer options in its Options menu (game/devmode.cpp)
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
