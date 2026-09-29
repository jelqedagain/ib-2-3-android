#include "libc/format.h"
#include <windows.h>

namespace libc {

DescribeFn g_describe_object = nullptr;

size_t w32len(const char32_t* s) {
    size_t n = 0;
    while (s[n]) n++;
    return n;
}

void append_utf8(std::string& out, char32_t c) {
    if (c < 0x80) {
        out += (char)c;
    } else if (c < 0x800) {
        out += (char)(0xc0 | (c >> 6));
        out += (char)(0x80 | (c & 0x3f));
    } else if (c < 0x10000) {
        out += (char)(0xe0 | (c >> 12));
        out += (char)(0x80 | ((c >> 6) & 0x3f));
        out += (char)(0x80 | (c & 0x3f));
    } else {
        out += (char)(0xf0 | (c >> 18));
        out += (char)(0x80 | ((c >> 12) & 0x3f));
        out += (char)(0x80 | ((c >> 6) & 0x3f));
        out += (char)(0x80 | (c & 0x3f));
    }
}

std::string w32_to_utf8(const char32_t* s, size_t n) {
    std::string out;
    if (!s) return out;
    for (size_t i = 0; i < n && s[i]; i++) append_utf8(out, s[i]);
    return out;
}

std::u32string utf8_to_w32(std::string_view s) {
    std::u32string out;
    for (size_t i = 0; i < s.size();) {
        u8 c = (u8)s[i];
        char32_t cp;
        int extra;
        if (c < 0x80) { cp = c; extra = 0; }
        else if ((c >> 5) == 6) { cp = c & 0x1f; extra = 1; }
        else if ((c >> 4) == 14) { cp = c & 0x0f; extra = 2; }
        else if ((c >> 3) == 30) { cp = c & 0x07; extra = 3; }
        else { cp = 0xfffd; extra = 0; }
        i++;
        for (int k = 0; k < extra && i < s.size(); k++, i++) cp = (cp << 6) | ((u8)s[i] & 0x3f);
        out += cp;
    }
    return out;
}

std::wstring utf8_to_wide(std::string_view s) {
    if (s.empty()) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, 0);
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}

std::string wide_to_utf8(std::wstring_view s) {
    if (s.empty()) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string o(n, 0);
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), o.data(), n, nullptr, nullptr);
    return o;
}

