// CoreFoundation functions (toll-free bridged to our Foundation objects), Security keychain,
// SystemConfiguration reachability.
#include "foundation/foundation.h"
#include <filesystem>
#include "libc/format.h"
#include "libc/vfs.h"
#include "objc/internal.h"
#include <fstream>
#include <mutex>
#include <windows.h>

namespace ns {

namespace {

struct UuidData : objc::HostData {
    u8 bytes[16];
};
Class g_uuid;

id new_uuid(const u8* b) {
    id u = objc::alloc(g_uuid);
    std::memcpy(objc::ensure<UuidData>(u).bytes, b, 16);
    return u;
}

std::string percent_escape(const std::string& s, const std::string& extra_legal, const std::string& extra_escape) {
    std::string out;
    for (unsigned char c : s) {
        bool legal = (isalnum(c) || std::strchr("-._~!*'();:@&=+$,/?#[]", c)) && extra_escape.find((char)c) == std::string::npos;
        if (extra_legal.find((char)c) != std::string::npos) legal = true;
        if (legal) out += (char)c;
        else {
            char buf[4];
            snprintf(buf, sizeof buf, "%%%02X", c);
            out += buf;
        }
    }
    return out;
}

std::string percent_unescape(const std::string& s) {
    std::string out;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && isxdigit((u8)s[i + 1]) && isxdigit((u8)s[i + 2])) {
            out += (char)std::strtol(s.substr(i + 1, 2).c_str(), nullptr, 16);
            i += 2;
        } else {
            out += s[i];
        }
    }
    return out;
}

// ---- keychain: generic passwords keyed by service/account, persisted as a plist ----
std::mutex g_kc_mutex;
id g_keychain = 0;  // NSMutableDictionary "service|account" -> data
std::string kc_file() { return vfs::host_home() + "/Library/keychain.plist"; }
void kc_load() {
    if (g_keychain) return;
    g_keychain = objc::retain(mutable_dict());
    if (id loaded = plist_from_file(kc_file())) {
        for (auto& [k, v] : dict_items(loaded)) dict_set(g_keychain, k, v);
        objc::release(loaded);
    }
}
void kc_save() {
    std::string xml = plist_to_xml(g_keychain);
    std::ofstream f(std::filesystem::path(libc::utf8_to_wide(kc_file())), std::ios::binary);
    f.write(xml.data(), xml.size());
}
id kc_key(id query) {
    return str(utf8(dict_get(query, str("svce"))) + "|" + utf8(dict_get(query, str("acct"))) + "|" +
               utf8(dict_get(query, str("agrp"))));
}

constexpr s32 errSecSuccess = 0, errSecItemNotFound = -25300, errSecDuplicateItem = -25299;

}  // namespace

