// The game's language. Both games pick theirs from iOS's preferred language (AppleLanguages[0], and
// for Spanish IB3 also the locale), mapped to the suffix of their text files (Coalesced_FRA.bin...).
// The launcher chooses the suffix (settings.ini [Game] Language, or the phone's language); this
// reports the iOS codes each game's own table turns back into that suffix.
#include "game/game.h"
#include "libc/vfs.h"
#include "settings.h"
#include <cstring>
#include <filesystem>

namespace game {

namespace {

struct Entry {
    const char* suffix;
    const char* language;  // AppleLanguages[0]
    const char* locale;    // NSLocale identifier
};

// IB3 matches "pt-PT" and, for Spanish, the locale ("es_MX" -> ESM); IB2 matches "pt_PT" and has no
// Latin American Spanish.
constexpr Entry kEntries[] = {
    {"INT", "en", "en_US"}, {"FRA", "fr", "fr_FR"},      {"DEU", "de", "de_DE"},      {"ITA", "it", "it_IT"},
    {"ESN", "es", "es_ES"}, {"ESM", "es", "es_MX"},      {"BRA", "pt", "pt_BR"},      {"POR", "pt-PT", "pt_PT"},
    {"DUT", "nl", "nl_NL"}, {"SWE", "sv", "sv_SE"},      {"POL", "pl", "pl_PL"},      {"CZE", "cs", "cs_CZ"},
    {"HUN", "hu", "hu_HU"}, {"SLO", "sk", "sk_SK"},      {"RUS", "ru", "ru_RU"},      {"JPN", "ja", "ja_JP"},
    {"KOR", "ko", "ko_KR"}, {"CHN", "zh-Hans", "zh_CN"}, {"THA", "th", "th_TH"},      {"IND", "id", "id_ID"},
};

const Entry& chosen() {
    static const Entry* entry = [] {
        const std::string& want = settings::get().language;
        const Entry* e = &kEntries[0];
        for (const Entry& x : kEntries)
            if (want == x.suffix) e = &x;
        std::error_code ec;
        if (e != &kEntries[0] &&
            !std::filesystem::exists(vfs::host_bundle() + "/CookedIPhone/Coalesced_" + e->suffix + ".bin", ec)) {
            LOG_WARN("language: this game has no %s text; using English", e->suffix);
            e = &kEntries[0];
        }
        if (e != &kEntries[0]) LOG_INFO("language: %s (%s)", e->suffix, e->locale);
        return e;
    }();
    return *entry;
}

}  // namespace

std::string ios_language() {
    const Entry& e = chosen();
    if (is_infinity_blade_2() && std::strcmp(e.suffix, "POR") == 0) return "pt_PT";
    return e.language;
}

std::string ios_locale() { return chosen().locale; }

}  // namespace game
