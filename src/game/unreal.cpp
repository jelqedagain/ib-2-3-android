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
GuestAddr g_app_malloc = 0;
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
    g_app_malloc = img.find("__Z9appMallocjj");
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

// UBoolProperty::BitMask: script bools share a 32-bit word, each a bit of it. Its place in the property
// object is found once from two of Actor's bools that share a word (bStatic, bHidden): the first field
// after the offset where both hold a single, different bit.
int g_bitmask_field = -1;

bool bool_property(cpu::Thread& t, GuestAddr obj, const char* name, int& offset, u32& mask) {
    GuestAddr cls = obj ? *gptr<u64>(obj + kObjClass) : 0;
    if (!cls || (g_offset_field < 0 && !locate_offset_field(t, cls))) return false;
    if (g_bitmask_field < 0) {
        GuestAddr a = find_property(t, cls, "bStatic"), b = find_property(t, cls, "bHidden");
        if (!a || !b || *gptr<u32>(a + g_offset_field) != *gptr<u32>(b + g_offset_field)) return false;
        auto bit = [](u32 v) { return v && !(v & (v - 1)); };
        for (int off = g_offset_field + 4; off < g_offset_field + 0x80; off += 4) {
            u32 ma = *gptr<u32>(a + off), mb = *gptr<u32>(b + off);
            if (bit(ma) && bit(mb) && ma != mb) {
                g_bitmask_field = off;
                LOG_INFO("unreal: UBoolProperty::BitMask at +0x%x", off);
                break;
            }
        }
        if (g_bitmask_field < 0) {
            LOG_WARN("unreal: could not locate UBoolProperty::BitMask");
            return false;
        }
    }
    GuestAddr prop = find_property(t, cls, name);
    if (!prop) return false;
    offset = (int)*gptr<u32>(prop + g_offset_field);
    mask = *gptr<u32>(prop + g_bitmask_field);
    return mask != 0;
}

bool read_bool(cpu::Thread& t, GuestAddr obj, const char* name, bool& out) {
    int off;
    u32 mask;
    if (!bool_property(t, obj, name, off, mask)) return false;
    out = (*gptr<u32>(obj + off) & mask) != 0;
    return true;
}

bool write_bool(cpu::Thread& t, GuestAddr obj, const char* name, bool value) {
    int off;
    u32 mask;
    if (!bool_property(t, obj, name, off, mask)) return false;
    u32& word = *gptr<u32>(obj + off);
    word = value ? word | mask : word & ~mask;
    return true;
}

int param_offset(cpu::Thread& t, GuestAddr obj, const std::string& func, const char* param) {
    GuestAddr fn = t.call(g_find_function, {obj, fname(t, func), 0});
    if (!fn) return -1;
    if (g_offset_field < 0 && !locate_offset_field(t, *gptr<u64>(obj + kObjClass))) return -1;
    GuestAddr prop = find_property(t, fn, param);
    return prop ? (int)*gptr<u32>(prop + g_offset_field) : -1;
}

