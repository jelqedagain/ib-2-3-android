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

// Applies settings.ini (graphics, frame rate) to the engine's config as it loads.
void install_config(const macho::Image& img);

// Keeps the Unreal/ChAIR logo movie from playing twice when loading is slow (after class realization).
void install_startup_movie_fix();

// Test automation: runs a script of timed key presses, touches and screenshots (-script file).
void run_script(const std::string& path);

}  // namespace game
