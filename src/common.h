// Shared basics for the IB3 runtime.
//
// Memory model: guest (ARM64 iOS) addresses are identical to host addresses. The
// Mach-O image is mapped at its preferred address and every guest-visible
// allocation is ordinary host memory, so HLE code can use guest pointers directly.
#pragma once
#include <cstdint>
#include <cstddef>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <string_view>

using u8 = std::uint8_t;
using u16 = std::uint16_t;
using u32 = std::uint32_t;
using u64 = std::uint64_t;
using s8 = std::int8_t;
using s16 = std::int16_t;
using s32 = std::int32_t;
using s64 = std::int64_t;
using GuestAddr = u64;

template <typename T>
inline T* gptr(GuestAddr a) { return reinterpret_cast<T*>(static_cast<uintptr_t>(a)); }
template <typename T>
inline GuestAddr gaddr(T* p) { return static_cast<GuestAddr>(reinterpret_cast<uintptr_t>(p)); }

namespace logging {
enum class Level { Trace, Debug, Info, Warn, Error };
void write(Level lvl, const char* fmt, ...) __attribute__((format(printf, 2, 3)));
extern Level min_level;
bool enabled(Level lvl);
void set_thread_name(const char* name);
void set_thread_trace(bool on);  // per-thread trace override
// When set (normal play, not tests), fatal errors and crashes are also shown in a message box.
extern bool g_error_dialogs;
extern std::string g_app_name;  // for those messages ("Infinity Blade III"; the app's label on Android)
void show_error_dialog(const char* what);
}  // namespace logging

#define LOG_TRACE(...) do { if (logging::enabled(logging::Level::Trace)) logging::write(logging::Level::Trace, __VA_ARGS__); } while (0)
#define LOG_DEBUG(...) do { if (logging::enabled(logging::Level::Debug)) logging::write(logging::Level::Debug, __VA_ARGS__); } while (0)
#define LOG_INFO(...) logging::write(logging::Level::Info, __VA_ARGS__)
#define LOG_WARN(...) logging::write(logging::Level::Warn, __VA_ARGS__)
#define LOG_ERROR(...) logging::write(logging::Level::Error, __VA_ARGS__)

[[noreturn]] void fatal(const char* fmt, ...) __attribute__((format(printf, 1, 2)));
