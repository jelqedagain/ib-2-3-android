// NSString / NSMutableString / __NSCFConstantString.
#include "foundation/foundation.h"
#include "libc/format.h"
#include "objc/internal.h"
#include <algorithm>
#include <mutex>
#include <unordered_map>

namespace ns {

namespace {

struct StrData : objc::HostData {
    std::u16string s;
    std::string utf8_cache;  // backing store for -UTF8String
    std::vector<char32_t> w32_cache;
};

Class g_string, g_mutable, g_constant;

constexpr u64 NSASCIIStringEncoding = 1, NSUTF8StringEncoding = 4, NSISOLatin1StringEncoding = 5,
              NSUnicodeStringEncoding = 10, NSWindowsCP1252StringEncoding = 12, NSMacOSRomanStringEncoding = 30,
              NSUTF16LittleEndianStringEncoding = 0x94000100, NSUTF16BigEndianStringEncoding = 0x90000100,
              NSUTF32StringEncoding = 0x8c000100, NSUTF32LittleEndianStringEncoding = 0x9c000100;

std::u16string utf8_to_u16(std::string_view s) {
    std::u16string out;
    for (char32_t c : libc::utf8_to_w32(s)) {
        if (c >= 0x10000) {
            c -= 0x10000;
            out += (char16_t)(0xd800 + (c >> 10));
            out += (char16_t)(0xdc00 + (c & 0x3ff));
        } else {
            out += (char16_t)c;
        }
    }
    return out;
}

std::string u16_to_utf8(std::u16string_view s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        char32_t c = s[i];
        if (c >= 0xd800 && c < 0xdc00 && i + 1 < s.size() && s[i + 1] >= 0xdc00 && s[i + 1] < 0xe000) {
            c = 0x10000 + ((c - 0xd800) << 10) + (s[i + 1] - 0xdc00);
            i++;
        }
        libc::append_utf8(out, c);
    }
    return out;
}

// Decodes bytes in `enc` to UTF-16.
bool decode(const u8* p, u64 n, u64 enc, std::u16string& out) {
    out.clear();
    switch (enc) {
    case NSASCIIStringEncoding:
    case NSUTF8StringEncoding:
        out = utf8_to_u16(std::string_view(reinterpret_cast<const char*>(p), n));
        return true;
    case NSISOLatin1StringEncoding:
    case NSWindowsCP1252StringEncoding:
    case NSMacOSRomanStringEncoding:
        for (u64 i = 0; i < n; i++) out += (char16_t)p[i];
        return true;
    case NSUnicodeStringEncoding:
    case NSUTF16LittleEndianStringEncoding: {
        const char16_t* w = reinterpret_cast<const char16_t*>(p);
        u64 count = n / 2;
        if (enc == NSUnicodeStringEncoding && count && w[0] == 0xfeff) w++, count--;
        out.assign(w, count);
        return true;
    }
    case NSUTF16BigEndianStringEncoding:
        for (u64 i = 0; i + 1 < n; i += 2) out += (char16_t)((p[i] << 8) | p[i + 1]);
        return true;
    case NSUTF32StringEncoding:
    case NSUTF32LittleEndianStringEncoding: {
        std::string u;
        const char32_t* w = reinterpret_cast<const char32_t*>(p);
        for (u64 i = 0; i < n / 4; i++) libc::append_utf8(u, w[i]);
        out = utf8_to_u16(u);
        return true;
    }
    default:
        LOG_WARN("NSString: unsupported encoding 0x%llx", (unsigned long long)enc);
        out = utf8_to_u16(std::string_view(reinterpret_cast<const char*>(p), n));
        return true;
    }
}

std::vector<u8> encode(const std::u16string& s, u64 enc) {
    std::vector<u8> out;
    switch (enc) {
    case NSUnicodeStringEncoding:
    case NSUTF16LittleEndianStringEncoding:
        out.resize(s.size() * 2);
        std::memcpy(out.data(), s.data(), out.size());
        return out;
    case NSUTF32StringEncoding:
    case NSUTF32LittleEndianStringEncoding: {
        auto w = libc::utf8_to_w32(u16_to_utf8(s));
        out.resize(w.size() * 4);
        std::memcpy(out.data(), w.data(), out.size());
        return out;
    }
    case NSISOLatin1StringEncoding:
    case NSWindowsCP1252StringEncoding:
    case NSASCIIStringEncoding:
        for (char16_t c : s) out.push_back(c < 256 ? (u8)c : '?');
        return out;
    default: {
        std::string u = u16_to_utf8(s);
        out.assign(u.begin(), u.end());
        return out;
    }
    }
}

// __NSCFConstantString layout: isa, flags, ptr, length
struct ConstStr {
    u64 isa;
    u64 flags;
    u64 ptr;
    u64 length;
};

StrData* sdata(id s) { return objc::get<StrData>(s); }

std::mutex g_const_cache_mutex;
std::unordered_map<id, std::string> g_const_utf8;  // UTF-16 constant strings -> UTF-8

id new_string(Class cls, std::u16string s) {
    id obj = objc::alloc(cls);
    auto d = std::make_unique<StrData>();
    d->s = std::move(s);
    objc::set_host_data(obj, std::move(d));
    return obj;
}

std::u16string& mut(id s) {
    StrData* d = sdata(s);
    if (!d) {
        objc::set_host_data(s, std::make_unique<StrData>());
        d = sdata(s);
    }
    d->utf8_cache.clear();
    return d->s;
}

void ret_range(cpu::Thread& t, NSRange r) {
    t.set_x(0, r.location);
    t.set_x(1, r.length);
}

// options: 1 caseInsensitive, 2 literal, 4 backwards, 8 anchored, 64 numeric
NSRange find(const std::u16string& hay, const std::u16string& needle, u64 opts, NSRange range) {
    if (needle.empty() || range.location + range.length > hay.size()) return {NSNotFound, 0};
    auto lower = [](char16_t c) -> char16_t { return (c >= 'A' && c <= 'Z') ? c + 32 : c; };
    bool ci = opts & 1, back = opts & 4, anchored = opts & 8;
    auto match_at = [&](u64 i) {
        for (u64 k = 0; k < needle.size(); k++) {
            char16_t a = hay[i + k], b = needle[k];
            if (ci ? lower(a) != lower(b) : a != b) return false;
        }
        return true;
    };
    if (needle.size() > range.length) return {NSNotFound, 0};
    u64 first = range.location, last = range.location + range.length - needle.size();
    if (back) {
        for (u64 i = last + 1; i-- > first;) {
            if (match_at(i)) return {i, needle.size()};
            if (anchored) break;
        }
    } else {
        for (u64 i = first; i <= last; i++) {
            if (match_at(i)) return {i, needle.size()};
            if (anchored) break;
        }
    }
    return {NSNotFound, 0};
}

std::u16string path_last_component(const std::u16string& p) {
    std::u16string s = p;
    while (s.size() > 1 && s.back() == u'/') s.pop_back();
    size_t slash = s.rfind(u'/');
    return slash == std::u16string::npos ? s : (s.size() == 1 ? s : s.substr(slash + 1));
}

}  // namespace

