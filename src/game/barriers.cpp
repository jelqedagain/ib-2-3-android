// Infinity Blade II's engine separates threads with "dsb st", which orders stores only. The render
// thread reads commands from a ring buffer the game thread fills: it loads the write position, then
// the command. Without ordering between those loads, a phone CPU may read the command before it sees
// the new position and run a half-written command (a Galaxy S25 FE, Exynos 2400, crashed in
// RenderingThreadMain on a garbage command vtable). Apple's CPUs never showed it. Infinity Blade III's newer engine uses "dsb sy"
// at the same places; give Infinity Blade II the same full barriers.
#include "game/game.h"
#include "cpu.h"
#include "macho.h"

namespace game {

void strengthen_memory_barriers(const macho::Image& img) {
    constexpr u32 kDsbSt = 0xd5033e9f, kDsbSy = 0xd5033f9f;
    const macho::Section* text = img.section("__TEXT", "__text");
    if (!text) return;
    u32* code = gptr<u32>(text->addr);
    size_t n = text->size / 4, patched = 0;
    for (size_t i = 0; i < n; i++) {
        if (code[i] != kDsbSt) continue;
        code[i] = kDsbSy;
#if IB3_NATIVE_CPU  // the CPU runs this code directly: make it see the new instruction
        __builtin___clear_cache(reinterpret_cast<char*>(code + i), reinterpret_cast<char*>(code + i + 1));
#endif
        patched++;
    }
    if (patched) LOG_INFO("made %zu store-only memory barriers full barriers", patched);
}

}  // namespace game
