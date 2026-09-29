// printf-family formatting for guest calls (Darwin arm64 varargs, 32-bit wchar_t).
#pragma once
#include "common.h"
#include <string>

namespace libc {

// Darwin arm64 va_list is a plain pointer into 8-byte argument slots.
struct VaList {
    GuestAddr p;
    u64 next() {
        u64 v = *gptr<u64>(p);
        p += 8;
        return v;
    }
    double next_double() {
        u64 b = next();
        double d;
        std::memcpy(&d, &b, 8);
        return d;
    }
};

// Formats `%@` arguments (set by the Objective-C runtime).
using DescribeFn = std::string (*)(GuestAddr obj);
extern DescribeFn g_describe_object;

std::string format(const char* fmt, VaList& va, bool nsstring_mode = false);
std::string format_w32(const char32_t* fmt, VaList& va);  // result is UTF-8

// UTF helpers for the guest's 32-bit wchar_t.
size_t w32len(const char32_t* s);
std::string w32_to_utf8(const char32_t* s, size_t n = std::string::npos);
std::u32string utf8_to_w32(std::string_view s);
void append_utf8(std::string& out, char32_t c);
std::wstring utf8_to_wide(std::string_view s);  // host UTF-16
std::string wide_to_utf8(std::wstring_view s);

}  // namespace libc
