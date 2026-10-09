// IB3's end credits (after the Worker; the level runs "ShowCredits 1", later "ShowCredits 2") scroll
// for minutes and cannot be dragged (their modes have bCanScroll off). Holding a finger on them
// scrolls them faster. SwordCreditsScene is a menu scene; its SwordCreditsList scrolls
// ScrollPixelPerSec (set from the text height and the mode's TotalSeconds) and the scene closes,
// letting the game go on, when the list has scrolled through.
#include "game/game.h"
#include "game/unreal.h"
#include "uikit/uikit.h"
#include <windows.h>
#include <string>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace game {

namespace {

constexpr float kFastFactor = 8;

GuestAddr g_list = 0;  // the end credits list being shown
float g_normal_speed = 0;
bool g_fast = false;

GuestAddr credits_scene(cpu::Thread& t) {
    GuestAddr input = ue::player_input(t);
    ue::TArray<u64> stack{};
    if (!input || !ue::read_property(t, input, "MobileMenuStack", stack)) return 0;
    for (int i = stack.num - 1; i >= 0; i--)
        if (GuestAddr scene = stack.at(i); scene && ue::is_a(t, scene, "SwordCreditsScene")) return scene;
    return 0;
}

#ifdef __ANDROID__
// debug.ibport.showcredits 1 / 2 opens the end credits as the last level does (0 = the options
// menu's credits), for tests.
void debug_show_credits(cpu::Thread& t) {
    static u64 last = 0;
    static std::string shown;
    u64 now = GetTickCount64();
    if (now - last < 1000) return;
    last = now;
    char v[PROP_VALUE_MAX] = "";
    __system_property_get("debug.ibport.showcredits", v);
    if (shown == v) return;
    shown = v;
    if (!v[0]) return;
    GuestAddr pc = ue::player_controller(t);
    if (!pc) return;
    struct {
        u64 data;  // FString Mode
        s32 num, max;
        u8 locals[256];
    } params{gaddr(ue::wide(v)), (s32)shown.size() + 1, (s32)shown.size() + 1, {}};
    bool ok = ue::call_event(t, pc, "ShowCredits", &params);
    LOG_INFO("credits: ShowCredits %s%s (debug.ibport.showcredits)", v, ok ? "" : " not found");
}
#endif

}  // namespace

void credits_tick(cpu::Thread& t) {
    if (is_ib2()) return;
#ifdef __ANDROID__
    debug_show_credits(t);
#endif
    GuestAddr scene = credits_scene(t);
    GuestAddr list = 0;
    if (scene) ue::read_property(t, scene, "CreditsList", list);
    int speed_off = list ? ue::property_offset(t, list, "ScrollPixelPerSec") : -1;
    if (speed_off < 0) {
        g_list = 0;
        return;
    }
    float* speed = gptr<float>(list + speed_off);
    if (list != g_list) {
        g_list = list;
        g_fast = false;
        g_normal_speed = 0;
        int mode = ue::property_offset(t, scene, "CurMode");  // its first member is IniSection
        std::string name = mode >= 0 ? ue::read_fstring(scene + mode) : "";
        if (name.rfind("FinalCredit", 0) != 0) return;  // the options menu's credits can be dragged
        g_normal_speed = *speed;
        LOG_INFO("credits: %s at %.0f px/s; holding a finger scrolls them %.0fx faster", name.c_str(), *speed, kFastFactor);
    }
    if (!g_normal_speed) return;
    bool fast = uikit::touches_down() > 0;
    if (fast == g_fast) return;
    g_fast = fast;
    *speed = fast ? g_normal_speed * kFastFactor : g_normal_speed;
}

}  // namespace game
