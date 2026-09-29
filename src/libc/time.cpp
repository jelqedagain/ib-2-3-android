// <time.h>, <sys/time.h>, mach time, sleeping.
#include "hle.h"
#include "modules.h"
#include <atomic>
#include <ctime>
#include <thread>
#include <windows.h>

namespace libc {

namespace {

// Darwin arm64 struct tm (56 bytes).
struct DarwinTm {
    s32 tm_sec, tm_min, tm_hour, tm_mday, tm_mon, tm_year, tm_wday, tm_yday, tm_isdst;
    s32 pad;
    s64 tm_gmtoff;
    const char* tm_zone;
};
static_assert(sizeof(DarwinTm) == 56);

void to_darwin(DarwinTm* out, const std::tm& t, s64 gmtoff, const char* zone) {
    out->tm_sec = t.tm_sec;
    out->tm_min = t.tm_min;
    out->tm_hour = t.tm_hour;
    out->tm_mday = t.tm_mday;
    out->tm_mon = t.tm_mon;
    out->tm_year = t.tm_year;
    out->tm_wday = t.tm_wday;
    out->tm_yday = t.tm_yday;
    out->tm_isdst = t.tm_isdst;
    out->pad = 0;
    out->tm_gmtoff = gmtoff;
    out->tm_zone = zone;
}

LARGE_INTEGER g_freq;
u64 ticks24() {
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    return (u64)((unsigned __int128)now.QuadPart * 24000000 / g_freq.QuadPart);
}

// Precise sleep: coarse Sleep() then spin the remainder.
void precise_sleep_us(u64 us) {
    u64 end = ticks24() + us * 24;
    if (us > 2000) Sleep((DWORD)(us / 1000 - 1));
    while (ticks24() < end) std::this_thread::yield();
}

}  // namespace

u64 mach_ticks() { return ticks24(); }

void install_time() {
    using hle::fn;
    QueryPerformanceFrequency(&g_freq);
    timeBeginPeriod(1);

    fn("_time", [](s64* out) -> s64 {
        s64 t = std::time(nullptr);
        if (out) *out = t;
        return t;
    });
    fn("_gettimeofday", [](void* tv, void*) {
        if (tv) {
            FILETIME ft;
            GetSystemTimePreciseAsFileTime(&ft);
            u64 t = ((u64)ft.dwHighDateTime << 32 | ft.dwLowDateTime) / 10 - 11644473600000000ull;  // us since 1970
            *static_cast<s64*>(tv) = (s64)(t / 1000000);
            *reinterpret_cast<s32*>(static_cast<u8*>(tv) + 8) = (s32)(t % 1000000);
        }
        return 0;
    });
    fn("_localtime_r", [](const s64* t, DarwinTm* out) {
        std::tm tm;
        __time64_t tt = *t;
        _localtime64_s(&tm, &tt);
        long tz;
        _get_timezone(&tz);
        to_darwin(out, tm, -(s64)tz + (tm.tm_isdst > 0 ? 3600 : 0), hle::static_cstr("LOCAL"));
        return out;
    });
    fn("_gmtime_r", [](const s64* t, DarwinTm* out) {
        std::tm tm;
        __time64_t tt = *t;
        _gmtime64_s(&tm, &tt);
        to_darwin(out, tm, 0, hle::static_cstr("UTC"));
        return out;
    });
    fn("_gmtime", [](const s64* t) {
        static thread_local DarwinTm* buf = static_cast<DarwinTm*>(std::calloc(1, sizeof(DarwinTm)));
        std::tm tm;
        __time64_t tt = *t;
        _gmtime64_s(&tm, &tt);
        to_darwin(buf, tm, 0, hle::static_cstr("UTC"));
        return buf;
    });
    fn("_mktime", [](DarwinTm* in) -> s64 {
        std::tm tm{};
        tm.tm_sec = in->tm_sec;
        tm.tm_min = in->tm_min;
        tm.tm_hour = in->tm_hour;
        tm.tm_mday = in->tm_mday;
        tm.tm_mon = in->tm_mon;
        tm.tm_year = in->tm_year;
        tm.tm_isdst = in->tm_isdst;
        return _mktime64(&tm);
    });
    fn("_difftime", [](s64 a, s64 b) { return (double)(a - b); });

    // Called millions of times per second; read the (24 MHz) counter without leaving the JIT.
    hle::native("_mach_absolute_time", {
        0xd53be020,  // mrs x0, cntpct_el0
        0xd65f03c0,  // ret
    });
    fn("_mach_timebase_info", [](u32* info) {
        info[0] = 125;  // 24 MHz ticks -> ns
        info[1] = 3;
        return 0;
    });
    fn("_mach_wait_until", [](cpu::Thread& t, u64 deadline) {
        u64 now = ticks24();
        static std::atomic<u64> calls{0};
        if (logging::enabled(logging::Level::Debug) && calls++ % 300 == 0)
            LOG_DEBUG("mach_wait_until: %.3f ms\n%s", deadline > now ? (deadline - now) / 24000.0 : 0.0, t.backtrace().c_str());
        if (deadline > now) precise_sleep_us((deadline - now) / 24);
        return 0;
    });
    fn("_usleep", [](u32 us) {
        precise_sleep_us(us);
        return 0;
    });
    fn("_sched_yield", []() {
        static std::atomic<u64> calls{0};
        if (logging::enabled(logging::Level::Debug) && calls++ % 200000 == 0)
            LOG_DEBUG("sched_yield from %s", cpu::symbolize(cpu::current().x(30)).c_str());
        SwitchToThread();
        return 0;
    });
    fn("_pause", []() {
        Sleep(INFINITE);
        return 0;
    });
    fn("_CFAbsoluteTimeGetCurrent", []() {
        FILETIME ft;
        GetSystemTimePreciseAsFileTime(&ft);
        u64 t = ((u64)ft.dwHighDateTime << 32 | ft.dwLowDateTime);  // 100ns since 1601
        return (double)(t - 126227808000000000ull) / 1e7;           // seconds since 2001-01-01
    });
}

}  // namespace libc
