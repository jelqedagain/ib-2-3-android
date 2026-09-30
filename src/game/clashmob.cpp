// ClashMobs offline (Infinity Blade III): the port answers the game's ClashMob server requests itself
// (McpClashMobManagerV3: /sword/api/challenges...), with events defined locally, so ClashMobs can be played
// without Epic's servers. Every other online request stays offline.
//
// The game needs an MCP account id for everything about the player (joining, progress, rewards). It only
// makes one through Facebook / Google+ / Game Center, so the port gives it a local one (clashmob_tick).
// Progress is kept in clashmob-state.ini next to the saves.
#include "game/game.h"
#include "game/unreal.h"
#include "foundation/foundation.h"
#include "libc/vfs.h"
#include "macho.h"
#include <windows.h>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#endif
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <iterator>
#include <map>
#include <unordered_map>
#include <mutex>
#include <random>
#include <sstream>
#include <string>

namespace game {

namespace {

constexpr const char* kIdFile = "clashmob-id";
constexpr const char* kStateFile = "clashmob-state.ini";

GuestAddr g_dlmalloc = 0;  // the game's allocator: strings handed to its scripts must come from it

// ---- the events ----
//
// Events are read from clashmob-events.ini next to the saves when there is one, else the ones below. One section
// per event. The port's own keys:
//   Days=7         how long the event runs; it then starts again, with everyone's progress reset
//   Goal=5         the event's goal (shown in its details)
//   Score=Total    what the reward tiers count: Total (all plays together) or Best (the best single play)
// Every other line goes into the event file the game downloads: SwordBattleEvent properties such as BattleType,
// BossObj (a SwordBossItems.ini item), BossLevel / BossScaledLevel (the boss is at least BossLevel, and at the
// player's level times BossScaledLevel), BossHealth, BossBaseDamage, EndTime (seconds per play, 30 by default),
// MaxPlays, reward tiers (.RewardType = an eTouchRewardActor treasure, .RewardData, .RewardGoal; ".Key=" adds an
// array entry), MapName, SubMapName (the scenery) and QuestMapPin (where it shows on the world map). Keep the
// tiers' money to one currency: the event's summary adds up all tiers and labels the sum with the last one's.
//
// What one play scores, by BattleType: BT_KillNBosses 1 per boss killed (one boss per play), BT_Kill1Boss the
// damage done to one big boss, BT_TimeSurvival the seconds survived, BT_TreasureCollection the bags collected,
// BT_BattleChallengeTrigger the number of BattleChallengeFocus actions (parries...). A play ends when the fight
// does, so a survival boss needs a BossHealth no one can get through in EndTime seconds.

constexpr const char* kEventsFile = "clashmob-events.ini";

constexpr const char* kDefaultEvents = R"([port-darkknight]
Days=7
Goal=5
Score=Total
Title=The Dark Knight Trial
Desc=A band of DARK KNIGHTS has overrun the Obelisk! Kill one each time you play. Kill 5 to earn every reward.
BattleType=BT_KillNBosses
BossObj=10ft_SnS_BlackKnight
BossLevel=10
BossScaledLevel=1.0
MaxPlays=10
.RewardType=TRA_Gold_Large
.RewardData=
.RewardGoal=1
.RewardType=TRA_GrabBag_Uber
.RewardData=
.RewardGoal=3
.RewardType=TRA_GrabBag_LargeGem
.RewardData=
.RewardGoal=5
MapName=00_ClashMob_BaseScripting
SubMapName=cm_obelisk_art
QuestMapPin=MapPin_Obelisk_A

[port-goliath]
Days=7
Goal=100000
Score=Best
Title=Clash with the MX-Goliath
Desc=The MX-GOLIATH has 100,000 health. Do as much damage as you can in 30 seconds!
BattleType=BT_Kill1Boss
BossObj=20ft_B_MX-Goliath
BossLevel=10
BossScaledLevel=1.0
BossHealth=100000
MaxPlays=10
.RewardType=TRA_Gold_Medium
.RewardData=
.RewardGoal=500
.RewardType=TRA_GrabBag_LargeGem
.RewardData=
.RewardGoal=2000
.RewardType=TRA_GrabBag_Uber
.RewardData=
.RewardGoal=5000
MapName=00_ClashMob_BaseScripting
SubMapName=cm_dunes_art
QuestMapPin=MapPin_Dunes_A

[port-emberknight]
Days=7
Goal=60
Score=Best
Title=Survive the Ember Knight
Desc=The EMBER KNIGHT cannot be beaten. Stay alive as long as you can!
BattleType=BT_TimeSurvival
BossObj=10ft_SnS_LavaLord
BossLevel=15
BossScaledLevel=1.5
BossHealth=10000000
EndTime=60
MaxPlays=10
.RewardType=TRA_Gold_Medium
.RewardData=
.RewardGoal=15
.RewardType=TRA_Gold_Large
.RewardData=
.RewardGoal=30
.RewardType=TRA_GrabBag_Uber
.RewardData=
.RewardGoal=60
MapName=00_ClashMob_BaseScripting
SubMapName=C01_CM_Monastery_Art
QuestMapPin=MapPin_Monastary_A
)";

struct Event {
    std::string id;        // this run of the event (its section name and the day it started)
    time_t start, end;
    int goal;
    bool total;            // reward tiers count all plays together (else the best play)
    std::string file;      // the SwordBattleEvent ini the game downloads
};

std::vector<Event> events() {
    std::string text;
    if (std::ifstream f{kEventsFile}) text.assign(std::istreambuf_iterator<char>(f), {});
    if (text.empty()) text = kDefaultEvents;

    std::vector<Event> out;
    std::string line, name;
    int days = 7;
    Event ev{};
    auto finish = [&] {
        if (name.empty()) return;
        time_t now = time(nullptr), period = std::max(days, 1) * 86400;
        ev.start = now - now % period;
        ev.end = ev.start + period;
        ev.id = name + "-" + std::to_string(ev.start / 86400);
        ev.file = "[SwordBattleEvent]\nVersion=1.4\n" + ev.file;  // must list SwordClashMobManager.BattleEventVersion
        out.push_back(ev);
    };
    std::istringstream in(text);
    while (std::getline(in, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.empty() || line[0] == ';') continue;
        if (line[0] == '[' && line.back() == ']') {
            finish();
            name = line.substr(1, line.size() - 2);
            days = 7;
            ev = Event{};
            ev.goal = 1;
            ev.total = true;
            continue;
        }
        size_t eq = line.find('=');
        std::string key = eq == std::string::npos ? line : line.substr(0, eq), value = eq == std::string::npos ? "" : line.substr(eq + 1);
        if (key == "Days") days = std::atoi(value.c_str());
        else if (key == "Goal") ev.goal = std::atoi(value.c_str());
        else if (key == "Score") ev.total = _stricmp(value.c_str(), "Best") != 0;
        else ev.file += line + "\n";
    }
    finish();
    return out;
}

// ---- the player's progress ----

struct Status {
    std::string slot;
    int attempts = 0, successful = 0, progress = 0, high = 0, award = 0;
    bool complete = false, accepted = false;
    time_t accept_time = 0, update_time = 0;
};

std::mutex g_mutex;
std::map<std::string, Status> g_status;  // by challenge id
bool g_loaded = false;

void load_state() {
    if (g_loaded) return;
    g_loaded = true;
    std::ifstream f(kStateFile);
    std::string line, section;
    while (std::getline(f, line)) {
        while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
        if (line.size() > 2 && line[0] == '[') {
            section = line.substr(1, line.size() - 2);
            continue;
        }
        size_t eq = line.find('=');
        if (section.empty() || eq == std::string::npos) continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        Status& s = g_status[section];
        long long n = std::strtoll(v.c_str(), nullptr, 10);
        if (k == "Slot") s.slot = v;
        else if (k == "Attempts") s.attempts = (int)n;
        else if (k == "Successful") s.successful = (int)n;
        else if (k == "Progress") s.progress = (int)n;
        else if (k == "High") s.high = (int)n;
        else if (k == "Award") s.award = (int)n;
        else if (k == "Complete") s.complete = n != 0;
        else if (k == "Accepted") s.accepted = n != 0;
        else if (k == "AcceptTime") s.accept_time = (time_t)n;
        else if (k == "UpdateTime") s.update_time = (time_t)n;
    }
}

void save_state() {
    std::ofstream f(kStateFile, std::ios::binary);
    f << "; ClashMob progress, kept by the port (offline ClashMobs)\n";
    for (auto& [id, s] : g_status)
        f << "[" << id << "]\nSlot=" << s.slot << "\nAttempts=" << s.attempts << "\nSuccessful=" << s.successful
          << "\nProgress=" << s.progress << "\nHigh=" << s.high << "\nAward=" << s.award << "\nComplete=" << s.complete
          << "\nAccepted=" << s.accepted << "\nAcceptTime=" << (long long)s.accept_time
          << "\nUpdateTime=" << (long long)s.update_time << "\n";
}

// The local player's MCP id, made once.
const std::string& player_id() {
    static std::string id;
    if (!id.empty()) return id;
    std::ifstream(kIdFile) >> id;
    if (id.empty()) {
        std::random_device rd;
        char buf[40];
        snprintf(buf, sizeof buf, "port-%08x%08x", rd(), rd());
        id = buf;
        std::ofstream(kIdFile) << id;
    }
    return id;
}

// ---- JSON ----

std::string iso_time(time_t t) {
    if (!t) return "";
    char buf[32];
    struct tm tm_utc;
    gmtime_r(&t, &tm_utc);
    strftime(buf, sizeof buf, "%Y-%m-%dT%H:%M:%S.000Z", &tm_utc);
    return buf;
}

struct Json {
    std::string s = "{";
    Json& add(const char* key, const std::string& raw) {
        if (s.size() > 1) s += ",";
        s += "\"" + std::string(key) + "\":" + raw;
        return *this;
    }
    Json& str(const char* key, const std::string& v) { return add(key, "\"" + v + "\""); }
    Json& num(const char* key, long long v) { return add(key, std::to_string(v)); }
    Json& flag(const char* key, bool v) { return add(key, v ? "true" : "false"); }
    std::string done() { return s + "}"; }
};

// One challenge as McpClashMobManagerV3.ParseChallenge reads it (every key must be present).
std::string challenge_json(const Event& e, const Status& s) {
    time_t now = time(nullptr);
    bool over = now >= e.end, won = s.progress >= e.goal;
    return Json()
        .str("challengeId", e.id)
        .str("visibleDate", iso_time(e.start - 3600))
        .str("startDate", iso_time(e.start))
        .str("endDate", iso_time(e.end))
        .str("completedDate", over || won ? iso_time(won ? s.update_time : e.end) : "")
        .str("purgeDate", iso_time(e.end + 30 * 86400))
        .str("challengeType", "SOLO")  // "SOCIAL" is a co-op mob (parent and child rounds)
        .num("attempts", s.attempts)
        .num("successfulAttempts", s.successful)
        .num("goalValue", e.goal)
        .num("goalStartValue", 0)
        .num("goalCurrentValue", s.progress)
        .flag("started", now >= e.start)
        .flag("visible", true)
        .flag("completed", over || won)
        .flag("successful", won)
        .str("facebookId", "")
        .num("facebookLikes", 0)
        .num("facebookComments", 0)
        .num("facebookLikeScalar", 0)
        .num("facebookCommentScalar", 0)
        .num("facebookLikeGoalProgress", 0)
        .num("facebookCommentGoalProgress", 0)
        .str("twitterId", "")
        .num("twitterRetweets", 0)
        .num("twitterGoalProgress", 0)
        .num("twitterRetweetsScalar", 0)
        .str("parentChallengeId", "")
        .str("activeChildChallengeId", "")
        .add("childChallengeList", "[]")
        .str("childChallengeGatingType", "SUCCESS")
        .num("childChallengeGatingValue", 0)
        .str("challengeRatingType", "TOTAL_PROGRESS")
        .str("startedAt", iso_time(e.start))
        .num("minChallengeDuration", 0)
        .add("files", "[" + Json()
                                .str("filename", "BattleEvent_1.4.ib3")
                                .str("uniqueFileName", e.id + "_BattleEvent_1.4.ib3")
                                .str("hash", std::to_string(std::hash<std::string>()(e.file)))
                                .str("type", "ib3")
                                .flag("shouldKeepPostChallenge", false)
                                .done() +
                              "]")
        .done();
}

// The player's status in a challenge, as ParseUserChallengeStatus reads it.
// The game compares highGoalProgress with the reward tiers; for "Score=Total" events that is every play together.
std::string status_json(const Event& e, const Status& s) {
    return Json()
        .str("challengeId", e.id)
        .str("epicId", "")  // the game's account id: none (see clashmob_script_call)
        .str("saveSlotId", s.slot)
        .num("numAttempts", s.attempts)
        .num("numSuccessfulAttempts", s.successful)
        .num("goalProgress", s.progress)
        .flag("didComplete", s.complete)
        .str("lastUpdateTime", iso_time(s.update_time))
        .num("userAwardGiven", s.award)
        .str("acceptTime", iso_time(s.accept_time))
        .flag("didPreregister", false)
        .flag("likedViaFacebook", false)
        .flag("commentedViaFacebook", false)
        .flag("retweeted", false)
        .num("highGoalProgress", e.total ? s.progress : s.high)
        .num("rank", 1)
        .num("percentRank", 100)
        .done();
}

// ---- requests ----

void split_url(const std::string& url, std::string& path, std::string& query) {
    size_t start = url.find("://");
    start = start == std::string::npos ? 0 : url.find('/', start + 3);
    if (start == std::string::npos) start = url.size();
    size_t q = url.find('?', start);
    path = url.substr(start, q == std::string::npos ? std::string::npos : q - start);
    query = q == std::string::npos ? "" : url.substr(q + 1);
}

std::string query_value(const std::string& query, const std::string& key) {
    std::istringstream in(query);
    std::string part;
    while (std::getline(in, part, '&'))
        if (part.rfind(key + "=", 0) == 0) return part.substr(key.size() + 1);
    return "";
}

// "/sword/api/x" -> {"sword", "api", "x"}. Empty parts in the middle stay: the player has no account id, so
// the game asks for ".../users//saveSlots/0".
std::vector<std::string> split_path(const std::string& path) {
    std::vector<std::string> out;
    std::istringstream in(path.size() > 1 && path[0] == '/' ? path.substr(1) : path);
    std::string part;
    while (std::getline(in, part, '/')) out.push_back(part);
    return out;
}

const Event* find_event(const std::vector<Event>& all, const std::string& id) {
    for (auto& e : all)
        if (e.id == id) return &e;
    return nullptr;
}

bool serve(const ns::HttpRequest& req, ns::HttpResponse& resp) {
    std::string path, query;
    split_url(req.url, path, query);
    std::string body(req.body.begin(), req.body.begin() + std::min<size_t>(req.body.size(), 600));
    LOG_INFO("clashmob: %s %s%s%s %s", req.method.c_str(), path.c_str(), query.empty() ? "" : "?", query.c_str(), body.c_str());

    auto p = split_path(path);
    if (p.size() < 3 || p[0] != "sword" || p[1] != "api" || p[2] != "challenges") return false;  // offline
    std::lock_guard lock(g_mutex);
    load_state();
    auto all = events();
    time_t now = time(nullptr);

    // GET /sword/api/challenges: the event list
    if (p.size() == 3 && req.method == "GET") {
        resp.body = "[";
        for (auto& e : all) resp.body += (resp.body.size() > 1 ? "," : "") + challenge_json(e, g_status[e.id]);
        resp.body += "]";
        return true;
    }
    const Event* e = find_event(all, p[3]);
    if (!e) {
        resp.status = 404;
        resp.body = "{}";
        return true;
    }
    Status& s = g_status[e->id];

    // GET /sword/api/challenges/{id}: one event; .../children: none
    if (p.size() == 4 && req.method == "GET") {
        resp.body = challenge_json(*e, s);
        return true;
    }
    if (p.size() == 5 && p[4] == "children") {
        resp.body = "[]";
        return true;
    }
    // GET /sword/api/challenges/{id}/file/{name}: the event definition
    if (p.size() == 6 && p[4] == "file") {
        resp.content_type = "application/octet-stream";
        resp.body = e->file;
        return true;
    }
    // POST /sword/api/challenges/{id}/users: the statuses of the players listed in the body
    if (p.size() == 5 && p[4] == "users") {
        resp.body = s.accepted ? "[" + status_json(*e, s) + "]" : "[]";  // the one player there is
        return true;
    }
    // POST /sword/api/challenges/{id}/users/{epicId}/saveSlots/{slot}[/updateProgress|/updateReward]
    if (p.size() >= 8 && p[4] == "users" && p[6] == "saveSlots") {
        s.slot = p[7];
        s.update_time = now;
        if (p.size() == 8) {  // accept
            if (!s.accepted) s.accept_time = now;
            s.accepted = true;
        } else if (p[8] == "updateProgress") {
            int add = std::atoi(query_value(query, "goalProgress").c_str());
            s.attempts++;
            if (add > 0) s.successful++;
            s.progress += add;
            s.high = std::max(s.high, add);
            s.complete = s.complete || _stricmp(query_value(query, "didComplete").c_str(), "true") == 0 || s.progress >= e->goal;
        } else if (p[8] == "updateReward") {
            s.award = std::atoi(query_value(query, "rewardValue").c_str());
        }
        save_state();
        resp.body = status_json(*e, s);
        return true;
    }
    resp.status = 404;
    resp.body = "{}";
    return true;
}

}  // namespace

