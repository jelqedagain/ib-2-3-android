// Launcher window (settings, first-run install, Play).
#pragma once

namespace launcher {

// Runs the launcher until it closes; returns the process exit code.
int run();
// Test aid: renders the launcher (and its key-binding window) off-screen to PNG files.
int screenshot(const char* main_png, const char* keys_png);

}  // namespace launcher
