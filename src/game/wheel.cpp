// IB3's prize wheel (Supplies -> a grab bag, e.g. the gem wheels): SwordGrabBagScene waits for the
// player to spin its SwordGrabBagItemWheel, then plays the spin, the landing and the prize flash
// (states SpinToItem, HoldOnItem, FlashItemWon, FadeToInventory, AwardPrize). The prize is picked
// before the spin. With settings [Game] FastWheel on, the world's TimeDilation is raised while the
// wheel is past waiting for the spin, so the animation is over in a moment and the game awards the
// same prize the usual way.
#include "game/game.h"
#include "game/unreal.h"
#include "settings.h"
#include <windows.h>

namespace game {

namespace {

constexpr float kFastFactor = 25;

GuestAddr grab_bag_scene(cpu::Thread& t) {
    GuestAddr input = ue::player_input(t);
    ue::TArray<u64> stack{};
    if (!input || !ue::read_property(t, input, "MobileMenuStack", stack)) return 0;
    for (int i = stack.num - 1; i >= 0; i--)
        if (GuestAddr scene = stack.at(i); scene && ue::is_a(t, scene, "SwordGrabBagScene")) return scene;
    return 0;
}

}  // namespace

void wheel_tick(cpu::Thread& t) {
    static GuestAddr world = 0;  // the WorldInfo whose time is sped up, 0 when none
    static float normal = 1;
    static int last_state = -1;
    if (is_infinity_blade_2() || !settings::get().fast_wheel) return;
    GuestAddr scene = grab_bag_scene(t), wheel = 0;
    if (scene) ue::read_property(t, scene, "ItemWheel", wheel);
    u8 state = 0;
    u32 rolling = 0;
    int bit = wheel ? ue::property_offset(t, wheel, "bIsRollingToItem") : -1;
    if (wheel) ue::read_property(t, wheel, "WheelState", state);
    if (bit >= 0) rolling = *gptr<u32>(wheel + bit);
    if (wheel && state != last_state) LOG_INFO("wheel: state %d (rolling bits %#x)", state, rolling);
    last_state = wheel ? state : -1;
    bool fast = wheel && state != 0;  // 0: waiting for the player's spin
    if (fast && !world) {
        GuestAddr pc = ue::player_controller(t);
        int off = -1;
        if (pc && ue::read_property(t, pc, "WorldInfo", world) && world) off = ue::property_offset(t, world, "TimeDilation");
        if (off < 0) {
            world = 0;
            return;
        }
        normal = *gptr<float>(world + off);
        *gptr<float>(world + off) = normal * kFastFactor;
        LOG_INFO("wheel: spinning, time %gx", normal * kFastFactor);
    } else if (!fast && world) {
        int off = ue::property_offset(t, world, "TimeDilation");
        if (off >= 0) *gptr<float>(world + off) = normal;
        LOG_INFO("wheel: back to normal time (%g)", normal);
        world = 0;
    }
}

}  // namespace game
