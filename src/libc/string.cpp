// <string.h>, <wchar.h> (32-bit wchar_t), <ctype.h>, <math.h>, <stdlib.h> basics.
#include "hle.h"
#include "libc/format.h"
#include "modules.h"
#include <cmath>
#include <cwctype>
#include <mutex>
#include <vector>
#include <windows.h>

namespace libc {

namespace {

using w32 = char32_t;

int w32cmp(const w32* a, const w32* b) {
    while (*a && *a == *b) a++, b++;
    return (*a > *b) - (*a < *b);
}
int w32ncmp(const w32* a, const w32* b, u64 n) {
    for (; n; n--, a++, b++) {
        if (*a != *b) return (*a > *b) - (*a < *b);
        if (!*a) return 0;
    }
    return 0;
}

// Darwin's _RuneLocale: runetype[] at offset 60, maplower[] at 1084, mapupper[] at 2108.
constexpr size_t kRunetypeOff = 60, kMapLowerOff = 1084, kMapUpperOff = 2108, kRuneLocaleSize = 3200;
enum : u32 {
    CT_A = 0x100, CT_C = 0x200, CT_D = 0x400, CT_G = 0x800, CT_L = 0x1000, CT_P = 0x2000,
    CT_S = 0x4000, CT_U = 0x8000, CT_X = 0x10000, CT_B = 0x20000, CT_R = 0x40000,
};
u32 ctype_mask(u32 c) {
    if (c > 0x7f) {
        u32 m = 0;
        if (iswalpha((wint_t)c)) m |= CT_A | CT_G | CT_R;
        if (iswupper((wint_t)c)) m |= CT_U;
        if (iswlower((wint_t)c)) m |= CT_L;
        if (iswspace((wint_t)c)) m |= CT_S;
        return m;
    }
    u32 m = 0;
    if (c < 0x20 || c == 0x7f) m |= CT_C;
    if (c == ' ' || c == '\t' || c == '\n' || c == '\v' || c == '\f' || c == '\r') m |= CT_S;
    if (c == ' ' || c == '\t') m |= CT_B;
    if (c >= '0' && c <= '9') m |= CT_D | CT_X | (c - '0');
    if ((c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F')) m |= CT_X;
    if (c >= 'a' && c <= 'z') m |= CT_L | CT_A;
    if (c >= 'A' && c <= 'Z') m |= CT_U | CT_A;
    if (c > 0x20 && c < 0x7f) m |= CT_G | CT_R;
    if (c == ' ') m |= CT_R;
    if (c > 0x20 && c < 0x7f && !(m & (CT_A | CT_D))) m |= CT_P;
    return m;
}

GuestAddr make_rune_locale() {
    u8* rl = static_cast<u8*>(hle::alloc_static(kRuneLocaleSize, 16));
    std::memcpy(rl, "RuneMagA", 8);
    std::memcpy(rl + 8, "NONE", 4);
    auto* runetype = reinterpret_cast<u32*>(rl + kRunetypeOff);
    auto* lower = reinterpret_cast<s32*>(rl + kMapLowerOff);
    auto* upper = reinterpret_cast<s32*>(rl + kMapUpperOff);
    for (u32 c = 0; c < 256; c++) {
        runetype[c] = ctype_mask(c);
        lower[c] = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        upper[c] = (c >= 'a' && c <= 'z') ? c - 32 : c;
    }
    return gaddr(rl);
}

// In-place sort of guest elements with a guest comparator (heap-free merge sort via index table).
void guest_qsort(GuestAddr base, u64 n, u64 size, GuestAddr cmp) {
    if (n < 2) return;
    cpu::Thread& t = cpu::current();
    std::vector<u64> idx(n);
    for (u64 i = 0; i < n; i++) idx[i] = i;
    std::vector<u8> copy(n * size);
    std::memcpy(copy.data(), gptr<void>(base), n * size);
    // Comparator must see stable addresses: compare within the original array (unchanged until the end).
    std::stable_sort(idx.begin(), idx.end(), [&](u64 a, u64 b) {
        return (s32)t.call(cmp, {base + a * size, base + b * size}) < 0;
    });
    for (u64 i = 0; i < n; i++) std::memcpy(gptr<u8>(base) + i * size, copy.data() + idx[i] * size, size);
}

std::mutex g_rand_mutex;
u32 g_rand_state = 1;
u32 next_rand() {
    std::lock_guard lock(g_rand_mutex);
    g_rand_state = g_rand_state * 1103515245 + 12345;
    return (g_rand_state >> 1) & 0x7fffffff;
}

}  // namespace

void install_string() {
    using hle::fn;
    // memory
    fn("_memcpy", [](void* d, const void* s, u64 n) { return std::memcpy(d, s, n); });
    fn("_memmove", [](void* d, const void* s, u64 n) { return std::memmove(d, s, n); });
    fn("_memset", [](void* d, int c, u64 n) { return std::memset(d, c, n); });
    fn("_memcmp", [](const void* a, const void* b, u64 n) { return std::memcmp(a, b, n); });
    fn("_memchr", [](const void* s, int c, u64 n) { return (void*)std::memchr(s, c, n); });
    fn("_bzero", [](void* d, u64 n) { std::memset(d, 0, n); });
    fn("_memset_pattern16", [](void* d, const void* pat, u64 n) {
        u8* p = static_cast<u8*>(d);
        for (u64 i = 0; i < n; i += 16) std::memcpy(p + i, pat, n - i < 16 ? n - i : 16);
    });
    fn("_malloc", [](u64 n) { return std::malloc(n ? n : 1); });
    fn("_calloc", [](u64 n, u64 s) { return std::calloc(n ? n : 1, s ? s : 1); });
    fn("_free", [](void* p) { std::free(p); });
    hle::fn("_realloc", [](void* p, u64 n) { return std::realloc(p, n ? n : 1); });

    // narrow strings
    fn("_strlen", [](const char* s) -> u64 { return std::strlen(s); });
    fn("_strcmp", [](const char* a, const char* b) { return std::strcmp(a, b); });
    fn("_strncmp", [](const char* a, const char* b, u64 n) { return std::strncmp(a, b, n); });
    fn("_strcasecmp", [](const char* a, const char* b) { return _stricmp(a, b); });
    fn("_strncasecmp", [](const char* a, const char* b, u64 n) { return _strnicmp(a, b, n); });
    fn("_strcpy", [](char* d, const char* s) { return std::strcpy(d, s); });
    fn("_strncpy", [](char* d, const char* s, u64 n) { return std::strncpy(d, s, n); });
    fn("_strcat", [](char* d, const char* s) { return std::strcat(d, s); });
    fn("___strcat_chk", [](char* d, const char* s, u64) { return std::strcat(d, s); });
    fn("_strchr", [](const char* s, int c) { return (char*)std::strchr(s, c); });
    fn("_strrchr", [](const char* s, int c) { return (char*)std::strrchr(s, c); });
    fn("_strstr", [](const char* a, const char* b) { return (char*)std::strstr(a, b); });
    fn("_strdup", [](const char* s) {
        size_t n = std::strlen(s) + 1;
        char* d = static_cast<char*>(std::malloc(n));
        std::memcpy(d, s, n);
        return d;
    });
    fn("_strerror", [](int e) { return (char*)hle::static_cstr("error " + std::to_string(e)); });
    fn("_atoi", [](const char* s) { return std::atoi(s); });
    fn("_strtol", [](const char* s, char** end, int base) -> s64 { return std::strtoll(s, end, base); });
    fn("_strtoull", [](const char* s, char** end, int base) -> u64 { return std::strtoull(s, end, base); });
    fn("_strtod", [](const char* s, char** end) { return std::strtod(s, end); });

    // 32-bit wide strings
    fn("_wcslen", [](const w32* s) -> u64 { return w32len(s); });
    fn("_wcscmp", [](const w32* a, const w32* b) { return w32cmp(a, b); });
    fn("_wcsncmp", [](const w32* a, const w32* b, u64 n) { return w32ncmp(a, b, n); });
    fn("_wcscpy", [](w32* d, const w32* s) {
        w32* r = d;
        while ((*d++ = *s++)) {}
        return r;
    });
    fn("_wcsncpy", [](w32* d, const w32* s, u64 n) {
        u64 i = 0;
        for (; i < n && s[i]; i++) d[i] = s[i];
        for (; i < n; i++) d[i] = 0;
        return d;
    });
    fn("_wcscat", [](w32* d, const w32* s) {
        w32* r = d;
        d += w32len(d);
        while ((*d++ = *s++)) {}
        return r;
    });
    fn("_wcschr", [](const w32* s, u32 c) -> w32* {
        for (;; s++) {
            if (*s == c) return const_cast<w32*>(s);
            if (!*s) return nullptr;
        }
    });
    fn("_wcsrchr", [](const w32* s, u32 c) -> w32* {
        const w32* r = nullptr;
        for (;; s++) {
            if (*s == c) r = s;
            if (!*s) return const_cast<w32*>(r);
        }
    });
    fn("_wcsstr", [](const w32* h, const w32* n) -> w32* {
        size_t nl = w32len(n);
        if (!nl) return const_cast<w32*>(h);
        for (; *h; h++)
            if (w32ncmp(h, n, nl) == 0) return const_cast<w32*>(h);
        return nullptr;
    });
    fn("_wcstod", [](const w32* s, w32** end) {
        std::string u = w32_to_utf8(s);
        char* e;
        double v = std::strtod(u.c_str(), &e);
        if (end) *end = const_cast<w32*>(s) + (e - u.c_str());  // ASCII number prefix: 1 char == 1 code point
        return v;
    });
    fn("_wcstoul", [](const w32* s, w32** end, int base) -> u64 {
        std::string u = w32_to_utf8(s);
        char* e;
        u64 v = std::strtoull(u.c_str(), &e, base);
        if (end) *end = const_cast<w32*>(s) + (e - u.c_str());
        return v;
    });
    fn("_wcstoull", [](const w32* s, w32** end, int base) -> u64 {
        std::string u = w32_to_utf8(s);
        char* e;
        u64 v = std::strtoull(u.c_str(), &e, base);
        if (end) *end = const_cast<w32*>(s) + (e - u.c_str());
        return v;
    });

    // ctype
    hle::data_lazy("__DefaultRuneLocale", make_rune_locale);
    fn("___maskrune", [](s32 c, u64 mask) -> s32 { return (s32)(ctype_mask((u32)c) & mask); });
    // Hot (UE3 name hashing): ASCII in native code, everything else in the host.
    GuestAddr toupper_slow = cpu::make_stub("___toupper", [](cpu::Thread& t) {
        s32 c = (s32)t.x(0);
        t.set_x(0, (u64)(s64)(c > 0x7f ? (s32)towupper((wint_t)c) : c));
    });
    hle::native("___toupper", {
        0x7101fc1f,  // cmp  w0, #0x7f
        0x540000c8,  // b.hi slow
        0x51018401,  // sub  w1, w0, #'a'
        0x7100643f,  // cmp  w1, #25
        0x54000048,  // b.hi done
        0x51008000,  // sub  w0, w0, #32
        0xd65f03c0,  // done: ret
        0x58000050,  // slow: ldr x16, #8
        0xd61f0200,  // br   x16
        (u32)toupper_slow, (u32)(toupper_slow >> 32),
    });

    // math
    fn("_sinf", [](float x) { return std::sin(x); });
    fn("_cosf", [](float x) { return std::cos(x); });
    fn("_tanf", [](float x) { return std::tan(x); });
    fn("_asinf", [](float x) { return std::asin(x); });
    fn("_acosf", [](float x) { return std::acos(x); });
    fn("_atanf", [](float x) { return std::atan(x); });
    fn("_atan2f", [](float y, float x) { return std::atan2(y, x); });
    fn("_expf", [](float x) { return std::exp(x); });
    fn("_exp2f", [](float x) { return std::exp2(x); });
    fn("_logf", [](float x) { return std::log(x); });
    fn("_log10f", [](float x) { return std::log10(x); });
    fn("_powf", [](float x, float y) { return std::pow(x, y); });
    fn("_fmodf", [](float x, float y) { return std::fmod(x, y); });
    fn("_fmod", [](double x, double y) { return std::fmod(x, y); });
    fn("_ldexp", [](double x, int e) { return std::ldexp(x, e); });
    fn("_ldexpf", [](float x, int e) { return std::ldexp(x, e); });
    fn("_frexp", [](double x, int* e) { return std::frexp(x, e); });
    hle::raw("___sincosf_stret", [](cpu::Thread& t) {
        float x = t.s(0);
        t.set_s(0, std::sin(x));
        t.set_s(1, std::cos(x));
    });

    // stdlib
    fn("_qsort", [](GuestAddr base, u64 n, u64 size, GuestAddr cmp) { guest_qsort(base, n, size, cmp); });
    fn("_rand", []() -> s32 { return (s32)next_rand(); });
    fn("_srand", [](u32 seed) {
        std::lock_guard lock(g_rand_mutex);
        g_rand_state = seed;
    });
    fn("_srandomdev", []() {
        std::lock_guard lock(g_rand_mutex);
        g_rand_state = (u32)GetTickCount64();
    });
    fn("_arc4random", []() -> u32 { return (next_rand() << 1) ^ next_rand(); });
}

}  // namespace libc
