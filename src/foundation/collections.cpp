// NSArray, NSDictionary, NSSet (+ mutable), NSEnumerator, NSNumber, NSValue, NSNull, NSData.
#include "foundation/foundation.h"
#include <filesystem>
#include "libc/format.h"
#include "libc/vfs.h"
#include "objc/internal.h"
#include <algorithm>
#include <fstream>

namespace ns {

namespace {

Class g_array, g_marray, g_dict, g_mdict, g_set, g_mset, g_number, g_value, g_null, g_data, g_mdata, g_enum;
id g_null_obj = 0;

struct ArrData : objc::HostData {
    std::vector<id> v;
    u64 mutations = 0;
    ~ArrData() override {
        for (id o : v) objc::release(o);
    }
};
struct DictData : objc::HostData {
    std::vector<std::pair<id, id>> kv;
    std::vector<id> key_scratch;  // for fast enumeration
    u64 mutations = 0;
    ~DictData() override {
        for (auto& [k, v] : kv) {
            objc::release(k);
            objc::release(v);
        }
    }
    s64 find(id key) const {
        u64 h = ns::hash(key);
        for (size_t i = 0; i < kv.size(); i++)
            if ((kv[i].first == key) || (ns::hash(kv[i].first) == h && ns::equal(kv[i].first, key))) return (s64)i;
        return -1;
    }
};
struct SetData : objc::HostData {
    std::vector<id> v;
    u64 mutations = 0;
    ~SetData() override {
        for (id o : v) objc::release(o);
    }
    s64 find(id o) const {
        for (size_t i = 0; i < v.size(); i++)
            if (v[i] == o || ns::equal(v[i], o)) return (s64)i;
        return -1;
    }
};
struct EnumData : objc::HostData {
    std::vector<id> v;
    size_t i = 0;
    ~EnumData() override {
        for (id o : v) objc::release(o);
    }
};
enum class NumType { Bool, Int, UInt, Double, Float };
struct NumData : objc::HostData {
    NumType type = NumType::Int;
    s64 i = 0;
    double d = 0;
    char objc_type = 'q';
};
struct ValueData : objc::HostData {
    std::vector<u8> bytes;
    std::string type;
};
struct BytesData : objc::HostData {
    std::vector<u8> b;
};

ArrData& arr(id a) { return objc::ensure<ArrData>(a); }
DictData& dct(id d) { return objc::ensure<DictData>(d); }
SetData& st(id s) { return objc::ensure<SetData>(s); }
BytesData& bytes(id d) { return objc::ensure<BytesData>(d); }

id new_array(Class c, std::vector<id> items) {
    id a = objc::alloc(c);
    auto& d = arr(a);
    for (id o : items) d.v.push_back(objc::retain(o));
    return a;
}
id new_dict(Class c) {
    id d = objc::alloc(c);
    dct(d);
    return d;
}
void dict_put(id d, id key, id value) {
    auto& dd = dct(d);
    value = objc::retain(value);
    s64 i = dd.find(key);
    if (i >= 0) {
        objc::release(dd.kv[i].second);
        dd.kv[i].second = value;
    } else {
        dd.kv.push_back({(id)objc::send(key, "copyWithZone:", {0}), value});
    }
    dd.mutations++;
}
id new_number(NumType t, s64 i, double d, char objc_type) {
    id n = objc::alloc(g_number);
    auto& nd = objc::ensure<NumData>(n);
    nd.type = t;
    nd.i = i;
    nd.d = d;
    nd.objc_type = objc_type;
    return n;
}
NumData* num(id n) { return objc::get<NumData>(n); }

// Variadic nil-terminated list: first object in a register, the rest in 8-byte stack slots.
std::vector<id> nil_terminated(cpu::Thread& t, int first_reg) {
    std::vector<id> out;
    id first = t.x(first_reg);
    if (!first) return out;
    out.push_back(first);
    for (int i = 0;; i++) {
        id o = t.stack_slot(i);
        if (!o) break;
        out.push_back(o);
    }
    return out;
}

// NSFastEnumerationState: state, itemsPtr, mutationsPtr, extra[5]
u64 fast_enum(GuestAddr state, GuestAddr stackbuf, u64 len, const std::vector<id>& items, u64* mutations) {
    u64* s = gptr<u64>(state);
    u64 start = s[0];
    if (start >= items.size()) return 0;
    u64 n = std::min<u64>(len, items.size() - start);
    std::memcpy(gptr<void>(stackbuf), items.data() + start, n * 8);
    s[1] = stackbuf;
    s[2] = gaddr(mutations);
    s[0] = start + n;
    return n;
}

std::vector<u8> read_host_file(const std::string& host) {
    std::ifstream f(std::filesystem::path(libc::utf8_to_wide(host)), std::ios::binary);
    if (!f) return {};
    return std::vector<u8>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

}  // namespace

// ---------------------------------------------------------------------------
// helpers used by other modules
bool is_array(id o) { return o && objc::is_kind_of(o, g_array); }
bool is_dict(id o) { return o && objc::is_kind_of(o, g_dict); }
bool is_number(id o) { return o && objc::is_kind_of(o, g_number); }
bool is_data(id o) { return o && objc::is_kind_of(o, g_data); }
id null_object() { return g_null_obj; }

std::vector<id> array_items(id a) {
    if (!a) return {};
    if (auto* d = objc::get<ArrData>(a)) return d->v;
    if (auto* s = objc::get<SetData>(a)) return s->v;
    u64 n = objc::send(a, "count");
    std::vector<id> out;
    for (u64 i = 0; i < n; i++) out.push_back(objc::send(a, "objectAtIndex:", {i}));
    return out;
}
id array(const std::vector<id>& items) { return objc::autorelease(new_array(g_array, items)); }
id mutable_array() { return objc::autorelease(new_array(g_marray, {})); }
id dict(const std::vector<std::pair<id, id>>& items) {
    id d = new_dict(g_dict);
    for (auto& [k, v] : items) dict_put(d, k, v);
    return objc::autorelease(d);
}
id mutable_dict() { return objc::autorelease(new_dict(g_mdict)); }
id dict_get(id d, id key) {
    if (!d) return 0;
    if (auto* dd = objc::get<DictData>(d)) {
        s64 i = dd->find(key);
        return i < 0 ? 0 : dd->kv[i].second;
    }
    return objc::send(d, "objectForKey:", {key});
}
void dict_set(id d, id key, id value) {
    if (!value) return;
    dict_put(d, key, value);
}
std::vector<std::pair<id, id>> dict_items(id d) {
    if (auto* dd = objc::get<DictData>(d)) return dd->kv;
    std::vector<std::pair<id, id>> out;
    for (id k : array_items(objc::send(d, "allKeys"))) out.push_back({k, objc::send(d, "objectForKey:", {k})});
    return out;
}
id number_int(s64 v) { return objc::autorelease(new_number(NumType::Int, v, (double)v, 'q')); }
id number_uint(u64 v) { return objc::autorelease(new_number(NumType::UInt, (s64)v, (double)v, 'Q')); }
id number_double(double v) { return objc::autorelease(new_number(NumType::Double, (s64)v, v, 'd')); }
id number_bool(bool v) { return objc::autorelease(new_number(NumType::Bool, v, v, 'c')); }
double number_double_value(id n) {
    NumData* d = num(n);
    if (d) return d->type == NumType::Double || d->type == NumType::Float ? d->d : (double)d->i;
    if (is_string(n)) return std::strtod(utf8(n).c_str(), nullptr);
    return 0;
}
s64 number_int_value(id n) {
    NumData* d = num(n);
    if (d) return d->type == NumType::Double || d->type == NumType::Float ? (s64)d->d : d->i;
    if (is_string(n)) return std::strtoll(utf8(n).c_str(), nullptr, 10);
    return 0;
}
id data_with(const void* p, u64 n) {
    id d = objc::alloc(g_data);
    bytes(d).b.assign(static_cast<const u8*>(p), static_cast<const u8*>(p) + n);
    return objc::autorelease(d);
}
std::vector<u8> data_bytes(id d) {
    if (!d) return {};
    if (auto* b = objc::get<BytesData>(d)) return b->b;
    u64 n = objc::send(d, "length");
    const u8* p = gptr<u8>(objc::send(d, "bytes"));
    return std::vector<u8>(p, p + n);
}

u64 hash(id o) {
    if (!o) return 0;
    if (is_string(o)) {
        u64 h = 1469598103934665603ull;
        for (char16_t c : utf16(o)) h = (h ^ c) * 1099511628211ull;
        return h;
    }
    if (NumData* n = num(o)) return n->type == NumType::Double || n->type == NumType::Float ? (u64)(s64)n->d : (u64)n->i;
    return objc::send(o, "hash");
}
bool equal(id a, id b) {
    if (a == b) return true;
    if (!a || !b) return false;
    if (is_string(a) && is_string(b)) return utf16(a) == utf16(b);
    if (num(a) && num(b)) return number_double_value(a) == number_double_value(b);
    return (objc::send(a, "isEqual:", {b}) & 0xff) != 0;
}

void install_collections() {
    using objc::class_method;
    using objc::method;
    g_array = objc::host_class("NSArray");
    g_marray = objc::host_class("NSMutableArray", "NSArray");
    g_dict = objc::host_class("NSDictionary");
    g_mdict = objc::host_class("NSMutableDictionary", "NSDictionary");
    g_set = objc::host_class("NSSet");
    g_mset = objc::host_class("NSMutableSet", "NSSet");
    g_number = objc::host_class("NSNumber", "NSValue");
    objc::host_class("NSDecimalNumber", "NSNumber");
    g_value = objc::host_class("NSValue");
    g_null = objc::host_class("NSNull");
    g_data = objc::host_class("NSData");
    g_mdata = objc::host_class("NSMutableData", "NSData");
    g_enum = objc::host_class("NSEnumerator");
    g_null_obj = objc::alloc(g_null);
    static u64 s_kcfnull;
    s_kcfnull = g_null_obj;
    hle::data("_kCFNull", gaddr(&s_kcfnull));

    // ---------------- NSArray ----------------
    Class A = g_array, MA = g_marray;
    class_method(A, "array", [](Class c, SEL) { return objc::autorelease(new_array(c, {})); });
    class_method(A, "arrayWithObject:", [](Class c, SEL, id o) { return objc::autorelease(new_array(c, {o})); });
    class_method(A, "arrayWithArray:", [](Class c, SEL, id o) { return objc::autorelease(new_array(c, array_items(o))); });
    class_method(A, "arrayWithObjects:count:", [](Class c, SEL, const id* p, u64 n) {
        return objc::autorelease(new_array(c, std::vector<id>(p, p + n)));
    });
    objc::add_method(A, "arrayWithObjects:", [](cpu::Thread& t) {
        t.set_x(0, objc::autorelease(new_array(t.x(0), nil_terminated(t, 2))));
    }, true);
    class_method(A, "arrayWithContentsOfFile:", [](Class c, SEL, id path) -> id {
        id p = plist_from_file(vfs::to_host(utf8(path).c_str()));
        if (!is_array(p)) {
            objc::release(p);
            return 0;
        }
        id r = new_array(c, array_items(p));
        objc::release(p);
        return objc::autorelease(r);
    });
    class_method(MA, "arrayWithCapacity:", [](Class c, SEL, u64) { return objc::autorelease(new_array(c, {})); });
    method(A, "init", [](id self, SEL) {
        arr(self);
        return self;
    });
    method(A, "initWithArray:", [](id self, SEL, id o) {
        for (id x : array_items(o)) arr(self).v.push_back(objc::retain(x));
        return self;
    });
    method(A, "initWithObjects:count:", [](id self, SEL, const id* p, u64 n) {
        for (u64 i = 0; i < n; i++) arr(self).v.push_back(objc::retain(p[i]));
        return self;
    });
    objc::add_method(A, "initWithObjects:", [](cpu::Thread& t) {
        for (id x : nil_terminated(t, 2)) arr(t.x(0)).v.push_back(objc::retain(x));
    });
    method(MA, "initWithCapacity:", [](id self, SEL, u64) {
        arr(self);
        return self;
    });
    method(A, "count", [](id self, SEL) -> u64 { return arr(self).v.size(); });
    auto at = [](id self, SEL, u64 i) -> id {
        auto& v = arr(self).v;
        if (i >= v.size()) {
            LOG_WARN("-[NSArray objectAtIndex:] index %llu beyond bounds %zu", (unsigned long long)i, v.size());
            return 0;
        }
        return v[i];
    };
    method(A, "objectAtIndex:", at);
    method(A, "objectAtIndexedSubscript:", at);
    method(A, "firstObject", [](id self, SEL) -> id { return arr(self).v.empty() ? 0 : arr(self).v.front(); });
    method(A, "lastObject", [](id self, SEL) -> id { return arr(self).v.empty() ? 0 : arr(self).v.back(); });
    method(A, "containsObject:", [](id self, SEL, id o) {
        for (id x : array_items(self))
            if (equal(x, o)) return true;
        return false;
    });
    method(A, "indexOfObject:", [](id self, SEL, id o) -> u64 {
        auto v = array_items(self);
        for (size_t i = 0; i < v.size(); i++)
            if (equal(v[i], o)) return i;
        return NSNotFound;
    });
    method(A, "indexOfObjectIdenticalTo:", [](id self, SEL, id o) -> u64 {
        auto v = array_items(self);
        for (size_t i = 0; i < v.size(); i++)
            if (v[i] == o) return i;
        return NSNotFound;
    });
    method(A, "arrayByAddingObject:", [](id self, SEL, id o) {
        auto v = array_items(self);
        v.push_back(o);
        return array(v);
    });
    method(A, "arrayByAddingObjectsFromArray:", [](id self, SEL, id o) {
        auto v = array_items(self);
        for (id x : array_items(o)) v.push_back(x);
        return array(v);
    });
    objc::add_method(A, "subarrayWithRange:", [](cpu::Thread& t) {
        auto v = array_items(t.x(0));
        u64 loc = std::min<u64>(t.x(2), v.size()), len = std::min<u64>(t.x(3), v.size() - loc);
        t.set_x(0, array(std::vector<id>(v.begin() + loc, v.begin() + loc + len)));
    });
    method(A, "componentsJoinedByString:", [](id self, SEL, id sep) {
        std::string out, s = utf8(sep);
        bool first = true;
        for (id x : array_items(self)) {
            if (!first) out += s;
            first = false;
            out += objc::describe(x);
        }
        return str(out);
    });
    method(A, "isEqualToArray:", [](id self, SEL, id o) {
        auto a = array_items(self), b = array_items(o);
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); i++)
            if (!equal(a[i], b[i])) return false;
        return true;
    });
    method(A, "isEqual:", [](id self, SEL, id o) { return is_array(o) && objc::send(self, "isEqualToArray:", {o}) != 0; });
    method(A, "copyWithZone:", [](id self, SEL, u64) { return new_array(g_array, array_items(self)); });
    method(A, "mutableCopyWithZone:", [](id self, SEL, u64) { return new_array(g_marray, array_items(self)); });
    method(A, "makeObjectsPerformSelector:", [](id self, SEL, SEL s) {
        for (id x : array_items(self)) objc::send_sel(x, s);
    });
    method(A, "makeObjectsPerformSelector:withObject:", [](id self, SEL, SEL s, id a) {
        for (id x : array_items(self)) objc::send_sel(x, s, {a});
    });
    method(A, "objectEnumerator", [](id self, SEL) {
        id e = objc::alloc(g_enum);
        auto& d = objc::ensure<EnumData>(e);
        for (id x : array_items(self)) d.v.push_back(objc::retain(x));
        return objc::autorelease(e);
    });
    method(A, "reverseObjectEnumerator", [](id self, SEL) {
        id e = objc::alloc(g_enum);
        auto& d = objc::ensure<EnumData>(e);
        auto v = array_items(self);
        for (auto it = v.rbegin(); it != v.rend(); ++it) d.v.push_back(objc::retain(*it));
        return objc::autorelease(e);
    });
    method(A, "countByEnumeratingWithState:objects:count:", [](id self, SEL, GuestAddr state, GuestAddr buf, u64 len) {
        auto& d = arr(self);
        return fast_enum(state, buf, len, d.v, &d.mutations);
    });
    method(A, "enumerateObjectsUsingBlock:", [](id self, SEL, GuestAddr block) {
        auto v = array_items(self);
        u8* stop = static_cast<u8*>(std::calloc(1, 8));
        for (size_t i = 0; i < v.size() && !*stop; i++) objc::call_block(block, {v[i], i, gaddr(stop)});
        std::free(stop);
    });
    method(A, "sortedArrayUsingSelector:", [](id self, SEL, SEL s) {
        auto v = array_items(self);
        std::stable_sort(v.begin(), v.end(), [s](id a, id b) { return (s64)objc::send_sel(a, s, {b}) < 0; });
        return array(v);
    });
    method(A, "sortedArrayUsingComparator:", [](id self, SEL, GuestAddr block) {
        auto v = array_items(self);
        std::stable_sort(v.begin(), v.end(), [block](id a, id b) { return (s64)objc::call_block(block, {a, b}) < 0; });
        return array(v);
    });
    method(A, "sortedArrayUsingFunction:context:", [](id self, SEL, GuestAddr fn, u64 ctx) {
        auto v = array_items(self);
        std::stable_sort(v.begin(), v.end(), [fn, ctx](id a, id b) { return (s64)cpu::current().call(fn, {a, b, ctx}) < 0; });
        return array(v);
    });
    method(A, "description", [](id self, SEL) {
        std::string s = "(\n";
        for (id x : array_items(self)) s += "    " + objc::describe(x) + ",\n";
        return str(s + ")");
    });
    method(A, "writeToFile:atomically:", [](id self, SEL, id path, bool) {
        std::string host = vfs::to_host(utf8(path).c_str());
        std::ofstream f(std::filesystem::path(libc::utf8_to_wide(host)), std::ios::binary);
        std::string x = plist_to_xml(self);
        f.write(x.data(), x.size());
        return (bool)f;
    });

