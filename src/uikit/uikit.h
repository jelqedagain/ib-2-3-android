// UIKit / QuartzCore host implementation and the platform window that backs the screen.
#pragma once
#include "hle.h"
#include "objc/runtime.h"
#include <functional>
#include <string>

namespace uikit {

// Emulated device (iPhone 6 Plus: 736x414 points landscape, 1920x1080 native).
struct DeviceProfile {
    const char* machine;  // hw.machine
    const char* model;
    s64 idiom;  // 0 phone, 1 pad
    double width_pt, height_pt;  // landscape bounds in points
    double scale, native_scale;
};
extern DeviceProfile g_device;
extern bool g_test_mode;  // show the window without taking focus

CGRect layer_bounds(objc::id layer);
double layer_scale(objc::id layer);
void on_frame_presented();
u64 frames_presented();

// Touch input in screen points (main thread only). finger 0 is the mouse.
void touch_down(int finger, CGPoint p);
void touch_move(int finger, CGPoint p);
void touch_up(int finger, CGPoint p);
bool touch_active(int finger);
int touches_down();  // fingers on the screen; safe from any thread
// Receives WM_KEYDOWN/WM_KEYUP virtual-key codes (vk -1 = focus lost: release everything).
extern std::function<void(int vk, bool down)> g_key_handler;
// The platform window (uikit/window_win32.cpp, port/android/window_android.cpp).
void create_window();         // on the main thread; installs ns::g_main_pump if it has messages to pump
void* main_window();          // HWND / ANativeWindow* (nullptr: render off screen)
void show_fps(unsigned fps);  // any thread (settings: ShowFPS)
#ifdef __ANDROID__
unsigned shown_fps();  // the last one shown, 0 when the counter is off
void render_text_panel(const std::vector<std::string>& lines, float px, std::vector<u8>& rgba, int& w, int& h);
#endif
void app_will_terminate();    // tells the app delegate (uikit.cpp)
void app_set_active(bool active);  // main thread: background / foreground, like iOS
bool app_active();                 // whether the app is in the foreground (any thread)
// Boot screen (launch image / startup movie) shown until the game's first frame (boot.cpp).
void create_boot_window(void* parent_hwnd, const std::wstring& launch_image);
void resize_boot_window(int w, int h);
void end_boot_window();

void install();       // UIKit core classes + UIApplicationMain
void install_misc();  // CoreGraphics, controls, GameKit, StoreKit, social stubs

}  // namespace uikit
