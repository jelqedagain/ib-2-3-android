// Save editor, game side (both games). The launcher's Edit save page writes the values the player wants
// into saveedit-pending.ini, and they are written into the save object the game has just read from disk,
// when SwordPC.FinishLoadingPlayerFromSaveGame starts building the player from it. (The game saves a
// snapshot of what it loaded plus what the game itself changed, so values changed in the middle of a
// session would not stick.) Once the player is in the world the game saves, and item actions (give all
// items...) run through the developers' own cheat functions. The current values go to saveedit-current.ini
// for the launcher to show.
//
// The games check a loaded save: IB3 resets a character whose level, XP and stats do not add up
// (SwordPlayer.FixHackedPlayerStats), and both look for bought gold that was not paid for
// (SwordPC.CheckGoldPurchasedHack). Once the player has used the editor (saveedit-used), those checks are
// skipped, so an edited save keeps its numbers; games whose saves were never edited run them as before.
//
// Debug (Android): `setprop debug.ibport.dumpsave N` (a new N each time) logs the player's save fields.
#include "game/game.h"
#include "game/unreal.h"
#include "hook.h"
#include "macho.h"
#include "settings.h"
#include <windows.h>
#include <cstdio>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif

namespace game {

namespace {

constexpr const char* kPendingFile = "saveedit-pending.ini";
constexpr const char* kCurrentFile = "saveedit-current.ini";
constexpr const char* kUsedMarker = "saveedit-used";
constexpr u32 kCurrencySize = 0x24;  // IB3: Currency[2] (gold, chips) of FCurrency { Current, Purchased, ... }
constexpr u32 kCurrencyTotalAcquired = 0x10;
constexpr u64 kFrameNode = 0x18;     // FFrame::Node (the function a frame runs)

GuestAddr g_tick = 0, g_process_internal = 0;  // trampolines
bool g_skip_checks = false, g_save_pending = false;

// Script functions skipped for edited saves: the games' checks that undo edited numbers.
const std::set<std::string> kSkippedChecks = {"FixHackedPlayerStats", "CheckGoldPurchasedHack", "LogHackedStatsEvent",
                                              "HackedSaveFile"};

// A number kept in the save (SwordSaveGame, in IB3 also its Stats struct) and on the player
// (SwordPlayer) under the same name.
struct Field {
    const char* id;    // key in the .ini files (the launcher uses the same ones)
    const char* prop;  // script property name
};
constexpr Field kFields[] = {
    {"Level", "PawnLevel"},
    {"XP", "CurrentXP"},  // XP toward the next level (0 right after a level-up)
    {"StatPoints", "AvailableStatPoints"},
    {"StatHealth", "PawnStatHealth"},
    {"StatShield", "PawnStatShield"},
    {"StatDamage", "PawnStatDamage"},
    {"StatMagic", "PawnStatMagic"},
    {"Bloodline", "GenerationCount"},  // IB3 shows it as Awakening
    {"MaxBloodline", "MaxGeneration"},
    {"WorldLevel", "WorldLevel"},
    {"NewGamePlus", "NewPlusCount"},
    {"GemCarry", "GemCarryUpgradeCount"},  // IB3
};

using Edits = std::map<std::string, std::string>;

bool file_exists(const char* path) { return std::ifstream(path).good(); }

struct Objects {
    GuestAddr pc = 0, pawn = 0, save = 0;
    bool ok() const { return pc && pawn && save; }
};

Objects find_objects(cpu::Thread& t) {
    Objects o;
    o.pc = ue::player_controller(t);
    if (!o.pc) return o;
    ue::read_property(t, o.pc, "Pawn", o.pawn);
    if (o.pawn && !ue::is_a(t, o.pawn, "SwordPlayer")) o.pawn = 0;
    ue::read_property(t, o.pc, "SaveGameObject", o.save);
    return o;
}

bool read_int(cpu::Thread& t, GuestAddr obj, const char* prop, int& out) {
    s32 v = 0;
    if (!obj || !ue::read_property(t, obj, prop, v)) return false;
    out = v;
    return true;
}

bool write_int(cpu::Thread& t, GuestAddr obj, const char* prop, int value) {
    int off = obj ? ue::property_offset(t, obj, prop) : -1;
    if (off < 0) return false;
    *gptr<s32>(obj + off) = value;
    return true;
}

// Gold (index 0) and IB3's chips (1): the current amount, or the lifetime total the gold checks compare
// it with. IB3 keeps them in Currency[2]; IB2 has CurrentGold and TotalGoldAquired (sic).
s32* currency(cpu::Thread& t, GuestAddr obj, int index, bool total = false) {
    if (!obj) return nullptr;
    if (!is_infinity_blade_2()) {
        int off = ue::property_offset(t, obj, "Currency");
        return off < 0 ? nullptr : gptr<s32>(obj + off + index * kCurrencySize + (total ? kCurrencyTotalAcquired : 0));
    }
    if (index != 0) return nullptr;
    int off = ue::property_offset(t, obj, total ? "TotalGoldAquired" : "CurrentGold");
    return off < 0 ? nullptr : gptr<s32>(obj + off);
}

std::map<std::string, int> current_values(cpu::Thread& t, const Objects& o) {
    std::map<std::string, int> v;
    if (s32* g = currency(t, o.pawn, 0)) v["Gold"] = *g;
    if (s32* c = currency(t, o.pawn, 1)) v["Chips"] = *c;
    int n;
    for (const Field& f : kFields)
        if (read_int(t, o.pawn, f.prop, n) || read_int(t, o.save, f.prop, n)) v[f.id] = n;
    ue::TArray<u8> items{};
    if (ue::read_property(t, o.save, "PlayerInventory", items)) v["Items"] = items.num;
    return v;
}

Edits read_edits() {
    Edits out;
    std::ifstream f(kPendingFile);
    std::string line;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        size_t eq = line.find('=');
        if (line.empty() || line[0] == ';' || line[0] == '[' || eq == std::string::npos || eq + 1 == line.size()) continue;
        out[line.substr(0, eq)] = line.substr(eq + 1);
    }
    return out;
}

void write_edits(const Edits& edits) {
    if (edits.empty()) {
        std::remove(kPendingFile);
        return;
    }
    std::ofstream f(kPendingFile, std::ios::binary);
    f << "[Edits]\n";
    for (auto& [k, v] : edits) f << k << "=" << v << "\n";
}

bool take(Edits& edits, const char* id, int& out) {
    auto it = edits.find(id);
    if (it == edits.end()) return false;
    out = (int)std::strtol(it->second.c_str(), nullptr, 10);
    edits.erase(it);
    return true;
}

// The game's own limits, for the launcher to keep edits within (worked out once per run).
std::string limits(cpu::Thread& t, const Objects& o) {
    static std::string cached;
    if (!cached.empty()) return cached;
    std::ostringstream s;
    ue::TArray<s32> xp{};  // XP needed to go from level i + 1 to i + 2
    if (ue::read_property(t, o.pawn, "NextLevelXPTargets", xp) && xp.num > 0) {
        s << "XPTable=";
        for (int i = 0; i < xp.num; i++) s << (i ? "," : "") << xp.at(i);
        s << "\n";
    }
    int n;
    if (read_int(t, o.pawn, "CSMaxAwakening", n) && n > 0) s << "MaxBloodline=" << n << "\n";
    // IB3's gem bag: the upgrade count after which more upgrades no longer make the bag bigger.
    int count = 0;
    if (!is_infinity_blade_2() && read_int(t, o.pawn, "GemCarryUpgradeCount", count)) {
        auto capacity = [&](int upgrades) {
            write_int(t, o.pawn, "GemCarryUpgradeCount", upgrades);
            alignas(16) u8 params[512] = {};  // GetMaxGemCarryCount(bool bNextUpgrade) -> int at +4
            ue::call_event(t, o.pawn, "GetMaxGemCarryCount", params);
            return *reinterpret_cast<s32*>(params + 4);
        };
        int max = 0;
        for (int u = 0, last = capacity(0); u < 200; u++) {
            int next = capacity(u + 1);
            if (next <= last) break;
            max = u + 1;
            last = next;
        }
        write_int(t, o.pawn, "GemCarryUpgradeCount", count);
        LOG_INFO("saveedit: gem bag takes %d upgrade(s) (%d gems now)", max, capacity(count));
        if (max > 0) s << "MaxGemCarry=" << max << "\n";
    }
    return cached = s.str();
}

void write_current(const std::map<std::string, int>& v, const std::string& limits) {
    std::ostringstream s;
    s << "; Written by the game: the player's current values, for the launcher's Edit save page.\n[Current]\n";
    s << "Game=" << (is_infinity_blade_2() ? "IB2" : "IB3") << "\n";
    for (auto& [k, n] : v) s << k << "=" << n << "\n";
    s << "[Limits]\n" << limits;
    std::string tmp = std::string(kCurrentFile) + ".tmp";
    std::ofstream(tmp, std::ios::binary) << s.str();
    std::rename(tmp.c_str(), kCurrentFile);
}

// Calls script function `func` of `obj`; `params` is its parameter block (then room for its locals).
bool call(cpu::Thread& t, GuestAddr obj, const char* func, u8* params) {
    bool ok = ue::call_event(t, obj, func, params);
    LOG_INFO("saveedit: %s%s", func, ok ? "" : " (not found)");
    return ok;
}
bool call(cpu::Thread& t, GuestAddr obj, const char* func) {
    alignas(16) u8 params[512] = {};
    return call(t, obj, func, params);
}

// --- While the save loads -----------------------------------------------------------------------

// SwordPC.FinishLoadingPlayerFromSaveGame is starting: the pending numbers go into the save it loaded.
void on_finish_loading(cpu::Thread& t, GuestAddr pc) {
    static bool done = false;  // once per run: later calls (map changes) carry what the player earned since
    GuestAddr obj = 0;
    if (done || !ue::read_property(t, pc, "SaveGameObject", obj) || !obj) return;
    done = true;
    Edits edits = read_edits();
    std::map<std::string, int> load_edits;
    int n;
    for (const char* id : {"Gold", "Chips"})
        if (take(edits, id, n)) load_edits[id] = n;
    for (const Field& f : kFields)
        if (take(edits, f.id, n)) load_edits[f.id] = n;
    // Safety limits (the launcher keeps edits in range too): nothing negative, room for the game to add to
    // gold, no level past the XP table (50) unless the save is there already, stats at least 1.
    int have = 0;
    for (auto& [id, value] : load_edits) {
        value = std::max(value, 0);
        if (id == "Gold" || id == "Chips") value = std::min(value, 999999999);
        if (id == "Level") value = std::clamp(value, 1, std::max(50, read_int(t, obj, "PawnLevel", have) ? have : 50));
        if ((id.rfind("Stat", 0) == 0 && id != "StatPoints") || id == "Bloodline") value = std::max(value, 1);
    }
    // A new level starts with no XP toward the next one (more XP than the level needs is what IB3's XP
    // check rejects).
    if (load_edits.count("Level") && !load_edits.count("XP")) load_edits["XP"] = 0;
    // Levels gained come with the stat points a level-up gives (LevelStatPointsPerLevel: 2 in both games),
    // unless the player set the points themselves.
    int old_level = 0, points = 0;
    if (load_edits.count("Level") && !load_edits.count("StatPoints") && read_int(t, obj, "PawnLevel", old_level) &&
        load_edits["Level"] > old_level && read_int(t, obj, "AvailableStatPoints", points))
        load_edits["StatPoints"] = points + 2 * (load_edits["Level"] - old_level);
    if (load_edits.empty()) return;
    write_edits(edits);  // what is left (item actions) is done in the world
    std::ofstream(kUsedMarker) << "The save editor was used: the games' save checks are skipped.\n";
    g_skip_checks = true;
    g_save_pending = true;
    LOG_INFO("saveedit: applying %zu edit(s) to the loaded save", load_edits.size());
    for (auto& [id, value] : load_edits) {
        if (id == "Gold" || id == "Chips") {
            s32* cur = currency(t, obj, id == "Chips");
            s32* total = currency(t, obj, id == "Chips", true);
            if (!cur) continue;
            if (total && value > *cur) *total += value - *cur;  // keep "gold ever acquired" >= gold
            *cur = value;
            LOG_INFO("saveedit: save %s = %d", id.c_str(), value);
            continue;
        }
        for (const Field& f : kFields) {
            if (id != f.id) continue;
            bool done_field = write_int(t, obj, f.prop, value);
            int off = ue::struct_member_offset(t, obj, "Stats", f.prop);  // IB3 keeps a copy there
            if (off >= 0) *gptr<s32>(obj + off) = value, done_field = true;
            LOG_INFO("saveedit: save %s = %d%s", f.prop, value, done_field ? "" : " (no such field)");
        }
    }
    write_int(t, obj, "PotentiallyManipulatedData", 0);
    write_int(t, obj, "PotentialGoldHack", 0);
}

// UObject::ProcessInternal runs a script function's body. The ones this file watches:
enum class Watched { None, Check, FinishLoading };
Watched watched(cpu::Thread& t, GuestAddr function) {
    static std::mutex mutex;
    static std::unordered_map<GuestAddr, Watched> known;
    std::lock_guard lock(mutex);
    auto it = known.find(function);
    if (it != known.end()) return it->second;
    std::string name = ue::object_name(t, function);
    Watched w = kSkippedChecks.count(name) ? Watched::Check
                : name == "FinishLoadingPlayerFromSaveGame" ? Watched::FinishLoading : Watched::None;
    return known[function] = w;
}

// --- In the world -------------------------------------------------------------------------------

void save_now(cpu::Thread& t, const Objects& o) {
    alignas(16) u8 params[512] = {};
    if (is_infinity_blade_2()) {
        // SaveGame(string Filename, bool bSkipCommonSave): the file of the slot being played.
        alignas(16) u8 name[512] = {};
        if (!call(t, o.pc, "GetCurrentSaveFilename", name)) return;
        LOG_INFO("saveedit: saving to %s", ue::read_fstring(gaddr(name)).c_str());
        std::memcpy(params, name, sizeof(ue::FString));  // the returned FString
    }
    call(t, o.pc, "SaveGame", params);  // IB3: SaveGame(int RequiredSave = 0, ...)
}

bool apply_in_world(cpu::Thread& t, const Objects& o, Edits edits) {
    int n;
    bool changed = false;
    // IB3 only: IB2 has the same cheats, but they do nothing there. IB3's all perks cheat also makes the
    // character level 50 with every stat at 100.
    if (!is_infinity_blade_2()) {
        if (take(edits, "GiveAllItems", n) && n) changed |= call(t, o.pc, "SetPlayerGiveAllItems");
        if (take(edits, "GiveAllPerks", n) && n) changed |= call(t, o.pc, "SetPlayerGiveAllPerks");
    }
    for (auto& [k, v] : edits) LOG_WARN("saveedit: edit %s was not applied", k.c_str());
    return changed;
}

void dump_save(cpu::Thread& t) {
    GuestAddr pc = ue::player_controller(t);
    if (!pc) {
        LOG_INFO("saveedit: no player controller yet");
        return;
    }
    ue::dump_values(t, pc);
    const char* words = "gold|chip|currenc|xp|level|stat|save|generation|bloodline|rebirth|potion|gem|item|perk|skill|give|hack";
    ue::dump_functions(t, pc, words);
    GuestAddr pawn = 0, save = 0;
    if (ue::read_property(t, pc, "Pawn", pawn) && pawn) {
        ue::dump_values(t, pawn);
        ue::dump_functions(t, pawn, words);
    }
    if (ue::read_property(t, pc, "SaveGameObject", save) && save) ue::dump_functions(t, save, words);
}

// IB3 raises its engine's frame-rate limit to 62 itself after reading its config (config.cpp's values), so
// a 30 or 120 limit is put on the engine object too. At 60 it runs as it always has (60-62).
void enforce_frame_cap(cpu::Thread& t) {
    if (is_infinity_blade_2() || settings::get().max_fps == 60) return;  // IB2: startup commands (config.cpp)
    GuestAddr engine = ue::engine();
    if (!engine) return;
    float want = (float)settings::get().max_fps, lo = 0, hi = 0;
    if (!ue::read_property(t, engine, "MaxSmoothedFrameRate", hi) || !ue::read_property(t, engine, "MinSmoothedFrameRate", lo))
        return;
    if (hi == want && lo == want) return;
    LOG_INFO("saveedit: engine frame rate %g-%g, set to %g (settings)", lo, hi, want);
    *gptr<float>(engine + ue::property_offset(t, engine, "MaxSmoothedFrameRate")) = want;
    *gptr<float>(engine + ue::property_offset(t, engine, "MinSmoothedFrameRate")) = want;
}

void on_tick(cpu::Thread& t) {
    static u64 last_check = 0, player_since = 0;
    static std::map<std::string, int> written;
    u64 now = GetTickCount64();
    if (now - last_check < 1000) return;
    last_check = now;
#ifdef __ANDROID__
    static std::string last_dump = "0";
    char v[PROP_VALUE_MAX] = "";
    if (__system_property_get("debug.ibport.dumpsave", v) > 0 && last_dump != v) {
        bool first = last_dump == "0";
        last_dump = v;
        if (!(first && std::string(v) == "0")) dump_save(t);
    }
#endif
    enforce_frame_cap(t);
    clashmob_tick(t);
    Objects o = find_objects(t);
    if (!o.ok()) {
        player_since = 0;
        return;
    }
    if (!player_since) player_since = now;
    if (now - player_since < 5000) return;  // let the player finish loading from the save first

    Edits edits = read_edits();
    if (!edits.empty()) {
        std::remove(kPendingFile);  // once only, even if something in it fails
        LOG_INFO("saveedit: applying %zu item action(s) in the world", edits.size());
        g_save_pending |= apply_in_world(t, o, edits);
    }
    if (g_save_pending) {  // the edited numbers are only in memory until the game saves
        g_save_pending = false;
        save_now(t, o);
    }
    auto values = current_values(t, o);
    if (values != written) {
        write_current(values, limits(t, o));
        written = values;
    }
}

}  // namespace