    // NSMutableArray
    method(MA, "addObject:", [](id self, SEL, id o) {
        if (!o) {
            LOG_WARN("-[NSMutableArray addObject:] nil");
            return;
        }
        arr(self).v.push_back(objc::retain(o));
        arr(self).mutations++;
    });
    method(MA, "addObjectsFromArray:", [](id self, SEL, id o) {
        for (id x : array_items(o)) arr(self).v.push_back(objc::retain(x));
        arr(self).mutations++;
    });
    method(MA, "insertObject:atIndex:", [](id self, SEL, id o, u64 i) {
        auto& v = arr(self).v;
        v.insert(v.begin() + std::min<u64>(i, v.size()), objc::retain(o));
        arr(self).mutations++;
    });
    method(MA, "removeObjectAtIndex:", [](id self, SEL, u64 i) {
        auto& v = arr(self).v;
        if (i >= v.size()) return;
        id o = v[i];
        v.erase(v.begin() + i);
        arr(self).mutations++;
        objc::release(o);
    });
    method(MA, "removeLastObject", [](id self, SEL) {
        auto& v = arr(self).v;
        if (v.empty()) return;
        id o = v.back();
        v.pop_back();
        arr(self).mutations++;
        objc::release(o);
    });
    method(MA, "removeAllObjects", [](id self, SEL) {
        auto v = std::move(arr(self).v);
        arr(self).v.clear();
        arr(self).mutations++;
        for (id o : v) objc::release(o);
    });
    auto remove_matching = [](id self, id o, bool identical) {
        auto& v = arr(self).v;
        std::vector<id> removed;
        for (size_t i = 0; i < v.size();) {
            if (identical ? v[i] == o : equal(v[i], o)) {
                removed.push_back(v[i]);
                v.erase(v.begin() + i);
            } else {
                i++;
            }
        }
        arr(self).mutations++;
        for (id r : removed) objc::release(r);
    };
    static decltype(remove_matching) s_remove = remove_matching;
    method(MA, "removeObject:", [](id self, SEL, id o) { s_remove(self, o, false); });
    method(MA, "removeObjectIdenticalTo:", [](id self, SEL, id o) { s_remove(self, o, true); });
    method(MA, "removeObjectsInArray:", [](id self, SEL, id other) {
        for (id o : array_items(other)) s_remove(self, o, false);
    });
    auto replace_at = [](id self, SEL, u64 i, id o) {
        auto& v = arr(self).v;
        if (i == v.size()) {
            v.push_back(objc::retain(o));
            return;
        }
        if (i > v.size()) return;
        id old = v[i];
        v[i] = objc::retain(o);
        objc::release(old);
    };
    method(MA, "replaceObjectAtIndex:withObject:", replace_at);
    objc::add_method(MA, "setObject:atIndexedSubscript:", [](cpu::Thread& t) {
        id self = t.x(0), o = t.x(2);
        u64 i = t.x(3);
        auto& v = arr(self).v;
        if (i == v.size()) v.push_back(objc::retain(o));
        else if (i < v.size()) {
            id old = v[i];
            v[i] = objc::retain(o);
            objc::release(old);
        }
    });
    method(MA, "exchangeObjectAtIndex:withObjectAtIndex:", [](id self, SEL, u64 a, u64 b) {
        auto& v = arr(self).v;
        if (a < v.size() && b < v.size()) std::swap(v[a], v[b]);
    });
    method(MA, "sortUsingSelector:", [](id self, SEL, SEL s) {
        auto& v = arr(self).v;
        std::stable_sort(v.begin(), v.end(), [s](id a, id b) { return (s64)objc::send_sel(a, s, {b}) < 0; });
    });
    method(MA, "sortUsingComparator:", [](id self, SEL, GuestAddr block) {
        auto& v = arr(self).v;
        std::stable_sort(v.begin(), v.end(), [block](id a, id b) { return (s64)objc::call_block(block, {a, b}) < 0; });
    });
    method(MA, "sortUsingFunction:context:", [](id self, SEL, GuestAddr fn, u64 ctx) {
        auto& v = arr(self).v;
        std::stable_sort(v.begin(), v.end(), [fn, ctx](id a, id b) { return (s64)cpu::current().call(fn, {a, b, ctx}) < 0; });
    });

