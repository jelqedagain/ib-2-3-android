// Game files: installing IB3 from the player's own .ipa, and finding them later.
#pragma once
#include "common.h"
#include <functional>
#include <string>

namespace launcher {

std::wstring game_root();  // <exe dir>\game\ (holds Payload\SwordGame.app)
std::wstring app_dir();    // <exe dir>\game\Payload\SwordGame.app\ (with trailing backslash)
bool game_installed();
std::string installed_version();  // CFBundleShortVersionString, or ""

// Extracts Payload/SwordGame.app from an .ipa (a zip archive) into game_root(). Runs on the
// calling thread; `progress` gets 0..1. On failure returns false with a readable `error`.
bool install_from_ipa(const std::wstring& ipa, const std::function<void(double)>& progress, std::wstring& error);

std::wstring widen(const std::string& utf8);
std::string narrow(const std::wstring& w);

}  // namespace launcher
