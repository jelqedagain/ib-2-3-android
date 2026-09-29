// Keyboard input, laid out like the Infinity Blade II PC port by default (see game/actions.h;
// players can rebind keys in the launcher). Game actions are sent to Unreal
// (UGameViewportClient::InputKey, on the game thread) as the input names IB3's own touch controls
// use (e.g. "Sword_BttnBlock"), so the game's PlayerInput bindings run the real commands. Raw keys
// are not passed through: IB3's config binds most of them to developer and cheat commands
// (S = slash, M = full magic, B = exit boss fight, ...).
// Enter and Escape were PC-only script commands in IB2 (AcceptPrompt, AskQuitGame); IB3 has no
// equivalent, so they are implemented here on top of IB3's menu system: Enter presses the open
// prompt's confirm button, Escape its back button (or asks to quit when no menu is open).
#include "foundation/foundation.h"
#include "game/actions.h"
#include "game/game.h"
#include "game/unreal.h"
#include "hle.h"
#include "hook.h"
#include "macho.h"
#include "settings.h"
#include "uikit/uikit.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <windows.h>

namespace game {

namespace {

GuestAddr g_input_key = 0;  // USwordGameViewportClient::InputKey
GuestAddr g_tick_original = 0;
GuestAddr g_exec_original = 0;
bool g_have_reflection = false;

std::mutex g_mutex;
std::condition_variable g_resume;
bool g_paused = false;  // the game thread waits while the quit prompt is open
std::deque<std::pair<std::string, int>> g_queue;  // (IB3 input name or @action, EInputEvent)
std::set<std::string> g_held;

constexpr int IE_Pressed = 0, IE_Released = 1;
constexpr u64 kViewportOffset = 0x70;  // UGameViewportClient::Viewport
constexpr int kPromptFinger = 80;

// Keyboard key (Unreal name) -> IB3 input name or @action, from game::kActions and the user's
// [Controls] bindings in settings.ini. Built once at startup.
std::unordered_map<std::string, std::string> g_bindings;
constexpr const char* kDumpMenusKey = "F8";  // debug: log the open menu scenes and their buttons

// Buttons Enter presses, most preferred first (IB3 MobileMenuObject tags; a trailing '_'
// in IB3's tags is followed by an index).
const char* const kAcceptTags[] = {
    "OkBttn", "YesBttn", "ContinueBttn", "SimpleContinueBttn", "ProceedBttn", "FinishBttn", "CollectBttn",
    "NextBttn", "ToGameBttn", "StartBttn", "PlayBttn", "CloseDialogBttn", "CloseBttn",
};
// Buttons Escape presses to leave a menu, most preferred first.
const char* const kBackTags[] = {
    "CancelBttn", "CxlBttn", "NoBttn", "LaterBttn", "CloseDialogBttn", "CloseBttn", "ExitPageBttn", "GlobalBackBttn",
};

// --- Menus (UE3 MobileMenuScene / MobileMenuObject) ---------------------------------------

constexpr u64 kMenuObjectFlags = 0x60;  // bool bitfield; 0x1000 = can be touched (see HitTest)
constexpr u32 kMenuObjectTouchable = 0x1000;
constexpr u64 kSceneFlags = 0x90;       // bit 0: scene ignores input (ProcessMenuInput)

struct MenuButton {
    std::string tag, cls;
    u32 flags = 0;
    float x = 0, y = 0, w = 0, h = 0;  // viewport pixels
};
struct MenuScene {
    std::string cls;
    bool ignores_input = false;
    std::vector<MenuButton> buttons;
};

// The open menu scenes, bottom to top.
std::vector<MenuScene> read_menus(cpu::Thread& t) {
    std::vector<MenuScene> out;
    GuestAddr input = ue::player_input(t);
    ue::TArray<u64> stack{};
    if (!input || !ue::read_property(t, input, "MobileMenuStack", stack)) return out;
    for (int i = 0; i < stack.num; i++) {
        GuestAddr scene = stack.at(i);
        if (!scene) continue;
        MenuScene s;
        s.cls = ue::class_name(t, scene);
        s.ignores_input = *gptr<u8>(scene + kSceneFlags) & 1;
        ue::TArray<u64> objects{};
        ue::read_property(t, scene, "MenuObjects", objects);
        for (int j = 0; j < objects.num; j++) {
            GuestAddr obj = objects.at(j);
            if (!obj) continue;
            MenuButton b;
            b.cls = ue::class_name(t, obj);
            int tag = ue::property_offset(t, obj, "Tag");
            if (tag >= 0) b.tag = ue::read_fstring(obj + tag);
            b.flags = *gptr<u32>(obj + kMenuObjectFlags);
            ue::read_property(t, obj, "Width", b.w);
            ue::read_property(t, obj, "Height", b.h);
            float pos[2] = {};
            if (ue::call_event(t, obj, "GetRealPosition", pos)) {
                b.x = pos[0];
                b.y = pos[1];
            }
            s.buttons.push_back(b);
        }
        out.push_back(std::move(s));
    }
    return out;
}

void dump_menus(cpu::Thread& t) {
    auto menus = read_menus(t);
    LOG_INFO("menus: %zu scene(s) open", menus.size());
    for (auto& s : menus) {
        LOG_INFO("  scene %s%s", s.cls.c_str(), s.ignores_input ? " (no input)" : "");
        for (auto& b : s.buttons)
            LOG_INFO("    %-28s %-24s flags=%08x at (%.0f,%.0f) size %.0fx%.0f", b.tag.c_str(), b.cls.c_str(), b.flags,
                     b.x, b.y, b.w, b.h);
    }
}

bool tag_matches(const std::string& tag, const char* want) {
    size_t n = strlen(want);
    if (tag.size() < n || _strnicmp(tag.c_str(), want, n) != 0) return false;
    return tag.size() == n || tag[n] == '_' || isdigit((unsigned char)tag[n]);
}

void tap_pixels(float px, float py) {
    double scale = uikit::g_device.native_scale;  // game pixels per point
    CGPoint p{px / scale, py / scale};
    std::thread([p] {
        ns::post_to_main([p] { uikit::touch_down(kPromptFinger, p); });
        Sleep(90);
        ns::post_to_main([p] { uikit::touch_up(kPromptFinger, p); });
    }).detach();
}

// Presses the first button (by preference) of the top-most menu that has one of `tags`.
template <size_t N>
bool press_menu_button(cpu::Thread& t, const char* const (&tags)[N], const char* key) {
    auto menus = read_menus(t);
    for (auto s = menus.rbegin(); s != menus.rend(); ++s) {
        if (s->ignores_input) continue;
        for (const char* want : tags) {
            for (auto& b : s->buttons) {
                if (!(b.flags & kMenuObjectTouchable) || b.w <= 0 || b.h <= 0 || !tag_matches(b.tag, want)) continue;
                LOG_INFO("%s: pressing %s in %s", key, b.tag.c_str(), s->cls.c_str());
                tap_pixels(b.x + b.w / 2, b.y + b.h / 2);
                return true;
            }
        }
    }
    return false;
}

// IB2 PC's AskQuitGame. Runs on the window thread; the game waits until it is answered.
void ask_quit_game() {
    static std::atomic<bool> asking{false};
    if (uikit::g_test_mode || asking.exchange(true)) return;
    {
        std::lock_guard lock(g_mutex);
        g_paused = true;
    }
    HWND hwnd = (HWND)uikit::main_window();
    int answer = MessageBoxW(hwnd, L"Quit Infinity Blade III?", L"Infinity Blade III",
                             MB_OKCANCEL | MB_ICONQUESTION | MB_DEFBUTTON1);
    {
        std::lock_guard lock(g_mutex);
        g_paused = false;
    }
    g_resume.notify_all();
    asking = false;
    if (answer == IDOK) PostMessageW(hwnd, WM_CLOSE, 0, 0);
}

// --- Game-thread hooks ---------------------------------------------------------------------

// Runs on the game thread at the start of every viewport-client tick.
void on_tick(cpu::Thread& t) {
    GuestAddr client = t.x(0);
    std::deque<std::pair<std::string, int>> events;
    {
        std::unique_lock lock(g_mutex);
        g_resume.wait(lock, [] { return !g_paused; });
        events.swap(g_queue);
    }
    GuestAddr viewport = client ? *gptr<u64>(client + kViewportOffset) : 0;
    for (auto& [key, event] : events) {
        if (!viewport) break;
        if (key[0] == '@') {
            if (event != IE_Pressed || !g_have_reflection) continue;
            if (key == "@AcceptPrompt") press_menu_button(t, kAcceptTags, "Enter");  // IB2 PC's AcceptPrompt
            else if (key == "@Escape" && !press_menu_button(t, kBackTags, "Escape")) ns::post_to_main(ask_quit_game);
            else if (key == "@DumpMenus") dump_menus(t);
            continue;
        }
        u64 name = ue::fname(t, key);
        t.call_raw(g_input_key, [&](cpu::Thread& c) {
            c.set_x(0, client);
            c.set_x(1, viewport);
            c.set_x(2, 0);  // ControllerId
            c.set_x(3, name);
            c.set_x(4, (u64)event);
            c.set_x(5, 0);  // bGamepad
            c.set_s(0, 1.0f);
        });
        LOG_DEBUG("key %s %s -> %s", key.c_str(), event == IE_Pressed ? "pressed" : "released",
                  (t.last_x0() & 0xff) ? "handled" : "unhandled");
    }
    t.jump(g_tick_original);
}

// Logs each key-binding command Unreal runs (e.g. "ButtonBlockCenter"), to verify bindings.
void on_exec_input_commands(cpu::Thread& t) {
    if (logging::enabled(logging::Level::Debug)) {
        std::string cmd;
        for (const u32* w = gptr<u32>(t.x(1)); w && *w && cmd.size() < 200; w++) cmd += *w < 0x80 ? (char)*w : '?';
        LOG_DEBUG("input command: %s", cmd.c_str());
    }
    t.jump(g_exec_original);
}

void queue_event(const std::string& key, bool down) {
    std::string input;
    if (auto it = g_bindings.find(key); it != g_bindings.end()) input = it->second;
    else if (key == kDumpMenusKey) input = "@DumpMenus";
    else return;
    std::lock_guard lock(g_mutex);
    if (down) {
        if (!g_held.insert(key).second) return;
    } else if (!g_held.erase(key)) {
        return;
    }
    g_queue.push_back({input, down ? IE_Pressed : IE_Released});
}

}  // namespace

void press_key(const std::string& ue_name, bool down) { queue_event(ue_name, down); }

void install_keyboard(const macho::Image& img) {
    GuestAddr tick = img.find("__ZN19UGameViewportClient4TickEf");
    g_input_key = img.find("__ZN24USwordGameViewportClient8InputKeyEP9FViewporti5FName11EInputEventfj");
    g_have_reflection = ue::init(img);
    if (!tick || !g_input_key) {
        LOG_WARN("keyboard: engine entry points not found; keyboard controls disabled");
        return;
    }
    for (const Action& a : kActions) {
        std::string key = settings::key_for(a.id, a.default_key);
        if (!key.empty()) g_bindings[key] = a.input;
    }
    g_tick_original = hook::install(tick, "UGameViewportClient::Tick", on_tick);
    if (!g_tick_original) return;
    if (GuestAddr exec = img.find("__ZN6UInput17ExecInputCommandsEPKwR13FOutputDevice"))
        g_exec_original = hook::install(exec, "UInput::ExecInputCommands", on_exec_input_commands);

    uikit::g_key_handler = [](int vk, bool down) {
        if (vk < 0) {  // focus lost: release everything that is held
            std::set<std::string> held;
            {
                std::lock_guard lock(g_mutex);
                held = g_held;
            }
            for (auto& k : held) queue_event(k, false);
            return;
        }
        queue_event(key_name_for_vk(vk), down);
    };
    LOG_INFO("keyboard: IB2-style key bindings enabled");
}

}  // namespace game