    // ---------------- NSDictionary ----------------
    Class D = g_dict, MD = g_mdict;
    class_method(D, "dictionary", [](Class c, SEL) { return objc::autorelease(new_dict(c)); });
    class_method(MD, "dictionaryWithCapacity:", [](Class c, SEL, u64) { return objc::autorelease(new_dict(c)); });
    class_method(D, "dictionaryWithObject:forKey:", [](Class c, SEL, id v, id k) {
        id d = new_dict(c);
        dict_put(d, k, v);
        return objc::autorelease(d);
    });
    class_method(D, "dictionaryWithDictionary:", [](Class c, SEL, id o) {
        id d = new_dict(c);
        for (auto& [k, v] : dict_items(o)) dict_put(d, k, v);
        return objc::autorelease(d);
    });
    class_method(D, "dictionaryWithObjects:forKeys:", [](Class c, SEL, id vals, id keys) {
        id d = new_dict(c);
        auto v = array_items(vals), k = array_items(keys);
        for (size_t i = 0; i < std::min(v.size(), k.size()); i++) dict_put(d, k[i], v[i]);
        return objc::autorelease(d);
    });
    class_method(D, "dictionaryWithObjects:forKeys:count:", [](Class c, SEL, const id* vals, const id* keys, u64 n) {
        id d = new_dict(c);
        for (u64 i = 0; i < n; i++) dict_put(d, keys[i], vals[i]);
        return objc::autorelease(d);
    });
    objc::add_method(D, "dictionaryWithObjectsAndKeys:", [](cpu::Thread& t) {
        id d = new_dict(t.x(0));
        auto items = nil_terminated(t, 2);
        for (size_t i = 0; i + 1 < items.size(); i += 2) dict_put(d, items[i + 1], items[i]);
        t.set_x(0, objc::autorelease(d));
    }, true);
    class_method(D, "dictionaryWithContentsOfFile:", [](Class c, SEL, id path) -> id {
        id p = plist_from_file(vfs::to_host(utf8(path).c_str()));
        if (!is_dict(p)) {
            objc::release(p);
            return 0;
        }
        id d = new_dict(c);
        for (auto& [k, v] : dict_items(p)) dict_put(d, k, v);
        objc::release(p);
        return objc::autorelease(d);
    });
    method(D, "init", [](id self, SEL) {
        dct(self);
        return self;
    });
    method(MD, "initWithCapacity:", [](id self, SEL, u64) {
        dct(self);
        return self;
    });
    method(D, "initWithDictionary:", [](id self, SEL, id o) {
        for (auto& [k, v] : dict_items(o)) dict_put(self, k, v);
        return self;
    });
    method(D, "initWithDictionary:copyItems:", [](id self, SEL, id o, bool) {
        for (auto& [k, v] : dict_items(o)) dict_put(self, k, v);
        return self;
    });
    method(D, "initWithObjects:forKeys:", [](id self, SEL, id vals, id keys) {
        auto v = array_items(vals), k = array_items(keys);
        for (size_t i = 0; i < std::min(v.size(), k.size()); i++) dict_put(self, k[i], v[i]);
        return self;
    });
    method(D, "initWithObjects:forKeys:count:", [](id self, SEL, const id* vals, const id* keys, u64 n) {
        for (u64 i = 0; i < n; i++) dict_put(self, keys[i], vals[i]);
        return self;
    });
    objc::add_method(D, "initWithObjectsAndKeys:", [](cpu::Thread& t) {
        auto items = nil_terminated(t, 2);
        for (size_t i = 0; i + 1 < items.size(); i += 2) dict_put(t.x(0), items[i + 1], items[i]);
    });
    method(D, "count", [](id self, SEL) -> u64 { return dct(self).kv.size(); });
    auto get = [](id self, SEL, id k) -> id {
        auto& d = dct(self);
        s64 i = d.find(k);
        return i < 0 ? 0 : d.kv[i].second;
    };
    method(D, "objectForKey:", get);
    method(D, "objectForKeyedSubscript:", get);
    method(D, "valueForKey:", get);
    method(D, "allKeys", [](id self, SEL) {
        std::vector<id> v;
        for (auto& [k, x] : dct(self).kv) v.push_back(k);
        return array(v);
    });
    method(D, "allValues", [](id self, SEL) {
        std::vector<id> v;
        for (auto& [k, x] : dct(self).kv) v.push_back(x);
        return array(v);
    });
    method(D, "allKeysForObject:", [](id self, SEL, id o) {
        std::vector<id> v;
        for (auto& [k, x] : dct(self).kv)
            if (equal(x, o)) v.push_back(k);
        return array(v);
    });
    method(D, "keyEnumerator", [](id self, SEL) {
        id e = objc::alloc(g_enum);
        auto& d = objc::ensure<EnumData>(e);
        for (auto& [k, x] : dct(self).kv) d.v.push_back(objc::retain(k));
        return objc::autorelease(e);
    });
    method(D, "objectEnumerator", [](id self, SEL) {
        id e = objc::alloc(g_enum);
        auto& d = objc::ensure<EnumData>(e);
        for (auto& [k, x] : dct(self).kv) d.v.push_back(objc::retain(x));
        return objc::autorelease(e);
    });
    method(D, "countByEnumeratingWithState:objects:count:", [](id self, SEL, GuestAddr state, GuestAddr buf, u64 len) {
        auto& d = dct(self);
        if (*gptr<u64>(state) == 0) {
            d.key_scratch.clear();
            for (auto& [k, x] : d.kv) d.key_scratch.push_back(k);
        }
        return fast_enum(state, buf, len, d.key_scratch, &d.mutations);
    });
    method(D, "enumerateKeysAndObjectsUsingBlock:", [](id self, SEL, GuestAddr block) {
        auto kv = dct(self).kv;
        u8* stop = static_cast<u8*>(std::calloc(1, 8));
        for (auto& [k, v] : kv) {
            objc::call_block(block, {k, v, gaddr(stop)});
            if (*stop) break;
        }
        std::free(stop);
    });
    method(D, "isEqualToDictionary:", [](id self, SEL, id o) {
        auto a = dict_items(self), b = dict_items(o);
        if (a.size() != b.size()) return false;
        for (auto& [k, v] : a)
            if (!equal(dict_get(o, k), v)) return false;
        return true;
    });
    method(D, "copyWithZone:", [](id self, SEL, u64) {
        id d = new_dict(g_dict);
        for (auto& [k, v] : dict_items(self)) dict_put(d, k, v);
        return d;
    });
    method(D, "mutableCopyWithZone:", [](id self, SEL, u64) {
        id d = new_dict(g_mdict);
        for (auto& [k, v] : dict_items(self)) dict_put(d, k, v);
        return d;
    });
    method(D, "description", [](id self, SEL) {
        std::string s = "{\n";
        for (auto& [k, v] : dict_items(self)) s += "    " + objc::describe(k) + " = " + objc::describe(v) + ";\n";
        return str(s + "}");
    });
    method(D, "writeToFile:atomically:", [](id self, SEL, id path, bool) {
        std::string host = vfs::to_host(utf8(path).c_str());
        std::ofstream f(std::filesystem::path(libc::utf8_to_wide(host)), std::ios::binary);
        std::string x = plist_to_xml(self);
        f.write(x.data(), x.size());
        return (bool)f;
    });
    auto set = [](id self, SEL, id v, id k) {
        if (!k) return;
        if (!v) {
            objc::send(self, "removeObjectForKey:", {k});
            return;
        }
        dict_put(self, k, v);
    };
    method(MD, "setObject:forKey:", set);
    method(MD, "setValue:forKey:", set);
    method(MD, "setObject:forKeyedSubscript:", set);
    method(MD, "removeObjectForKey:", [](id self, SEL, id k) {
        auto& d = dct(self);
        s64 i = d.find(k);
        if (i < 0) return;
        auto kv = d.kv[i];
        d.kv.erase(d.kv.begin() + i);
        d.mutations++;
        objc::release(kv.first);
        objc::release(kv.second);
    });
    method(MD, "removeObjectsForKeys:", [](id self, SEL, id keys) {
        for (id k : array_items(keys)) objc::send(self, "removeObjectForKey:", {k});
    });
    method(MD, "removeAllObjects", [](id self, SEL) {
        auto kv = std::move(dct(self).kv);
        dct(self).kv.clear();
        dct(self).mutations++;
        for (auto& [k, v] : kv) {
            objc::release(k);
            objc::release(v);
        }
    });
    method(MD, "addEntriesFromDictionary:", [](id self, SEL, id o) {
        for (auto& [k, v] : dict_items(o)) dict_put(self, k, v);
    });