void install_clashmob(const macho::Image& img) {
    if (is_infinity_blade_2()) return;  // IB3 only for now
    g_dlmalloc = img.find("__Z8dlmallocm");
    ns::set_local_server(serve);
    LOG_INFO("clashmob: offline ClashMob server on (player %s)", player_id().c_str());
}

// Fills an FString with a copy of `s` made with the game's allocator (the game may free or regrow it).
void set_fstring(cpu::Thread& t, void* where, const std::string& s) {
    GuestAddr data = t.call(g_dlmalloc, {(s.size() + 1) * 4});  // UTF-32
    for (size_t i = 0; i <= s.size(); i++) gptr<u32>(data)[i] = i < s.size() ? (u8)s[i] : 0;
    auto* f = static_cast<ue::FString*>(where);
    f->data = data;
    f->num = f->max = (s32)s.size() + 1;
}

// Gives the game its MCP account id once its social manager (GEngine.MyMob) exists: SwordMyMobManager.McpId is
// set directly, in memory from the game's own allocator (the game frees or regrows it itself later).
void give_account(cpu::Thread& t);
void give_gifts(cpu::Thread& t);

void clashmob_tick(cpu::Thread& t) {
    if (is_infinity_blade_2()) return;
#ifdef __ANDROID__
    // For testing: adb shell setprop debug.ibport.clashmob menu<N> opens the ClashMob screen (each new value once).
    static std::string last = "";
    char v[PROP_VALUE_MAX] = "";
    if (__system_property_get("debug.ibport.clashmob", v) > 0 && last != v) {
        bool first = last.empty();
        last = v;
        GuestAddr pc = ue::player_controller(t);
        if (!first && pc && std::string(v).rfind("menu", 0) == 0) {
            alignas(16) u8 params[64] = {};
            LOG_INFO("clashmob: opening the ClashMob menu (%d)", ue::call_event(t, pc, "OpenClashMobMenu", params));
        }
        // debug.ibport.clashmob dump<N>: logs each ClashMob's state, and the player's prize wheels (for checking
        // that events show and rewards arrive)
        u64 manager = 0;
        GuestAddr engine = ue::engine();
        ue::TArray<u64> mobs{};
        if (!first && std::string(v).rfind("dump", 0) == 0 && engine && ue::read_property(t, engine, "ClashMobs", manager) &&
            manager && ue::read_property(t, manager, "ClashMobData", mobs)) {
            for (int i = 0; i < mobs.num; i++) {
                GuestAddr m = mobs.at(i);
                u8 mode = 0;
                u64 quest = 0, pin = 0;
                ue::read_property(t, m, "PlayMode", mode);
                ue::read_property(t, m, "Quest", quest);
                int left_off = ue::struct_member_offset(t, m, "Updated", "PlaysLeft");
                int state_off = ue::struct_member_offset(t, m, "Updated", "CurState");
                int earned_off = ue::struct_member_offset(t, m, "Updated", "RewardsEarned");
                u64 tag = 0;
                if (quest) {
                    ue::read_property(t, quest, "MapPin", pin);
                    ue::read_property(t, quest, "MapPinTag", tag);
                }
                int eid = ue::property_offset(t, m, "EventID");
                LOG_INFO("clashmob: mob %s mode %d state %d plays left %d rewards %d, quest %s pin %s (%s)",
                         eid >= 0 ? ue::read_fstring(m + eid).c_str() : "?", mode,
                         state_off >= 0 ? *gptr<u8>(m + state_off) : -1, left_off >= 0 ? *gptr<s32>(m + left_off) : -99,
                         earned_off >= 0 ? *gptr<s32>(m + earned_off) : -1, ue::object_name(t, quest).c_str(),
                         ue::name_string(t, tag).c_str(), pin ? "attached" : "not attached");
            }
        }
        // (and the player's prize wheels)
        u64 pawn = 0;
        if (!first && pc && std::string(v).rfind("dump", 0) == 0 && ue::read_property(t, pc, "Pawn", pawn) && pawn) {
            int off = ue::property_offset(t, pawn, "NumConsumable");
            if (off >= 0)
                LOG_INFO("clashmob: supplies: gem wheels S/M/L %d/%d/%d, prize wheels S/M/L %d/%d/%d, ClashMob prize wheels %d",
                         gptr<s32>(pawn + off)[25], gptr<s32>(pawn + off)[26], gptr<s32>(pawn + off)[27],
                         gptr<s32>(pawn + off)[22], gptr<s32>(pawn + off)[23], gptr<s32>(pawn + off)[24],
                         gptr<s32>(pawn + off)[28]);
        }
    }
#endif
    give_gifts(t);
    give_account(t);
}