bool is_string(id obj) { return obj && objc::is_kind_of(obj, g_string); }

std::u16string utf16(id s) {
    if (!s) return {};
    if (objc::isa(s) == g_constant) {
        auto* c = gptr<ConstStr>(s);
        if ((c->flags & 0xff) == 0xd0)  // UTF-16 constant (__ustring)
            return std::u16string(gptr<char16_t>(c->ptr), c->length);
        return utf8_to_u16(std::string_view(gptr<char>(c->ptr), c->length));
    }
    if (StrData* d = sdata(s)) return d->s;
    // Guest subclass (class cluster primitives): ask the object.
    u64 len = objc::send(s, "length");
    std::u16string out;
    for (u64 i = 0; i < len; i++) out += (char16_t)objc::send(s, "characterAtIndex:", {i});
    return out;
}

std::string utf8(id s) {
    if (!s) return {};
    if (objc::isa(s) == g_constant) {
        auto* c = gptr<ConstStr>(s);
        if ((c->flags & 0xff) != 0xd0) return std::string(gptr<char>(c->ptr), c->length);
    }
    return u16_to_utf8(utf16(s));
}

id str_retained(std::string_view s) { return new_string(g_string, utf8_to_u16(s)); }
id str16_retained(std::u16string s) { return new_string(g_string, std::move(s)); }
id str(std::string_view s) { return objc::autorelease(str_retained(s)); }

