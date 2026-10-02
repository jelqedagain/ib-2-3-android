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
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <unordered_map>
#include <vector>
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

// How many gems the gem bag holds (IB3: GetMaxGemCarryCount, which grows with bag upgrades; IB2: the fixed
// MaxUnequippedGems its HasFullGemInventory checks), or -1.
int gem_bag_size(cpu::Thread& t, GuestAddr pawn) {
    int n = -1;
    if (is_infinity_blade_2()) return read_int(t, pawn, "MaxUnequippedGems", n) ? n : -1;
    alignas(16) u8 params[512] = {};  // GetMaxGemCarryCount(bool bNextUpgrade) -> int at +4
    return ue::call_event(t, pawn, "GetMaxGemCarryCount", params) ? *reinterpret_cast<s32*>(params + 4) : -1;
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
    ue::TArray<u8> gems{};  // the gem bag: gems in it, and how many it holds
    if (ue::read_property(t, o.pawn, "PlayerUnequippedGems", gems)) v["Gems"] = gems.num;
    if (int size = gem_bag_size(t, o.pawn); size >= 0) v["GemBagSize"] = size;
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

// Loads the engine's item cache for class `cls` (as the menus do: Load / Free count requests) and passes
// each item to `fn`. Returns the number of items, or -1 without such a cache.
template <class Fn>
int for_each_cached_item(cpu::Thread& t, const char* cls, Fn fn) {
    GuestAddr engine = ue::engine();
    ue::TArray<GuestAddr> caches{}, classes{};
    if (!ue::read_property(t, engine, "ItemCaches", caches) || !ue::read_property(t, engine, "ItemCacheClasses", classes))
        return -1;
    for (int i = 0; i < caches.num && i < classes.num; i++) {
        GuestAddr cache = caches.at(i);
        int off = cache ? ue::property_offset(t, cache, "CurrentClass") : -1;
        if (off < 0 || !classes.at(i) || ue::object_name(t, classes.at(i)) != cls) continue;
        if (!*gptr<GuestAddr>(cache + off)) *gptr<GuestAddr>(cache + off) = classes.at(i);
        alignas(16) u8 params[256] = {};
        if (!ue::call_event(t, cache, "Load", params)) return -1;
        ue::TArray<GuestAddr> items{};
        ue::read_property(t, cache, "Items", items);
        for (int j = 0; j < items.num; j++)
            if (items.at(j)) fn(items.at(j));
        std::memset(params, 0, sizeof(params));
        ue::call_event(t, cache, "Free", params);
        return items.num;
    }
    return -1;
}

// The game's kinds of gems for the launcher's gem shop choice: GemKind.<template name>=<name the game shows>.
std::string gem_kinds(cpu::Thread& t) {
    std::ostringstream s;
    int kinds = 0;
    int n = for_each_cached_item(t, "SwordInventoryItemGem", [&](GuestAddr gem) {
        std::string name = ue::object_name(t, gem);
        if (name.rfind("Potion_", 0) == 0 || name.rfind("Spawn_", 0) == 0) return;  // potions, treasure touches
        auto text = [&](const char* func) {  // GetFriendlyName() / GetDescription(): the text the game shows
            int ret = ue::param_offset(t, gem, func, "ReturnValue");
            alignas(16) u8 params[512] = {};
            std::string s = ret >= 0 && ue::call_event(t, gem, func, params) ? ue::read_fstring(gaddr(params + ret)) : "";
            for (char& c : s)
                if (c == '\n' || c == '\r' || c == '|') c = ' ';
            return s;
        };
        std::string shown = text("GetFriendlyName");
        if (shown.empty()) return;
        s << "GemKind." << name << "=" << shown << "|" << text("GetDescription") << "\n";
        kinds++;
    });
    LOG_INFO("saveedit: %d gem kind(s) of %d cached gem item(s)", kinds, n);
    return s.str();
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
    s << gem_kinds(t);
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
enum class Watched { None, Check, FinishLoading, BuyStoreGem, RemoveStoreRow, CheatRow };
Watched watched(cpu::Thread& t, GuestAddr function) {
    static std::mutex mutex;
    static std::unordered_map<GuestAddr, Watched> known;
    std::lock_guard lock(mutex);
    auto it = known.find(function);
    if (it != known.end()) return it->second;
    std::string name = ue::object_name(t, function);
    Watched w = kSkippedChecks.count(name) ? Watched::Check
                : name == "FinishLoadingPlayerFromSaveGame" ? Watched::FinishLoading
                : name == "OnBuyStoreGem" ? Watched::BuyStoreGem
                : name == "RemoveItemFromItemList" ? Watched::RemoveStoreRow
                : is_cheat_row_handler(name) ? Watched::CheatRow : Watched::None;
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

// IB2's give-all cheat (SwordPlayer.GiveAllItems) hands out what the engine's item caches hold, and
// those are filled only while a menu needs them, so it gave nothing. The caches are loaded around it,
// the way the menus do (Load / Free count requests).
bool give_all_items_ib2(cpu::Thread& t, const Objects& o) {
    GuestAddr engine = ue::engine();
    ue::TArray<GuestAddr> caches{}, classes{};
    if (!ue::read_property(t, engine, "ItemCaches", caches) || !ue::read_property(t, engine, "ItemCacheClasses", classes))
        return false;
    std::vector<GuestAddr> loaded;
    for (int i = 0; i < caches.num; i++) {
        GuestAddr cache = caches.at(i), cls = 0;
        int off = cache ? ue::property_offset(t, cache, "CurrentClass") : -1;
        if (off < 0) continue;
        if (!*gptr<GuestAddr>(cache + off) && i < classes.num) *gptr<GuestAddr>(cache + off) = classes.at(i);
        cls = *gptr<GuestAddr>(cache + off);
        if (!call(t, cache, "Load")) continue;
        loaded.push_back(cache);
        ue::TArray<GuestAddr> items{};
        ue::read_property(t, cache, "Items", items);
        LOG_INFO("saveedit: item cache %d (%s) holds %d item(s)", i, cls ? ue::object_name(t, cls).c_str() : "?", items.num);
    }
    ue::TArray<u8> before{}, after{};
    ue::read_property(t, o.pawn, "PlayerInventory", before);
    bool ok = call(t, o.pc, "SetPlayerGiveAllItems");
    ue::read_property(t, o.pawn, "PlayerInventory", after);
    LOG_INFO("saveedit: inventory %d -> %d item(s)", before.num, after.num);
    for (GuestAddr cache : loaded) call(t, cache, "Free");
    return ok;
}

int array_count(cpu::Thread& t, GuestAddr obj, const char* prop) {
    ue::TArray<u8> a{};
    return ue::read_property(t, obj, prop, a) ? a.num : -1;
}

// The games' gem cheats (SwordPlayer.SetPlayerCreateNewListOfStoreGems / SwordPC.SetPlayerGiveRandomGem).
// Fills the gem shop: `type` = a gem's template name (IB3 Coalesced, e.g. FireGem), empty = one of every kind.
// `high`: IB3's bCheatHighEndGems (IB2's every-kind shop is always its highest levels). Returns how many gems
// the shop holds then, or -1.
int refill_gem_shop(cpu::Thread& t, const Objects& o, const std::string& type, bool high) {
    const char* fn = "SetPlayerCreateNewListOfStoreGems";
    alignas(16) u8 params[1024] = {};
    // bUseCheatGems: one of every kind of gem (AllSameType empty) or gems of one kind.
    int cheat_off = ue::param_offset(t, o.pawn, fn, "bUseCheatGems"), type_off = ue::param_offset(t, o.pawn, fn, "AllSameType"),
        high_off = ue::param_offset(t, o.pawn, fn, "bCheatHighEndGems");
    if (cheat_off >= 0) *reinterpret_cast<u32*>(params + cheat_off) = 1;
    if (type_off >= 0 && !type.empty()) *reinterpret_cast<ue::FString*>(params + type_off) = ue::make_fstring(t, type);
    if (high_off >= 0) *reinterpret_cast<u32*>(params + high_off) = high ? 1 : 0;
    int before = array_count(t, o.pawn, "CurrentStoreGems");
    bool changed = call(t, o.pawn, fn, params);
    int after = array_count(t, o.pawn, "CurrentStoreGems");
    LOG_INFO("saveedit: gem shop (%s%s): %d -> %d gem(s)", type.empty() ? "every kind" : type.c_str(), high ? ", high end" : "",
             before, after);
    return changed ? after : -1;
}

size_t shop_stride() { return is_infinity_blade_2() ? 16 : 24; }  // PlayerGemData: GemName, GemTier, ...

// The gems the store screen would show, made as it makes them (SwordPlayer.CreateStoreGemsList): one gem
// object per named slot of CurrentStoreGems, each knowing its slot (PlayerStoreGemIndex).
ue::TArray<GuestAddr> store_gem_objects(cpu::Thread& t, GuestAddr pawn) {
    alignas(16) u8 params[256] = {};
    int out = ue::param_offset(t, pawn, "CreateStoreGemsList", "StoreGems");
    if (out < 0 || !ue::call_event(t, pawn, "CreateStoreGemsList", params)) return {};
    return *reinterpret_cast<ue::TArray<GuestAddr>*>(params + out);
}

// Cheats, Strongest gems: every gem in the shop at its highest level and bonus, as the games' own high-end
// gems are made (GemTier = the number of upgrade tiers, the most SetAndValidateSaveData allows; the most
// random bonus there is). Each gem is changed through its object and written back with GetSaveData, so the
// slot holds what the game itself would save. Gems that cannot level (unique ones) stay as they are.
// Runs again only when the list changed (a new shop, a restocked gem). Returns how many slots changed.
int max_store_gems(cpu::Thread& t, GuestAddr pawn) {
    static std::vector<u8> done;  // the list as it was last made strongest
    ue::TArray<u8> list{};
    if (!ue::read_property(t, pawn, "CurrentStoreGems", list) || !list.data || list.num <= 0) return 0;
    const size_t stride = shop_stride(), bytes = (size_t)list.num * stride;
    u8* data = gptr<u8>(list.data);
    if (done.size() == bytes && std::memcmp(done.data(), data, bytes) == 0) return 0;
    int changed = 0;
    auto gems = store_gem_objects(t, pawn);
    for (int i = 0; i < gems.num; i++) {
        GuestAddr g = gems.at(i);
        s32 slot = -1;
        bool unique = false;
        ue::TArray<u8> tiers{};
        if (!g || !ue::read_property(t, g, "PlayerStoreGemIndex", slot) || slot < 0 || slot >= list.num) continue;
        if ((ue::read_bool(t, g, "bUniqueItem", unique) && unique) || !ue::read_property(t, g, "UpgradeTier", tiers)) continue;
        int tier_off = ue::property_offset(t, g, "GemTier"), add_off = ue::property_offset(t, g, "RandomAddPct");
        alignas(16) u8 mp[64] = {};
        int mr = ue::param_offset(t, g, "GetMaxRandomAdd", "ReturnValue");
        if (tier_off < 0 || add_off < 0 || mr < 0 || !ue::call_event(t, g, "GetMaxRandomAdd", mp)) continue;
        *gptr<u8>(g + tier_off) = (u8)tiers.num;
        *gptr<float>(g + add_off) = *reinterpret_cast<float*>(mp + mr);
        alignas(16) u8 sp[128] = {};
        int so = ue::param_offset(t, g, "GetSaveData", "GemSavedData");
        if (so < 0) continue;
        u8* entry = data + (size_t)slot * stride;
        std::memcpy(sp + so, entry, stride);  // whatever GetSaveData leaves alone stays as it was
        if (!ue::call_event(t, g, "GetSaveData", sp)) continue;
        if (std::memcmp(entry, sp + so, stride) != 0) changed++;
        std::memcpy(entry, sp + so, stride);
    }
    done.assign(data, data + bytes);
    if (changed) LOG_INFO("cheats: strongest gems: %d of %d shop gem(s) raised to their highest level", changed, gems.num);
    return changed;
}

bool give_all_items(cpu::Thread& t, const Objects& o) {
    return is_infinity_blade_2() ? give_all_items_ib2(t, o) : call(t, o.pc, "SetPlayerGiveAllItems");
}

bool apply_in_world(cpu::Thread& t, const Objects& o, Edits edits) {
    int n;
    bool changed = false;
    if (auto shop = edits.find("GemShop"); shop != edits.end()) {  // a gem's template name, or * for every kind
        std::string type = shop->second == "*" ? "" : shop->second;
        edits.erase(shop);
        int high = 0;
        take(edits, "GemShopHighEnd", high);
        changed |= refill_gem_shop(t, o, type, high != 0) >= 0;
    }
    if (take(edits, "GiveAllItems", n) && n) changed |= give_all_items(t, o);
    // IB3 only: IB2 has the same cheat, but it does nothing there. It also makes the character level 50
    // with every stat at 100.
    if (!is_infinity_blade_2() && take(edits, "GiveAllPerks", n) && n) changed |= call(t, o.pc, "SetPlayerGiveAllPerks");
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
    ue::TArray<GuestAddr> caches{};  // the engine's item caches (IB2: one per item type)
    if (ue::read_property(t, ue::engine(), "ItemCaches", caches))
        for (int i = 0; i < caches.num; i++) {
            GuestAddr c = caches.at(i);
            if (!c) continue;
            LOG_INFO("saveedit: item cache %d = %s (%s)", i, ue::object_name(t, c).c_str(), ue::class_name(t, c).c_str());
            ue::dump_properties(t, c);
            ue::dump_values(t, c, 1);
        }
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

// Cheats, Gem shop restock. The store screen lists gem objects made from the shop list (CurrentStoreGems,
// PlayerGemData: GemName first) when it opens. Buying one (BuyConsumable's buy button) runs
// SwordPlayer.OnBuyStoreGem, which moves that very object to the bag and marks its slot as sold (GemName None,
// the list keeps its length), then RemoveItemFromItemList drops its row from the screen. With restock on, the
// slot is filled again and the row, instead of being dropped, shows a new copy of the gem made from the slot,
// so the same gem can be bought again at once. A few times a second the list is also compared with a copy:
// a slot sold some other way gets its old bytes back. Other changes (a new shop) only refresh the copy.
constexpr u64 kFrameLocals = 0x30;  // FFrame::Locals: the running function's parameters
GuestAddr g_shop_pawn = 0, g_shop_data = 0;
int g_shop_num = 0;
std::vector<u8> g_shop_copy;

struct SoldGem {  // the shop gem OnBuyStoreGem is selling, until its row is dropped
    GuestAddr pawn = 0, gem = 0;
    int slot = -1;
    std::vector<u8> entry;  // its slot before the sale
} g_sold;

void note_bought_gem(cpu::Thread& t, GuestAddr pawn, GuestAddr frame) {
    g_sold = {};
    int off = ue::param_offset(t, pawn, "OnBuyStoreGem", "StoreGem");
    GuestAddr locals = *gptr<u64>(frame + kFrameLocals);
    GuestAddr gem = off >= 0 && locals ? *gptr<u64>(locals + off) : 0;
    s32 slot = -1;
    ue::TArray<u8> list{};
    if (!gem || !ue::read_property(t, gem, "PlayerStoreGemIndex", slot) || !ue::read_property(t, pawn, "CurrentStoreGems", list) ||
        !list.data || slot < 0 || slot >= list.num)
        return;
    const u8* entry = gptr<u8>(list.data) + (size_t)slot * shop_stride();
    g_sold = {pawn, gem, slot, std::vector<u8>(entry, entry + shop_stride())};
    LOG_INFO("saveedit: gem bought from the shop: %s (slot %d)", ue::object_name(t, gem).c_str(), slot);
}

// SwordItemListChildScene.RemoveItemFromItemList(ListItem) is about to drop the sold gem's row: true when the
// gem went back on the shelf instead (then the original is skipped).
bool restock_row(cpu::Thread& t, GuestAddr scene, GuestAddr frame) {
    SoldGem sold = std::move(g_sold);
    g_sold = {};
    if (!sold.gem) return false;
    int off = ue::param_offset(t, scene, "RemoveItemFromItemList", "ListItem");
    GuestAddr locals = *gptr<u64>(frame + kFrameLocals);
    GuestAddr row = off >= 0 && locals ? *gptr<u64>(locals + off) : 0, item = 0;
    if (!row || !ue::read_property(t, row, "Item", item) || item != sold.gem) return false;
    ue::TArray<u8> list{};
    const size_t stride = shop_stride();
    if (!ue::read_property(t, sold.pawn, "CurrentStoreGems", list) || !list.data || sold.slot >= list.num) return false;
    u8* entry = gptr<u8>(list.data) + (size_t)sold.slot * stride;
    std::memcpy(entry, sold.entry.data(), stride);  // back on the shelf
    if (list.data == g_shop_data && g_shop_copy.size() == (size_t)list.num * stride)
        std::memcpy(g_shop_copy.data() + (size_t)sold.slot * stride, sold.entry.data(), stride);
    // A new gem object from the slot, as CreateStoreGemsList makes them.
    const char* load = "LoadGemInstance";
    alignas(16) u8 lp[256] = {};
    int data_off = ue::param_offset(t, sold.pawn, load, "GemSavedData"), pc_off = ue::param_offset(t, sold.pawn, load, "InPC"),
        type_off = ue::param_offset(t, sold.pawn, load, "ForceType"), ret_off = ue::param_offset(t, sold.pawn, load, "ReturnValue");
    if (data_off < 0 || ret_off < 0) return false;
    std::memcpy(lp + data_off, entry, stride);
    if (pc_off >= 0) *reinterpret_cast<GuestAddr*>(lp + pc_off) = ue::player_controller(t);
    if (type_off >= 0) {  // IB3: the item type it is given (optional there, so not filled in when called like this)
        u8 type = 13;
        ue::read_property(t, sold.gem, "ItemType", type);
        lp[type_off] = type;
    }
    if (!ue::call_event(t, sold.pawn, load, lp)) return false;
    GuestAddr gem = *reinterpret_cast<GuestAddr*>(lp + ret_off);
    int slot_off = gem ? ue::property_offset(t, gem, "PlayerStoreGemIndex") : -1;
    if (slot_off < 0) return false;
    *gptr<s32>(gem + slot_off) = sold.slot;
    // The row shows it. Init also scales the row by the screen size over 960x640; those sizes keep it as it is.
    alignas(16) u8 ip[256] = {};
    int item_off = ue::param_offset(t, row, "Init", "NewItem"), w_off = ue::param_offset(t, row, "Init", "ScreenWidth"),
        h_off = ue::param_offset(t, row, "Init", "ScreenHeight"), p_off = ue::param_offset(t, row, "Init", "P");
    if (item_off < 0) return false;
    *reinterpret_cast<GuestAddr*>(ip + item_off) = gem;
    if (w_off >= 0) *reinterpret_cast<s32*>(ip + w_off) = 960;
    if (h_off >= 0) *reinterpret_cast<s32*>(ip + h_off) = 640;
    if (p_off >= 0) *reinterpret_cast<GuestAddr*>(ip + p_off) = sold.pawn;  // IB2: for its sale price
    if (!ue::call_event(t, row, "Init", ip)) return false;
    if (int sel = ue::property_offset(t, scene, "SelectedConsumable"); sel >= 0 && *gptr<GuestAddr>(scene + sel) == sold.gem)
        *gptr<GuestAddr>(scene + sel) = gem;  // the buy button sells the new one next
    alignas(16) u8 none[256] = {};
    ue::call_event(t, scene, "UpdateItemList", none);
    LOG_INFO("saveedit: gem shop restocked: %s back on the shelf (slot %d), its row kept", ue::object_name(t, gem).c_str(), sold.slot);
    return true;
}

void refresh_open_store(cpu::Thread& t) {
    GuestAddr input = ue::player_input(t);
    ue::TArray<u64> stack{};
    if (input && ue::read_property(t, input, "MobileMenuStack", stack))
        for (int i = stack.num - 1; i >= 0; i--)
            if (GuestAddr scene = stack.at(i); scene && ue::is_a(t, scene, "SwordItemListScene")) {
                alignas(16) u8 none[256] = {};
                ue::call_event(t, scene, "UpdateItemList", none);
                return;
            }
}

// debug.ibport.shopdump <new value>: logs the shop list (gem names, tiers) and the rows of an open store
// screen (gem, tier, shown or filtered out), to check what the player sees.
std::string gem_label(cpu::Thread& t, GuestAddr g) {
    u8 tier = 0;
    ue::read_property(t, g, "GemTier", tier);
    std::string name = ue::object_name(t, g);
    u64 tmpl = 0;
    if (ue::read_property(t, g, "ObjectTemplateName", tmpl) && tmpl) name = ue::name_string(t, tmpl);
    return name + ":" + std::to_string(tier);
}

void shop_dump(cpu::Thread& t) {
#ifdef __ANDROID__
    static std::string last = "0";
    char v[PROP_VALUE_MAX] = "";
    __system_property_get("debug.ibport.shopdump", v);
    if (last == v) return;
    bool first = last == "0";
    last = v;
    if (first && std::string(v) == "0") return;
    Objects o = find_objects(t);
    if (!o.pawn) return;
    const size_t stride = shop_stride();
    ue::TArray<u8> list{};
    if (ue::read_property(t, o.pawn, "CurrentStoreGems", list) && list.data) {
        LOG_INFO("shopdump: shop list: %d slot(s)", list.num);
        for (int j = 0; j < list.num; j += 30) {
            std::string names;
            for (int i = j; i < std::min(list.num, j + 30); i++) {
                u64 n;
                std::memcpy(&n, gptr<u8>(list.data) + i * stride, 8);
                names += " " + ue::name_string(t, n) + ":" + std::to_string(gptr<u8>(list.data)[i * stride + 8]);
            }
            LOG_INFO("shopdump:   slots:%s", names.c_str());
        }
    }
    GuestAddr input = ue::player_input(t);
    ue::TArray<u64> stack{};
    if (!input || !ue::read_property(t, input, "MobileMenuStack", stack)) return;
    for (int i = 0; i < stack.num; i++) {
        GuestAddr scene = stack.at(i), menu = 0;
        ue::TArray<GuestAddr> pages{};
        if (!scene || !ue::read_property(t, scene, "ItemListMenu", menu) || !menu || !ue::read_property(t, menu, "Items", pages)) continue;
        for (int k = 0; k < pages.num; k++) {
            GuestAddr page = pages.at(k), il = 0;
            ue::TArray<GuestAddr> rows{};
            if (!page || !ue::read_property(t, page, "ItemList", il) || !il || !ue::read_property(t, il, "Items", rows)) continue;
            std::vector<std::string> shown, hidden;
            for (int r = 0; r < rows.num; r++) {
                GuestAddr row = rows.at(r), item = 0;
                bool vis = false;
                if (!row || !ue::read_property(t, row, "Item", item) || !item || !ue::is_a(t, item, "SwordInventoryItemGem")) continue;
                ue::read_bool(t, row, "bIsVisible", vis);
                (vis ? shown : hidden).push_back(gem_label(t, item));
            }
            if (shown.empty() && hidden.empty()) continue;
            LOG_INFO("shopdump: screen %s page %d: %zu gem row(s) shown, %zu filtered out", ue::object_name(t, scene).c_str(), k,
                     shown.size(), hidden.size());
            for (auto* v : {&shown, &hidden})  // Android's log cuts long lines: 30 a line
                for (size_t j = 0; j < v->size(); j += 30) {
                    std::string line;
                    for (size_t q = j; q < std::min(v->size(), j + 30); q++) line += " " + (*v)[q];
                    LOG_INFO("shopdump:   %s:%s", v == &shown ? "shown" : "filtered out", line.c_str());
                }
        }
    }
#endif
}

void restock_tick(cpu::Thread& t) {
    static u64 last = 0;
    u64 now_ms = GetTickCount64();
    if (now_ms - last < 250) return;
    last = now_ms;
    g_sold = {};  // a sale whose row was never dropped (OnBuyStoreGem from somewhere else)
    shop_dump(t);
    if (!settings::get().gem_shop_restock) {
        g_shop_copy.clear();
        return;
    }
    GuestAddr pc = ue::player_controller(t), pawn = 0;
    if (!pc || !ue::read_property(t, pc, "Pawn", pawn) || !pawn || !ue::is_a(t, pawn, "SwordPlayer")) return;
    ue::TArray<u8> list{};
    if (!ue::read_property(t, pawn, "CurrentStoreGems", list) || !list.data || list.num <= 0) return;
    const size_t stride = shop_stride(), bytes = (size_t)list.num * stride;
    u8* now = gptr<u8>(list.data);
    if (pawn != g_shop_pawn || list.data != g_shop_data || list.num != g_shop_num || g_shop_copy.size() != bytes) {
        g_shop_pawn = pawn, g_shop_data = list.data, g_shop_num = list.num;
        g_shop_copy.assign(now, now + bytes);
        return;
    }
    const u64 none = ue::fname(t, "None");
    int restocked = 0;
    for (int i = 0; i < list.num; i++) {
        u8* entry = now + i * stride;
        u8* copy = g_shop_copy.data() + i * stride;
        if (std::memcmp(entry, copy, stride) == 0) continue;
        u64 name_now, name_was;
        std::memcpy(&name_now, entry, 8);
        std::memcpy(&name_was, copy, 8);
        if (name_now == none && name_was != none) {
            std::memcpy(entry, copy, stride);  // sold: back on the shelf
            restocked++;
        } else {
            std::memcpy(copy, entry, stride);  // the game changed it (a new gem): keep that
        }
    }
    if (!restocked) return;
    refresh_open_store(t);
    LOG_INFO("saveedit: gem shop restocked (%d gem(s) back on the shelf, %d in the shop)", restocked, list.num);
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

// For the in-game CHEATS rows (game/devmode.cpp). They save the game afterwards, as the launcher's edits do.
int cheat_refill_gem_shop(cpu::Thread& t, const std::string& type, bool strongest) {
    Objects o = find_objects(t);
    if (!o.ok()) return -1;
    int n = refill_gem_shop(t, o, type, strongest);
    if (n >= 0) save_now(t, o);
    return n;
}

int cheat_max_gem_shop(cpu::Thread& t) {
    Objects o = find_objects(t);
    return o.ok() ? max_store_gems(t, o.pawn) : -1;
}

int cheat_current_gold(cpu::Thread& t) {
    Objects o = find_objects(t);
    s32* g = o.ok() ? currency(t, o.pawn, 0) : nullptr;
    return g ? *g : -1;
}

int cheat_normal_gem_shop(cpu::Thread& t) {
    Objects o = find_objects(t);
    if (!o.ok()) return -1;
    alignas(16) u8 params[1024] = {};  // SetPlayerCreateNewListOfStoreGems with bUseCheatGems off: a normal shop
    if (!call(t, o.pawn, "SetPlayerCreateNewListOfStoreGems", params)) return -1;
    save_now(t, o);
    return array_count(t, o.pawn, "CurrentStoreGems");
}

int cheat_give_all_items(cpu::Thread& t) {
    Objects o = find_objects(t);
    if (!o.ok()) return -1;
    int before = array_count(t, o.pawn, "PlayerInventory");
    if (!give_all_items(t, o)) return -1;
    save_now(t, o);
    return std::max(0, array_count(t, o.pawn, "PlayerInventory") - std::max(0, before));
}

void install_save_editor(const macho::Image& img) {
    bool reflection = ue::init(img);
    GuestAddr tick = img.find("__ZN11UGameEngine4TickEf");
    if (!reflection || !tick) {
        LOG_WARN("saveedit: not available (reflection %d, tick %d)", reflection, !!tick);
        return;
    }
    g_tick = hook::install(tick, "UGameEngine::Tick", [](cpu::Thread& t) {
        on_tick(t);
        credits_tick(t);
        devmode_tick(t);
        wheel_tick(t);
        options_item_caches_tick(t);
        restock_tick(t);
        t.jump(g_tick);
    });
    // The script interpreter is hooked only when there are edits to make, or the save was edited (its
    // checks run on every load); otherwise the game runs exactly as before.
    g_skip_checks = file_exists(kUsedMarker);
    if (g_skip_checks || file_exists(kPendingFile) || settings::get().gem_shop_restock || settings::get().developer_mode) {
        if (GuestAddr pi = img.find("__ZN7UObject15ProcessInternalER6FFramePv"))
            g_process_internal = hook::install(pi, "UObject::ProcessInternal", [](cpu::Thread& t) {
                GuestAddr fn = *gptr<u64>(t.x(1) + kFrameNode);
                Watched w = watched(t, fn);
                if (w == Watched::FinishLoading) {
                    on_finish_loading(t, t.x(0));
                } else if (w == Watched::CheatRow && settings::get().developer_mode) {
                    if (cheat_row_called(t, t.x(0), t.x(1), ue::object_name(t, fn))) return;  // handled: skip the original
                } else if (w == Watched::BuyStoreGem && settings::get().gem_shop_restock) {
                    note_bought_gem(t, t.x(0), t.x(1));
                } else if (w == Watched::RemoveStoreRow && settings::get().gem_shop_restock) {
                    if (restock_row(t, t.x(0), t.x(1))) return;  // the row stays, with a new copy of the gem
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