// clashmob-gift.ini (PrizeWheels=N): ClashMob Prize Wheels to hand to the player once, in the world. (Wheels won
// before the port showed the prize wheel again were thrown away by the game.)
void give_gifts(cpu::Thread& t) {
    constexpr const char* kGiftFile = "clashmob-gift.ini";
    constexpr u8 kPrizeWheel = 28;  // eTouchRewardActor TRA_GrabBag_Uber
    static u64 last = 0;
    u64 now = GetTickCount64();
    if (now - last < 5000) return;
    last = now;
    std::ifstream f(kGiftFile);
    if (!f) return;
    int wheels = 0;
    std::string line;
    while (std::getline(f, line))
        if (line.rfind("PrizeWheels=", 0) == 0) wheels = std::atoi(line.c_str() + 12);
    f.close();
    GuestAddr pc = ue::player_controller(t);
    u64 pawn = 0;
    if (!pc || !ue::read_property(t, pc, "Pawn", pawn) || !pawn || !ue::is_a(t, pawn, "SwordPlayer")) return;
    std::remove(kGiftFile);
    if (wheels <= 0) return;
    const char* fn = "AddConsumable";
    int o_type = ue::param_offset(t, pawn, fn, "Consumable"), o_count = ue::param_offset(t, pawn, fn, "AddCount"),
        o_max = ue::param_offset(t, pawn, fn, "bIgnoreMax"), o_ret = ue::param_offset(t, pawn, fn, "ReturnValue");
    if (o_type < 0 || o_count < 0 || o_max < 0) {
        LOG_WARN("clashmob: %s not found; no gift", fn);
        return;
    }
    alignas(16) u8 params[128] = {};
    params[o_type] = kPrizeWheel;
    *reinterpret_cast<s32*>(params + o_count) = wheels;
    *reinterpret_cast<u32*>(params + o_max) = 1;
    ue::call_event(t, pawn, fn, params);
    LOG_INFO("clashmob: gave %d ClashMob Prize Wheel(s) (%s)", wheels,
             o_ret >= 0 && *reinterpret_cast<u32*>(params + o_ret) ? "added" : "refused");
}