namespace {

// Shared implementation; CharT is char or char32_t for the format string.
template <typename CharT>
std::string format_impl(const CharT* fmt, VaList& va, bool ns = false) {
    std::string out;
    if (!fmt) return "(null)";
    for (const CharT* p = fmt; *p;) {
        if (*p != '%') {
            if constexpr (sizeof(CharT) == 1) out += (char)*p;
            else append_utf8(out, *p);
            p++;
            continue;
        }
        const CharT* start = p++;
        if (*p == '%') {
            out += '%';
            p++;
            continue;
        }
        std::string spec = "%";
        // flags
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0' || *p == '\'') {
            if (*p != '\'') spec += (char)*p;
            p++;
        }
        // width
        if (*p == '*') {
            spec += std::to_string((int)va.next());
            p++;
        } else {
            while (*p >= '0' && *p <= '9') spec += (char)*p++;
        }
        // precision
        int precision = -1;
        if (*p == '.') {
            spec += '.';
            p++;
            if (*p == '*') {
                precision = (int)va.next();
                spec += std::to_string(precision);
                p++;
            } else {
                precision = 0;
                while (*p >= '0' && *p <= '9') {
                    precision = precision * 10 + (*p - '0');
                    spec += (char)*p++;
                }
            }
        }
        // length
        int lng = 0;  // 0 int, 1 char, 2 short, 3 long/longlong/size
        bool wide = false, long_double = false;
        for (;;) {
            if (*p == 'h') { lng = lng == 2 ? 1 : 2; p++; }
            else if (*p == 'l') { lng = 3; wide = true; p++; }
            else if (*p == 'q' || *p == 'j' || *p == 'z' || *p == 't') { lng = 3; p++; }
            else if (*p == 'L') { long_double = true; p++; }
            else break;
        }
        CharT conv = *p;
        if (!conv) break;
        p++;
        char buf[512];
        switch (conv) {
        case 'd':
        case 'i': {
            s64 v = (s64)va.next();
            if (lng == 0) v = (s32)v;
            else if (lng == 1) v = (s8)v;
            else if (lng == 2) v = (s16)v;
            snprintf(buf, sizeof buf, (spec + "lld").c_str(), (long long)v);
            out += buf;
            break;
        }
        case 'u':
        case 'o':
        case 'x':
        case 'X': {
            u64 v = va.next();
            if (lng == 0) v = (u32)v;
            else if (lng == 1) v = (u8)v;
            else if (lng == 2) v = (u16)v;
            snprintf(buf, sizeof buf, (spec + "ll" + (char)conv).c_str(), (unsigned long long)v);
            out += buf;
            break;
        }
        case 'D': case 'U': case 'O': {
            u64 v = va.next();
            char c = conv == 'D' ? 'd' : conv == 'U' ? 'u' : 'o';
            snprintf(buf, sizeof buf, (spec + "ll" + c).c_str(), (unsigned long long)v);
            out += buf;
            break;
        }
        case 'f': case 'F': case 'e': case 'E': case 'g': case 'G': case 'a': case 'A': {
            (void)long_double;  // long double == double on arm64
            double v = va.next_double();
            snprintf(buf, sizeof buf, (spec + (char)conv).c_str(), v);
            out += buf;
            break;
        }
        case 'c':
        case 'C': {
            u32 c = (u32)va.next();
            std::string s;
            if (ns && conv == 'C') append_utf8(s, c & 0xffff);
            else if (wide || conv == 'C') append_utf8(s, c);
            else s = std::string(1, (char)c);
            snprintf(buf, sizeof buf, (spec + "s").c_str(), s.c_str());
            out += buf;
            break;
        }
        case 's':
        case 'S': {
            GuestAddr a = va.next();
            std::string s;
            if (!a) s = "(null)";
            else if (ns && conv == 'S') {
                const char16_t* w = gptr<char16_t>(a);
                for (size_t k = 0; w[k] && (precision < 0 || k < (size_t)precision); k++) append_utf8(s, w[k]);
            } else if (wide || conv == 'S') s = w32_to_utf8(gptr<char32_t>(a), precision >= 0 ? (size_t)precision : std::string::npos);
            else if (precision >= 0) s.assign(gptr<char>(a), strnlen(gptr<char>(a), precision));
            else s = gptr<char>(a);
            // Width handling via host printf on the converted string.
            std::string sp = spec;
            if (precision >= 0) sp = sp.substr(0, sp.find('.'));
            std::string tmp(s.size() + 256, 0);
            int n = snprintf(tmp.data(), tmp.size(), (sp + "s").c_str(), s.c_str());
            out.append(tmp.data(), n > 0 ? n : 0);
            break;
        }
        case '@': {
            GuestAddr obj = va.next();
            std::string s = obj ? (g_describe_object ? g_describe_object(obj) : "<object>") : "(null)";
            out += s;
            break;
        }
        case 'p': {
            snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)va.next());
            out += buf;
            break;
        }
        case 'n': {
            GuestAddr a = va.next();
            if (a) *gptr<s32>(a) = (s32)out.size();
            break;
        }
        default:
            // Unknown conversion: emit verbatim.
            for (const CharT* q = start; q < p; q++) out += (char)*q;
            break;
        }
    }
    return out;
}

}  // namespace

std::string format(const char* fmt, VaList& va, bool ns) { return format_impl(fmt, va, ns); }
std::string format_w32(const char32_t* fmt, VaList& va) { return format_impl(fmt, va); }

}  // namespace libc
