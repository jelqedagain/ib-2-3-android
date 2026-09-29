// <stdio.h>: printf family, FILE streams, swscanf.
#include "hle.h"
#include "libc/format.h"
#include "libc/vfs.h"
#include "modules.h"
#include <cerrno>
#include <mutex>

namespace libc {

namespace {

std::mutex g_line_mutex;
std::string g_stdout_line;

// Guest stdout/stderr go to our log, line-buffered.
void guest_console(const std::string& s) {
    std::lock_guard lock(g_line_mutex);
    g_stdout_line += s;
    size_t nl;
    while ((nl = g_stdout_line.find('\n')) != std::string::npos) {
        LOG_INFO("[guest] %s", g_stdout_line.substr(0, nl).c_str());
        g_stdout_line.erase(0, nl + 1);
    }
    if (g_stdout_line.size() > 4096) {
        LOG_INFO("[guest] %s", g_stdout_line.c_str());
        g_stdout_line.clear();
    }
}

bool is_console(FILE* f) { return f == stdout || f == stderr; }

u64 write_bounded(char* buf, u64 size, const std::string& s) {
    if (buf && size) {
        u64 n = std::min<u64>(s.size(), size - 1);
        std::memcpy(buf, s.data(), n);
        buf[n] = 0;
    }
    return s.size();
}

// Minimal scanf over UTF-8 input; `wide_out` selects wchar_t (32-bit) targets for %s/%c.
int scan(const std::string& in, const std::string& fmt, VaList va, bool wide_default) {
    size_t i = 0, f = 0;
    int assigned = 0;
    auto skip_ws = [&] { while (i < in.size() && isspace((u8)in[i])) i++; };
    while (f < fmt.size()) {
        char c = fmt[f];
        if (isspace((u8)c)) {
            skip_ws();
            f++;
            continue;
        }
        if (c != '%') {
            if (i >= in.size() || in[i] != c) return assigned;
            i++, f++;
            continue;
        }
        f++;
        if (fmt[f] == '%') {
            if (i >= in.size() || in[i] != '%') return assigned;
            i++, f++;
            continue;
        }
        bool suppress = false;
        if (fmt[f] == '*') suppress = true, f++;
        int width = 0;
        while (isdigit((u8)fmt[f])) width = width * 10 + (fmt[f++] - '0');
        int lng = 0;
        while (fmt[f] == 'l' || fmt[f] == 'h' || fmt[f] == 'q' || fmt[f] == 'L' || fmt[f] == 'z' || fmt[f] == 'j') {
            lng += fmt[f] == 'l' || fmt[f] == 'q' || fmt[f] == 'L' || fmt[f] == 'z' || fmt[f] == 'j' ? 1 : -1;
            f++;
        }
        char conv = fmt[f++];
        if (conv != 'c' && conv != '[' && conv != 'n') skip_ws();
        if (conv == 'n') {
            if (!suppress) *gptr<s32>(va.next()) = (s32)i;
            continue;
        }
        if (i >= in.size()) return assigned ? assigned : -1;
        std::string tok = in.substr(i, width ? (size_t)width : std::string::npos);
        const char* s = tok.c_str();
        char* end = nullptr;
        switch (conv) {
        case 'd': case 'i': case 'u': case 'x': case 'X': case 'o': {
            int base = conv == 'x' || conv == 'X' ? 16 : conv == 'o' ? 8 : conv == 'i' ? 0 : 10;
            long long v = (conv == 'u' || conv == 'x' || conv == 'X') ? (long long)strtoull(s, &end, base) : strtoll(s, &end, base);
            if (end == s) return assigned;
            i += end - s;
            if (!suppress) {
                GuestAddr p = va.next();
                if (lng >= 1) *gptr<s64>(p) = v;
                else if (lng == -1) *gptr<s16>(p) = (s16)v;
                else if (lng <= -2) *gptr<s8>(p) = (s8)v;
                else *gptr<s32>(p) = (s32)v;
                assigned++;
            }
            break;
        }
        case 'f': case 'e': case 'g': case 'E': case 'G': case 'a': {
            double v = strtod(s, &end);
            if (end == s) return assigned;
            i += end - s;
            if (!suppress) {
                GuestAddr p = va.next();
                if (lng >= 1) *gptr<double>(p) = v;
                else *gptr<float>(p) = (float)v;
                assigned++;
            }
            break;
        }
        case 's': case 'c': case '[': {
            std::string word;
            if (conv == 'c') {
                word = tok.substr(0, width ? width : 1);
            } else if (conv == 's') {
                size_t k = 0;
                while (k < tok.size() && !isspace((u8)tok[k])) k++;
                word = tok.substr(0, k);
            } else {
                size_t close = fmt.find(']', f + 1);
                std::string set = fmt.substr(f, close - f);
                f = close + 1;
                bool neg = !set.empty() && set[0] == '^';
                if (neg) set.erase(0, 1);
                size_t k = 0;
                while (k < tok.size() && ((set.find(tok[k]) != std::string::npos) != neg)) k++;
                word = tok.substr(0, k);
            }
            if (word.empty()) return assigned;
            i += word.size();
            if (!suppress) {
                GuestAddr p = va.next();
                bool wide = lng >= 1 || wide_default;
                if (wide) {
                    std::u32string w = utf8_to_w32(word);
                    std::memcpy(gptr<void>(p), w.data(), w.size() * 4);
                    if (conv != 'c') gptr<char32_t>(p)[w.size()] = 0;
                } else {
                    std::memcpy(gptr<void>(p), word.data(), word.size());
                    if (conv != 'c') gptr<char>(p)[word.size()] = 0;
                }
                assigned++;
            }
            break;
        }
        default:
            LOG_WARN("scanf: unsupported conversion %%%c", conv);
            return assigned;
        }
    }
    return assigned;
}

}  // namespace

void install_stdio() {
    using hle::fn;
    static FILE* s_stdin = stdin;
    static FILE* s_stdout = stdout;
    static FILE* s_stderr = stderr;
    hle::data("___stdinp", gaddr(&s_stdin));
    hle::data("___stdoutp", gaddr(&s_stdout));
    hle::data("___stderrp", gaddr(&s_stderr));

    hle::raw("_printf", [](cpu::Thread& t) {
        VaList va{t.sp()};
        std::string s = format(gptr<char>(t.x(0)), va);
        guest_console(s);
        t.set_x(0, s.size());
    });
    hle::raw("_wprintf", [](cpu::Thread& t) {
        VaList va{t.sp()};
        std::string s = format_w32(gptr<char32_t>(t.x(0)), va);
        guest_console(s);
        t.set_x(0, s.size());
    });
    hle::raw("_fprintf", [](cpu::Thread& t) {
        FILE* f = gptr<FILE>(t.x(0));
        VaList va{t.sp()};
        std::string s = format(gptr<char>(t.x(1)), va);
        if (is_console(f)) guest_console(s);
        else fwrite(s.data(), 1, s.size(), f);
        t.set_x(0, s.size());
    });
    hle::raw("_snprintf", [](cpu::Thread& t) {
        VaList va{t.sp()};
        std::string s = format(gptr<char>(t.x(2)), va);
        t.set_x(0, write_bounded(gptr<char>(t.x(0)), t.x(1), s));
    });
    hle::raw("___snprintf_chk", [](cpu::Thread& t) {
        VaList va{t.sp()};
        std::string s = format(gptr<char>(t.x(4)), va);
        t.set_x(0, write_bounded(gptr<char>(t.x(0)), t.x(1), s));
    });
    hle::raw("___sprintf_chk", [](cpu::Thread& t) {
        VaList va{t.sp()};
        std::string s = format(gptr<char>(t.x(3)), va);
        t.set_x(0, write_bounded(gptr<char>(t.x(0)), ~0ull, s));
    });
    hle::raw("_vsnprintf", [](cpu::Thread& t) {
        VaList va{t.x(3)};
        std::string s = format(gptr<char>(t.x(2)), va);
        t.set_x(0, write_bounded(gptr<char>(t.x(0)), t.x(1), s));
    });
    hle::raw("_swscanf", [](cpu::Thread& t) {
        std::string in = w32_to_utf8(gptr<char32_t>(t.x(0)));
        std::string fmt = w32_to_utf8(gptr<char32_t>(t.x(1)));
        t.set_x(0, (u64)(s64)scan(in, fmt, VaList{t.sp()}, false));
    });

    fn("_puts", [](const char* s) {
        guest_console(std::string(s) + "\n");
        return 1;
    });
    fn("_putchar", [](int c) {
        guest_console(std::string(1, (char)c));
        return c;
    });
    fn("_fopen", [](const char* path, const char* mode) -> FILE* {
        std::wstring h = vfs::to_host_w(path);
        if (h.empty()) {
            vfs::set_errno(2);
            return nullptr;
        }
        std::string m = mode;
        if (m.find('b') == std::string::npos) m += 'b';
        FILE* f = _wfopen(h.c_str(), utf8_to_wide(m).c_str());
        if (!f) vfs::set_errno(vfs::darwin_errno_from_crt(errno));
        LOG_DEBUG("fopen(%s, %s) = %p", path, mode, (void*)f);
        return f;
    });
    fn("_fwrite", [](const void* p, u64 size, u64 n, FILE* f) -> u64 {
        if (is_console(f)) {
            guest_console(std::string(static_cast<const char*>(p), size * n));
            return n;
        }
        return std::fwrite(p, size, n, f);
    });
    fn("_fgets", [](char* buf, int n, FILE* f) { return std::fgets(buf, n, f); });
    fn("_fflush", [](FILE* f) { return is_console(f) ? 0 : std::fflush(f); });
}

}  // namespace libc