void give_account(cpu::Thread& t) {
    // Off: the game encrypts its saves with a key tied to the account, so with an account id it cannot
    // read the player's save and starts a new game. ClashMobs run with no account instead.
    constexpr bool kGiveAccount = false;
    if (!kGiveAccount || is_infinity_blade_2() || !g_dlmalloc) return;
    static bool done = false;
    if (done) return;
    GuestAddr engine = ue::engine();
    u64 mymob = 0;
    if (!engine || !ue::read_property(t, engine, "MyMob", mymob) || !mymob) return;
    done = true;
    int off = ue::property_offset(t, mymob, "McpId");
    if (off < 0) {
        LOG_WARN("clashmob: no McpId on %s", ue::class_name(t, mymob).c_str());
        return;
    }
    auto* s = gptr<ue::FString>(mymob + off);
    if (s->num > 1) {
        LOG_INFO("clashmob: the game already has MCP id %s", ue::read_fstring(mymob + off).c_str());
        return;
    }
    const std::string& id = player_id();
    set_fstring(t, s, id);  // an empty string's buffer (if any) is left alone
    LOG_INFO("clashmob: gave the game MCP id %s", ue::read_fstring(mymob + off).c_str());

    // Requests for the player carry an auth ticket for that id (McpUserAuthRequestWrapper); the server that
    // would hand one out is the port, so the ticket is simply given to the game's user manager.
    u64 users = 0;
    if (!ue::read_property(t, mymob, "McpUserManager", users) || !users) {
        LOG_WARN("clashmob: no MCP user manager; ClashMobs will not see the player");
        return;
    }
    const char* fn = "InjectUserCredentials";
    int o_id = ue::param_offset(t, users, fn, "McpId"), o_secret = ue::param_offset(t, users, fn, "ClientSecret"),
        o_token = ue::param_offset(t, users, fn, "Token");
    if (o_id < 0 || o_secret < 0 || o_token < 0) {
        LOG_WARN("clashmob: %s not found on %s", fn, ue::class_name(t, users).c_str());
        return;
    }
    alignas(16) u8 params[512] = {};
    set_fstring(t, params + o_id, id);
    set_fstring(t, params + o_secret, "port-secret");
    set_fstring(t, params + o_token, "port-ticket-" + id);
    ue::call_event(t, users, fn, params);
    LOG_INFO("clashmob: gave the game an auth ticket");
}

