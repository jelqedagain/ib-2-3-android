// IB3: saves made with Community Patch 3.0 open under any other .ipa (3.0.1, 2.5, the original game).
// The game encrypts each save with a key of its own (appEncryptDataWithKey). Community Patch 3.0 rerouted that to
// the game's single fixed key (appEncryptData) and taught appDecryptDataWithKey to open both kinds: it decrypts the
// first 16 bytes with the fixed key and keeps it if they look like the start of a save, else uses the save's key.
// 3.0.1 went back to the game's own code, which only knows the save's key, so 3.0 saves no longer opened (on
// iPhones too). With any .ipa whose appDecryptDataWithKey is the game's own, the same test runs the other way
// round: the save's key first, as the game does, then the fixed key. The game writes the save back in its own
// format the next time it saves.
#include "game/game.h"
#include "hle.h"
#include "hook.h"
#include "macho.h"
#include <cstring>
#include <mutex>

namespace game {

namespace {

GuestAddr g_decrypt_with_key = 0;  // trampoline: the game's appDecryptDataWithKey(u8* data, u32 size, char* key)
GuestAddr g_decrypt_fixed = 0;     // appDecryptData(u8* data, u32 size)
u8* g_block = nullptr;             // a 16-byte guest buffer for trial decryptions
std::mutex g_block_lock;

bool printable(u8 c) { return c >= 0x20 && c < 0x7f; }

// Community Patch 3.0's test (its code at 0x100007408): whether a decrypted first block starts a save. Its first
// word is a version (2, 0x911), a compressed-data tag, a small count followed by zeros, or the length of a short
// string whose first characters follow.
bool looks_like_save(const u8* b) {
    u32 w[4];
    memcpy(w, b, 16);
    if (w[0] == 2 || w[0] == 0x911 || w[0] == 0x5a697064) return true;
    if (w[0] <= 0x3f && (w[1] | w[2] | w[3]) == 0) return true;
    if (w[0] < 4 || w[0] > 0x40) return false;
    u32 chars = std::min<u32>(w[0] - 1, 12);  // the string's characters (the length counts its terminator)
    for (u32 i = 0; i < chars; i++)
        if (!printable(b[4 + i])) return false;
    return true;
}

// Decrypts a copy of the first block with `key` (or the fixed key when null) and tests it.
bool first_block_ok(cpu::Thread& t, GuestAddr data, GuestAddr key) {
    std::lock_guard<std::mutex> lock(g_block_lock);
    memcpy(g_block, gptr<u8>(data), 16);
    if (key) t.call(g_decrypt_with_key, {gaddr(g_block), 16, key});
    else t.call(g_decrypt_fixed, {gaddr(g_block), 16});
    return looks_like_save(g_block);
}

void on_decrypt_with_key(cpu::Thread& t) {
    GuestAddr data = t.x(0), key = t.x(2);
    u32 size = (u32)t.x(1);
    if (data && size >= 16 && !first_block_ok(t, data, key) && first_block_ok(t, data, 0)) {
        static bool logged = false;
        if (!logged) LOG_INFO("saves: opening a save made with Community Patch 3.0 (its fixed key); the game saves it in its own format from now on");
        logged = true;
        t.call(g_decrypt_fixed, {data, size});
        return;
    }
    t.call(g_decrypt_with_key, {t.x(0), t.x(1), t.x(2)});
}

}  // namespace

void install_cp30_save_reader(const macho::Image& img) {
    if (is_ib2()) return;
    GuestAddr with_key = img.find("__Z21appDecryptDataWithKeyPhjPc"), fixed = img.find("__Z14appDecryptDataPhj");
    if (!with_key || !fixed) return;
    u32 first = gptr<u32>(with_key)[0];
    if ((first & 0xFC000000) == 0x14000000) return;  // a branch: Community Patch 3.0 itself, which reads both kinds
    g_block = static_cast<u8*>(hle::alloc_static(16, 16));
    g_decrypt_fixed = fixed;
    g_decrypt_with_key = hook::install(with_key, "appDecryptDataWithKey", on_decrypt_with_key);
}

}  // namespace game
