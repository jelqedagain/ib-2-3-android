// Controllers on Android (built-in handheld controls, Bluetooth and USB pads): the app's input
// events update one pad state (android::pad_event, called by app.cpp), read by game/controller.cpp.
#include "game/pad.h"
#include "port/android/text.h"
#include "uikit/uikit.h"
#include <android/input.h>
#include <android/keycodes.h>
#include <mutex>

namespace game::pad {

const char* const kBackName = "Select";
const char* const kStartName = "Start";

namespace {
std::mutex g_mutex;
State g_state;
u16 g_pressed = 0;  // buttons pressed since the last read: a tap shorter than the poll still counts
bool g_trigger_axes = false;  // the pad reports analog triggers (so its L2/R2 keys are ignored)

u16 button_for(int32_t key) {
    switch (key) {
    case AKEYCODE_BUTTON_A: return kA;
    case AKEYCODE_BUTTON_B: return kB;
    case AKEYCODE_BUTTON_X: return kX;
    case AKEYCODE_BUTTON_Y: return kY;
    case AKEYCODE_BUTTON_L1: return kLeftShoulder;
    case AKEYCODE_BUTTON_R1: return kRightShoulder;
    case AKEYCODE_BUTTON_THUMBL: return kLeftThumb;
    case AKEYCODE_BUTTON_THUMBR: return kRightThumb;
    case AKEYCODE_BUTTON_START: return kStart;
    case AKEYCODE_BUTTON_SELECT: return kBack;
    case AKEYCODE_DPAD_UP: return kDpadUp;
    case AKEYCODE_DPAD_DOWN: return kDpadDown;
    case AKEYCODE_DPAD_LEFT: return kDpadLeft;
    case AKEYCODE_DPAD_RIGHT: return kDpadRight;
    default: return 0;
    }
}

bool from_pad(const AInputEvent* e) {
    int32_t source = AInputEvent_getSource(e);
    return (source & AINPUT_SOURCE_GAMEPAD) == AINPUT_SOURCE_GAMEPAD || (source & AINPUT_SOURCE_JOYSTICK) == AINPUT_SOURCE_JOYSTICK ||
           (source & AINPUT_SOURCE_DPAD) == AINPUT_SOURCE_DPAD;
}
}  // namespace

bool init() { return true; }

void read(State& s) {
    std::lock_guard lock(g_mutex);
    s = g_state;
    s.buttons |= g_pressed;
    g_pressed = 0;
}

bool game_in_foreground() { return uikit::app_active(); }

void render_legend(const std::vector<std::string>& lines, std::vector<u8>& rgba, int& w, int& h) {
    uikit::render_text_panel(lines, 24, rgba, w, h);
}

}  // namespace game::pad

namespace android {

// Takes the event if it comes from a game controller.
bool pad_event(const AInputEvent* e) {
    using namespace game::pad;
    if (!from_pad(e)) return false;
    std::lock_guard lock(g_mutex);
    State& s = g_state;
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) {
        int32_t key = AKeyEvent_getKeyCode(e);
        bool down = AKeyEvent_getAction(e) == AKEY_EVENT_ACTION_DOWN;
        if (key == AKEYCODE_BUTTON_L2 || key == AKEYCODE_BUTTON_R2) {
            if (!g_trigger_axes) (key == AKEYCODE_BUTTON_L2 ? s.lt : s.rt) = down ? 1.0f : 0.0f;
        } else if (u16 b = button_for(key)) {
            s.buttons = down ? (s.buttons | b) : (s.buttons & ~b);
            if (down) g_pressed |= b;
        } else if (key != AKEYCODE_BACK) {
            return false;  // e.g. volume keys
        }
        s.connected = true;
        return true;
    }
    if (AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION) return false;
    auto axis = [e](int32_t a) { return AMotionEvent_getAxisValue(e, a, 0); };
    s.lx = axis(AMOTION_EVENT_AXIS_X);
    s.ly = -axis(AMOTION_EVENT_AXIS_Y);
    s.rx = axis(AMOTION_EVENT_AXIS_Z) != 0 ? axis(AMOTION_EVENT_AXIS_Z) : axis(AMOTION_EVENT_AXIS_RX);
    s.ry = -(axis(AMOTION_EVENT_AXIS_RZ) != 0 ? axis(AMOTION_EVENT_AXIS_RZ) : axis(AMOTION_EVENT_AXIS_RY));
    float lt = std::max(axis(AMOTION_EVENT_AXIS_LTRIGGER), axis(AMOTION_EVENT_AXIS_BRAKE));
    float rt = std::max(axis(AMOTION_EVENT_AXIS_RTRIGGER), axis(AMOTION_EVENT_AXIS_GAS));
    if (lt > 0 || rt > 0) g_trigger_axes = true;
    if (g_trigger_axes) s.lt = lt, s.rt = rt;
    // Many pads report the d-pad as a hat axis instead of keys.
    float hx = axis(AMOTION_EVENT_AXIS_HAT_X), hy = axis(AMOTION_EVENT_AXIS_HAT_Y);
    u16 hat = (hx < -0.5f ? kDpadLeft : 0) | (hx > 0.5f ? kDpadRight : 0) | (hy < -0.5f ? kDpadUp : 0) | (hy > 0.5f ? kDpadDown : 0);
    static u16 s_hat = 0;
    s.buttons = (s.buttons & ~s_hat) | hat;
    s_hat = hat;
    s.connected = true;
    return true;
}

}  // namespace android