    // ---------------- NSSet ----------------
    Class Z = g_set, MZ = g_mset;
    auto new_set = [](Class c, const std::vector<id>& items) {
        id s = objc::alloc(c);
        auto& d = st(s);
        for (id o : items)
            if (d.find(o) < 0) d.v.push_back(objc::retain(o));
        return s;
    };
    static decltype(new_set) s_new_set = new_set;
    class_method(Z, "set", [](Class c, SEL) { return objc::autorelease(s_new_set(c, {})); });
    class_method(Z, "setWithArray:", [](Class c, SEL, id a) { return objc::autorelease(s_new_set(c, array_items(a))); });
    class_method(Z, "setWithSet:", [](Class c, SEL, id a) { return objc::autorelease(s_new_set(c, array_items(a))); });
    class_method(Z, "setWithObject:", [](Class c, SEL, id o) { return objc::autorelease(s_new_set(c, {o})); });
    objc::add_method(Z, "setWithObjects:", [](cpu::Thread& t) { t.set_x(0, objc::autorelease(s_new_set(t.x(0), nil_terminated(t, 2)))); }, true);
    class_method(MZ, "setWithCapacity:", [](Class c, SEL, u64) { return objc::autorelease(s_new_set(c, {})); });
    method(Z, "init", [](id self, SEL) {
        st(self);
        return self;
    });
    method(MZ, "initWithCapacity:", [](id self, SEL, u64) {
        st(self);
        return self;
    });
    method(Z, "initWithArray:", [](id self, SEL, id a) {
        for (id o : array_items(a))
            if (st(self).find(o) < 0) st(self).v.push_back(objc::retain(o));
        return self;
    });
    objc::add_method(Z, "initWithObjects:", [](cpu::Thread& t) {
        for (id o : nil_terminated(t, 2))
            if (st(t.x(0)).find(o) < 0) st(t.x(0)).v.push_back(objc::retain(o));
    });
    method(Z, "count", [](id self, SEL) -> u64 { return st(self).v.size(); });
    method(Z, "containsObject:", [](id self, SEL, id o) { return st(self).find(o) >= 0; });
    method(Z, "member:", [](id self, SEL, id o) -> id {
        s64 i = st(self).find(o);
        return i < 0 ? 0 : st(self).v[i];
    });
    method(Z, "anyObject", [](id self, SEL) -> id { return st(self).v.empty() ? 0 : st(self).v[0]; });
    method(Z, "allObjects", [](id self, SEL) { return array(st(self).v); });
    method(Z, "objectEnumerator", [](id self, SEL) {
        id e = objc::alloc(g_enum);
        auto& d = objc::ensure<EnumData>(e);
        for (id x : st(self).v) d.v.push_back(objc::retain(x));
        return objc::autorelease(e);
    });
    method(Z, "countByEnumeratingWithState:objects:count:", [](id self, SEL, GuestAddr state, GuestAddr buf, u64 len) {
        auto& d = st(self);
        return fast_enum(state, buf, len, d.v, &d.mutations);
    });
    method(Z, "copyWithZone:", [](id self, SEL, u64) { return s_new_set(g_set, st(self).v); });
    method(Z, "mutableCopyWithZone:", [](id self, SEL, u64) { return s_new_set(g_mset, st(self).v); });
    method(MZ, "addObject:", [](id self, SEL, id o) {
        auto& d = st(self);
        if (o && d.find(o) < 0) d.v.push_back(objc::retain(o)), d.mutations++;
    });
    method(MZ, "addObjectsFromArray:", [](id self, SEL, id a) {
        for (id o : array_items(a))
            if (st(self).find(o) < 0) st(self).v.push_back(objc::retain(o));
    });
    method(MZ, "unionSet:", [](id self, SEL, id a) {
        for (id o : array_items(a))
            if (st(self).find(o) < 0) st(self).v.push_back(objc::retain(o));
    });
    method(MZ, "removeObject:", [](id self, SEL, id o) {
        auto& d = st(self);
        s64 i = d.find(o);
        if (i < 0) return;
        id x = d.v[i];
        d.v.erase(d.v.begin() + i);
        d.mutations++;
        objc::release(x);
    });
    method(MZ, "removeAllObjects", [](id self, SEL) {
        auto v = std::move(st(self).v);
        st(self).v.clear();
        for (id o : v) objc::release(o);
    });

