// NSBundle, NSFileManager, NSUserDefaults, NSProcessInfo, NSLocale, NSTimeZone, NSAutoreleasePool,
// NSNotificationCenter, NSError/NSException, NSCharacterSet, NSURL, NSLog and friends.
#include "foundation/foundation.h"
#include "foundation/runloop.h"
#include "libc/format.h"
#include "libc/vfs.h"
#include "objc/internal.h"
#include <filesystem>
#include <fstream>
#include <mutex>
#include <windows.h>

namespace ns {

namespace {

namespace fs = std::filesystem;

Class g_bundle, g_filemgr, g_defaults, g_procinfo, g_locale, g_timezone, g_pool, g_center, g_notification, g_error,
    g_exception, g_charset, g_url;
id g_main_bundle = 0, g_file_manager = 0, g_std_defaults = 0, g_process_info = 0, g_current_locale = 0,
   g_default_center = 0, g_tz = 0;

fs::path host_path(id guest_path) { return fs::path(libc::utf8_to_wide(vfs::to_host(utf8(guest_path).c_str()))); }

// ---- NSBundle ----
struct BundleData : objc::HostData {
    std::string path;
    id info = 0;
};

// ---- NSUserDefaults ----
std::mutex g_defaults_mutex;
id g_defaults_dict = 0, g_registered = 0;
std::string defaults_file() { return vfs::host_home() + "/Library/Preferences/com.chairentertainment.IB3.plist"; }
id defaults_get(id key) {
    std::lock_guard lock(g_defaults_mutex);
    id v = dict_get(g_defaults_dict, key);
    if (!v && g_registered) v = dict_get(g_registered, key);
    if (!v && utf8(key) == "AppleLanguages") v = array({str("en")});
    return v;
}
void defaults_set(id key, id v) {
    std::lock_guard lock(g_defaults_mutex);
    if (v) dict_set(g_defaults_dict, key, v);
    else objc::send(g_defaults_dict, "removeObjectForKey:", {key});
}
void defaults_save() {
    std::string xml;
    {
        std::lock_guard lock(g_defaults_mutex);
        xml = plist_to_xml(g_defaults_dict);
    }
    std::ofstream f(std::filesystem::path(libc::utf8_to_wide(defaults_file())), std::ios::binary);
    f.write(xml.data(), xml.size());
}

// ---- NSNotificationCenter ----
struct Observer {
    id observer;  // target (or block token)
    SEL selector;
    GuestAddr block;
    std::string name;
    id object;
};
std::mutex g_obs_mutex;
std::vector<Observer> g_observers;
struct NoteData : objc::HostData {
    id name = 0, object = 0, user_info = 0;
    ~NoteData() override {
        objc::release(name);
        objc::release(user_info);
    }
};
id make_note(id name, id object, id info) {
    id n = objc::alloc(g_notification);
    auto& d = objc::ensure<NoteData>(n);
    d.name = objc::retain(name);
    d.object = object;
    d.user_info = objc::retain(info);
    return objc::autorelease(n);
}
void post_note(id note) {
    auto& d = objc::ensure<NoteData>(note);
    std::string name = utf8(d.name);
    std::vector<Observer> targets;
    {
        std::lock_guard lock(g_obs_mutex);
        for (auto& o : g_observers)
            if ((o.name.empty() || o.name == name) && (!o.object || o.object == d.object)) targets.push_back(o);
    }
    LOG_DEBUG("post notification %s -> %zu observers", name.c_str(), targets.size());
    for (auto& o : targets) {
        if (o.block) objc::call_block(o.block, {note});
        else objc::send_sel(o.observer, o.selector, {note});
    }
}

// ---- NSError / NSException ----
struct ErrorData : objc::HostData {
    id domain = 0, user_info = 0;
    s64 code = 0;
    ~ErrorData() override {
        objc::release(domain);
        objc::release(user_info);
    }
};
struct ExceptionData : objc::HostData {
    id name = 0, reason = 0, user_info = 0;
};

// ---- NSCharacterSet ----
struct CharsetData : objc::HostData {
    std::function<bool(char16_t)> pred;
};
id make_charset(std::function<bool(char16_t)> pred) {
    id c = objc::alloc(g_charset);
    objc::ensure<CharsetData>(c).pred = std::move(pred);
    return objc::autorelease(c);
}

// ---- NSURL ----
struct UrlData : objc::HostData {
    std::string s;
    bool file = false;
};
id make_url(std::string s, bool file) {
    id u = objc::alloc(g_url);
    auto& d = objc::ensure<UrlData>(u);
    d.s = std::move(s);
    d.file = file;
    return objc::autorelease(u);
}
std::string url_path(id u) {
    auto* d = objc::get<UrlData>(u);
    if (!d) return {};
    if (d->file) return d->s;
    std::string s = d->s;
    size_t scheme = s.find("://");
    if (scheme == std::string::npos) return s;
    size_t slash = s.find('/', scheme + 3);
    return slash == std::string::npos ? "" : s.substr(slash);
}

id search_path(u64 dir) {
    std::string home = vfs::kHomePath;
    switch (dir) {
    case 9: return str(home + "/Documents");
    case 5: return str(home + "/Library");
    case 13: return str(home + "/Library/Caches");
    case 14: return str(home + "/Library/Application Support");
    default:
        LOG_WARN("NSSearchPathForDirectoriesInDomains(%llu) unknown", (unsigned long long)dir);
        return str(home + "/Documents");
    }
}

}  // namespace

void install_system() {
    using objc::class_method;
    using objc::method;
    g_bundle = objc::host_class("NSBundle");
    g_filemgr = objc::host_class("NSFileManager");
    g_defaults = objc::host_class("NSUserDefaults");
    g_procinfo = objc::host_class("NSProcessInfo");
    g_locale = objc::host_class("NSLocale");
    g_timezone = objc::host_class("NSTimeZone");
    g_pool = objc::host_class("NSAutoreleasePool");
    g_center = objc::host_class("NSNotificationCenter");
    g_notification = objc::host_class("NSNotification");
    g_error = objc::host_class("NSError");
    g_exception = objc::host_class("NSException");
    g_charset = objc::host_class("NSCharacterSet");
    objc::host_class("NSMutableCharacterSet", "NSCharacterSet");
    g_url = objc::host_class("NSURL");

    // String-valued constants (keys, notification names, domains...) resolve to NSStrings of their own name.
    hle::add_resolver([](const std::string& sym) -> GuestAddr {
        static const char* suffixes[] = {"Key", "Notification", "NotificationName", "Domain", "Exception", "Scope",
                                         "AttributeName", "Code", "Mode", "Twitter"};
        static const char* prefixes[] = {"_kSec", "_AVAudioSessionCategory", "_AVLayerVideoGravity", "_kEAGL",
                                         "_NSGregorianCalendar", "_kCFBundle", "_NSFile", "_NSMetadataItem"};
        bool match = false;
        for (const char* s : suffixes) {
            size_t n = std::strlen(s);
            if (sym.size() > n && sym.compare(sym.size() - n, n, s) == 0) match = true;
        }
        for (const char* p : prefixes)
            if (sym.rfind(p, 0) == 0) match = true;
        if (!match || sym.size() < 2) return 0;
        auto* cell = static_cast<u64*>(hle::alloc_static(8));
        *cell = str_retained(sym.substr(1));
        return gaddr(cell);
    });
    static double s_foundation_version = 1144.17;
    hle::data("_NSFoundationVersionNumber", gaddr(&s_foundation_version));
    static u64 s_alloc_default = 0;
    hle::data("_kCFAllocatorDefault", gaddr(&s_alloc_default));
    hle::data("_kCFAllocatorSystemDefault", gaddr(&s_alloc_default));

    // ---- NSBundle ----
    Class B = g_bundle;
    class_method(B, "mainBundle", [](Class c, SEL) {
        if (!g_main_bundle) {
            g_main_bundle = objc::alloc(c);
            auto& d = objc::ensure<BundleData>(g_main_bundle);
            d.path = vfs::kBundlePath;
            d.info = plist_from_file(vfs::to_host((d.path + "/Info.plist").c_str()));
            if (!d.info) LOG_ERROR("could not read Info.plist");
        }
        return g_main_bundle;
    });
    class_method(B, "bundleForClass:", [](Class c, SEL, Class) { return objc::send(c, "mainBundle"); });
    class_method(B, "bundleWithPath:", [](Class c, SEL, id path) -> id {
        if (!fs::exists(host_path(path))) return 0;
        id b = objc::alloc(c);
        objc::ensure<BundleData>(b).path = utf8(path);
        return objc::autorelease(b);
    });
    class_method(B, "bundleWithIdentifier:", [](Class c, SEL, id) -> id { return 0; });
    method(B, "bundlePath", [](id self, SEL) { return str(objc::ensure<BundleData>(self).path); });
    method(B, "resourcePath", [](id self, SEL) { return str(objc::ensure<BundleData>(self).path); });
    method(B, "executablePath", [](id self, SEL) { return str(objc::ensure<BundleData>(self).path + "/SwordGame"); });
    method(B, "bundleURL", [](id self, SEL) { return make_url(objc::ensure<BundleData>(self).path, true); });
    method(B, "infoDictionary", [](id self, SEL) { return objc::ensure<BundleData>(self).info; });
    method(B, "localizedInfoDictionary", [](id self, SEL) { return objc::ensure<BundleData>(self).info; });
    method(B, "objectForInfoDictionaryKey:", [](id self, SEL, id key) { return dict_get(objc::ensure<BundleData>(self).info, key); });
    method(B, "bundleIdentifier", [](id self, SEL) { return dict_get(objc::ensure<BundleData>(self).info, str("CFBundleIdentifier")); });
    method(B, "preferredLocalizations", [](id, SEL) { return array({str("en")}); });
    method(B, "localizations", [](id, SEL) { return array({str("en")}); });
    method(B, "localizedStringForKey:value:table:", [](id, SEL, id key, id value, id) -> id {
        return value && objc::send(value, "length") ? value : key;
    });
    auto path_for = [](id self, id name, id type, id dir) -> id {
        std::string p = objc::ensure<BundleData>(self).path;
        if (dir && objc::send(dir, "length")) p += "/" + utf8(dir);
        std::string n = utf8(name);
        std::string t = utf8(type);
        if (!t.empty()) n += (t[0] == '.' ? "" : ".") + t;
        p += "/" + n;
        std::string host = vfs::to_host(p.c_str());
        if (host.empty() || !fs::exists(fs::path(libc::utf8_to_wide(host)))) {
            LOG_DEBUG("pathForResource: %s not found", p.c_str());
            return 0;
        }
        return str(p);
    };
    static decltype(path_for) s_path_for = path_for;
    method(B, "pathForResource:ofType:", [](id self, SEL, id name, id type) { return s_path_for(self, name, type, 0); });
    method(B, "pathForResource:ofType:inDirectory:", [](id self, SEL, id name, id type, id dir) { return s_path_for(self, name, type, dir); });
    method(B, "URLForResource:withExtension:", [](id self, SEL, id name, id ext) -> id {
        id p = s_path_for(self, name, ext, 0);
        return p ? make_url(utf8(p), true) : 0;
    });
    method(B, "pathsForResourcesOfType:inDirectory:", [](id self, SEL, id type, id dir) {
        std::string p = objc::ensure<BundleData>(self).path;
        if (dir && objc::send(dir, "length")) p += "/" + utf8(dir);
        std::string ext = "." + utf8(type);
        std::vector<id> out;
        std::error_code ec;
        for (auto& e : fs::directory_iterator(fs::path(libc::utf8_to_wide(vfs::to_host(p.c_str()))), ec)) {
            std::string fname = libc::wide_to_utf8(e.path().filename().wstring());
            if (utf8(type).empty() || (fname.size() > ext.size() && fname.compare(fname.size() - ext.size(), ext.size(), ext) == 0))
                out.push_back(str(p + "/" + fname));
        }
        return array(out);
    });
    method(B, "loadNibNamed:owner:options:", [](id, SEL, id name, id, id) -> id {
        LOG_WARN("loadNibNamed:%s ignored", utf8(name).c_str());
        return array({});
    });

    // ---- NSFileManager ----
    Class F = g_filemgr;
    class_method(F, "defaultManager", [](Class c, SEL) {
        if (!g_file_manager) g_file_manager = objc::alloc(c);
        return g_file_manager;
    });
    method(F, "fileExistsAtPath:", [](id, SEL, id path) {
        std::error_code ec;
        bool r = !vfs::to_host(utf8(path).c_str()).empty() && fs::exists(host_path(path), ec);
        LOG_DEBUG("fileExistsAtPath:%s = %d", utf8(path).c_str(), r);
        return r;
    });
    method(F, "fileExistsAtPath:isDirectory:", [](id, SEL, id path, u8* isdir) {
        std::error_code ec;
        if (vfs::to_host(utf8(path).c_str()).empty()) return false;
        auto p = host_path(path);
        bool r = fs::exists(p, ec);
        if (isdir) *isdir = r && fs::is_directory(p, ec);
        return r;
    });
    method(F, "isReadableFileAtPath:", [](id, SEL, id path) {
        std::error_code ec;
        return fs::exists(host_path(path), ec);
    });
    method(F, "isWritableFileAtPath:", [](id, SEL, id path) { return !vfs::to_host(utf8(path).c_str()).empty(); });
    method(F, "isDeletableFileAtPath:", [](id, SEL, id path) { return !vfs::to_host(utf8(path).c_str()).empty(); });
    method(F, "createDirectoryAtPath:withIntermediateDirectories:attributes:error:", [](id, SEL, id path, bool inter, id, u64* err) {
        std::error_code ec;
        if (err) *err = 0;
        if (vfs::to_host(utf8(path).c_str()).empty()) return false;
        if (inter) fs::create_directories(host_path(path), ec);
        else fs::create_directory(host_path(path), ec);
        return !ec;
    });
    method(F, "createDirectoryAtPath:attributes:", [](id, SEL, id path, id) {
        std::error_code ec;
        fs::create_directories(host_path(path), ec);
        return !ec;
    });
    method(F, "createFileAtPath:contents:attributes:", [](id, SEL, id path, id data, id) {
        std::string h = vfs::to_host(utf8(path).c_str());
        if (h.empty()) return false;
        std::ofstream f(std::filesystem::path(libc::utf8_to_wide(h)), std::ios::binary);
        auto b = data_bytes(data);
        f.write(reinterpret_cast<const char*>(b.data()), b.size());
        return (bool)f;
    });
    method(F, "contentsAtPath:", [](id, SEL, id path) { return objc::send(objc::class_named("NSData"), "dataWithContentsOfFile:", {path}); });
    method(F, "removeItemAtPath:error:", [](id, SEL, id path, u64* err) {
        std::error_code ec;
        if (err) *err = 0;
        if (vfs::to_host(utf8(path).c_str()).empty()) return false;
        return fs::remove_all(host_path(path), ec) > 0 && !ec;
    });
    method(F, "removeItemAtURL:error:", [](id, SEL, id url, u64* err) {
        std::error_code ec;
        if (err) *err = 0;
        return fs::remove_all(fs::path(libc::utf8_to_wide(vfs::to_host(url_path(url).c_str()))), ec) > 0;
    });
    method(F, "moveItemAtPath:toPath:error:", [](id, SEL, id a, id b, u64* err) {
        std::error_code ec;
        if (err) *err = 0;
        fs::rename(host_path(a), host_path(b), ec);
        return !ec;
    });
    method(F, "copyItemAtPath:toPath:error:", [](id, SEL, id a, id b, u64* err) {
        std::error_code ec;
        if (err) *err = 0;
        fs::copy(host_path(a), host_path(b), fs::copy_options::recursive | fs::copy_options::overwrite_existing, ec);
        return !ec;
    });
    method(F, "contentsOfDirectoryAtPath:error:", [](id, SEL, id path, u64* err) -> id {
        std::error_code ec;
        if (err) *err = 0;
        std::vector<id> out;
        for (auto& e : fs::directory_iterator(host_path(path), ec)) out.push_back(str(libc::wide_to_utf8(e.path().filename().wstring())));
        if (ec) return 0;
        return array(out);
    });
    method(F, "directoryContentsAtPath:", [](id self, SEL, id path) { return objc::send(self, "contentsOfDirectoryAtPath:error:", {path, 0}); });
    method(F, "attributesOfItemAtPath:error:", [](id, SEL, id path, u64* err) -> id {
        std::error_code ec;
        if (err) *err = 0;
        auto p = host_path(path);
        if (!fs::exists(p, ec)) return 0;
        bool dir = fs::is_directory(p, ec);
        u64 size = dir ? 0 : fs::file_size(p, ec);
        return dict({{str("NSFileSize"), number_uint(size)},
                     {str("NSFileType"), str(dir ? "NSFileTypeDirectory" : "NSFileTypeRegular")},
                     {str("NSFileModificationDate"), objc::send(objc::class_named("NSDate"), "date")}});
    });
    method(F, "fileAttributesAtPath:traverseLink:", [](id self, SEL, id path, bool) {
        return objc::send(self, "attributesOfItemAtPath:error:", {path, 0});
    });
    method(F, "attributesOfFileSystemForPath:error:", [](id, SEL, id, u64* err) {
        if (err) *err = 0;
        std::error_code ec;
        auto space = std::filesystem::space(std::filesystem::path(libc::utf8_to_wide(vfs::host_home())), ec);
        // Report like a 64 GB device so size checks behave.
        u64 f = std::min<u64>(ec ? 0 : space.available, 32ull << 30);
        return dict({{str("NSFileSystemFreeSize"), number_uint(f)}, {str("NSFileSystemSize"), number_uint(64ull << 30)}});
    });
    method(F, "setAttributes:ofItemAtPath:error:", [](id, SEL, id, id, u64* err) {
        if (err) *err = 0;
        return true;
    });
    method(F, "URLsForDirectory:inDomains:", [](id, SEL, u64 dir, u64) { return array({make_url(utf8(search_path(dir)), true)}); });
    method(F, "URLForUbiquityContainerIdentifier:", [](id, SEL, id) -> id { return 0; });  // no iCloud
    method(F, "ubiquityIdentityToken", [](id, SEL) -> id { return 0; });
    method(F, "currentDirectoryPath", [](id, SEL) { return str(vfs::kBundlePath); });
    method(F, "changeCurrentDirectoryPath:", [](id, SEL, id path) {
        vfs::set_cwd(utf8(path));
        return true;
    });
    method(F, "fileSystemRepresentationWithPath:", [](id, SEL, id path) { return objc::send(path, "UTF8String"); });
    method(F, "stringWithFileSystemRepresentation:length:", [](id, SEL, const char* p, u64 n) -> id {
        return p ? str(std::string_view(p, n)) : 0;
    });
    method(F, "displayNameAtPath:", [](id, SEL, id path) { return objc::send(path, "lastPathComponent"); });

    // ---- NSUserDefaults ----
    Class U = g_defaults;
    class_method(U, "standardUserDefaults", [](Class c, SEL) {
        if (!g_std_defaults) {
            g_std_defaults = objc::alloc(c);
            id loaded = plist_from_file(defaults_file());
            g_defaults_dict = objc::retain(mutable_dict());
            if (loaded) {
                for (auto& [k, v] : dict_items(loaded)) dict_set(g_defaults_dict, k, v);
                objc::release(loaded);
            }
        }
        return g_std_defaults;
    });
    class_method(U, "resetStandardUserDefaults", [](Class, SEL) {});
    method(U, "objectForKey:", [](id, SEL, id k) { return defaults_get(k); });
    method(U, "stringForKey:", [](id, SEL, id k) -> id {
        id v = defaults_get(k);
        return is_string(v) ? v : is_number(v) ? objc::send(v, "stringValue") : 0;
    });
    method(U, "arrayForKey:", [](id, SEL, id k) -> id {
        id v = defaults_get(k);
        return is_array(v) ? v : 0;
    });
    method(U, "dictionaryForKey:", [](id, SEL, id k) -> id {
        id v = defaults_get(k);
        return is_dict(v) ? v : 0;
    });
    method(U, "dataForKey:", [](id, SEL, id k) -> id {
        id v = defaults_get(k);
        return is_data(v) ? v : 0;
    });
    method(U, "integerForKey:", [](id, SEL, id k) -> s64 { return number_int_value(defaults_get(k)); });
    method(U, "boolForKey:", [](id, SEL, id k) { return number_int_value(defaults_get(k)) != 0; });
    method(U, "floatForKey:", [](id, SEL, id k) { return (float)number_double_value(defaults_get(k)); });
    method(U, "doubleForKey:", [](id, SEL, id k) { return number_double_value(defaults_get(k)); });
    method(U, "setObject:forKey:", [](id, SEL, id v, id k) { defaults_set(k, v); });
    method(U, "setValue:forKey:", [](id, SEL, id v, id k) { defaults_set(k, v); });
    method(U, "setInteger:forKey:", [](id, SEL, s64 v, id k) { defaults_set(k, number_int(v)); });
    method(U, "setBool:forKey:", [](id, SEL, bool v, id k) { defaults_set(k, number_bool(v)); });
    method(U, "setFloat:forKey:", [](id, SEL, float v, id k) { defaults_set(k, number_double(v)); });
    method(U, "setDouble:forKey:", [](id, SEL, double v, id k) { defaults_set(k, number_double(v)); });
    method(U, "removeObjectForKey:", [](id, SEL, id k) { defaults_set(k, 0); });
    method(U, "synchronize", [](id, SEL) {
        defaults_save();
        return true;
    });
    method(U, "registerDefaults:", [](id, SEL, id d) {
        std::lock_guard lock(g_defaults_mutex);
        if (!g_registered) g_registered = objc::retain(mutable_dict());
        for (auto& [k, v] : dict_items(d)) dict_set(g_registered, k, v);
    });
    method(U, "persistentDomainForName:", [](id, SEL, id) -> id { return 0; });
    method(U, "dictionaryRepresentation", [](id, SEL) {
        std::lock_guard lock(g_defaults_mutex);
        return g_defaults_dict;
    });

    // ---- NSProcessInfo ----
    Class P = g_procinfo;
    class_method(P, "processInfo", [](Class c, SEL) {
        if (!g_process_info) g_process_info = objc::alloc(c);
        return g_process_info;
    });
    method(P, "processName", [](id, SEL) { return str("SwordGame"); });
    method(P, "processIdentifier", [](id, SEL) -> s32 { return 1234; });
    method(P, "arguments", [](id, SEL) { return array({str(std::string(vfs::kBundlePath) + "/SwordGame")}); });
    method(P, "environment", [](id, SEL) { return dict({}); });
    method(P, "globallyUniqueString", [](id, SEL) {
        GUID g;
        CoCreateGuid(&g);
        char buf[64];
        snprintf(buf, sizeof buf, "%08lX-%04X-%04X-%02X%02X-%02X%02X%02X%02X%02X%02X", g.Data1, g.Data2, g.Data3, g.Data4[0], g.Data4[1],
                 g.Data4[2], g.Data4[3], g.Data4[4], g.Data4[5], g.Data4[6], g.Data4[7]);
        return str(buf);
    });
    method(P, "operatingSystemVersionString", [](id, SEL) { return str("Version 8.2 (Build 12D508)"); });
    method(P, "processorCount", [](id, SEL) -> u64 { return 2; });
    method(P, "activeProcessorCount", [](id, SEL) -> u64 { return 2; });
    method(P, "physicalMemory", [](id, SEL) -> u64 { return 1ull << 30; });
    method(P, "systemUptime", [](id, SEL) { return (double)GetTickCount64() / 1000.0; });
    method(P, "isOperatingSystemAtLeastVersion:", [](id, SEL, const s64* v) { return v[0] < 8 || (v[0] == 8 && v[1] <= 2); });

    // ---- NSLocale / NSTimeZone ----
    Class L = g_locale;
    class_method(L, "currentLocale", [](Class c, SEL) {
        if (!g_current_locale) g_current_locale = objc::alloc(c);
        return g_current_locale;
    });
    class_method(L, "systemLocale", [](Class c, SEL) { return objc::send(c, "currentLocale"); });
    class_method(L, "autoupdatingCurrentLocale", [](Class c, SEL) { return objc::send(c, "currentLocale"); });
    class_method(L, "localeWithLocaleIdentifier:", [](Class c, SEL, id) { return objc::send(c, "currentLocale"); });
    class_method(L, "preferredLanguages", [](Class, SEL) { return array({str("en")}); });
    method(L, "initWithLocaleIdentifier:", [](id self, SEL, id) { return self; });
    method(L, "localeIdentifier", [](id, SEL) { return str("en_US"); });
    method(L, "objectForKey:", [](id, SEL, id key) -> id {
        std::string k = utf8(key);
        if (k == "NSLocaleLanguageCode" || k == "kCFLocaleLanguageCodeKey") return str("en");
        if (k == "NSLocaleCountryCode" || k == "kCFLocaleCountryCodeKey") return str("US");
        if (k == "NSLocaleCurrencyCode") return str("USD");
        if (k == "NSLocaleIdentifier") return str("en_US");
        return 0;
    });
    method(L, "displayNameForKey:value:", [](id, SEL, id, id v) { return v; });
    Class TZ = g_timezone;
    auto tz = [](Class c, SEL) {
        if (!g_tz) g_tz = objc::alloc(c);
        return g_tz;
    };
    class_method(TZ, "systemTimeZone", tz);
    class_method(TZ, "localTimeZone", tz);
    class_method(TZ, "defaultTimeZone", tz);
    class_method(TZ, "timeZoneWithName:", tz);
    class_method(TZ, "timeZoneWithAbbreviation:", tz);
    class_method(TZ, "timeZoneForSecondsFromGMT:", tz);
    method(TZ, "secondsFromGMT", [](id, SEL) -> s64 {
#ifdef _WIN32
        long t;
        _get_timezone(&t);
        return -t;
#else
        time_t now = time(nullptr);
        std::tm tm{};
        localtime_r(&now, &tm);
        return tm.tm_gmtoff;
#endif
    });
    method(TZ, "name", [](id, SEL) { return str("America/New_York"); });
    method(TZ, "abbreviation", [](id, SEL) { return str("EST"); });

    // ---- NSAutoreleasePool ----
    struct PoolData : objc::HostData {
        u64 token = 0;
    };
    method(g_pool, "init", [](id self, SEL) {
        objc::ensure<PoolData>(self).token = objc::pool_push();
        return self;
    });
    auto drain = [](id self, SEL) {
        objc::pool_pop(objc::ensure<PoolData>(self).token);
        objc::destroy(self);
    };
    method(g_pool, "drain", drain);
    method(g_pool, "release", drain);
    method(g_pool, "autorelease", [](id self, SEL) { return self; });
    method(g_pool, "retain", [](id self, SEL) { return self; });

    // ---- NSNotificationCenter / NSNotification ----
    Class C = g_center;
    class_method(C, "defaultCenter", [](Class c, SEL) {
        if (!g_default_center) g_default_center = objc::alloc(c);
        return g_default_center;
    });
    method(C, "addObserver:selector:name:object:", [](id, SEL, id obs, SEL s, id name, id object) {
        std::lock_guard lock(g_obs_mutex);
        g_observers.push_back({obs, s, 0, utf8(name), object});
    });
    method(C, "addObserverForName:object:queue:usingBlock:", [](id, SEL, id name, id object, id, GuestAddr block) {
        id token = objc::alloc(objc::class_named("NSObject"));
        std::lock_guard lock(g_obs_mutex);
        g_observers.push_back({token, 0, objc::block_copy(block), utf8(name), object});
        return objc::autorelease(token);
    });
    method(C, "removeObserver:", [](id, SEL, id obs) {
        std::lock_guard lock(g_obs_mutex);
        std::erase_if(g_observers, [obs](const Observer& o) { return o.observer == obs; });
    });
    method(C, "removeObserver:name:object:", [](id, SEL, id obs, id name, id object) {
        std::string n = utf8(name);
        std::lock_guard lock(g_obs_mutex);
        std::erase_if(g_observers, [&](const Observer& o) {
            return o.observer == obs && (n.empty() || o.name == n) && (!object || o.object == object);
        });
    });
    method(C, "postNotification:", [](id, SEL, id note) { post_note(note); });
    method(C, "postNotificationName:object:", [](id, SEL, id name, id object) { post_note(make_note(name, object, 0)); });
    method(C, "postNotificationName:object:userInfo:", [](id, SEL, id name, id object, id info) {
        post_note(make_note(name, object, info));
    });
    Class N = g_notification;
    class_method(N, "notificationWithName:object:", [](Class, SEL, id name, id object) { return make_note(name, object, 0); });
    class_method(N, "notificationWithName:object:userInfo:", [](Class, SEL, id name, id object, id info) { return make_note(name, object, info); });
    method(N, "name", [](id self, SEL) { return objc::ensure<NoteData>(self).name; });
    method(N, "object", [](id self, SEL) { return objc::ensure<NoteData>(self).object; });
    method(N, "userInfo", [](id self, SEL) { return objc::ensure<NoteData>(self).user_info; });

    // ---- NSError ----
    Class E = g_error;
    auto make_error = [](Class c, id domain, s64 code, id info) {
        id e = objc::alloc(c);
        auto& d = objc::ensure<ErrorData>(e);
        d.domain = objc::retain(domain);
        d.code = code;
        d.user_info = objc::retain(info);
        return e;
    };
    static decltype(make_error) s_make_error = make_error;
    class_method(E, "errorWithDomain:code:userInfo:", [](Class c, SEL, id domain, s64 code, id info) {
        return objc::autorelease(s_make_error(c, domain, code, info));
    });
    method(E, "initWithDomain:code:userInfo:", [](id self, SEL, id domain, s64 code, id info) {
        auto& d = objc::ensure<ErrorData>(self);
        d.domain = objc::retain(domain);
        d.code = code;
        d.user_info = objc::retain(info);
        return self;
    });
    method(E, "domain", [](id self, SEL) { return objc::ensure<ErrorData>(self).domain; });
    method(E, "code", [](id self, SEL) -> s64 { return objc::ensure<ErrorData>(self).code; });
    method(E, "userInfo", [](id self, SEL) { return objc::ensure<ErrorData>(self).user_info; });
    method(E, "localizedDescription", [](id self, SEL) {
        auto& d = objc::ensure<ErrorData>(self);
        id desc = dict_get(d.user_info, str("NSLocalizedDescription"));
        return desc ? desc : str(utf8(d.domain) + " error " + std::to_string(d.code));
    });
    method(E, "description", [](id self, SEL) { return objc::send(self, "localizedDescription"); });

    // ---- NSException ----
    Class X = g_exception;
    class_method(X, "exceptionWithName:reason:userInfo:", [](Class c, SEL, id name, id reason, id info) {
        id e = objc::alloc(c);
        auto& d = objc::ensure<ExceptionData>(e);
        d.name = objc::retain(name);
        d.reason = objc::retain(reason);
        d.user_info = objc::retain(info);
        return objc::autorelease(e);
    });
    method(X, "initWithName:reason:userInfo:", [](id self, SEL, id name, id reason, id info) {
        auto& d = objc::ensure<ExceptionData>(self);
        d.name = objc::retain(name);
        d.reason = objc::retain(reason);
        d.user_info = objc::retain(info);
        return self;
    });
    objc::add_method(X, "raise:format:", [](cpu::Thread& t) {
        std::string fmt = utf8(t.x(3));
        libc::VaList v{t.sp()};
        fatal("+[NSException raise:%s format:] %s\n%s", utf8(t.x(2)).c_str(), libc::format(fmt.c_str(), v, true).c_str(),
              t.backtrace().c_str());
    }, true);
    method(X, "name", [](id self, SEL) { return objc::ensure<ExceptionData>(self).name; });
    method(X, "reason", [](id self, SEL) { return objc::ensure<ExceptionData>(self).reason; });
    method(X, "userInfo", [](id self, SEL) { return objc::ensure<ExceptionData>(self).user_info; });
    method(X, "callStackSymbols", [](id, SEL) { return array({}); });
    method(X, "raise", [](cpu::Thread& t, id self, SEL) {
        fatal("NSException raised: %s: %s\n%s", objc::describe(objc::ensure<ExceptionData>(self).name).c_str(),
              objc::describe(objc::ensure<ExceptionData>(self).reason).c_str(), t.backtrace().c_str());
    });
    Class AH = objc::host_class("NSAssertionHandler");
    class_method(AH, "currentHandler", [](Class c, SEL) {
        static id h = objc::alloc(c);
        return h;
    });
    objc::add_method(AH, "handleFailureInMethod:object:file:lineNumber:description:", [](cpu::Thread& t) {
        fatal("NSAssert failed in %s (%s:%lld): %s\n%s", objc::sel_name(t.x(2)), utf8(t.x(4)).c_str(), (long long)t.x(5),
              utf8(t.x(6)).c_str(), t.backtrace().c_str());
    });
    objc::add_method(AH, "handleFailureInFunction:file:lineNumber:description:", [](cpu::Thread& t) {
        fatal("NSCAssert failed in %s (%s:%lld): %s\n%s", utf8(t.x(2)).c_str(), utf8(t.x(3)).c_str(), (long long)t.x(4),
              utf8(t.x(5)).c_str(), t.backtrace().c_str());
    });

    // ---- NSCharacterSet ----
    Class CS = g_charset;
    class_method(CS, "whitespaceCharacterSet", [](Class, SEL) { return make_charset([](char16_t c) { return c == ' ' || c == '\t'; }); });
    class_method(CS, "whitespaceAndNewlineCharacterSet", [](Class, SEL) {
        return make_charset([](char16_t c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == 0x85; });
    });
    class_method(CS, "newlineCharacterSet", [](Class, SEL) { return make_charset([](char16_t c) { return c == '\n' || c == '\r'; }); });
    class_method(CS, "decimalDigitCharacterSet", [](Class, SEL) { return make_charset([](char16_t c) { return c >= '0' && c <= '9'; }); });
    class_method(CS, "alphanumericCharacterSet", [](Class, SEL) { return make_charset([](char16_t c) { return c < 128 && isalnum(c); }); });
    class_method(CS, "letterCharacterSet", [](Class, SEL) { return make_charset([](char16_t c) { return c < 128 && isalpha(c); }); });
    class_method(CS, "punctuationCharacterSet", [](Class, SEL) { return make_charset([](char16_t c) { return c < 128 && ispunct(c); }); });
    class_method(CS, "URLQueryAllowedCharacterSet", [](Class, SEL) {
        return make_charset([](char16_t c) { return c < 128 && (isalnum(c) || std::strchr("-._~!$&'()*+,;=:@/?", c)); });
    });
    class_method(CS, "characterSetWithCharactersInString:", [](Class, SEL, id s) {
        std::u16string chars = utf16(s);
        return make_charset([chars](char16_t c) { return chars.find(c) != std::u16string::npos; });
    });
    method(CS, "characterIsMember:", [](id self, SEL, u16 c) {
        auto* d = objc::get<CharsetData>(self);
        return d && d->pred && d->pred(c);
    });
    method(CS, "invertedSet", [](id self, SEL) {
        auto* d = objc::get<CharsetData>(self);
        auto pred = d ? d->pred : std::function<bool(char16_t)>([](char16_t) { return false; });
        return make_charset([pred](char16_t c) { return !pred(c); });
    });

    // ---- NSURL ----
    Class URL = g_url;
    class_method(URL, "fileURLWithPath:", [](Class, SEL, id p) { return make_url(utf8(p), true); });
    class_method(URL, "fileURLWithPath:isDirectory:", [](Class, SEL, id p, bool) { return make_url(utf8(p), true); });
    class_method(URL, "URLWithString:", [](Class, SEL, id s) -> id {
        if (!s) return 0;
        std::string u = utf8(s);
        if (u.rfind("file://", 0) == 0) return make_url(u.substr(7), true);
        return make_url(u, false);
    });
    method(URL, "initWithString:", [](id self, SEL, id s) {
        auto& d = objc::ensure<UrlData>(self);
        d.s = utf8(s);
        return self;
    });
    method(URL, "initFileURLWithPath:", [](id self, SEL, id s) {
        auto& d = objc::ensure<UrlData>(self);
        d.s = utf8(s);
        d.file = true;
        return self;
    });
    method(URL, "path", [](id self, SEL) { return str(url_path(self)); });
    method(URL, "isFileURL", [](id self, SEL) { return objc::ensure<UrlData>(self).file; });
    method(URL, "absoluteString", [](id self, SEL) {
        auto& d = objc::ensure<UrlData>(self);
        return str(d.file ? "file://" + d.s : d.s);
    });
    method(URL, "description", [](id self, SEL) { return objc::send(self, "absoluteString"); });
    method(URL, "absoluteURL", [](id self, SEL) { return self; });
    method(URL, "scheme", [](id self, SEL) {
        auto& d = objc::ensure<UrlData>(self);
        if (d.file) return str("file");
        size_t c = d.s.find(':');
        return str(c == std::string::npos ? "" : d.s.substr(0, c));
    });
    method(URL, "host", [](id self, SEL) -> id {
        auto& d = objc::ensure<UrlData>(self);
        size_t s = d.s.find("://");
        if (s == std::string::npos) return 0;
        size_t e = d.s.find_first_of("/:?", s + 3);
        return str(d.s.substr(s + 3, e == std::string::npos ? std::string::npos : e - s - 3));
    });
    method(URL, "query", [](id self, SEL) -> id {
        auto& d = objc::ensure<UrlData>(self);
        size_t q = d.s.find('?');
        return q == std::string::npos ? 0 : str(d.s.substr(q + 1));
    });
    method(URL, "lastPathComponent", [](id self, SEL) { return objc::send(str(url_path(self)), "lastPathComponent"); });
    method(URL, "pathExtension", [](id self, SEL) { return objc::send(str(url_path(self)), "pathExtension"); });
    method(URL, "URLByAppendingPathComponent:", [](id self, SEL, id c) {
        auto& d = objc::ensure<UrlData>(self);
        std::string s = d.s;
        if (!s.empty() && s.back() != '/') s += "/";
        return make_url(s + utf8(c), d.file);
    });
    method(URL, "URLByDeletingLastPathComponent", [](id self, SEL) {
        auto& d = objc::ensure<UrlData>(self);
        return make_url(utf8(objc::send(str(d.s), "stringByDeletingLastPathComponent")), d.file);
    });
    method(URL, "setResourceValue:forKey:error:", [](id, SEL, id, id, u64* err) {
        if (err) *err = 0;
        return true;
    });
    method(URL, "isEqual:", [](id self, SEL, id o) {
        auto* a = objc::get<UrlData>(self);
        auto* b = objc::get<UrlData>(o);
        return a && b && a->s == b->s;
    });
    method(URL, "copyWithZone:", [](id self, SEL, u64) { return objc::retain(self); });

    // ---- C functions ----
    using hle::fn;
    hle::raw("_NSLog", [](cpu::Thread& t) {
        std::string fmt = utf8(t.x(0));
        libc::VaList v{t.sp()};
        LOG_INFO("[NSLog] %s", libc::format(fmt.c_str(), v, true).c_str());
    });
    fn("_NSSearchPathForDirectoriesInDomains", [](u64 dir, u64, bool) { return array({search_path(dir)}); });
    fn("_NSTemporaryDirectory", []() { return str(std::string(vfs::kHomePath) + "/tmp/"); });
    fn("_NSClassFromString", [](id s) -> Class { return s ? objc::class_named(utf8(s)) : 0; });
    fn("_NSStringFromClass", [](Class c) -> id { return c ? str(objc::class_name(c)) : 0; });
    fn("_NSSelectorFromString", [](id s) -> SEL { return s ? objc::sel(utf8(s)) : 0; });
    fn("_NSStringFromSelector", [](SEL s) -> id { return s ? str(objc::sel_name(s)) : 0; });
    fn("_NSSetUncaughtExceptionHandler", [](u64) {});
}

}  // namespace ns