void install_cf() {
    using hle::fn;
    g_uuid = objc::host_class("__NSCFUUID");

    static u64 s_true, s_false;
    hle::data_lazy("_kCFBooleanTrue", [] {
        s_true = objc::retain(number_bool(true));
        return gaddr(&s_true);
    });
    hle::data_lazy("_kCFBooleanFalse", [] {
        s_false = objc::retain(number_bool(false));
        return gaddr(&s_false);
    });
    hle::data("_kCFTypeDictionaryValueCallBacks", gaddr(hle::alloc_static(48)));
    hle::data("_kCFTypeDictionaryKeyCallBacks", gaddr(hle::alloc_static(48)));

    fn("_CFRelease", [](id o) { objc::release(o); });
    fn("_CFRetain", [](id o) { return objc::retain(o); });
    fn("_CFMakeCollectable", [](id o) { return o; });
    fn("_CFGetTypeID", [](id o) -> u64 {
        if (is_string(o)) return 7;
        if (is_dict(o)) return 18;
        if (is_array(o)) return 19;
        if (is_data(o)) return 20;
        if (is_number(o)) return 22;
        return 1;
    });
    fn("_CFDataGetTypeID", []() -> u64 { return 20; });
    fn("_CFDictionaryCreateMutable", [](u64, s64, u64, u64) { return objc::retain(mutable_dict()); });
    fn("_CFDictionaryGetValue", [](id d, id k) { return dict_get(d, k); });
    fn("_CFDictionarySetValue", [](id d, id k, id v) { dict_set(d, k, v); });
    fn("_CFStringCreateCopy", [](u64, id s) { return str_retained(utf8(s)); });
    fn("_CFPropertyListCreateDeepCopy", [](u64, id p, u64 opts) -> id {
        id xml_copy = plist_from_xml(plist_to_xml(p));
        (void)opts;
        return xml_copy;
    });
    fn("_CFURLCreateStringByAddingPercentEscapes", [](u64, id s, id leave, id add, u32) {
        return str_retained(percent_escape(utf8(s), utf8(leave), utf8(add)));
    });
    fn("_CFURLCreateStringByReplacingPercentEscapes", [](u64, id s, id) { return str_retained(percent_unescape(utf8(s))); });
    fn("_CFURLCreateStringByReplacingPercentEscapesUsingEncoding", [](u64, id s, id, u32) {
        return str_retained(percent_unescape(utf8(s)));
    });
    fn("_CFUUIDCreate", [](u64) {
        GUID g;
        CoCreateGuid(&g);
        return new_uuid(reinterpret_cast<const u8*>(&g));
    });
    hle::raw("_CFUUIDCreateFromUUIDBytes", [](cpu::Thread& t) {
        // CFUUIDBytes (16 bytes) arrives in x1:x2.
        u64 b[2] = {t.x(1), t.x(2)};
        t.set_x(0, new_uuid(reinterpret_cast<const u8*>(b)));
    });
    hle::raw("_CFUUIDGetUUIDBytes", [](cpu::Thread& t) {
        u64 b[2];
        std::memcpy(b, objc::ensure<UuidData>(t.x(0)).bytes, 16);
        t.set_x(0, b[0]);
        t.set_x(1, b[1]);
    });
    fn("_CFUUIDCreateString", [](u64, id u) {
        const u8* b = objc::ensure<UuidData>(u).bytes;
        char buf[40];
        snprintf(buf, sizeof buf, "%02X%02X%02X%02X-%02X%02X-%02X%02X-%02X%02X-%02X%02X%02X%02X%02X%02X", b[0], b[1], b[2], b[3], b[4],
                 b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        return str_retained(buf);
    });

    // ---- Security ----
    fn("_SecItemCopyMatching", [](id query, u64* result) -> s32 {
        std::lock_guard lock(g_kc_mutex);
        kc_load();
        id v = dict_get(g_keychain, kc_key(query));
        LOG_DEBUG("SecItemCopyMatching %s -> %s", utf8(kc_key(query)).c_str(), v ? "found" : "not found");
        if (!v) return errSecItemNotFound;
        if (result) {
            bool want_attrs = dict_get(query, str("r_Attributes")) != 0;
            if (want_attrs) {
                id attrs = objc::retain(mutable_dict());
                dict_set(attrs, str("v_Data"), v);
                if (id a = dict_get(query, str("acct"))) dict_set(attrs, str("acct"), a);
                if (id s = dict_get(query, str("svce"))) dict_set(attrs, str("svce"), s);
                *result = attrs;
            } else {
                *result = objc::retain(v);
            }
        }
        return errSecSuccess;
    });
    fn("_SecItemAdd", [](id attrs, u64* result) -> s32 {
        std::lock_guard lock(g_kc_mutex);
        kc_load();
        id key = kc_key(attrs);
        if (dict_get(g_keychain, key)) return errSecDuplicateItem;
        id v = dict_get(attrs, str("v_Data"));
        dict_set(g_keychain, key, v ? v : data_with("", 0));
        kc_save();
        if (result) *result = 0;
        return errSecSuccess;
    });
    fn("_SecItemUpdate", [](id query, id attrs) -> s32 {
        std::lock_guard lock(g_kc_mutex);
        kc_load();
        id key = kc_key(query);
        if (!dict_get(g_keychain, key)) return errSecItemNotFound;
        if (id v = dict_get(attrs, str("v_Data"))) dict_set(g_keychain, key, v);
        kc_save();
        return errSecSuccess;
    });
    fn("_SecItemDelete", [](id query) -> s32 {
        std::lock_guard lock(g_kc_mutex);
        kc_load();
        objc::send(g_keychain, "removeObjectForKey:", {kc_key(query)});
        kc_save();
        return errSecSuccess;
    });
    // Keychain attribute keys have fixed short values in Security.framework.
    static const std::pair<const char*, const char*> sec_keys[] = {
        {"_kSecClass", "class"}, {"_kSecClassGenericPassword", "genp"}, {"_kSecClassKey", "keys"},
        {"_kSecAttrService", "svce"}, {"_kSecAttrAccount", "acct"}, {"_kSecAttrAccessGroup", "agrp"},
        {"_kSecAttrGeneric", "gena"}, {"_kSecAttrLabel", "labl"}, {"_kSecAttrDescription", "desc"},
        {"_kSecAttrAccessible", "pdmn"}, {"_kSecAttrAccessibleWhenUnlocked", "ak"},
        {"_kSecAttrAccessibleAfterFirstUnlockThisDeviceOnly", "cku"}, {"_kSecAttrApplicationTag", "atag"},
        {"_kSecAttrKeyType", "type"}, {"_kSecAttrKeyTypeRSA", "42"}, {"_kSecValueData", "v_Data"},
        {"_kSecReturnData", "r_Data"}, {"_kSecReturnAttributes", "r_Attributes"}, {"_kSecReturnRef", "r_Ref"},
        {"_kSecReturnPersistentRef", "r_PersistentRef"}, {"_kSecMatchLimit", "m_Limit"}, {"_kSecMatchLimitOne", "m_LimitOne"},
    };
    for (auto& [sym, value] : sec_keys) {
        std::string v = value;
        hle::data_lazy(sym, [v] {
            auto* cell = static_cast<u64*>(hle::alloc_static(8));
            *cell = str_retained(v);
            return gaddr(cell);
        });
    }

    // ---- SystemConfiguration: always offline ----
    fn("_SCNetworkReachabilityCreateWithName", [](u64, const char*) { return objc::alloc(objc::class_named("NSObject")); });
    fn("_SCNetworkReachabilityCreateWithAddress", [](u64, const void*) { return objc::alloc(objc::class_named("NSObject")); });
    fn("_SCNetworkReachabilityGetFlags", [](id, u32* flags) {
        *flags = 0;
        return true;
    });
    fn("_SCNetworkReachabilitySetCallback", [](id, u64, u64) { return true; });
    fn("_SCNetworkReachabilityScheduleWithRunLoop", [](id, u64, id) { return true; });
    fn("_SCNetworkReachabilityUnscheduleFromRunLoop", [](id, u64, id) { return true; });
    fn("_SCNetworkReachabilitySetDispatchQueue", [](id, u64) { return true; });
}

}  // namespace ns
