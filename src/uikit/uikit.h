// UIKit / QuartzCore host implementation and the Win32 window that backs the screen.
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
// Receives WM_KEYDOWN/WM_KEYUP virtual-key codes (vk -1 = focus lost: release everything).
extern std::function<void(int vk, bool down)> g_key_handler;
void* main_window();  // HWND
// Boot screen (launch image / startup movie) shown until the game's first frame (boot.cpp).
void create_boot_window(void* parent_hwnd, const std::wstring& launch_image);
void resize_boot_window(int w, int h);
void end_boot_window();

void install();       // UIKit core classes + UIApplicationMain
void install_misc();  // CoreGraphics, controls, GameKit, StoreKit, social stubs

}  // namespace uikit
