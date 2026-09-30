// Infinity Blade III specific integration (input, test scripting).
#pragma once
#include "common.h"
#include <string>

namespace macho { struct Image; }

namespace game {

// Hooks Unreal's input path so Windows keys reach IB3's own key bindings.
void install_keyboard(const macho::Image& img);
// Queues a keyboard event by Unreal key name ("A", "LeftShift", "One", ...); keys without a
// game binding are ignored. Thread-safe.
void press_key(const std::string& ue_name, bool down);

// Presses a game action by id (game/actions.h, or "@StartButton"), independent of key bindings.
void press_action(const char* id, bool down);
// Controller support needs to know whether a fight HUD is up (updated every game tick when on).
void track_hud(bool on);
bool hud_is_fight();
// Controller support (XInput), from settings.ini [Controller]; call once the window exists.
void start_controller();
// Draws the controller cursor and legend on top of a presented frame (render thread).
void draw_controller_overlay(int surface_w, int surface_h);
// Test aid (-script "pad <control> <value>"): drives a fake controller instead of XInput.
void set_fake_pad(const std::string& control, float value);

// Applies settings.ini (graphics, frame rate) to the engine's config as it loads.
void install_config(const macho::Image& img);

// Infinity Blade II on screens longer than 16:9: serves patched copies of its HUD layout script and
// menu backdrops. If the patch does not apply, the emulated screen goes back to 16:9.
void install_widescreen();

// Keeps the Unreal/ChAIR logo movie from playing twice when loading is slow (after class realization).
void install_startup_movie_fix();

// True when the running game is Infinity Blade II (its bundle id). Every change that is meant for only one of
// the two apps checks this, so a fix for one never changes the other (see CLAUDE.md).
bool is_infinity_blade_2();

// The language the launcher chose, as iOS reports it to this game: AppleLanguages[0] ("fr", "pt-PT"...)
// and the locale identifier ("fr_FR"). English when the game has no text in that language.
std::string ios_language();
std::string ios_locale();

// Infinity Blade II: turns its store-only memory barriers into full ones (before guest code runs).
void strengthen_memory_barriers(const macho::Image& img);

// Save editor: applies the launcher's Edit save values to the game's save and writes the current ones out.
void install_save_editor(const macho::Image& img);

// Diagnostics (-scripttrace): logs UnrealScript calls, state changes and map loads.
void install_script_trace(const macho::Image& img);
void install_script_trace_objc();  // after class realization

// Test automation: runs a script of timed key presses, touches and screenshots (-script file).
void run_script(const std::string& path);

}  // namespace game
