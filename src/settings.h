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
    // Cheats ([Cheats], the app's Cheats page; the in-game CHEATS rows change them too, game/devmode.cpp).
    bool developer_mode = false;    // InGame: a CHEATS section at the top of the in-game Options
    bool god_mode = false;          // GodMode
    bool unlimited_super = false;   // UnlimitedSuper: super move and magic always full
    bool fast_forward = false;      // FastForward: always fast-forward
    bool fast_wheel = false;        // FastWheel, IB3: the prize wheel's spin is over at once (game/wheel.cpp)
    bool gem_shop_restock = false;  // GemShopRestock: a gem bought in the gem shop is put back (game/saveedit.cpp)
    bool all_gems = false;  // AllGems: the gem shop sells every gem, each at its highest level (game/devmode.cpp)
    // ClashMobs ([ClashMob] Server, IB3): the community ClashMob server's address; empty = offline ClashMobs only
    std::string clashmob_server;
};

// Directory of the executable (with trailing backslash); on Android, the app's files folder.
std::wstring exe_dir();
std::wstring path();  // settings.ini

Settings& get();  // loaded on first use
void load();
void save();
// Changes one [Cheats] switch, in memory and in settings.ini (the in-game CHEATS rows).
void set_cheat(const char* key, bool on);

// Key bindings: Unreal key name ("S", "LeftShift", ...) for each action id.
std::string key_for(const char* action, const char* default_key);
void set_key(const char* action, const std::string& key);

}  // namespace settings
