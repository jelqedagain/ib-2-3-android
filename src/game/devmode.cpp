// Developer mode (both games): the game still has its developer options (god mode, kill boss, unlimited
// super...) as handler functions of SwordUserOptionsList, but the shipped list's Init no longer adds
// rows for them. With settings [Game] DeveloperMode on, rows bound to those handlers are added at the
// end of the in-game Options list, through the list's own AddOption.
#include "game/game.h"
#include "game/unreal.h"
#include "settings.h"
#include <windows.h>
#include <string>
#include <vector>

namespace game {

namespace {

struct Row {
    bool checkbox;
    const char* handler;  // SwordUserOptionsList function the row calls
    const char* caption;
    int games = 3;  // 1 IB2, 2 IB3, 3 both (IB2 lacks the Hideout and boss weapon handlers)
};
constexpr int kIB2 = 1, kIB3 = 2;
// Left out: "Delete save" (OnUpdateDeleteSave) deletes at once, without asking; "Toggle stats"
// (OnToggleStats) runs "showdebug", which crashes the shipped game in UObject::SaveConfig.
constexpr Row kRows[] = {
    {true, "OnUpdateToggleGodMode", "Debug - God mode"},
    {true, "OnToggleUnlimitedMagicSuper", "Debug - Unlimited super and magic"},
    {true, "OnUpdateFastForward", "Debug - Always fast forward"},
    {true, "OnToggleBossAttacks", "Debug - Boss attacks"},
    {false, "OnUpdateKillBoss", "Debug - Kill boss"},
    {false, "OnUpdateGiveGold", "Debug - Give gold"},
    {false, "OnSetBossNextWeapon", "Debug - Set boss next weapon", kIB3},
    {false, "OnUpdateLastSavePoint", "Debug - Reload last checkpoint"},
    {false, "OnUpdateLoadHideout", "Debug - Go to the Hideout", kIB3},
    {false, "OnUpdateBloodline", "Debug - Start next bloodline"},
    {false, "OnRebalancePlayerStats", "Debug - Rebalance player stats"},
    {false, "OnUpdateNameCharacter", "Debug - Rename character"},
    {false, "OnUpdateToggleFps", "Debug - Show FPS"},
    {true, "OnToggleGestureTest", "Debug - Gesture test"},
    {true, "OnUpdateDemoHud", "Debug - Demo HUD"},
    {true, "OnUpdateToggleGodrays", "Debug - Light shafts"},
    {true, "OnUpdateToggleShadows", "Debug - Shadows"},
    {true, "OnUpdateTutorial", "Debug - Tutorial"},
    {false, "OnDumpUnencryptedSaveFile", "Debug - Dump unencrypted save"},
    {false, "OnFixedSaveSlotScreen", "Debug - Load unencrypted save"},
};

GuestAddr g_list = 0;  // the options list the rows were added to
int g_count = 0;       // its item count afterwards (fewer means it was rebuilt)

GuestAddr options_scene(cpu::Thread& t) {
    GuestAddr input = ue::player_input(t);
    ue::TArray<u64> stack{};
    if (!input || !ue::read_property(t, input, "MobileMenuStack", stack)) return 0;
    for (int i = stack.num - 1; i >= 0; i--)
        if (GuestAddr scene = stack.at(i); scene && ue::is_a(t, scene, "SwordUserOptionsScene")) return scene;
    return 0;
}

bool add_row(cpu::Thread& t, GuestAddr list, GuestAddr item_class, const char* handler, const char* caption) {
    // Parameter offsets from the function itself: ItemClass, OnUpdate (delegate: object, function
    // name), Caption, optional GadgetOptions, ReturnValue.
    static int o_class = -1, o_update, o_caption, o_result;
    if (o_class < 0) {
        o_class = ue::param_offset(t, list, "AddOption", "ItemClass");
        o_update = ue::param_offset(t, list, "AddOption", "OnUpdate");
        o_caption = ue::param_offset(t, list, "AddOption", "Caption");
        o_result = ue::param_offset(t, list, "AddOption", "ReturnValue");
        LOG_INFO("devmode: AddOption parameters at %d %d %d %d", o_class, o_update, o_caption, o_result);
    }
    if (o_class < 0 || o_update < 0 || o_caption < 0 || o_result < 0) return false;
    alignas(16) u8 params[512] = {};
    *reinterpret_cast<u64*>(params + o_class) = item_class;
    *reinterpret_cast<u64*>(params + o_update) = list;
    *reinterpret_cast<u64*>(params + o_update + 8) = ue::fname(t, handler);
    *reinterpret_cast<ue::FString*>(params + o_caption) = ue::make_fstring(t, caption);
    if (!ue::call_event(t, list, "AddOption", params)) return false;
    GuestAddr item = *reinterpret_cast<u64*>(params + o_result), text = 0;
    if (!item) return false;
    // The row's text object keeps the caption's buffer, which ProcessEvent frees with the parameters:
    // give it a copy of its own.
    int off = -1;
    if (ue::read_property(t, item, "Caption", text) && text && (off = ue::property_offset(t, text, "TextString")) >= 0)
        *gptr<ue::FString>(text + off) = ue::make_fstring(t, caption);
    return true;
}

}  // namespace

// The cheat console commands the rows use ("God"...) go to the player controller's CheatManager. IB3
// creates one; IB2 does not, so its god mode did nothing. AddCheats(bForce) creates it.
void ensure_cheat_manager(cpu::Thread& t) {
    static GuestAddr checked_pc = 0;
    GuestAddr pc = ue::player_controller(t), cheats = 0;
    if (!pc || pc == checked_pc) return;
    checked_pc = pc;
    if (!ue::read_property(t, pc, "CheatManager", cheats) || cheats) return;
    alignas(16) u8 params[256] = {};
    params[0] = 1;  // bForce
    bool ok = ue::call_event(t, pc, "AddCheats", params);
    ue::read_property(t, pc, "CheatManager", cheats);
    LOG_INFO("devmode: cheat manager %s", cheats ? "created" : ok ? "not created" : "not available (no AddCheats)");
}

// God mode is a flag of the player controller, and both games make a new controller for each area, so
// it went off when the player moved on while the Options switch still read "on". It is carried over.
void keep_god_mode(cpu::Thread& t) {
    static GuestAddr last_pc = 0;
    static bool god = false;
    static u64 last_check = 0;
    GuestAddr pc = ue::player_controller(t);
    if (!pc) return;
    if (pc != last_pc) {
        last_pc = pc;
        if (!god) return;
        int cmd = ue::param_offset(t, pc, "ConsoleCommand", "Command");
        if (cmd < 0) return;
        alignas(16) u8 params[256] = {};
        *reinterpret_cast<ue::FString*>(params + cmd) = ue::make_fstring(t, "God");
        ue::call_event(t, pc, "ConsoleCommand", params);
        LOG_INFO("devmode: god mode kept in the new area");
        return;
    }
    u64 now = GetTickCount64();
    if (now - last_check < 1000) return;
    last_check = now;
    GuestAddr pawn = 0;  // InGodMode is a Pawn function (it reads the controller's bGodMode)
    if (!ue::read_property(t, pc, "Pawn", pawn) || !pawn) return;
    alignas(16) u8 params[64] = {};
    if (ue::call_event(t, pawn, "InGodMode", params)) {
        bool now_god = params[0] != 0;
        if (now_god != god) LOG_INFO("devmode: god mode %s (controller 0x%llx)", now_god ? "on" : "off", (unsigned long long)pc);
        god = now_god;
    }
}

void devmode_tick(cpu::Thread& t) {
    if (!settings::get().developer_mode) return;
    ensure_cheat_manager(t);
    keep_god_mode(t);
    GuestAddr scene = options_scene(t), list = 0;
    if (scene) ue::read_property(t, scene, "OptionList", list);
    ue::TArray<u64> items{};
    if (!list || !ue::read_property(t, list, "Items", items)) {
        g_list = 0;
        return;
    }
    if (list == g_list && items.num >= g_count) return;
    // Only the main Options list (it has the volume sliders), and the classes of its own rows.
    GuestAddr checkbox = 0, button = 0;
    bool main_list = false;
    for (int i = 0; i < items.num; i++) {
        GuestAddr it = items.at(i);
        if (!it) continue;
        std::string cls = ue::class_name(t, it);
        if (cls == "SwordOptionListSlider") main_list = true;
        if (cls == "SwordOptionListCheckBox") checkbox = *gptr<u64>(it + ue::kObjClass);
        if (cls == "SwordOptionListButton") button = *gptr<u64>(it + ue::kObjClass);
    }
    g_list = list;
    g_count = items.num;
    if (!main_list || !checkbox || !button) return;
    alignas(16) u8 gap[256] = {};
    ue::call_event(t, list, "AddListGap", gap);
    int added = 0;
    const int game = is_infinity_blade_2() ? kIB2 : kIB3;
    for (const Row& r : kRows)
        if (r.games & game) added += add_row(t, list, r.checkbox ? checkbox : button, r.handler, r.caption);
    if (ue::read_property(t, list, "Items", items)) g_count = items.num;
    LOG_INFO("devmode: %d developer options added to the Options list (%d items)", added, g_count);
}

}  // namespace game
