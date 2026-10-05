// Cheats (both games). The app's Cheats page keeps switches in settings.ini [Cheats]; this file keeps the
// game in line with them (god mode, unlimited super and magic, always fast-forward) and, with the in-game
// cheats on ([Cheats] InGame), puts a CHEATS section at the TOP of the in-game Options list:
//
// - The switches the app also has. Their rows are bound to handlers of SwordUserOptionsList the shipped
//   game never shows (memory-leak checks, FPS chart...): saveedit.cpp's interpreter hook hands those calls
//   to cheat_row_called, which changes settings.ini, so the app and the game always agree.
// - Actions that happen at once: the game's own leftover developer handlers (kill boss, give gold...) and
//   ours on spare handlers (get every item, gem shop refills).
// - A DEVELOPER section below with the rest of the game's developer handlers.
//
// A row of the dev mod .ipa bound to one of those spare handlers still runs its own code: only the rows
// added here are taken over.
#include "game/game.h"
#include "game/unreal.h"
#include "settings.h"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <set>
#include <string>
#include <vector>

namespace game {

namespace {

enum class Kind { Header, Check, Button, Gap };
enum class Action { None, GiveAllItems };

struct Row {
    Kind kind;
    const char* handler;  // SwordUserOptionsList function the row calls (nullptr: header, gap)
    const char* caption;
    int games = 3;               // 1 IB2, 2 IB3, 3 both (IB2 lacks the Hideout and boss weapon handlers)
    const char* cheat = nullptr;  // [Cheats] key of a switch shared with the app
    Action action = Action::None;
    const char* graphics = nullptr;  // IB2: [Graphics] key of the effect the row toggles (the app's Settings has it too)
};
constexpr int kIB2 = 1, kIB3 = 2;

// Spare handlers (both games have them; the shipped Options list never shows them).
constexpr const char* kSpare[] = {"OnUpdateMemLeakCheck", "OnUpdateMemLeakCheckRun", "OnResetFPSChart", "OnDumpFPSChart",
                                  "OnUpdateTestListBox", "OnYesNoLater", "OnUpdateEncryption"};

// Left out: "Delete save" (OnUpdateDeleteSave) deletes at once, without asking; "Toggle stats"
// (OnToggleStats) runs "showdebug", which crashes the shipped game in UObject::SaveConfig.
const Row kRows[] = {
    {Kind::Header, nullptr, "CHEATS  (developer mode)"},
    {Kind::Check, "OnUpdateMemLeakCheck", "God mode", 3, "GodMode"},
    {Kind::Check, "OnUpdateMemLeakCheckRun", "Unlimited super and magic", 3, "UnlimitedSuper"},
    {Kind::Check, "OnResetFPSChart", "Always fast forward", 3, "FastForward"},
    {Kind::Check, "OnToggleBossAttacks", "Boss attacks"},
    {Kind::Button, "OnUpdateKillBoss", "Kill boss"},
    {Kind::Button, "OnUpdateGiveGold", "Give gold"},
    {Kind::Button, "OnUpdateTestListBox", "Get every item", 3, nullptr, Action::GiveAllItems},
    {Kind::Check, "OnYesNoLater", "Gem shop - all gems", 3, "AllGems"},
    {Kind::Check, "OnDumpFPSChart", "Gem shop - restock", 3, "GemShopRestock"},
    {Kind::Gap, nullptr, nullptr},
    {Kind::Header, nullptr, "DEVELOPER"},
    {Kind::Button, "OnUpdateLastSavePoint", "Reload last checkpoint"},
    {Kind::Button, "OnUpdateLoadHideout", "Go to the Hideout", kIB3},
    {Kind::Button, "OnUpdateBloodline", "Start next bloodline"},
    {Kind::Button, "OnSetBossNextWeapon", "Set boss next weapon", kIB3},
    {Kind::Button, "OnRebalancePlayerStats", "Rebalance player stats"},
    {Kind::Button, "OnUpdateNameCharacter", "Rename character"},
    {Kind::Button, "OnUpdateToggleFps", "Show FPS"},
    {Kind::Check, "OnUpdateToggleGodrays", "Light shafts", 3, nullptr, Action::None, "LightShafts"},
    {Kind::Check, "OnUpdateToggleShadows", "Shadows", 3, nullptr, Action::None, "DynamicShadows"},
    {Kind::Check, "OnUpdateDemoHud", "Demo HUD"},
    {Kind::Check, "OnToggleGestureTest", "Gesture test"},
    {Kind::Check, "OnUpdateTutorial", "Tutorial"},
    {Kind::Button, "OnDumpUnencryptedSaveFile", "Dump unencrypted save"},
    {Kind::Button, "OnFixedSaveSlotScreen", "Load unencrypted save"},
    {Kind::Gap, nullptr, nullptr},
};

constexpr u64 kFrameLocals = 0x30;  // FFrame::Locals: the running function's parameters

GuestAddr g_list = 0;  // the options list the rows were added to
int g_count = 0;       // its item count afterwards (fewer means it was rebuilt)
std::set<GuestAddr> g_ours;        // rows added here
std::vector<std::pair<GuestAddr, const Row*>> g_rows;  // the same, with what they are
Action g_pending = Action::None;   // a button pressed, done on the next tick
GuestAddr g_pending_item = 0;
GuestAddr g_gold_item = 0;         // Give gold was pressed: its row shows the gold on the next tick

GuestAddr options_scene(cpu::Thread& t) {
    GuestAddr input = ue::player_input(t);
    ue::TArray<u64> stack{};
    if (!input || !ue::read_property(t, input, "MobileMenuStack", stack)) return 0;
    for (int i = stack.num - 1; i >= 0; i--)
        if (GuestAddr scene = stack.at(i); scene && ue::is_a(t, scene, "SwordUserOptionsScene")) return scene;
    return 0;
}

void set_caption(cpu::Thread& t, GuestAddr item, const std::string& caption) {
    GuestAddr text = 0;
    int off = -1;
    if (ue::read_property(t, item, "Caption", text) && text && (off = ue::property_offset(t, text, "TextString")) >= 0)
        *gptr<ue::FString>(text + off) = ue::make_fstring(t, caption);
}

GuestAddr add_row(cpu::Thread& t, GuestAddr list, GuestAddr item_class, const char* handler, const char* caption) {
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
    if (o_class < 0 || o_update < 0 || o_caption < 0 || o_result < 0) return 0;
    alignas(16) u8 params[512] = {};
    *reinterpret_cast<u64*>(params + o_class) = item_class;
    if (handler) {  // a header row stays unbound: tapping it does nothing
        *reinterpret_cast<u64*>(params + o_update) = list;
        *reinterpret_cast<u64*>(params + o_update + 8) = ue::fname(t, handler);
    }
    *reinterpret_cast<ue::FString*>(params + o_caption) = ue::make_fstring(t, caption);
    if (!ue::call_event(t, list, "AddOption", params)) return 0;
    GuestAddr item = *reinterpret_cast<u64*>(params + o_result);
    // The row's text object keeps the caption's buffer, which ProcessEvent frees with the parameters:
    // give it a copy of its own.
    if (item) set_caption(t, item, caption);
    return item;
}

void set_checked(cpu::Thread& t, GuestAddr item, bool on) {
    alignas(16) u8 params[64] = {};
    params[0] = on;  // SetChecked(bool bChecked)
    ue::call_event(t, item, "SetChecked", params);
}

bool cheat_on(const char* key) {
    const auto& s = settings::get();
    std::string k = key;
    return k == "GodMode" ? s.god_mode : k == "UnlimitedSuper" ? s.unlimited_super : k == "FastForward" ? s.fast_forward
           : k == "GemShopRestock" ? s.gem_shop_restock : k == "AllGems" ? s.all_gems : false;
}

bool graphics_on(const char* key) {
    const auto& s = settings::get();
    return std::string(key) == "LightShafts" ? s.light_shafts : s.dynamic_shadows;
}

// Moves the rows added at the end of the list (from index `first`) to its top.
void move_to_top(ue::TArray<u64>& items, int first) {
    std::vector<u64> all(items.num);
    for (int i = 0; i < items.num; i++) all[i] = items.at(i);
    std::vector<u64> moved(all.begin() + first, all.end());
    moved.insert(moved.end(), all.begin(), all.begin() + first);
    for (int i = 0; i < items.num; i++) gptr<u64>(items.data)[i] = moved[i];
}

void add_rows(cpu::Thread& t, GuestAddr list) {
    ue::TArray<u64> items{};
    if (!ue::read_property(t, list, "Items", items)) return;
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
    const int first = items.num, game = is_infinity_blade_2() ? kIB2 : kIB3;
    g_ours.clear();
    g_rows.clear();
    int added = 0;
    for (const Row& r : kRows) {
        if (!(r.games & game)) continue;
        if (r.kind == Kind::Gap) {
            alignas(16) u8 gap[256] = {};
            ue::call_event(t, list, "AddListGap", gap);  // IB3 only; IB2 has no gaps
            continue;
        }
        GuestAddr item = add_row(t, list, r.kind == Kind::Check ? checkbox : button, r.handler, r.caption);
        if (!item) continue;
        added++;
        g_ours.insert(item);
        g_rows.push_back({item, &r});
        if (r.cheat) set_checked(t, item, cheat_on(r.cheat));
        // IB2's light shafts and shadows rows toggle the effect ("scale toggle ..."); their check mark starts as the
        // effect is (on unless the app's Settings turned it off at startup), so it shows the truth.
        if (r.graphics && game == kIB2) set_checked(t, item, graphics_on(r.graphics));
    }
    if (ue::read_property(t, list, "Items", items) && items.num > first) move_to_top(items, first);
    g_count = items.num;
    LOG_INFO("devmode: %d cheat rows added at the top of the Options list (%d items)", added, g_count);
}

// God mode is a flag of the player controller, and both games make a new controller for each area: the
// controller is checked every second and the console's "God" toggles it to the switch's state.
void apply_god_mode(cpu::Thread& t, GuestAddr pc, GuestAddr pawn) {
    alignas(16) u8 params[64] = {};
    if (!ue::call_event(t, pawn, "InGodMode", params)) return;  // InGodMode is a Pawn function
    bool on = params[0] != 0, want = settings::get().god_mode;
    if (on == want) return;
    int cmd = ue::param_offset(t, pc, "ConsoleCommand", "Command");
    if (cmd < 0) return;
    alignas(16) u8 c[256] = {};
    *reinterpret_cast<ue::FString*>(c + cmd) = ue::make_fstring(t, "God");
    ue::call_event(t, pc, "ConsoleCommand", c);
    LOG_INFO("cheats: god mode %s", want ? "on" : "off");
}

// The switches the game keeps as script bools: unlimited super and magic (the player), always fast-forward
// (the controller).
void apply_flag(cpu::Thread& t, GuestAddr obj, const char* prop, bool want, const char* what) {
    bool on = false;
    if (!obj || !ue::read_bool(t, obj, prop, on) || on == want) return;
    if (ue::write_bool(t, obj, prop, want)) LOG_INFO("cheats: %s %s", what, want ? "on" : "off");
}

// Unlimited magic: the magic meter (SwordPlayer.CurrentMagicLevel; magic can be cast at 1.0) is kept full while
// no spell is being cast, as the game's own FillSuperAndMagicMeters fills it. The HUD's bDebugMagic, which the
// game's developer toggle sets, is read by nothing in either game.
void fill_magic(cpu::Thread& t, GuestAddr pawn) {
    float level = 0;
    bool casting = false;
    int off = ue::property_offset(t, pawn, "CurrentMagicLevel");
    if (off < 0 || (level = *gptr<float>(pawn + off)) >= 1.0f) return;
    if (ue::read_bool(t, pawn, "bIsInMagicMode", casting) && casting) return;
    *gptr<float>(pawn + off) = 1.0f;
    static bool logged = false;
    if (!logged) LOG_INFO("cheats: magic meter refilled (unlimited magic)");
    logged = true;
}

// Commas every three digits, like the game's own numbers.
std::string grouped(int n) {
    std::string s = std::to_string(n);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert((size_t)i, ",");
    return s;
}

void run_pending(cpu::Thread& t) {
    if (GuestAddr item = g_gold_item) {
        g_gold_item = 0;
        int gold = cheat_current_gold(t);
        if (gold >= 0 && g_ours.count(item)) set_caption(t, item, "Give gold - you have " + grouped(gold));
    }
    Action a = g_pending;
    GuestAddr item = g_pending_item;
    g_pending = Action::None;
    if (a == Action::None) return;
    int n = cheat_give_all_items(t);
    std::string result = n < 0 ? "Get every item - not here" : n == 0 ? "Get every item - you have all"
                                                             : "Get every item - " + std::to_string(n) + " added";
    LOG_INFO("cheats: %s", result.c_str());
    if (item && g_ours.count(item)) set_caption(t, item, result);  // the row shows what happened
}

// Cheats, All gems ([Cheats] AllGems): the gem shop sells every gem, each at its highest level (the game's own
// cheat shop, SetPlayerCreateNewListOfStoreGems with bUseCheatGems and bCheatHighEndGems). It is filled once,
// and again when the game makes a new shop (a rebirth: the list's length changes; a sale keeps it). Turned off,
// the game makes its normal shop once. What was applied is kept in gemshop-applied, so this holds across runs.
constexpr const char* kGemShopState = "gemshop-applied";

void gem_shop_tick(cpu::Thread& t, GuestAddr pawn) {
    static bool loaded = false;
    static std::string applied;  // "all|<count>", empty: nothing applied
    if (!loaded) {
        loaded = true;
        std::ifstream f(kGemShopState);
        std::getline(f, applied);
    }
    ue::TArray<u8> list{};
    if (!ue::read_property(t, pawn, "CurrentStoreGems", list)) return;
    if (!settings::get().all_gems) {
        if (applied.empty()) return;
        int n = cheat_normal_gem_shop(t);
        LOG_INFO("cheats: gem shop back to normal (%d gems)", n);
        applied.clear();
        std::remove(kGemShopState);
        return;
    }
    if (applied != "all|" + std::to_string(list.num)) {
        int n = cheat_refill_gem_shop(t, "", true);
        if (n < 0) return;
        LOG_INFO("cheats: gem shop sells all gems (%d)", n);
        applied = "all|" + std::to_string(n);
        std::ofstream(kGemShopState) << applied << "\n";
    }
    cheat_max_gem_shop(t);  // any the game's cheat left below their highest level (IB2's has no switch for it)
}

}  // namespace

bool is_cheat_row_handler(const std::string& name) {
    if (name == "OnUpdateGiveGold") return true;  // runs as it is; its row then shows the gold
    if ((name == "OnUpdateToggleGodrays" || name == "OnUpdateToggleShadows") && is_infinity_blade_2())
        return true;  // runs as it is; the new state is kept in settings.ini
    for (const char* h : kSpare)
        if (name == h) return true;
    return false;
}

bool cheat_row_called(cpu::Thread& t, GuestAddr list, GuestAddr frame, const std::string& handler) {
    int off = ue::param_offset(t, list, handler, "Item");
    GuestAddr locals = *gptr<u64>(frame + kFrameLocals);
    GuestAddr item = off >= 0 && locals ? *gptr<u64>(locals + off) : 0;
    if (!item || !g_ours.count(item)) return false;  // not one of ours (the dev mod .ipa's own rows)
    if (handler == "OnUpdateGiveGold") {
        g_gold_item = item;
        return false;  // the game's own handler gives the gold
    }
    for (auto& [it, r] : g_rows) {
        if (it != item) continue;
        if (r->graphics) {
            if (!is_infinity_blade_2()) return false;
            // The tap has flipped the check mark; the game's handler then toggles the effect. Kept for the next start.
            bool on = false;
            ue::read_bool(t, item, "bIsChecked", on);
            settings::set_graphics(r->graphics, on);
            LOG_INFO("settings: %s %s (in-game Options)", r->graphics, on ? "on" : "off");
            return false;
        }
        if (r->cheat) {
            bool on = false;
            ue::read_bool(t, item, "bIsChecked", on);
            settings::set_cheat(r->cheat, on);
            LOG_INFO("cheats: %s %s (in-game Options)", r->cheat, on ? "on" : "off");
        } else if (r->action != Action::None) {
            g_pending = r->action;
            g_pending_item = item;
        }
        return true;
    }
    return false;
}

// The cheat console commands ("God"...) go to the player controller's CheatManager. IB3 creates one;
// IB2 does not, so its god mode did nothing. AddCheats(bForce) creates it.
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

// Cheats, Always fast forward: presses the HUD's fast-forward button (bottom right in cutscenes and the walks
// between fights) as soon as it shows, the way a tap does: SwordHudBase.MatineeFastForward(), which sets
// bFastForwardMode. (The game's own "always fast forward" developer flag is read by nothing.)
void auto_fast_forward(cpu::Thread& t) {
    static u64 last = 0;
    u64 now = GetTickCount64();
    if (now - last < 200) return;
    last = now;
    GuestAddr pc = ue::player_controller(t), hud = 0, button = 0;
    if (!pc || !ue::read_property(t, pc, "myHUD", hud) || !hud || !ue::is_a(t, hud, "SwordHudBase")) return;
    bool mode = false, disabled = false, hidden = true;
    if (!ue::read_property(t, hud, "FastForwardBttn", button) || !button) return;
    if (!ue::read_bool(t, hud, "bFastForwardMode", mode) || mode) return;
    if (ue::read_bool(t, hud, "bFastForwardDisabled", disabled) && disabled) return;
    if (!ue::read_bool(t, button, "bIsInvisible", hidden) || hidden) return;
    alignas(16) u8 params[256] = {};
    ue::call_event(t, hud, "MatineeFastForward", params);
    LOG_INFO("cheats: fast forward pressed");
}

void devmode_tick(cpu::Thread& t) {
    const auto& s = settings::get();
    run_pending(t);
    if (s.fast_forward) auto_fast_forward(t);
    // The switches, every second (they can change in the game's Options, and areas make new controllers).
    static u64 last = 0;
    u64 now = GetTickCount64();
    if (now - last >= 1000) {
        last = now;
        GuestAddr pc = ue::player_controller(t), pawn = 0;
        if (pc && ue::read_property(t, pc, "Pawn", pawn) && pawn && ue::is_a(t, pawn, "SwordPlayer")) {
            if (s.god_mode || s.developer_mode) ensure_cheat_manager(t);
            apply_god_mode(t, pc, pawn);
            // Where the game keeps them (OnToggleUnlimitedMagicSuper, OnUpdateFastForward): the super move on the
            // player, magic on the HUD, always-fast-forward on the common save file.
            GuestAddr hud = 0, common = 0;
            ue::read_property(t, pc, "myHUD", hud);
            ue::read_property(t, pc, "CommonSaveFile", common);
            apply_flag(t, pawn, "bDebugSuperMove", s.unlimited_super, "unlimited super");
            apply_flag(t, hud, "bDebugMagic", s.unlimited_super, "the dev flag for unlimited magic");  // read by nothing
            if (s.unlimited_super) fill_magic(t, pawn);
            apply_flag(t, common, "bDebugAlwaysFastForward", false, "the dev flag for always fast forward");  // read by nothing
            gem_shop_tick(t, pawn);
        }
    }
    if (!s.developer_mode) return;
    GuestAddr scene = options_scene(t), list = 0;
    if (scene) ue::read_property(t, scene, "OptionList", list);
    ue::TArray<u64> items{};
    if (!list || !ue::read_property(t, list, "Items", items)) {
        g_list = 0;
        return;
    }
    if (list == g_list && items.num >= g_count) return;
    add_rows(t, list);
}

}  // namespace game
