// Controller support (Xbox-style pads), laid out like the IB2 controller
// mod but built into the game: the left stick moves an on-screen cursor, touches are virtual
// fingers (the real mouse is never moved), and buttons press the same game actions as the
// keyboard (game/actions.h). So they follow the same rules: a button only works when its
// on-screen control would, and the fight buttons follow the weapon.
//
//   Left stick   cursor; A taps there (hold A to drag / scroll lists), and also stabs and
//                mashes sword clashes
//   Right stick  camera; in fights, or holding RT, swipe attacks; holding R3, scroll
//   LB / RB      left / right fight button           LT or B   center fight button (hold)
//   X            magic                               Y         super move
//   D-pad        left / up / right: magic slots 1-3; down: boss info / final strike
//   Back         accept the prompt                   Start     back out of a menu, else the menu
//   L3           fast-forward cutscenes (hold)       L3 + R3   show / hide the controls legend
// Reading the pad is platform code (game/pad.h).
#include "foundation/foundation.h"
#include "game/game.h"
#include "game/pad.h"
#include "gles/gl.h"
#include "settings.h"
#include "uikit/uikit.h"
#include <windows.h>
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace game {

namespace {

using Pad = pad::State;

constexpr double kH = 414;                              // screen height, in points
double screen_w() { return uikit::g_device.width_pt; }  // 736, or wider on phones
constexpr int kFingerCursor = 70, kFingerCamera = 71, kFingerSwipe = 72, kFingerScroll = 73;
constexpr float kDeadzone = 0.22f, kTrigger = 0.15f;
constexpr float kCameraStart = 0.40f, kSwipeStart = 0.55f, kSwipeEnd = 0.35f;
constexpr double kSwipeSeconds = 0.07, kSwipeStickySeconds = 0.7;

bool g_pad_ok = false;

std::mutex g_fake_mutex;
bool g_fake = false;
Pad g_fake_pad;

// Shared with the overlay (render thread).
std::mutex g_overlay_mutex;
CGPoint g_cursor{368, kH / 2};
bool g_cursor_shown = false, g_legend_shown = false;
std::atomic<bool> g_pad_active{false};

void apply_deadzone(float& x, float& y) {
    float m = std::sqrt(x * x + y * y);
    if (m < kDeadzone) {
        x = y = 0;
        return;
    }
    float s = std::min(1.0f, (m - kDeadzone) / (1 - kDeadzone)) / m;
    x *= s;
    y *= s;
}

Pad read_pad() {
    Pad p;
    bool fake;
    {
        std::lock_guard lock(g_fake_mutex);
        fake = g_fake;
        if (fake) p = g_fake_pad;
    }
    // Hidden test runs only listen to the scripted pad, never to a real controller.
    if (!fake && g_pad_ok && !uikit::g_test_mode) pad::read(p);
    apply_deadzone(p.lx, p.ly);
    apply_deadzone(p.rx, p.ry);
    return p;
}

CGPoint on_screen(CGPoint p) { return {std::clamp(p.x, 0.0, screen_w() - 1), std::clamp(p.y, 0.0, kH - 1)}; }

// A virtual finger (touches are delivered on the main thread, in order).
struct Finger {
    int id;
    bool down = false;
    CGPoint pos{};
    void post(int phase) {
        CGPoint p = pos;
        int f = id;
        ns::post_to_main([f, phase, p] {
            if (phase == 0) uikit::touch_down(f, p);
            else if (phase == 1) uikit::touch_move(f, p);
            else uikit::touch_up(f, p);
        });
    }
    void press(CGPoint p) {
        if (down) return;
        pos = on_screen(p);
        down = true;
        post(0);
    }
    void move(CGPoint p) {
        p = on_screen(p);
        if (!down || (p.x == pos.x && p.y == pos.y)) return;
        pos = p;
        post(1);
    }
    void release() {
        if (!down) return;
        down = false;
        post(2);
    }
    bool at_edge() const { return pos.x <= 0 || pos.y <= 0 || pos.x >= screen_w() - 1 || pos.y >= kH - 1; }
};

void run() {
    logging::set_thread_name("controller");
    const auto& st = settings::get();
    const double cursor_speed = kH * 0.9 * st.cursor_speed / 100;  // points per second at full tilt
#ifdef __ANDROID__
    // Handheld sticks are short: a slower camera with the cursor's quadratic response (a gentle push
    // turns gently). There is no launcher to change it in, unlike on Windows.
    const double camera_speed = kH * 0.8 * st.camera_speed / 100;
    constexpr bool kCameraCurve = true;
#else
    const double camera_speed = kH * 1.2 * st.camera_speed / 100;
    constexpr bool kCameraCurve = false;
#endif
    const double swipe_length = kH * 0.52 * st.swipe_size / 100;
    const double scroll_speed = kH * 1.0;
    const CGPoint centre{screen_w() / 2, kH * 0.46};  // where the enemy stands

    Finger cursor_finger{kFingerCursor}, camera_finger{kFingerCamera}, swipe_finger{kFingerSwipe},
        scroll_finger{kFingerScroll};
    CGPoint cursor{screen_w() / 2, kH / 2};
    std::set<std::string> held;
    Pad prev;
    bool active = false, a_tap_pending = false, legend_combo = false;
    bool swiping = false, swipe_ready = true;
    double swipe_t = 0;
    CGPoint swipe_from{}, swipe_to{};
    u64 last = GetTickCount64(), last_cursor_use = 0, a_down_at = 0, rt_up_at = 0, legend_until = 0, camera_still_since = 0;

    auto action = [&](const char* id, bool down) {
        if (down ? held.insert(id).second : held.erase(id) > 0) press_action(id, down);
    };
    auto release_everything = [&] {
        for (Finger* f : {&cursor_finger, &camera_finger, &swipe_finger, &scroll_finger}) f->release();
        for (auto id : std::set<std::string>(held)) action(id.c_str(), false);
        swiping = false;
        a_tap_pending = false;
    };

    for (;;) {
        Sleep(8);
        u64 now = GetTickCount64();
        double dt = std::min(0.1, (now - last) / 1000.0);
        last = now;
        Pad p = read_pad();
        bool focused = uikit::g_test_mode || pad::game_in_foreground();
        if (!p.connected || !focused) {  // leave the game alone while it is in the background
            if (active) {
                release_everything();
                LOG_INFO("controller %s", p.connected ? "paused (game not focused)" : "disconnected");
            }
            active = false;
            g_pad_active = false;
            prev = Pad{};
            continue;
        }
        if (!active) {
            active = true;
            legend_until = now + 12000;  // show the controls for a while
            LOG_INFO("controller active");
            track_hud(true);
        }
        g_pad_active = true;
        auto pressed = [&](u16 b) { return (p.buttons & b) != 0; };
        auto went_down = [&](u16 b) { return (p.buttons & b) && !(prev.buttons & b); };

        // L3 + R3 shows / hides the legend (and is neither fast-forward nor scrolling).
        bool l3 = pressed(pad::kLeftThumb), r3 = pressed(pad::kRightThumb);
        if (l3 && r3 && (went_down(pad::kLeftThumb) || went_down(pad::kRightThumb))) {
            legend_until = now < legend_until ? 0 : now + 12000;
            legend_combo = true;
        }
        if (!l3 && !r3) legend_combo = false;

        // Buttons: game actions, held while the button is.
        action("DodgeLeft", pressed(pad::kLeftShoulder));
        action("DodgeRight", pressed(pad::kRightShoulder));
        action("Block", pressed(pad::kB) || p.lt > kTrigger);
        action("Magic", pressed(pad::kX));
        action("SuperMove", pressed(pad::kY));
        action("Magic1", pressed(pad::kDpadLeft));
        action("Magic2", pressed(pad::kDpadUp));
        action("Magic3", pressed(pad::kDpadRight));
        action("BossInfo", pressed(pad::kDpadDown));  // only one of these two exists at a time
        action("FinalStrike", pressed(pad::kDpadDown));
        action("Accept", pressed(pad::kBack));
        action("@StartButton", pressed(pad::kStart));
        action("FastForward", l3 && !legend_combo);

        // Left stick: the cursor (quadratic response for fine aiming).
        if (p.lx || p.ly) {
            double m = std::hypot(p.lx, p.ly);
            cursor = on_screen({cursor.x + p.lx * m * cursor_speed * dt, cursor.y - p.ly * m * cursor_speed * dt});
            last_cursor_use = now;
        }

        // A: stab and clash-mash (the game ignores whichever makes no sense), then tap at the
        // cursor a moment later; holding A keeps the finger down so the cursor drags.
        if (went_down(pad::kA)) {
            for (const char* id : {"Stab", "ClashMash"}) {
                press_action(id, true);
                press_action(id, false);
            }
            a_down_at = now;
            a_tap_pending = true;
            last_cursor_use = now;
        }
        if (pressed(pad::kA)) {
            if (a_tap_pending && now - a_down_at >= 60) {
                a_tap_pending = false;
                cursor_finger.press(cursor);
            } else {
                cursor_finger.move(cursor);
            }
        } else if (prev.buttons & pad::kA) {
            if (a_tap_pending) cursor_finger.press(cursor);  // quick tap: down and up
            a_tap_pending = false;
            cursor_finger.release();
        }

        // Right stick: scroll (R3 held), swipes (fights, or RT held and a moment after), camera.
        double rmag = std::hypot(p.rx, p.ry);
        bool rt = p.rt > kTrigger;
        if (!rt && prev.rt > kTrigger) rt_up_at = now;
        bool scroll_mode = r3 && !legend_combo;
        bool swipe_mode = !scroll_mode && (rt || now - rt_up_at < kSwipeStickySeconds * 1000 || hud_is_fight());
        bool camera_mode = !scroll_mode && !swipe_mode;

        if (scroll_mode && rmag > 0) {
            if (!scroll_finger.down) scroll_finger.press(cursor);
            // Stick up shows what is above: the finger drags down, like a touch scroll.
            scroll_finger.move({scroll_finger.pos.x - p.rx * scroll_speed * dt, scroll_finger.pos.y + p.ry * scroll_speed * dt});
            if (scroll_finger.at_edge()) scroll_finger.release();  // picks up again from the cursor
        } else {
            scroll_finger.release();
        }

        if (swipe_mode) {
            if (swiping) {
                swipe_t = std::min(1.0, swipe_t + dt / kSwipeSeconds);
                swipe_finger.move({swipe_from.x + (swipe_to.x - swipe_from.x) * swipe_t,
                                   swipe_from.y + (swipe_to.y - swipe_from.y) * swipe_t});
                if (swipe_t >= 1) {
                    swipe_finger.release();
                    swiping = false;
                }
            } else if (swipe_ready && rmag > kSwipeStart) {
                double dx = p.rx / rmag, dy = -p.ry / rmag;  // a flick draws a slash through the centre
                swipe_from = {centre.x - dx * swipe_length / 2, centre.y - dy * swipe_length / 2};
                swipe_to = {centre.x + dx * swipe_length / 2, centre.y + dy * swipe_length / 2};
                swipe_finger.press(swipe_from);
                swiping = true;
                swipe_ready = false;
                swipe_t = 0;
            }
            if (rmag < kSwipeEnd) swipe_ready = true;
        } else if (swiping) {
            swipe_finger.release();
            swiping = false;
        }

        if (camera_mode && rmag > kCameraStart) {
            if (!camera_finger.down) camera_finger.press(centre);
            double k = camera_speed * dt * (kCameraCurve ? rmag : 1.0);
            camera_finger.move({camera_finger.pos.x + p.rx * k, camera_finger.pos.y - p.ry * k});
            camera_still_since = 0;
        } else if (camera_finger.down) {
            // Hold still a moment before letting go, so the game doesn't fling the view.
            if (!camera_still_since) camera_still_since = now;
            if (!camera_mode || now - camera_still_since > 150) {
                camera_finger.release();
                camera_still_since = 0;
            }
        }

        {
            std::lock_guard lock(g_overlay_mutex);
            g_cursor = cursor;
            g_cursor_shown = now - last_cursor_use < 3000 || cursor_finger.down || scroll_finger.down;
            g_legend_shown = now < legend_until;
        }
        prev = p;
    }
}

// The controls legend, rendered once.
struct Legend {
    int w = 0, h = 0;
    std::vector<u8> rgba;
};

const Legend& legend() {
    static Legend l = [] {
        Legend l;
        std::string back = pad::kBackName, start = pad::kStartName;
        pad::render_legend(
            {
                "Left stick  cursor       A  tap, hold to drag (also stab / clash)       Right stick  camera, swipes in fights",
                "RT + right stick  swipe       R3 + right stick  scroll       LB / RB  left / right fight button       LT or B  center button",
                "X  magic       Y  super move       D-pad  spells 1-3, down: boss info / final strike       " + back + "  accept prompt",
                start + "  menu / back       L3  fast-forward cutscenes (hold)       L3 + R3  show / hide these controls",
            },
            l.rgba, l.w, l.h);
        return l;
    }();
    return l;
}

}  // namespace

void draw_controller_overlay(int sw, int sh) {
    if (!g_pad_active) return;
    CGPoint c;
    bool cursor, show_legend;
    {
        std::lock_guard lock(g_overlay_mutex);
        c = g_cursor;
        cursor = g_cursor_shown;
        show_legend = g_legend_shown;
    }
    // The game image is letterboxed into the window the same way touches are mapped.
    double kW = screen_w(), ow = sw, oh = sw * kH / kW;
    if (oh > sh) oh = sh, ow = sh * kW / kH;
    double ox = (sw - ow) / 2, oy = (sh - oh) / 2, scale = oh / kH;
    if (show_legend && !legend().rgba.empty()) {
        const Legend& l = legend();
        int dw = (int)std::min(ow * 0.96, (double)l.w * scale / 2.0);
        int dh = dw * l.h / l.w;
        gles::draw_rgba_rect(l.rgba.data(), l.w, l.h, 1, (sw - dw) / 2, (int)(oy + oh - dh - 8 * scale), dw, dh, sh);
    }
    if (cursor) {
        int px = (int)(ox + c.x * scale), py = (int)(oy + c.y * scale);
        int arm = std::max(7, (int)(9 * scale)), t = std::max(1, (int)(1.2 * scale));
        gles::fill_rect(px - arm - 1, py - t - 1, 2 * arm + 2, 2 * t + 2, sh, 0, 0, 0);  // outline
        gles::fill_rect(px - t - 1, py - arm - 1, 2 * t + 2, 2 * arm + 2, sh, 0, 0, 0);
        gles::fill_rect(px - arm, py - t, 2 * arm, 2 * t, sh, 1, 1, 1);
        gles::fill_rect(px - t, py - arm, 2 * t, 2 * arm, sh, 1, 1, 1);
    }
}

void set_fake_pad(const std::string& control, float v) {
    std::lock_guard lock(g_fake_mutex);
    if (control == "off") {
        g_fake = false;
        return;
    }
    g_fake = true;
    Pad& p = g_fake_pad;
    p.connected = true;
    static const std::pair<const char*, u16> buttons[] = {
        {"A", pad::kA}, {"B", pad::kB}, {"X", pad::kX}, {"Y", pad::kY},
        {"LB", pad::kLeftShoulder}, {"RB", pad::kRightShoulder}, {"BACK", pad::kBack},
        {"START", pad::kStart}, {"L3", pad::kLeftThumb}, {"R3", pad::kRightThumb},
        {"UP", pad::kDpadUp}, {"DOWN", pad::kDpadDown}, {"LEFT", pad::kDpadLeft},
        {"RIGHT", pad::kDpadRight},
    };
    for (auto& [name, bit] : buttons)
        if (control == name) p.buttons = v > 0.5f ? (p.buttons | bit) : (p.buttons & ~bit);
    if (control == "LX") p.lx = v;
    if (control == "LY") p.ly = v;
    if (control == "RX") p.rx = v;
    if (control == "RY") p.ry = v;
    if (control == "LT") p.lt = v;
    if (control == "RT") p.rt = v;
}

void start_controller() {
    if (!settings::get().controller) return;
    g_pad_ok = pad::init();
    gles::g_overlay = draw_controller_overlay;
    std::thread(run).detach();
}

}  // namespace game
