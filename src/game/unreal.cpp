#include "game/unreal.h"
#include "hle.h"
#include "macho.h"
#include <map>
#include <unordered_map>

namespace ue {

namespace {

GuestAddr g_fname_ctor = 0;     // FName::FName(const char*, EFindName)
GuestAddr g_name_tostring = 0;  // FName::ToString() const -> FString (x8)
GuestAddr g_app_free = 0;       // appFree(void*)
GuestAddr g_find_property = 0;  // FindField<UProperty>(UStruct*, const wchar_t*)
GuestAddr g_find_function = 0;  // UObject::FindFunction(FName, UBOOL) const
GuestAddr g_engine = 0;         // &GEngine

// UProperty::Offset, located at runtime (see locate_offset_field).
int g_offset_field = -1;

std::unordered_map<std::string, u64> g_names;
std::unordered_map<u64, std::string> g_name_strings;
std::map<std::pair<GuestAddr, std::string>, int> g_offsets;  // (class, property) -> offset

GuestAddr find_property(cpu::Thread& t, GuestAddr cls, const std::string& name) {
    return t.call(g_find_property, {cls, gaddr(wide(name))});
}

// UObject's script-visible natives ("Outer" at 0x40, "Class" at 0x50) give away where
// UProperty keeps its Offset.
bool locate_offset_field(cpu::Thread& t, GuestAddr cls) {
    GuestAddr outer = find_property(t, cls, "Outer"), klass = find_property(t, cls, "Class");
    if (!outer || !klass) return false;
    for (int off = 0x60; off < 0x100; off += 4) {
        if (*gptr<u32>(outer + off) == 0x40 && *gptr<u32>(klass + off) == kObjClass) {
            g_offset_field = off;
            LOG_INFO("unreal: UProperty::Offset at +0x%x", off);
            return true;
        }
    }
    LOG_WARN("unreal: could not locate UProperty::Offset");
    return false;
}

GuestAddr super_class(cpu::Thread& t, GuestAddr cls) {
    // UStruct::GetInheritanceSuper (vtable slot used by FindField).
    GuestAddr vt = *gptr<u64>(cls);
    return t.call(*gptr<u64>(vt + 0x260), {cls});
}

}  // namespace

const u32* wide(const std::string& s) {
    static std::unordered_map<std::string, const u32*> cache;
    auto it = cache.find(s);
    if (it != cache.end()) return it->second;
    auto* w = static_cast<u32*>(hle::alloc_static((s.size() + 1) * 4, 8));
    for (size_t i = 0; i < s.size(); i++) w[i] = (u8)s[i];
    w[s.size()] = 0;
    return cache[s] = w;
}

bool init(const macho::Image& img) {
    g_fname_ctor = img.find("__ZN5FNameC1EPKc9EFindName");
    g_name_tostring = img.find("__ZNK5FName8ToStringEv");
    g_app_free = img.find("__Z7appFreePv");
    g_find_property = img.find("__Z9FindFieldI9UPropertyEPT_P7UStructPKw");
    g_find_function = img.find("__ZNK7UObject12FindFunctionE5FNamej");
    g_engine = img.find("_GEngine");
    bool ok = g_fname_ctor && g_name_tostring && g_app_free && g_find_property && g_find_function && g_engine;
    if (!ok)
        LOG_WARN("unreal: missing symbols (fname %d tostring %d free %d findfield %d findfunc %d engine %d)",
                 !!g_fname_ctor, !!g_name_tostring, !!g_app_free, !!g_find_property, !!g_find_function, !!g_engine);
    return ok;
}

u64 fname(cpu::Thread& t, const std::string& name) {
    auto it = g_names.find(name);
    if (it != g_names.end()) return it->second;
    auto* buf = static_cast<u64*>(hle::alloc_static(8));
    t.call(g_fname_ctor, {gaddr(buf), gaddr(hle::static_cstr(name)), 1 /* FNAME_Add */});
    g_names[name] = *buf;
    return *buf;
}

std::string read_fstring(GuestAddr fstring) {
    auto* s = gptr<FString>(fstring);
    std::string out;
    if (!s->data) return out;
    for (int i = 0; i < s->num && i < 1024; i++) {
        u32 c = gptr<u32>(s->data)[i];
        if (!c) break;
        out += c < 0x80 ? (char)c : '?';
    }
    return out;
}

std::string name_string(cpu::Thread& t, u64 name) {
    auto it = g_name_strings.find(name);
    if (it != g_name_strings.end()) return it->second;
    u64 fname_copy = name;
    FString result{};
    t.call_raw(g_name_tostring, [&](cpu::Thread& c) {
        c.set_x(0, gaddr(&fname_copy));
        c.set_x(8, gaddr(&result));
    });
    std::string s = read_fstring(gaddr(&result));
    if (result.data) t.call(g_app_free, {result.data});
    return g_name_strings[name] = s;
}

std::string object_name(cpu::Thread& t, GuestAddr obj) {
    return obj ? name_string(t, *gptr<u64>(obj + kObjName)) : "None";
}

std::string class_name(cpu::Thread& t, GuestAddr obj) {
    return obj ? object_name(t, *gptr<u64>(obj + kObjClass)) : "None";
}

bool is_a(cpu::Thread& t, GuestAddr obj, const std::string& name) {
    if (!obj) return false;
    for (GuestAddr cls = *gptr<u64>(obj + kObjClass); cls; cls = super_class(t, cls))
        if (object_name(t, cls) == name) return true;
    return false;
}

int property_offset(cpu::Thread& t, GuestAddr obj, const char* name) {
    GuestAddr cls = *gptr<u64>(obj + kObjClass);
    auto key = std::make_pair(cls, std::string(name));
    auto it = g_offsets.find(key);
    if (it != g_offsets.end()) return it->second;
    if (g_offset_field < 0 && !locate_offset_field(t, cls)) return -1;
    GuestAddr prop = find_property(t, cls, name);
    int off = prop ? (int)*gptr<u32>(prop + g_offset_field) : -1;
    LOG_DEBUG("unreal: %s.%s at %d", object_name(t, cls).c_str(), name, off);
    return g_offsets[key] = off;
}

bool call_event(cpu::Thread& t, GuestAddr obj, const std::string& func, void* params) {
    GuestAddr fn = t.call(g_find_function, {obj, fname(t, func), 0});
    if (!fn) return false;
    GuestAddr vt = *gptr<u64>(obj);
    t.call(*gptr<u64>(vt + 0x200), {obj, fn, gaddr(params), 0});  // UObject::ProcessEvent
    return true;
}

void dump_properties(cpu::Thread& t, GuestAddr obj, const char* filter) {
    if (!obj) return;
    if (g_offset_field < 0 && !locate_offset_field(t, *gptr<u64>(obj + kObjClass))) return;
    LOG_INFO("properties of %s (%s):", object_name(t, obj).c_str(), class_name(t, obj).c_str());
    for (GuestAddr cls = *gptr<u64>(obj + kObjClass); cls; cls = super_class(t, cls)) {
        std::string cname = object_name(t, cls);
        if (cname == "Object") break;
        for (GuestAddr f = *gptr<u64>(cls + kStructChildren); f; f = *gptr<u64>(f + kFieldNext)) {
            std::string type = class_name(t, f);
            if (type.find("Property") == std::string::npos) continue;  // functions, enums, consts...
            std::string name = object_name(t, f);
            if (filter && name.find(filter) == std::string::npos && type.find(filter) == std::string::npos) continue;
            LOG_INFO("  %-18s +0x%-5x %-28s %s", cname.c_str(), *gptr<u32>(f + g_offset_field), name.c_str(), type.c_str());
        }
    }
}

GuestAddr player_input(cpu::Thread& t) {
    GuestAddr engine = *gptr<u64>(g_engine);
    TArray<u64> players{};
    if (!read_property(t, engine, "GamePlayers", players) || players.num < 1) return 0;
    GuestAddr local_player = players.at(0), controller = 0, input = 0;
    if (!read_property(t, local_player, "Actor", controller) || !controller) return 0;
    read_property(t, controller, "PlayerInput", input);
    return input;
}

}  // namespace ue