id string_with_format(Class cls, const char* fmt, GuestAddr va, bool retained) {
    libc::VaList v{va};
    id r = new_string(cls, utf8_to_u16(libc::format(fmt, v, true)));
    return retained ? r : objc::autorelease(r);
}

namespace {
// Returns a stable C string for -UTF8String / -cStringUsingEncoding: (lives as long as the string).
const char* stable_utf8(id s) {
    if (objc::isa(s) == g_constant) {
        auto* c = gptr<ConstStr>(s);
        if ((c->flags & 0xff) != 0xd0) return gptr<char>(c->ptr);
        std::lock_guard lock(g_const_cache_mutex);
        auto& cached = g_const_utf8[s];
        if (cached.empty()) cached = utf8(s);
        return cached.c_str();
    }
    StrData* d = sdata(s);
    if (!d) return hle::static_cstr(utf8(s));
    d->utf8_cache = u16_to_utf8(d->s);
    return d->utf8_cache.c_str();
}
}  // namespace

void install_string() {
    using objc::class_method;
    using objc::method;
    g_string = objc::host_class("NSString");
    g_mutable = objc::host_class("NSMutableString", "NSString");
    g_constant = objc::host_class("__NSCFConstantString", "NSString");
    objc::host_class("NSConstantString", "NSString");
    hle::data("___CFConstantStringClassReference", g_constant);
    objc::g_is_nsstring = is_string;
    objc::g_nsstring_utf8 = utf8;
    libc::g_describe_object = objc::describe;

    Class S = g_string, M = g_mutable;

    // creation
    class_method(S, "string", [](Class c, SEL) { return objc::autorelease(new_string(c, {})); });
    class_method(S, "stringWithUTF8String:", [](Class c, SEL, const char* p) -> id {
        return p ? objc::autorelease(new_string(c, utf8_to_u16(p))) : 0;
    });
    class_method(S, "stringWithCString:", [](Class c, SEL, const char* p) -> id {
        return p ? objc::autorelease(new_string(c, utf8_to_u16(p))) : 0;
    });
    class_method(S, "stringWithCString:encoding:", [](Class c, SEL, const char* p, u64 enc) -> id {
        if (!p) return 0;
        std::u16string s;
        if (enc == NSUTF32LittleEndianStringEncoding || enc == NSUTF32StringEncoding)
            decode(reinterpret_cast<const u8*>(p), libc::w32len(reinterpret_cast<const char32_t*>(p)) * 4, enc, s);
        else if (enc == NSUTF16LittleEndianStringEncoding || enc == NSUnicodeStringEncoding) {
            const char16_t* w = reinterpret_cast<const char16_t*>(p);
            size_t n = 0;
            while (w[n]) n++;
            decode(reinterpret_cast<const u8*>(p), n * 2, enc, s);
        } else
            decode(reinterpret_cast<const u8*>(p), std::strlen(p), enc, s);
        return objc::autorelease(new_string(c, s));
    });
    class_method(S, "stringWithString:", [](Class c, SEL, id other) { return objc::autorelease(new_string(c, utf16(other))); });
    class_method(S, "stringWithCharacters:length:", [](Class c, SEL, const char16_t* p, u64 n) {
        return objc::autorelease(new_string(c, std::u16string(p, n)));
    });
    objc::add_method(S, "stringWithFormat:", [](cpu::Thread& t) {
        std::string fmt = utf8(t.x(2));
        t.set_x(0, string_with_format(t.x(0), fmt.c_str(), t.sp(), false));
    }, true);
    class_method(S, "stringWithContentsOfFile:encoding:error:", [](Class c, SEL, id path, u64 enc, u64* err) -> id {
        id data = objc::send(objc::class_named("NSData"), "dataWithContentsOfFile:", {path});
        if (!data) {
            if (err) *err = 0;
            return 0;
        }
        auto bytes = data_bytes(data);
        std::u16string s;
        decode(bytes.data(), bytes.size(), enc, s);
        return objc::autorelease(new_string(c, s));
    });
    class_method(S, "stringWithContentsOfFile:usedEncoding:error:", [](Class c, SEL, id path, u64* enc, u64* err) -> id {
        id data = objc::send(objc::class_named("NSData"), "dataWithContentsOfFile:", {path});
        if (!data) return 0;
        auto bytes = data_bytes(data);
        std::u16string s;
        decode(bytes.data(), bytes.size(), NSUTF8StringEncoding, s);
        if (enc) *enc = NSUTF8StringEncoding;
        return objc::autorelease(new_string(c, s));
    });
    class_method(S, "pathWithComponents:", [](Class c, SEL, id arr) {
        std::u16string out;
        for (id comp : array_items(arr)) {
            std::u16string s = utf16(comp);
            if (!out.empty() && out.back() != u'/') out += u'/';
            out += s;
        }
        return objc::autorelease(new_string(c, out));
    });

    method(S, "init", [](id self, SEL) {
        mut(self);
        return self;
    });
    method(S, "initWithUTF8String:", [](id self, SEL, const char* p) -> id {
        if (!p) {
            objc::release(self);
            return 0;
        }
        mut(self) = utf8_to_u16(p);
        return self;
    });
    method(S, "initWithString:", [](id self, SEL, id other) {
        mut(self) = utf16(other);
        return self;
    });
    method(S, "initWithCharacters:length:", [](id self, SEL, const char16_t* p, u64 n) {
        mut(self) = std::u16string(p, n);
        return self;
    });
    method(S, "initWithBytes:length:encoding:", [](id self, SEL, const u8* p, u64 n, u64 enc) {
        decode(p, n, enc, mut(self));
        return self;
    });
    method(S, "initWithBytesNoCopy:length:encoding:freeWhenDone:", [](id self, SEL, const u8* p, u64 n, u64 enc, bool) {
        decode(p, n, enc, mut(self));
        return self;
    });
    method(S, "initWithCString:encoding:", [](id self, SEL, const char* p, u64 enc) {
        decode(reinterpret_cast<const u8*>(p), std::strlen(p), enc, mut(self));
        return self;
    });
    method(S, "initWithData:encoding:", [](id self, SEL, id data, u64 enc) {
        auto b = data_bytes(data);
        decode(b.data(), b.size(), enc, mut(self));
        return self;
    });
    objc::add_method(S, "initWithFormat:", [](cpu::Thread& t) {
        std::string fmt = utf8(t.x(2));
        libc::VaList v{t.sp()};
        mut(t.x(0)) = utf8_to_u16(libc::format(fmt.c_str(), v, true));
    });
    objc::add_method(S, "initWithFormat:arguments:", [](cpu::Thread& t) {
        std::string fmt = utf8(t.x(2));
        libc::VaList v{t.x(3)};
        mut(t.x(0)) = utf8_to_u16(libc::format(fmt.c_str(), v, true));
    });

    // accessors
    method(S, "length", [](id self, SEL) -> u64 { return utf16(self).size(); });
    method(S, "characterAtIndex:", [](id self, SEL, u64 i) -> u16 {
        auto s = utf16(self);
        return i < s.size() ? s[i] : 0;
    });
    objc::add_method(S, "getCharacters:range:", [](cpu::Thread& t) {
        auto s = utf16(t.x(0));
        u64 loc = t.x(3), len = t.x(4);
        if (loc + len <= s.size()) std::memcpy(gptr<void>(t.x(2)), s.data() + loc, len * 2);
    });
    method(S, "getCharacters:", [](id self, SEL, char16_t* out) {
        auto s = utf16(self);
        std::memcpy(out, s.data(), s.size() * 2);
    });
    method(S, "UTF8String", [](id self, SEL) { return stable_utf8(self); });
    method(S, "cString", [](id self, SEL) { return stable_utf8(self); });
    method(S, "fileSystemRepresentation", [](id self, SEL) { return stable_utf8(self); });
    method(S, "cStringUsingEncoding:", [](id self, SEL, u64 enc) -> const void* {
        if (enc == NSUTF8StringEncoding || enc == NSASCIIStringEncoding) return stable_utf8(self);
        auto bytes = encode(utf16(self), enc);
        bytes.insert(bytes.end(), 4, 0);
        void* p = hle::alloc_static(bytes.size(), 4);  // leaks, like an autoreleased buffer that never drains
        std::memcpy(p, bytes.data(), bytes.size());
        return p;
    });
    method(S, "getCString:maxLength:encoding:", [](id self, SEL, char* buf, u64 max, u64 enc) {
        auto bytes = encode(utf16(self), enc);
        size_t term = (enc == NSUTF32LittleEndianStringEncoding || enc == NSUTF32StringEncoding) ? 4
                      : (enc == NSUTF16LittleEndianStringEncoding || enc == NSUnicodeStringEncoding) ? 2 : 1;
        if (bytes.size() + term > max) return false;
        std::memcpy(buf, bytes.data(), bytes.size());
        std::memset(buf + bytes.size(), 0, term);
        return true;
    });
    method(S, "lengthOfBytesUsingEncoding:", [](id self, SEL, u64 enc) -> u64 { return encode(utf16(self), enc).size(); });
    method(S, "maximumLengthOfBytesUsingEncoding:", [](id self, SEL, u64 enc) -> u64 { return utf16(self).size() * 4 + 4; });
    method(S, "dataUsingEncoding:", [](id self, SEL, u64 enc) {
        auto b = encode(utf16(self), enc);
        return data_with(b.data(), b.size());
    });
    method(S, "dataUsingEncoding:allowLossyConversion:", [](id self, SEL, u64 enc, bool) {
        auto b = encode(utf16(self), enc);
        return data_with(b.data(), b.size());
    });
    method(S, "description", [](id self, SEL) { return self; });
    method(S, "copyWithZone:", [](id self, SEL, u64) -> id {
        if (objc::isa(self) == g_string || objc::isa(self) == g_constant) return objc::retain(self);
        return new_string(g_string, utf16(self));
    });
    method(S, "mutableCopyWithZone:", [](id self, SEL, u64) { return new_string(g_mutable, utf16(self)); });
    method(S, "hash", [](id self, SEL) -> u64 {
        u64 h = 1469598103934665603ull;
        for (char16_t c : utf16(self)) h = (h ^ c) * 1099511628211ull;
        return h;
    });
    method(S, "isEqual:", [](id self, SEL, id o) { return o == self || (is_string(o) && utf16(o) == utf16(self)); });
    method(S, "isEqualToString:", [](id self, SEL, id o) { return o == self || (o && utf16(o) == utf16(self)); });
    method(S, "compare:", [](id self, SEL, id o) -> s64 {
        auto a = utf16(self), b = utf16(o);
        return a < b ? -1 : a > b ? 1 : 0;
    });
    auto ci_compare = [](id self, SEL, id o) -> s64 {
        auto a = utf16(self), b = utf16(o);
        auto low = [](std::u16string s) {
            for (auto& c : s) c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            return s;
        };
        a = low(a), b = low(b);
        return a < b ? -1 : a > b ? 1 : 0;
    };
    method(S, "caseInsensitiveCompare:", ci_compare);
    method(S, "localizedCaseInsensitiveCompare:", ci_compare);
    method(S, "compare:options:", [](id self, SEL, id o, u64 opts) -> s64 {
        auto a = utf16(self), b = utf16(o);
        if (opts & 1) {
            for (auto& c : a) c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
            for (auto& c : b) c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        }
        return a < b ? -1 : a > b ? 1 : 0;
    });
    method(S, "hasPrefix:", [](id self, SEL, id p) {
        auto s = utf16(self), q = utf16(p);
        return !q.empty() && s.compare(0, q.size(), q) == 0;
    });
    method(S, "hasSuffix:", [](id self, SEL, id p) {
        auto s = utf16(self), q = utf16(p);
        return !q.empty() && s.size() >= q.size() && s.compare(s.size() - q.size(), q.size(), q) == 0;
    });
    objc::add_method(S, "rangeOfString:", [](cpu::Thread& t) {
        auto s = utf16(t.x(0));
        ret_range(t, find(s, utf16(t.x(2)), 0, {0, s.size()}));
    });
    objc::add_method(S, "rangeOfString:options:", [](cpu::Thread& t) {
        auto s = utf16(t.x(0));
        ret_range(t, find(s, utf16(t.x(2)), t.x(3), {0, s.size()}));
    });
    objc::add_method(S, "rangeOfString:options:range:", [](cpu::Thread& t) {
        ret_range(t, find(utf16(t.x(0)), utf16(t.x(2)), t.x(3), {t.x(4), t.x(5)}));
    });
    objc::add_method(S, "rangeOfCharacterFromSet:", [](cpu::Thread& t) {
        auto s = utf16(t.x(0));
        for (u64 i = 0; i < s.size(); i++)
            if (objc::send(t.x(2), "characterIsMember:", {s[i]}) & 0xff) {
                ret_range(t, {i, 1});
                return;
            }
        ret_range(t, {NSNotFound, 0});
    });
    method(S, "containsString:", [](id self, SEL, id o) {
        auto s = utf16(self);
        return find(s, utf16(o), 0, {0, s.size()}).location != NSNotFound;
    });
    method(S, "substringFromIndex:", [](id self, SEL, u64 i) {
        auto s = utf16(self);
        return objc::autorelease(new_string(g_string, i <= s.size() ? s.substr(i) : std::u16string()));
    });
    method(S, "substringToIndex:", [](id self, SEL, u64 i) {
        auto s = utf16(self);
        return objc::autorelease(new_string(g_string, s.substr(0, std::min<u64>(i, s.size()))));
    });
    objc::add_method(S, "substringWithRange:", [](cpu::Thread& t) {
        auto s = utf16(t.x(0));
        u64 loc = t.x(2), len = t.x(3);
        t.set_x(0, objc::autorelease(new_string(g_string, loc <= s.size() ? s.substr(loc, len) : std::u16string())));
    });
    objc::add_method(S, "stringByReplacingCharactersInRange:withString:", [](cpu::Thread& t) {
        auto s = utf16(t.x(0));
        u64 loc = std::min<u64>(t.x(2), s.size()), len = std::min<u64>(t.x(3), s.size() - loc);
        t.set_x(0, objc::autorelease(new_string(g_string, s.substr(0, loc) + utf16(t.x(4)) + s.substr(loc + len))));
    });
    method(S, "stringByAppendingString:", [](id self, SEL, id o) { return objc::autorelease(new_string(g_string, utf16(self) + utf16(o))); });
    objc::add_method(S, "stringByAppendingFormat:", [](cpu::Thread& t) {
        std::string fmt = utf8(t.x(2));
        libc::VaList v{t.sp()};
        t.set_x(0, objc::autorelease(new_string(g_string, utf16(t.x(0)) + utf8_to_u16(libc::format(fmt.c_str(), v, true)))));
    });
    method(S, "stringByReplacingOccurrencesOfString:withString:", [](id self, SEL, id a, id b) {
        auto s = utf16(self), from = utf16(a), to = utf16(b);
        if (!from.empty())
            for (size_t pos = 0; (pos = s.find(from, pos)) != std::u16string::npos; pos += to.size()) s.replace(pos, from.size(), to);
        return objc::autorelease(new_string(g_string, s));
    });
    method(S, "stringByTrimmingCharactersInSet:", [](id self, SEL, id set) {
        auto s = utf16(self);
        size_t b = 0, e = s.size();
        while (b < e && (objc::send(set, "characterIsMember:", {s[b]}) & 0xff)) b++;
        while (e > b && (objc::send(set, "characterIsMember:", {s[e - 1]}) & 0xff)) e--;
        return objc::autorelease(new_string(g_string, s.substr(b, e - b)));
    });
    method(S, "componentsSeparatedByString:", [](id self, SEL, id sep) {
        auto s = utf16(self), d = utf16(sep);
        std::vector<id> parts;
        size_t start = 0, pos;
        while (!d.empty() && (pos = s.find(d, start)) != std::u16string::npos) {
            parts.push_back(objc::autorelease(new_string(g_string, s.substr(start, pos - start))));
            start = pos + d.size();
        }
        parts.push_back(objc::autorelease(new_string(g_string, s.substr(start))));
        return array(parts);
    });
    method(S, "lowercaseString", [](id self, SEL) {
        auto s = utf16(self);
        for (auto& c : s) c = (c >= 'A' && c <= 'Z') ? c + 32 : c;
        return objc::autorelease(new_string(g_string, s));
    });
    method(S, "uppercaseString", [](id self, SEL) {
        auto s = utf16(self);
        for (auto& c : s) c = (c >= 'a' && c <= 'z') ? c - 32 : c;
        return objc::autorelease(new_string(g_string, s));
    });
    method(S, "intValue", [](id self, SEL) -> s32 { return (s32)std::strtol(utf8(self).c_str(), nullptr, 10); });
    method(S, "integerValue", [](id self, SEL) -> s64 { return std::strtoll(utf8(self).c_str(), nullptr, 10); });
    method(S, "longLongValue", [](id self, SEL) -> s64 { return std::strtoll(utf8(self).c_str(), nullptr, 10); });
    method(S, "floatValue", [](id self, SEL) { return (float)std::strtod(utf8(self).c_str(), nullptr); });
    method(S, "doubleValue", [](id self, SEL) { return std::strtod(utf8(self).c_str(), nullptr); });
    method(S, "boolValue", [](id self, SEL) {
        std::string s = utf8(self);
        size_t i = s.find_first_not_of(" \t\n");
        if (i == std::string::npos) return false;
        char c = s[i];
        return c == 'Y' || c == 'y' || c == 'T' || c == 't' || (c >= '1' && c <= '9');
    });

    // paths
    method(S, "lastPathComponent", [](id self, SEL) { return objc::autorelease(new_string(g_string, path_last_component(utf16(self)))); });
    method(S, "pathExtension", [](id self, SEL) {
        auto last = path_last_component(utf16(self));
        size_t dot = last.rfind(u'.');
        return objc::autorelease(new_string(g_string, dot == std::u16string::npos ? std::u16string() : last.substr(dot + 1)));
    });
    method(S, "stringByDeletingLastPathComponent", [](id self, SEL) {
        auto s = utf16(self);
        while (s.size() > 1 && s.back() == u'/') s.pop_back();
        size_t slash = s.rfind(u'/');
        std::u16string r = slash == std::u16string::npos ? u"" : slash == 0 ? u"/" : s.substr(0, slash);
        return objc::autorelease(new_string(g_string, r));
    });
    method(S, "stringByDeletingPathExtension", [](id self, SEL) {
        auto s = utf16(self);
        size_t slash = s.rfind(u'/'), dot = s.rfind(u'.');
        if (dot != std::u16string::npos && (slash == std::u16string::npos || dot > slash + 1)) s = s.substr(0, dot);
        return objc::autorelease(new_string(g_string, s));
    });
    method(S, "stringByAppendingPathComponent:", [](id self, SEL, id comp) {
        auto s = utf16(self), c = utf16(comp);
        while (!c.empty() && c.front() == u'/') c.erase(0, 1);
        if (s.empty()) return objc::autorelease(new_string(g_string, c));
        if (s.back() != u'/') s += u'/';
        return objc::autorelease(new_string(g_string, s + c));
    });
    method(S, "stringByAppendingPathExtension:", [](id self, SEL, id ext) {
        return objc::autorelease(new_string(g_string, utf16(self) + u"." + utf16(ext)));
    });
    method(S, "stringByStandardizingPath", [](id self, SEL) { return self; });
    method(S, "stringByResolvingSymlinksInPath", [](id self, SEL) { return self; });
    method(S, "stringByExpandingTildeInPath", [](id self, SEL) { return self; });
    method(S, "isAbsolutePath", [](id self, SEL) {
        auto s = utf16(self);
        return !s.empty() && s[0] == u'/';
    });
    method(S, "pathComponents", [](id self, SEL) {
        auto s = utf16(self);
        std::vector<id> parts;
        if (!s.empty() && s[0] == u'/') parts.push_back(str("/"));
        size_t i = 0;
        while (i < s.size()) {
            size_t j = s.find(u'/', i);
            if (j == std::u16string::npos) j = s.size();
            if (j > i) parts.push_back(objc::autorelease(new_string(g_string, s.substr(i, j - i))));
            i = j + 1;
        }
        return array(parts);
    });
    method(S, "stringByAddingPercentEscapesUsingEncoding:", [](id self, SEL, u64) {
        std::string out;
        for (unsigned char c : utf8(self)) {
            if (isalnum(c) || std::strchr("-._~!*'();:@&=+$,/?#[]", c)) out += (char)c;
            else {
                char buf[4];
                snprintf(buf, sizeof buf, "%%%02X", c);
                out += buf;
            }
        }
        return str(out);
    });
    method(S, "stringByReplacingPercentEscapesUsingEncoding:", [](id self, SEL, u64) {
        std::string s = utf8(self), out;
        for (size_t i = 0; i < s.size(); i++) {
            if (s[i] == '%' && i + 2 < s.size() && isxdigit((u8)s[i + 1]) && isxdigit((u8)s[i + 2])) {
                out += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
                i += 2;
            } else {
                out += s[i];
            }
        }
        return str(out);
    });
    method(S, "writeToFile:atomically:encoding:error:", [](id self, SEL, id path, bool, u64 enc, u64*) {
        auto b = encode(utf16(self), enc);
        id data = data_with(b.data(), b.size());
        return (objc::send(data, "writeToFile:atomically:", {path, 1}) & 0xff) != 0;
    });

    // NSMutableString
    class_method(M, "stringWithCapacity:", [](Class c, SEL, u64) { return objc::autorelease(new_string(c, {})); });
    method(M, "initWithCapacity:", [](id self, SEL, u64) {
        mut(self);
        return self;
    });
    method(M, "appendString:", [](id self, SEL, id o) { mut(self) += utf16(o); });
    objc::add_method(M, "appendFormat:", [](cpu::Thread& t) {
        std::string fmt = utf8(t.x(2));
        libc::VaList v{t.sp()};
        mut(t.x(0)) += utf8_to_u16(libc::format(fmt.c_str(), v, true));
    });
    method(M, "setString:", [](id self, SEL, id o) { mut(self) = utf16(o); });
    method(M, "insertString:atIndex:", [](id self, SEL, id o, u64 i) {
        auto& s = mut(self);
        s.insert(std::min<u64>(i, s.size()), utf16(o));
    });
    objc::add_method(M, "deleteCharactersInRange:", [](cpu::Thread& t) {
        auto& s = mut(t.x(0));
        if (t.x(2) < s.size()) s.erase(t.x(2), t.x(3));
    });
    objc::add_method(M, "replaceCharactersInRange:withString:", [](cpu::Thread& t) {
        auto& s = mut(t.x(0));
        if (t.x(2) <= s.size()) s.replace(t.x(2), t.x(3), utf16(t.x(4)));
    });
    objc::add_method(M, "replaceOccurrencesOfString:withString:options:range:", [](cpu::Thread& t) {
        auto& s = mut(t.x(0));
        auto from = utf16(t.x(2)), to = utf16(t.x(3));
        u64 n = 0;
        if (!from.empty())
            for (size_t pos = 0; (pos = s.find(from, pos)) != std::u16string::npos; pos += to.size(), n++) s.replace(pos, from.size(), to);
        t.set_x(0, n);
    });
}

}  // namespace ns
