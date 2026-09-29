// Controller support (XInput / Xbox-style pads), laid out like the Infinity Blade II controller
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
#include "foundation/foundation.h"
#include "game/game.h"
#include "gles/gl.h"
#include "settings.h"
#include "uikit/uikit.h"
#include <windows.h>
#include <xinput.h>
#include <atomic>
#include <cmath>
#include <mutex>
#include <set>
#include <thread>
#include <vector>

namespace game {

namespace {

struct Pad {
    bool connected = false;
    WORD buttons = 0;
    float lx = 0, ly = 0, rx = 0, ry = 0;  // -1..1, up is positive
    float lt = 0, rt = 0;                  // 0..1
};

constexpr double kW = 736, kH = 414;  // the screen, in points
constexpr int kFingerCursor = 70, kFingerCamera = 71, kFingerSwipe = 72, kFingerScroll = 73;
constexpr float kDeadzone = 0.22f, kTrigger = 0.15f;
constexpr float kCameraStart = 0.40f, kSwipeStart = 0.55f, kSwipeEnd = 0.35f;
constexpr double kSwipeSeconds = 0.07, kSwipeStickySeconds = 0.7;

using GetStateFn = DWORD(WINAPI*)(DWORD, XINPUT_STATE*);
GetStateFn g_get_state = nullptr;

std::mutex g_fake_mutex;
bool g_fake = false;
Pad g_fake_pad;

// Shared with the overlay (render thread).
std::mutex g_overlay_mutex;
CGPoint g_cursor{kW / 2, kH / 2};
bool g_cursor_shown = false, g_legend_shown = false;
std::atomic<bool> g_pad_active{false};

float axis(SHORT v) { return std::max(-1.0f, v / 32767.0f); }

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
    if (!fake && g_get_state && !uikit::g_test_mode) {
        for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
            XINPUT_STATE st{};
            if (g_get_state(i, &st) != ERROR_SUCCESS) continue;
            const XINPUT_GAMEPAD& g = st.Gamepad;
            p.connected = true;
            p.buttons = g.wButtons;
            p.lx = axis(g.sThumbLX), p.ly = axis(g.sThumbLY);
            p.rx = axis(g.sThumbRX), p.ry = axis(g.sThumbRY);
            p.lt = g.bLeftTrigger / 255.0f, p.rt = g.bRightTrigger / 255.0f;
            break;
        }
    }
    apply_deadzone(p.lx, p.ly);
    apply_deadzone(p.rx, p.ry);
    return p;
}

CGPoint on_screen(CGPoint p) { return {std::clamp(p.x, 0.0, kW - 1), std::clamp(p.y, 0.0, kH - 1)}; }

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
    bool at_edge() const { return pos.x <= 0 || pos.y <= 0 || pos.x >= kW - 1 || pos.y >= kH - 1; }
};