void install_save_editor(const macho::Image& img) {
    bool reflection = ue::init(img);
    GuestAddr tick = img.find("__ZN11UGameEngine4TickEf");
    if (!reflection || !tick) {
        LOG_WARN("saveedit: not available (reflection %d, tick %d)", reflection, !!tick);
        return;
    }
    g_tick = hook::install(tick, "UGameEngine::Tick", [](cpu::Thread& t) {
        on_tick(t);
        t.jump(g_tick);
    });
    // The script interpreter is hooked only when there are edits to make, or the save was edited (its
    // checks run on every load); otherwise the game runs exactly as before.
    g_skip_checks = file_exists(kUsedMarker);
    if (g_skip_checks || file_exists(kPendingFile) || clashmob_wants_script_hook()) {
        if (GuestAddr pi = img.find("__ZN7UObject15ProcessInternalER6FFramePv"))
            g_process_internal = hook::install(pi, "UObject::ProcessInternal", [](cpu::Thread& t) {
                if (clashmob_script_call(t, t.x(1), t.x(2))) return;
                GuestAddr fn = *gptr<u64>(t.x(1) + kFrameNode);
                Watched w = watched(t, fn);
                if (w == Watched::FinishLoading) {
                    on_finish_loading(t, t.x(0));
                } else if (w == Watched::Check && g_skip_checks) {
                    static std::set<GuestAddr> logged;
                    if (logged.insert(fn).second) LOG_INFO("saveedit: skipping the game's check %s", ue::object_name(t, fn).c_str());
                    if (t.x(2)) *gptr<u32>(t.x(2)) = 0;  // a check's result: nothing found
                    return;
                }
                t.jump(g_process_internal);
            });
    }
    LOG_INFO("saveedit: ready (interpreter hook %d, checks %s)", !!g_process_internal, g_skip_checks ? "skipped" : "on");
}

}  // namespace game
