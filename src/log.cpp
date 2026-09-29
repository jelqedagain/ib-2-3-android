#include "common.h"
#include <atomic>
#include <cstdarg>
#include <string>
#include <mutex>
#include <windows.h>

namespace logging {
Level min_level = Level::Info;
static std::mutex g_mutex;
static FILE* g_file = nullptr;

static thread_local bool t_trace = false;
void set_thread_trace(bool on) { t_trace = on; }
bool enabled(Level lvl) { return lvl >= min_level || (t_trace && lvl == Level::Trace); }

static thread_local char t_name[24] = "";
void set_thread_name(const char* name) { snprintf(t_name, sizeof t_name, "%s", name); }

void write(Level lvl, const char* fmt, ...) {
    if (!enabled(lvl)) return;
    static const char* names[] = {"TRACE", "DEBUG", "INFO ", "WARN ", "ERROR"};
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    std::lock_guard lock(g_mutex);
    if (!g_file) g_file = std::fopen("ib3rt.log", "w");
    unsigned tid = GetCurrentThreadId();
    static const u64 start = GetTickCount64();
    double secs = (GetTickCount64() - start) / 1000.0;
    std::fprintf(stderr, "[%s %7.2f %5u %-12.12s] %s\n", names[(int)lvl], secs, tid, t_name, buf);
    if (g_file) {
        std::fprintf(g_file, "[%s %7.2f %5u %-12.12s] %s\n", names[(int)lvl], secs, tid, t_name, buf);
        std::fflush(g_file);
    }
}
bool g_error_dialogs = false;

void show_error_dialog(const char* what) {
    if (!g_error_dialogs) return;
    static std::atomic<bool> shown{false};
    if (shown.exchange(true)) return;  // one dialog, even if several threads fail
    std::string text = std::string("Infinity Blade III stopped because of an error:\n\n") + what +
                       "\n\nDetails were saved to ib3rt.log next to the game. If you report the problem, please "
                       "include that file.";
    MessageBoxA(nullptr, text.c_str(), "Vibefinity Blade 3", MB_ICONERROR | MB_TOPMOST);
}
}  // namespace logging

void fatal(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    logging::write(logging::Level::Error, "FATAL: %s", buf);
    std::fflush(stderr);
    logging::show_error_dialog(buf);
    ExitProcess(1);
}