bool clashmob_wants_script_hook() { return !is_infinity_blade_2(); }

// Script functions answered here:
// - SwordMyMobManager.UserHasMcpId(): the ClashMob code only shows an event to players with an online account. The
//   player has none (one would make the game look for another set of saves), so for the ClashMob code only, the
//   answer is yes. Everything else (saves, cloud) still sees no account.
// - SwordQuestData.GetShowQuestType(): the map shows the quest types the story has introduced so far, and ClashMobs
//   come late in it. A ClashMob quest counts as a side quest, so ClashMobs show from the start.
// - SwordBattleEvent.RewardGoalString(RewardIdx): the text of a reward tier. For damage events (BT_Kill1Boss) the
//   game's text has no number ("KILL TITAN" for every tier), so the port writes "DO 5,000 DAMAGE".
// - SwordInventoryItem.ShouldBeHidden() for the ClashMob Prize Wheel (TRA_GrabBag_Uber): it was hidden when
//   ClashMobs ended (HiddenLevel=-1), and the game throws hidden rewards away. It is shown again.
enum class Target { None, UserHasMcpId, GetShowQuestType, RewardGoalString, ShouldBeHidden };

std::string with_commas(long long v) {
    std::string s = std::to_string(v);
    for (int i = (int)s.size() - 3; i > 0; i -= 3) s.insert(i, ",");
    return s;
}