    // ---------------- NSEnumerator ----------------
    method(g_enum, "nextObject", [](id self, SEL) -> id {
        auto& d = objc::ensure<EnumData>(self);
        return d.i < d.v.size() ? d.v[d.i++] : 0;
    });
    method(g_enum, "allObjects", [](id self, SEL) {
        auto& d = objc::ensure<EnumData>(self);
        std::vector<id> rest(d.v.begin() + d.i, d.v.end());
        d.i = d.v.size();
        return array(rest);
    });
    method(g_enum, "countByEnumeratingWithState:objects:count:", [](id self, SEL, GuestAddr state, GuestAddr buf, u64 len) {
        auto& d = objc::ensure<EnumData>(self);
        static u64 no_mutations = 0;
        return fast_enum(state, buf, len, d.v, &no_mutations);
    });

    // ---------------- NSNumber ----------------
    Class N = g_number;
    class_method(N, "numberWithBool:", [](Class, SEL, bool v) { return number_bool(v); });
    class_method(N, "numberWithChar:", [](Class, SEL, s8 v) { return objc::autorelease(new_number(NumType::Int, v, v, 'c')); });
    class_method(N, "numberWithUnsignedChar:", [](Class, SEL, u8 v) { return objc::autorelease(new_number(NumType::Int, v, v, 'C')); });
    class_method(N, "numberWithShort:", [](Class, SEL, s16 v) { return objc::autorelease(new_number(NumType::Int, v, v, 's')); });
    class_method(N, "numberWithUnsignedShort:", [](Class, SEL, u16 v) { return objc::autorelease(new_number(NumType::Int, v, v, 'S')); });
    class_method(N, "numberWithInt:", [](Class, SEL, s32 v) { return objc::autorelease(new_number(NumType::Int, v, v, 'i')); });
    class_method(N, "numberWithUnsignedInt:", [](Class, SEL, u32 v) { return objc::autorelease(new_number(NumType::Int, v, v, 'I')); });
    class_method(N, "numberWithInteger:", [](Class, SEL, s64 v) { return number_int(v); });
    class_method(N, "numberWithLong:", [](Class, SEL, s64 v) { return number_int(v); });
    class_method(N, "numberWithLongLong:", [](Class, SEL, s64 v) { return number_int(v); });
    class_method(N, "numberWithUnsignedInteger:", [](Class, SEL, u64 v) { return number_uint(v); });
    class_method(N, "numberWithUnsignedLong:", [](Class, SEL, u64 v) { return number_uint(v); });
    class_method(N, "numberWithUnsignedLongLong:", [](Class, SEL, u64 v) { return number_uint(v); });
    class_method(N, "numberWithFloat:", [](Class, SEL, float v) { return objc::autorelease(new_number(NumType::Float, (s64)v, v, 'f')); });
    class_method(N, "numberWithDouble:", [](Class, SEL, double v) { return number_double(v); });
    auto init_int = [](id self, SEL, s64 v) {
        auto& n = objc::ensure<NumData>(self);
        n.type = NumType::Int;
        n.i = v;
        n.d = (double)v;
        n.objc_type = 'q';
        return self;
    };
    method(N, "initWithInt:", [](id self, SEL, s32 v) {
        auto& n = objc::ensure<NumData>(self);
        n.i = v, n.d = v, n.objc_type = 'i';
        return self;
    });
    method(N, "initWithInteger:", init_int);
    method(N, "initWithLongLong:", init_int);
    method(N, "initWithUnsignedInt:", [](id self, SEL, u32 v) {
        auto& n = objc::ensure<NumData>(self);
        n.i = v, n.d = v, n.objc_type = 'I';
        return self;
    });
    method(N, "initWithBool:", [](id self, SEL, bool v) {
        auto& n = objc::ensure<NumData>(self);
        n.type = NumType::Bool, n.i = v, n.d = v, n.objc_type = 'c';
        return self;
    });
    method(N, "initWithFloat:", [](id self, SEL, float v) {
        auto& n = objc::ensure<NumData>(self);
        n.type = NumType::Float, n.d = v, n.i = (s64)v, n.objc_type = 'f';
        return self;
    });
    method(N, "initWithDouble:", [](id self, SEL, double v) {
        auto& n = objc::ensure<NumData>(self);
        n.type = NumType::Double, n.d = v, n.i = (s64)v, n.objc_type = 'd';
        return self;
    });
    method(N, "boolValue", [](id self, SEL) { return number_double_value(self) != 0; });
    method(N, "charValue", [](id self, SEL) -> s8 { return (s8)number_int_value(self); });
    method(N, "unsignedCharValue", [](id self, SEL) -> u8 { return (u8)number_int_value(self); });
    method(N, "shortValue", [](id self, SEL) -> s16 { return (s16)number_int_value(self); });
    method(N, "intValue", [](id self, SEL) -> s32 { return (s32)number_int_value(self); });
    method(N, "unsignedIntValue", [](id self, SEL) -> u32 { return (u32)number_int_value(self); });
    method(N, "integerValue", [](id self, SEL) -> s64 { return number_int_value(self); });
    method(N, "longValue", [](id self, SEL) -> s64 { return number_int_value(self); });
    method(N, "longLongValue", [](id self, SEL) -> s64 { return number_int_value(self); });
    method(N, "unsignedIntegerValue", [](id self, SEL) -> u64 { return (u64)number_int_value(self); });
    method(N, "unsignedLongLongValue", [](id self, SEL) -> u64 { return (u64)number_int_value(self); });
    method(N, "floatValue", [](id self, SEL) { return (float)number_double_value(self); });
    method(N, "doubleValue", [](id self, SEL) { return number_double_value(self); });
    method(N, "objCType", [](id self, SEL) {
        static const char* types[] = {"c", "i", "q", "d", "f", "I", "Q", "s", "S", "C"};
        char t = num(self) ? num(self)->objc_type : 'q';
        for (const char* s : types)
            if (s[0] == t) return s;
        return "q";
    });
    method(N, "stringValue", [](id self, SEL) { return objc::send(self, "description"); });
    method(N, "description", [](id self, SEL) {
        NumData* n = num(self);
        if (!n) return str("0");
        if (n->type == NumType::Double || n->type == NumType::Float) {
            char buf[64];
            snprintf(buf, sizeof buf, "%.17g", n->d);
            return str(buf);
        }
        return str(std::to_string(n->i));
    });
    method(N, "hash", [](id self, SEL) { return hash(self); });
    method(N, "isEqual:", [](id self, SEL, id o) { return is_number(o) && number_double_value(o) == number_double_value(self); });
    method(N, "isEqualToNumber:", [](id self, SEL, id o) { return o && number_double_value(o) == number_double_value(self); });
    method(N, "compare:", [](id self, SEL, id o) -> s64 {
        double a = number_double_value(self), b = number_double_value(o);
        return a < b ? -1 : a > b ? 1 : 0;
    });
    method(N, "copyWithZone:", [](id self, SEL, u64) { return objc::retain(self); });

