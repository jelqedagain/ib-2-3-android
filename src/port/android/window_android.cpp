// The Android window behind the game's screen. Command-line runs (adb shell) have no window and
// render off screen; the app build hands over its ANativeWindow before the game starts.
#include "uikit/uikit.h"
#include <atomic>

namespace uikit {

namespace {
std::atomic<void*> g_window{nullptr};
}  // namespace

void set_native_window(void* window) { g_window = window; }

void create_window() {}
void* main_window() { return g_window.load(); }

void show_fps(unsigned fps) {
    static unsigned n = 0;
    if (++n % 5 == 0) LOG_INFO("%u FPS", fps);
}

// No boot screen yet: the launch image stays up in the app until the first frame.
void create_boot_window(void*, const std::wstring&) {}
void resize_boot_window(int, int) {}
void end_boot_window() {}

}  // namespace uikit