void run() {
    logging::set_thread_name("controller");
    const auto& st = settings::get();
    const double cursor_speed = kH * 0.9 * st.cursor_speed / 100;  // points per second at full tilt
    const double camera_speed = kH * 1.2 * st.camera_speed / 100;
    const double swipe_length = kH * 0.52 * st.swipe_size / 100;
    const double scroll_speed = kH * 1.0;
    const CGPoint centre{kW / 2, kH * 0.46};  // where the enemy stands

    Finger cursor_finger{kFingerCursor}, camera_finger{kFingerCamera}, swipe_finger{kFingerSwipe},
        scroll_finger{kFingerScroll};
    CGPoint cursor{kW / 2, kH / 2};
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
        bool focused = uikit::g_test_mode || GetForegroundWindow() == (HWND)uikit::main_window();
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
        auto pressed = [&](WORD b) { return (p.buttons & b) != 0; };
        auto went_down = [&](WORD b) { return (p.buttons & b) && !(prev.buttons & b); };

        // L3 + R3 shows / hides the legend (and is neither fast-forward nor scrolling).
        bool l3 = pressed(XINPUT_GAMEPAD_LEFT_THUMB), r3 = pressed(XINPUT_GAMEPAD_RIGHT_THUMB);
        if (l3 && r3 && (went_down(XINPUT_GAMEPAD_LEFT_THUMB) || went_down(XINPUT_GAMEPAD_RIGHT_THUMB))) {
            legend_until = now < legend_until ? 0 : now + 12000;
            legend_combo = true;
        }
        if (!l3 && !r3) legend_combo = false;

        // Buttons: game actions, held while the button is.
        action("DodgeLeft", pressed(XINPUT_GAMEPAD_LEFT_SHOULDER));
        action("DodgeRight", pressed(XINPUT_GAMEPAD_RIGHT_SHOULDER));
        action("Block", pressed(XINPUT_GAMEPAD_B) || p.lt > kTrigger);
        action("Magic", pressed(XINPUT_GAMEPAD_X));
        action("SuperMove", pressed(XINPUT_GAMEPAD_Y));
        action("Magic1", pressed(XINPUT_GAMEPAD_DPAD_LEFT));
        action("Magic2", pressed(XINPUT_GAMEPAD_DPAD_UP));
        action("Magic3", pressed(XINPUT_GAMEPAD_DPAD_RIGHT));
        action("BossInfo", pressed(XINPUT_GAMEPAD_DPAD_DOWN));  // only one of these two exists at a time
        action("FinalStrike", pressed(XINPUT_GAMEPAD_DPAD_DOWN));
        action("Accept", pressed(XINPUT_GAMEPAD_BACK));
        action("@StartButton", pressed(XINPUT_GAMEPAD_START));
        action("FastForward", l3 && !legend_combo);

        // Left stick: the cursor (quadratic response for fine aiming).
        if (p.lx || p.ly) {
            double m = std::hypot(p.lx, p.ly);
            cursor = on_screen({cursor.x + p.lx * m * cursor_speed * dt, cursor.y - p.ly * m * cursor_speed * dt});
            last_cursor_use = now;
        }

        // A: stab and clash-mash (the game ignores whichever makes no sense), then tap at the
        // cursor a moment later; holding A keeps the finger down so the cursor drags.
        if (went_down(XINPUT_GAMEPAD_A)) {
            for (const char* id : {"Stab", "ClashMash"}) {
                press_action(id, true);
                press_action(id, false);
            }
            a_down_at = now;
            a_tap_pending = true;
            last_cursor_use = now;
        }
        if (pressed(XINPUT_GAMEPAD_A)) {
            if (a_tap_pending && now - a_down_at >= 60) {
                a_tap_pending = false;
                cursor_finger.press(cursor);
            } else {
                cursor_finger.move(cursor);
            }
        } else if (prev.buttons & XINPUT_GAMEPAD_A) {
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
            camera_finger.move({camera_finger.pos.x + p.rx * camera_speed * dt, camera_finger.pos.y - p.ry * camera_speed * dt});
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

// The controls legend, drawn once with GDI.
struct Legend {
    int w = 0, h = 0;
    std::vector<u8> rgba;
};

const Legend& legend() {
    static Legend l = [] {
        Legend l;
        const wchar_t* lines[] = {
            L"Left stick  cursor       A  tap, hold to drag (also stab / clash)       Right stick  camera, swipes in fights",
            L"RT + right stick  swipe       R3 + right stick  scroll       LB / RB  left / right fight button       LT or B  center button",
            L"X  magic       Y  super move       D-pad  spells 1-3, down: boss info / final strike       Back  accept prompt",
            L"Start  menu / back       L3  fast-forward cutscenes (hold)       L3 + R3  show / hide these controls",
        };
        constexpr int kLines = 4, kLineH = 40, kPad = 16;
        HFONT font = CreateFontW(-24, 0, 0, 0, FW_SEMIBOLD, 0, 0, 0, DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
                                 CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        {  // size the panel to the longest line
            HDC measure = CreateCompatibleDC(nullptr);
            HGDIOBJ f = SelectObject(measure, font);
            int widest = 0;
            for (auto* line : lines) {
                RECT r{0, 0, 0, 0};
                DrawTextW(measure, line, -1, &r, DT_CALCRECT | DT_SINGLELINE);
                widest = std::max(widest, (int)r.right);
            }
            SelectObject(measure, f);
            DeleteDC(measure);
            l.w = widest + 2 * kPad + 8;
            l.h = kLines * kLineH + 2 * kPad;
        }
        BITMAPINFO bi{};
        bi.bmiHeader = {sizeof(BITMAPINFOHEADER), l.w, -l.h, 1, 32, BI_RGB};
        void* bits = nullptr;
        HDC dc = CreateCompatibleDC(nullptr);
        HBITMAP bmp = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old_bmp = SelectObject(dc, bmp);
        RECT all{0, 0, l.w, l.h};
        HBRUSH bg = CreateSolidBrush(RGB(14, 20, 34));
        FillRect(dc, &all, bg);
        DeleteObject(bg);
        HGDIOBJ old_font = SelectObject(dc, font);
        SetBkMode(dc, TRANSPARENT);
        SetTextColor(dc, RGB(225, 235, 245));
        for (int i = 0; i < kLines; i++) {
            RECT r{kPad + 4, kPad + i * kLineH, l.w - kPad, kPad + (i + 1) * kLineH};
            DrawTextW(dc, lines[i], -1, &r, DT_LEFT | DT_SINGLELINE | DT_VCENTER);
        }
        GdiFlush();
        l.rgba.resize((size_t)l.w * l.h * 4);
        const u8* src = static_cast<const u8*>(bits);
        for (size_t i = 0; i < l.rgba.size(); i += 4) {
            l.rgba[i] = src[i + 2];
            l.rgba[i + 1] = src[i + 1];
            l.rgba[i + 2] = src[i];
            l.rgba[i + 3] = 255;
        }
        SelectObject(dc, old_font);
        SelectObject(dc, old_bmp);
        DeleteObject(font);
        DeleteObject(bmp);
        DeleteDC(dc);
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
    double ow = sw, oh = sw * kH / kW;
    if (oh > sh) oh = sh, ow = sh * kW / kH;
    double ox = (sw - ow) / 2, oy = (sh - oh) / 2, scale = oh / kH;
    if (show_legend) {
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
    static const std::pair<const char*, WORD> buttons[] = {
        {"A", XINPUT_GAMEPAD_A}, {"B", XINPUT_GAMEPAD_B}, {"X", XINPUT_GAMEPAD_X}, {"Y", XINPUT_GAMEPAD_Y},
        {"LB", XINPUT_GAMEPAD_LEFT_SHOULDER}, {"RB", XINPUT_GAMEPAD_RIGHT_SHOULDER}, {"BACK", XINPUT_GAMEPAD_BACK},
        {"START", XINPUT_GAMEPAD_START}, {"L3", XINPUT_GAMEPAD_LEFT_THUMB}, {"R3", XINPUT_GAMEPAD_RIGHT_THUMB},
        {"UP", XINPUT_GAMEPAD_DPAD_UP}, {"DOWN", XINPUT_GAMEPAD_DPAD_DOWN}, {"LEFT", XINPUT_GAMEPAD_DPAD_LEFT},
        {"RIGHT", XINPUT_GAMEPAD_DPAD_RIGHT},
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
    for (const wchar_t* dll : {L"xinput1_4.dll", L"xinput1_3.dll", L"xinput9_1_0.dll"}) {
        if (HMODULE m = LoadLibraryW(dll)) {
            g_get_state = reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState"));
            if (g_get_state) break;
        }
    }
    if (!g_get_state) LOG_WARN("controller: XInput is not available");
    gles::g_overlay = draw_controller_overlay;
    std::thread(run).detach();
}

}  // namespace game