    // ---------------- NSValue ----------------
    Class V = g_value;
    class_method(V, "valueWithPointer:", [](Class c, SEL, u64 p) {
        id v = objc::alloc(c);
        auto& d = objc::ensure<ValueData>(v);
        d.bytes.resize(8);
        std::memcpy(d.bytes.data(), &p, 8);
        d.type = "^v";
        return objc::autorelease(v);
    });
    class_method(V, "valueWithNonretainedObject:", [](Class c, SEL, u64 p) {
        id v = objc::alloc(c);
        auto& d = objc::ensure<ValueData>(v);
        d.bytes.resize(8);
        std::memcpy(d.bytes.data(), &p, 8);
        d.type = "@";
        return objc::autorelease(v);
    });
    class_method(V, "value:withObjCType:", [](Class c, SEL, const u8* p, const char* type) {
        id v = objc::alloc(c);
        auto& d = objc::ensure<ValueData>(v);
        size_t n = type[0] == '{' ? 32 : 8;  // good enough for CGPoint/CGSize/CGRect/NSRange
        d.bytes.assign(p, p + n);
        d.type = type;
        return objc::autorelease(v);
    });
    method(V, "pointerValue", [](id self, SEL) -> u64 {
        auto* d = objc::get<ValueData>(self);
        u64 p = 0;
        if (d && d->bytes.size() >= 8) std::memcpy(&p, d->bytes.data(), 8);
        return p;
    });
    method(V, "nonretainedObjectValue", [](id self, SEL) -> u64 {
        auto* d = objc::get<ValueData>(self);
        u64 p = 0;
        if (d && d->bytes.size() >= 8) std::memcpy(&p, d->bytes.data(), 8);
        return p;
    });
    method(V, "getValue:", [](id self, SEL, u8* out) {
        if (auto* d = objc::get<ValueData>(self)) std::memcpy(out, d->bytes.data(), d->bytes.size());
    });