bool clashmob_script_call(cpu::Thread& t, GuestAddr frame, GuestAddr result) {
    constexpr u64 kFrameNode = 0x18, kFrameObject = 0x20, kFramePrevious = 0x38, kObjOuter = 0x40;
    static std::mutex mutex;
    static std::unordered_map<GuestAddr, Target> targets;
    static std::unordered_map<GuestAddr, bool> callers;
    GuestAddr fn = *gptr<u64>(frame + kFrameNode);
    std::lock_guard lock(mutex);
    auto it = targets.find(fn);
    if (it == targets.end()) {
        std::string name = ue::object_name(t, fn), cls = ue::object_name(t, *gptr<u64>(fn + kObjOuter));
        Target k = name == "UserHasMcpId" && cls == "SwordMyMobManager"      ? Target::UserHasMcpId
                   : name == "GetShowQuestType" && cls == "SwordQuestData" ? Target::GetShowQuestType
                   : name == "RewardGoalString" && cls == "SwordBattleEvent" ? Target::RewardGoalString
                   : name == "ShouldBeHidden"                                  ? Target::ShouldBeHidden
                                                                            : Target::None;
        it = targets.emplace(fn, k).first;
    }
    if (it->second == Target::GetShowQuestType) {
        GuestAddr quest = *gptr<u64>(frame + kFrameObject);
        u64 clashmob = 0;
        if (!ue::read_property(t, quest, "ClashMob", clashmob) || !clashmob) return false;
        if (result) *gptr<u8>(result) = 1;  // SQT_QuestSecondary
        return true;
    }
    if (it->second == Target::ShouldBeHidden) {
        GuestAddr item = *gptr<u64>(frame + kFrameObject);
        if (ue::object_name(t, item) != "TRA_GrabBag_Uber") return false;
        if (result) *gptr<u32>(result) = 0;
        return true;
    }
    if (it->second == Target::RewardGoalString) {
        constexpr u64 kFrameLocals = 0x30;
        GuestAddr event = *gptr<u64>(frame + kFrameObject);
        u8 type = 0;
        ue::TArray<float> goals{};
        int idx_off = ue::param_offset(t, event, "RewardGoalString", "RewardIdx");
        if (!result || idx_off < 0 || !ue::read_property(t, event, "BattleType", type) || type != 1 ||
            !ue::read_property(t, event, "RewardGoal", goals))
            return false;
        s32 idx = *gptr<s32>(*gptr<u64>(frame + kFrameLocals) + idx_off);
        if (idx < 0 || idx >= goals.num) return false;
        set_fstring(t, gptr<void>(result), "DO " + with_commas((long long)goals.at(idx)) + " DAMAGE");
        return true;
    }
    if (it->second != Target::UserHasMcpId) return false;
    GuestAddr prev = *gptr<u64>(frame + kFramePrevious);
    GuestAddr caller = prev ? *gptr<u64>(prev + kFrameNode) : 0;
    auto c = callers.find(caller);
    if (c == callers.end()) {
        std::string cls = caller ? ue::object_name(t, *gptr<u64>(caller + kObjOuter)) : "";
        c = callers.emplace(caller, cls.find("ClashMob") != std::string::npos).first;
        LOG_INFO("clashmob: UserHasMcpId from %s.%s -> %s", cls.c_str(), caller ? ue::object_name(t, caller).c_str() : "?",
                 c->second ? "yes" : "the game's answer");
    }
    if (!c->second) return false;
    if (result) *gptr<u32>(result) = 1;
    return true;
}

}  // namespace game