FString make_fstring(cpu::Thread& t, const std::string& s) {
    FString out{0, (s32)s.size() + 1, (s32)s.size() + 1};
    if (!g_app_malloc) return {};
    out.data = t.call(g_app_malloc, {(u64)out.num * 4, 8});
    u32* w = gptr<u32>(out.data);
    for (size_t i = 0; i < s.size(); i++) w[i] = (unsigned char)s[i];
    w[s.size()] = 0;
    return out;
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

namespace {

// UStructProperty::Struct: the first pointer after UProperty::Offset that points at a ScriptStruct.
// Located once, on a property known to be a struct; only pointers near the property itself (the
// same heap) are followed.
int g_struct_field = -1;

GuestAddr struct_of(cpu::Thread& t, GuestAddr prop) {
    if (g_struct_field < 0) {
        for (int off = (g_offset_field + 4 + 7) & ~7; off < g_offset_field + 0x80; off += 8) {
            GuestAddr p = *gptr<u64>(prop + off);
            if (!p || (p & 7) || (p > prop ? p - prop : prop - p) > (1ull << 32)) continue;
            if (class_name(t, p) == "ScriptStruct") {
                g_struct_field = off;
                LOG_INFO("unreal: UStructProperty::Struct at +0x%x", off);
                break;
            }
        }
        if (g_struct_field < 0) return 0;
    }
    return *gptr<u64>(prop + g_struct_field);
}

std::string value_string(cpu::Thread& t, const std::string& type, GuestAddr at) {
    char v[160] = "";
    if (type == "IntProperty") snprintf(v, sizeof v, "%d", *gptr<s32>(at));
    else if (type == "FloatProperty") snprintf(v, sizeof v, "%g", *gptr<float>(at));
    else if (type == "ByteProperty") snprintf(v, sizeof v, "%u", *gptr<u8>(at));
    else if (type == "StrProperty") snprintf(v, sizeof v, "\"%.120s\"", read_fstring(at).c_str());
    else if (type == "NameProperty") snprintf(v, sizeof v, "%s", name_string(t, *gptr<u64>(at)).c_str());
    else if (type == "ArrayProperty") snprintf(v, sizeof v, "%d entries", gptr<TArray<u8>>(at)->num);
    else if (type == "ObjectProperty") snprintf(v, sizeof v, "%s", *gptr<u64>(at) ? class_name(t, *gptr<u64>(at)).c_str() : "none");
    return v;
}

void dump_struct(cpu::Thread& t, GuestAddr st, GuestAddr base, const std::string& indent) {
    for (GuestAddr f = *gptr<u64>(st + kStructChildren); f; f = *gptr<u64>(f + kFieldNext)) {
        std::string type = class_name(t, f);
        if (type.find("Property") == std::string::npos) continue;
        u32 off = *gptr<u32>(f + g_offset_field);
        LOG_INFO("%s  .%-24s %-15s +0x%-4x %s", indent.c_str(), object_name(t, f).c_str(), type.c_str(), off,
                 value_string(t, type, base + off).c_str());
    }
}

}  // namespace

int struct_member_offset(cpu::Thread& t, GuestAddr obj, const char* struct_prop, const char* member) {
    int base = property_offset(t, obj, struct_prop);
    if (base < 0) return -1;
    GuestAddr prop = find_property(t, *gptr<u64>(obj + kObjClass), struct_prop);
    GuestAddr st = prop ? struct_of(t, prop) : 0;
    for (GuestAddr f = st ? *gptr<u64>(st + kStructChildren) : 0; f; f = *gptr<u64>(f + kFieldNext))
        if (object_name(t, f) == member) return base + (int)*gptr<u32>(f + g_offset_field);
    return -1;
}

void dump_functions(cpu::Thread& t, GuestAddr obj, const char* words) {
    if (!obj) return;
    std::string list;
    for (GuestAddr cls = *gptr<u64>(obj + kObjClass); cls; cls = super_class(t, cls)) {
        std::string cname = object_name(t, cls);
        if (cname.rfind("Sword", 0) != 0) break;
        for (GuestAddr f = *gptr<u64>(cls + kStructChildren); f; f = *gptr<u64>(f + kFieldNext)) {
            if (class_name(t, f) != "Function") continue;
            std::string name = object_name(t, f), lower = name;
            for (char& c : lower) c = (char)tolower((unsigned char)c);
            bool hit = false;
            for (const char *w = words, *e; *w; w = *e ? e + 1 : e) {
                e = strchr(w, '|');
                if (!e) e = w + strlen(w);
                if (lower.find(std::string(w, e)) != std::string::npos) hit = true;
            }
            if (hit) {
                list += " " + cname + "." + name;
                if (name.rfind("SetPlayer", 0) == 0 || name == "SaveGame" || name.rfind("Give", 0) == 0 || name.rfind("GetMaxGem", 0) == 0) {
                    list += "(";  // a function's parameters are its first child properties
                    for (GuestAddr p = *gptr<u64>(f + kStructChildren); p; p = *gptr<u64>(p + kFieldNext))
                        list += class_name(t, p) + " " + object_name(t, p) + "@" + std::to_string(*gptr<u32>(p + g_offset_field)) + ",";
                    list += ")";
                }
            }
            if (list.size() > 700) {  // Android's log cuts long lines
                LOG_INFO("functions of %s:%s", class_name(t, obj).c_str(), list.c_str());
                list.clear();
            }
        }
    }
    if (!list.empty()) LOG_INFO("functions of %s:%s", class_name(t, obj).c_str(), list.c_str());
}

void dump_values(cpu::Thread& t, GuestAddr obj, int depth) {
    if (!obj) return;
    if (g_offset_field < 0 && !locate_offset_field(t, *gptr<u64>(obj + kObjClass))) return;
    std::string indent(depth * 2, ' ');
    LOG_INFO("%svalues of %s (%s):", indent.c_str(), object_name(t, obj).c_str(), class_name(t, obj).c_str());
    std::vector<GuestAddr> children;
    for (GuestAddr cls = *gptr<u64>(obj + kObjClass); cls; cls = super_class(t, cls)) {
        std::string cname = object_name(t, cls);
        if (cname.rfind("Sword", 0) != 0) break;  // the game's own classes only, not the engine's
        for (GuestAddr f = *gptr<u64>(cls + kStructChildren); f; f = *gptr<u64>(f + kFieldNext)) {
            std::string type = class_name(t, f);
            if (type.find("Property") == std::string::npos) continue;
            std::string name = object_name(t, f);
            u32 off = *gptr<u32>(f + g_offset_field);
            GuestAddr at = obj + off;
            if (type == "ObjectProperty") {
                GuestAddr o = *gptr<u64>(at);
                if (o && depth < 1 && class_name(t, o).rfind("Sword", 0) == 0) children.push_back(o);
            }
            LOG_INFO("%s  %-26s %-15s +0x%-5x %s", indent.c_str(), name.c_str(), type.c_str(), off,
                     value_string(t, type, at).c_str());
            if (type == "StructProperty")
                if (GuestAddr st = struct_of(t, f)) dump_struct(t, st, at, indent + "  ");
        }
    }
    std::sort(children.begin(), children.end());
    children.erase(std::unique(children.begin(), children.end()), children.end());
    for (GuestAddr c : children) dump_values(t, c, depth + 1);
}

GuestAddr engine() { return g_engine ? *gptr<u64>(g_engine) : 0; }

GuestAddr player_controller(cpu::Thread& t) {
    GuestAddr engine = *gptr<u64>(g_engine);
    TArray<u64> players{};
    GuestAddr controller = 0;
    if (!read_property(t, engine, "GamePlayers", players) || players.num < 1) return 0;
    read_property(t, players.at(0), "Actor", controller);
    return controller;
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