    // ---------------- NSNull ----------------
    class_method(g_null, "null", [](Class, SEL) { return g_null_obj; });
    method(g_null, "description", [](id, SEL) { return str("<null>"); });
    method(g_null, "release", [](id, SEL) {});
    method(g_null, "retain", [](id self, SEL) { return self; });

    // ---------------- NSData ----------------
    Class DA = g_data, MDA = g_mdata;
    auto new_data = [](Class c, const u8* p, u64 n) {
        id d = objc::alloc(c);
        bytes(d).b.assign(p, p + n);
        return d;
    };
    static decltype(new_data) s_new_data = new_data;
    class_method(DA, "data", [](Class c, SEL) { return objc::autorelease(s_new_data(c, nullptr, 0)); });
    class_method(DA, "dataWithBytes:length:", [](Class c, SEL, const u8* p, u64 n) { return objc::autorelease(s_new_data(c, p, n)); });
    class_method(DA, "dataWithBytesNoCopy:length:", [](Class c, SEL, u8* p, u64 n) {
        id d = objc::autorelease(s_new_data(c, p, n));
        std::free(p);
        return d;
    });
    class_method(DA, "dataWithBytesNoCopy:length:freeWhenDone:", [](Class c, SEL, u8* p, u64 n, bool free_it) {
        id d = objc::autorelease(s_new_data(c, p, n));
        if (free_it) std::free(p);
        return d;
    });
    class_method(DA, "dataWithData:", [](Class c, SEL, id o) {
        auto b = data_bytes(o);
        return objc::autorelease(s_new_data(c, b.data(), b.size()));
    });
    class_method(DA, "dataWithContentsOfFile:", [](Class c, SEL, id path) -> id {
        std::string host = vfs::to_host(utf8(path).c_str());
        if (host.empty()) return 0;
        std::ifstream f(std::filesystem::path(libc::utf8_to_wide(host)), std::ios::binary);
        if (!f) return 0;
        std::vector<u8> b((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return objc::autorelease(s_new_data(c, b.data(), b.size()));
    });
    class_method(DA, "dataWithContentsOfFile:options:error:", [](Class c, SEL, id path, u64, u64* err) -> id {
        if (err) *err = 0;
        return objc::send(c, "dataWithContentsOfFile:", {path});
    });
    class_method(MDA, "dataWithCapacity:", [](Class c, SEL, u64) { return objc::autorelease(s_new_data(c, nullptr, 0)); });
    class_method(MDA, "dataWithLength:", [](Class c, SEL, u64 n) {
        id d = s_new_data(c, nullptr, 0);
        bytes(d).b.resize(n);
        return objc::autorelease(d);
    });
    method(DA, "init", [](id self, SEL) {
        bytes(self);
        return self;
    });
    method(DA, "initWithBytes:length:", [](id self, SEL, const u8* p, u64 n) {
        bytes(self).b.assign(p, p + n);
        return self;
    });
    method(DA, "initWithBytesNoCopy:length:freeWhenDone:", [](id self, SEL, u8* p, u64 n, bool free_it) {
        bytes(self).b.assign(p, p + n);
        if (free_it) std::free(p);
        return self;
    });
    method(DA, "initWithData:", [](id self, SEL, id o) {
        bytes(self).b = data_bytes(o);
        return self;
    });
    method(DA, "initWithContentsOfFile:", [](id self, SEL, id path) -> id {
        std::string host = vfs::to_host(utf8(path).c_str());
        std::ifstream f(std::filesystem::path(libc::utf8_to_wide(host)), std::ios::binary);
        if (host.empty() || !f) {
            objc::release(self);
            return 0;
        }
        bytes(self).b.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return self;
    });
    method(MDA, "initWithCapacity:", [](id self, SEL, u64) {
        bytes(self);
        return self;
    });
    method(MDA, "initWithLength:", [](id self, SEL, u64 n) {
        bytes(self).b.resize(n);
        return self;
    });
    method(DA, "length", [](id self, SEL) -> u64 { return bytes(self).b.size(); });
    method(DA, "bytes", [](id self, SEL) -> u64 { return bytes(self).b.empty() ? gaddr(hle::alloc_static(1)) : gaddr(bytes(self).b.data()); });
    method(DA, "getBytes:length:", [](id self, SEL, u8* out, u64 n) {
        auto& b = bytes(self).b;
        std::memcpy(out, b.data(), std::min<u64>(n, b.size()));
    });
    objc::add_method(DA, "getBytes:range:", [](cpu::Thread& t) {
        auto& b = bytes(t.x(0)).b;
        u64 loc = t.x(3), len = t.x(4);
        if (loc + len <= b.size()) std::memcpy(gptr<void>(t.x(2)), b.data() + loc, len);
    });
    objc::add_method(DA, "subdataWithRange:", [](cpu::Thread& t) {
        auto& b = bytes(t.x(0)).b;
        u64 loc = std::min<u64>(t.x(2), b.size()), len = std::min<u64>(t.x(3), b.size() - loc);
        t.set_x(0, data_with(b.data() + loc, len));
    });
    method(DA, "isEqualToData:", [](id self, SEL, id o) { return data_bytes(o) == bytes(self).b; });
    method(DA, "isEqual:", [](id self, SEL, id o) { return is_data(o) && data_bytes(o) == bytes(self).b; });
    method(DA, "copyWithZone:", [](id self, SEL, u64) {
        auto& b = bytes(self).b;
        return s_new_data(g_data, b.data(), b.size());
    });
    method(DA, "mutableCopyWithZone:", [](id self, SEL, u64) {
        auto& b = bytes(self).b;
        return s_new_data(g_mdata, b.data(), b.size());
    });
    method(DA, "writeToFile:atomically:", [](id self, SEL, id path, bool) {
        std::string host = vfs::to_host(utf8(path).c_str());
        if (host.empty()) return false;
        std::ofstream f(std::filesystem::path(libc::utf8_to_wide(host)), std::ios::binary);
        auto& b = bytes(self).b;
        f.write(reinterpret_cast<const char*>(b.data()), b.size());
        return (bool)f;
    });
    method(DA, "writeToFile:options:error:", [](id self, SEL, id path, u64, u64* err) {
        if (err) *err = 0;
        return objc::send(self, "writeToFile:atomically:", {path, 1}) != 0;
    });
    method(DA, "description", [](id self, SEL) {
        char buf[64];
        snprintf(buf, sizeof buf, "<NSData %zu bytes>", bytes(self).b.size());
        return str(buf);
    });
    method(MDA, "mutableBytes", [](id self, SEL) -> u64 { return gaddr(bytes(self).b.data()); });
    method(MDA, "setLength:", [](id self, SEL, u64 n) { bytes(self).b.resize(n); });
    method(MDA, "increaseLengthBy:", [](id self, SEL, u64 n) { bytes(self).b.resize(bytes(self).b.size() + n); });
    method(MDA, "appendBytes:length:", [](id self, SEL, const u8* p, u64 n) { bytes(self).b.insert(bytes(self).b.end(), p, p + n); });
    method(MDA, "appendData:", [](id self, SEL, id o) {
        auto b = data_bytes(o);
        bytes(self).b.insert(bytes(self).b.end(), b.begin(), b.end());
    });
    method(MDA, "setData:", [](id self, SEL, id o) { bytes(self).b = data_bytes(o); });
    objc::add_method(MDA, "replaceBytesInRange:withBytes:", [](cpu::Thread& t) {
        auto& b = bytes(t.x(0)).b;
        u64 loc = t.x(2), len = t.x(3);
        if (loc + len > b.size()) b.resize(loc + len);
        std::memcpy(b.data() + loc, gptr<void>(t.x(4)), len);
    });
    objc::add_method(MDA, "resetBytesInRange:", [](cpu::Thread& t) {
        auto& b = bytes(t.x(0)).b;
        u64 loc = t.x(2), len = t.x(3);
        if (loc + len > b.size()) b.resize(loc + len);
        std::memset(b.data() + loc, 0, len);
    });
}

}  // namespace ns
