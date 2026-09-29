#include "objc/runtime.h"
#include "libc/format.h"
#include "macho.h"
#include <atomic>
#include <mutex>
#include <shared_mutex>
#include <unordered_map>
#include <set>
#include <unordered_set>
#include <typeinfo>
#include <vector>
#include <windows.h>

namespace objc {

// Set by Foundation: UTF-8 contents of an NSString (or nullopt if not a string).
std::string (*g_nsstring_utf8)(id) = nullptr;
bool (*g_is_nsstring)(id) = nullptr;

namespace {

constexpr u32 kHeaderMagic = 0x214a424f;  // "OBJ!"
struct Header {
    u32 magic;
    std::atomic<u32> rc;
    HostBag* host;
};
static_assert(sizeof(Header) == 16);
Header* header(id obj) { return gptr<Header>(obj - sizeof(Header)); }

struct ClassInfo {
    Class cls = 0;
    bool meta = false;
    bool host = false;
    std::string name;
    ClassInfo* super = nullptr;
    ClassInfo* partner = nullptr;  // class <-> metaclass
    u32 instance_size = 8;
    GuestAddr ro = 0;
    std::unordered_map<SEL, GuestAddr> methods;
    std::unordered_map<SEL, GuestAddr> cache;
    std::vector<GuestAddr> protocols;
    std::atomic<int> init_state{0};
    DWORD init_thread = 0;
};

const macho::Image* g_image = nullptr;
std::shared_mutex g_lock;
std::unordered_map<Class, ClassInfo*> g_classes;
std::unordered_map<std::string, ClassInfo*> g_by_name;
std::mutex g_sel_mutex;
std::unordered_map<std::string, SEL> g_sels;
GuestAddr g_msgsend = 0;
GuestAddr g_empty_cache = 0;

// Block classes (isa values).
Class g_stack_block = 0, g_global_block = 0, g_malloc_block = 0;

ClassInfo* info(Class c) {
    std::shared_lock lock(g_lock);
    auto it = g_classes.find(c);
    return it == g_classes.end() ? nullptr : it->second;
}

void flush_caches_locked() {
    for (auto& [k, ci] : g_classes) ci->cache.clear();
}

// class_t: isa, superclass, cache, vtable, data (class_ro_t*)
// class_ro_t: flags, instanceStart, instanceSize, reserved, ivarLayout, name, baseMethods,
//             baseProtocols, ivars, weakIvarLayout, baseProperties
struct GuestClass {
    u64 isa, superclass, cache, vtable, data;
};
struct GuestRo {
    u32 flags, instance_start, instance_size, reserved;
    u64 ivar_layout, name, base_methods, base_protocols, ivars, weak_ivar_layout, base_properties;
};

void add_method_list(ClassInfo* ci, GuestAddr list) {
    if (!list) return;
    u32 entsize = *gptr<u32>(list) & ~3u;
    u32 count = *gptr<u32>(list + 4);
    for (u32 i = 0; i < count; i++) {
        GuestAddr m = list + 8 + (u64)i * entsize;
        const char* name = gptr<char>(*gptr<u64>(m));
        GuestAddr imp = *gptr<u64>(m + 16);
        ci->methods[sel(name)] = imp;
    }
}

void add_protocol_list(ClassInfo* ci, GuestAddr list) {
    if (!list) return;
    u64 n = *gptr<u64>(list);
    for (u64 i = 0; i < n; i++) ci->protocols.push_back(gptr<u64>(list + 8)[i]);
}

ClassInfo* realize_guest(Class c);

ClassInfo* realize_guest(Class c) {
    {
        std::shared_lock lock(g_lock);
        auto it = g_classes.find(c);
        if (it != g_classes.end()) return it->second;
    }
    auto* gc = gptr<GuestClass>(c);
    auto* ro = gptr<GuestRo>(gc->data & ~7ull);
    auto* ci = new ClassInfo;
    auto* mi = new ClassInfo;
    ci->cls = c;
    ci->name = gptr<char>(ro->name);
    ci->instance_size = ro->instance_size;
    ci->ro = gaddr(ro);
    ci->partner = mi;
    mi->cls = gc->isa;
    mi->meta = true;
    mi->name = ci->name;
    mi->partner = ci;
    auto* mgc = gptr<GuestClass>(gc->isa);
    auto* mro = gptr<GuestRo>(mgc->data & ~7ull);
    mi->ro = gaddr(mro);
    add_method_list(ci, ro->base_methods);
    add_method_list(mi, mro->base_methods);
    add_protocol_list(ci, ro->base_protocols);
    {
        std::unique_lock lock(g_lock);
        g_classes[c] = ci;
        g_classes[gc->isa] = mi;
        g_by_name[ci->name] = ci;
    }
    if (gc->superclass) {
        ClassInfo* sup = info(gc->superclass);
        if (!sup) sup = realize_guest(gc->superclass);
        ci->super = sup;
        mi->super = sup->partner;
    } else {
        mi->super = ci;  // root class: metaclass inherits from the class
    }
    return ci;
}

// --- autorelease pools ---
thread_local std::vector<id>* t_pool = nullptr;
std::vector<id>& pool() {
    if (!t_pool) t_pool = new std::vector<id>;
    return *t_pool;
}

// --- weak references and associated objects ---
std::mutex g_side_mutex;
std::unordered_map<id, std::vector<GuestAddr>> g_weak;
std::unordered_map<id, std::unordered_map<u64, std::pair<id, u64>>> g_assoc;
std::unordered_map<id, std::recursive_mutex*> g_sync;

void ensure_initialized(ClassInfo* ci);

}  // namespace

// ---------------------------------------------------------------------------
// selectors
SEL sel(std::string_view name) {
    std::lock_guard lock(g_sel_mutex);
    auto it = g_sels.find(std::string(name));
    if (it != g_sels.end()) return it->second;
    SEL s = gaddr(hle::static_cstr(name));
    g_sels.emplace(std::string(name), s);
    return s;
}
const char* sel_name(SEL s) { return s ? gptr<char>(s) : "(null)"; }

// ---------------------------------------------------------------------------
// classes
Class isa(id obj) { return obj ? *gptr<u64>(obj) : 0; }

Class class_named(std::string_view name) {
    std::shared_lock lock(g_lock);
    auto it = g_by_name.find(std::string(name));
    return it == g_by_name.end() ? 0 : it->second->cls;
}

std::string class_name(Class c) {
    ClassInfo* ci = info(c);
    return ci ? ci->name : "<unknown class>";
}

Class superclass(Class c) {
    ClassInfo* ci = info(c);
    return ci && ci->super ? ci->super->cls : 0;
}

bool is_metaclass(Class c) {
    ClassInfo* ci = info(c);
    return ci && ci->meta;
}

bool class_is_subclass(Class c, Class parent) {
    for (ClassInfo* ci = info(c); ci; ci = ci->super) {
        if (ci->cls == parent) return true;
        if (ci->meta && ci->super && !ci->super->meta) break;  // stop at root metaclass
    }
    return false;
}

bool is_kind_of(id obj, Class cls) { return obj && class_is_subclass(isa(obj), cls); }

u32 instance_size(Class c) {
    ClassInfo* ci = info(c);
    return ci ? ci->instance_size : 8;
}

Class host_class(std::string_view name_sv, std::string_view super_name) {
    std::string name(name_sv);
    if (Class c = class_named(name)) return c;
    ClassInfo* sup = nullptr;
    if (!super_name.empty() && super_name != name) {
        Class sc = host_class(super_name, super_name == "NSObject" ? "" : "NSObject");
        sup = info(sc);
    }
    auto* gc = static_cast<GuestClass*>(hle::alloc_static(sizeof(GuestClass)));
    auto* mgc = static_cast<GuestClass*>(hle::alloc_static(sizeof(GuestClass)));
    auto* ro = static_cast<GuestRo*>(hle::alloc_static(sizeof(GuestRo)));
    auto* mro = static_cast<GuestRo*>(hle::alloc_static(sizeof(GuestRo)));
    ro->name = mro->name = gaddr(hle::static_cstr(name));
    ro->instance_size = ro->instance_start = 8;
    mro->flags = 1;  // RO_META
    mro->instance_size = mro->instance_start = sizeof(GuestClass);
    gc->data = gaddr(ro);
    mgc->data = gaddr(mro);
    gc->cache = mgc->cache = g_empty_cache;
    gc->isa = gaddr(mgc);

    auto* ci = new ClassInfo;
    auto* mi = new ClassInfo;
    ci->cls = gaddr(gc);
    ci->host = mi->host = true;
    ci->name = mi->name = name;
    ci->partner = mi;
    ci->ro = gaddr(ro);
    mi->cls = gaddr(mgc);
    mi->meta = true;
    mi->partner = ci;
    mi->ro = gaddr(mro);
    if (sup) {
        ci->super = sup;
        mi->super = sup->partner;
        gc->superclass = sup->cls;
        mgc->superclass = sup->partner->cls;
        // Metaclass isa always points at the root metaclass.
        ClassInfo* root = sup;
        while (root->super) root = root->super;
        mgc->isa = root->partner->cls;
    } else {
        mi->super = ci;
        mgc->superclass = ci->cls;
        mgc->isa = mi->cls;
    }
    std::unique_lock lock(g_lock);
    g_classes[ci->cls] = ci;
    g_classes[mi->cls] = mi;
    g_by_name[name] = ci;
    return ci->cls;
}

void add_method(Class cls, std::string_view selector, cpu::Handler h, bool class_method) {
    ClassInfo* ci = info(cls);
    if (!ci) fatal("add_method on unknown class");
    if (class_method) ci = ci->partner;
    std::string label = std::string(class_method ? "+[" : "-[") + ci->name + " " + std::string(selector) + "]";
    GuestAddr imp = cpu::make_stub(label, std::move(h));
    SEL s = sel(selector);
    std::unique_lock lock(g_lock);
    ci->methods[s] = imp;
    flush_caches_locked();
}

void override_method(std::string_view cname, std::string_view selector, cpu::Handler h, bool class_method) {
    Class c = class_named(cname);
    if (!c) {
        LOG_DEBUG("override_method: class %.*s not present", (int)cname.size(), cname.data());
        return;
    }
    add_method(c, selector, std::move(h), class_method);
}

void stub_out_class_methods(std::string_view cname) {
    Class c = class_named(cname);
    ClassInfo* ci = c ? info(c) : nullptr;
    if (!ci || ci->host) return;
    static GuestAddr noop = cpu::make_stub("<stubbed SDK method>", [](cpu::Thread& t) {
        t.set_x(0, 0);
        t.set_v(0, {0, 0});
    });
    std::unique_lock lock(g_lock);
    size_t n = 0;
    for (auto& [s, imp] : ci->partner->methods)
        if (std::strcmp(sel_name(s), "load") && std::strcmp(sel_name(s), "initialize")) imp = noop, n++;
    flush_caches_locked();
    LOG_INFO("stubbed out %zu class methods of %.*s", n, (int)cname.size(), cname.data());
}

GuestAddr lookup_imp(Class cls, SEL s) {
    ClassInfo* ci = info(cls);
    if (!ci) return 0;
    {
        std::shared_lock lock(g_lock);
        auto it = ci->cache.find(s);
        if (it != ci->cache.end()) return it->second;
    }
    std::unique_lock lock(g_lock);
    GuestAddr imp = 0;
    for (ClassInfo* c = ci; c; c = c->super) {
        auto it = c->methods.find(s);
        if (it != c->methods.end()) {
            imp = it->second;
            break;
        }
        if (c->meta && c->super && !c->super->meta) {
            // Root metaclass -> root class: continue into instance methods.
            ClassInfo* root = c->super;
            auto r = root->methods.find(s);
            if (r != root->methods.end()) imp = r->second;
            break;
        }
    }
    ci->cache[s] = imp;
    return imp;
}

bool responds_to(id obj, SEL s) { return obj && lookup_imp(isa(obj), s) != 0; }

// ---------------------------------------------------------------------------
// objects
bool is_static(id obj) {
    if (!obj) return true;
    if (g_image && g_image->contains(obj)) return true;
    Class c = isa(obj);
    if (c == g_stack_block || c == g_global_block || c == g_malloc_block) return true;
    if (info(obj)) return true;  // class object
    return header(obj)->magic != kHeaderMagic;
}

id alloc(Class cls) {
    ClassInfo* ci = info(cls);
    if (!ci) fatal("alloc of unknown class 0x%llx", (unsigned long long)cls);
    ensure_initialized(ci);
    size_t size = std::max<u32>(ci->instance_size, 8);
    auto* h = static_cast<Header*>(std::calloc(1, sizeof(Header) + size));
    h->magic = kHeaderMagic;
    h->rc.store(1);
    id obj = gaddr(h) + sizeof(Header);
    *gptr<u64>(obj) = cls;
    return obj;
}

id retain(id obj) {
    if (!obj) return 0;
    Class c = isa(obj);
    if (c == g_malloc_block || c == g_stack_block) return c == g_malloc_block ? block_copy(obj) : obj;
    if (is_static(obj)) return obj;
    header(obj)->rc.fetch_add(1, std::memory_order_relaxed);
    return obj;
}

void release(id obj) {
    if (!obj) return;
    if (isa(obj) == g_malloc_block) {
        block_release(obj);
        return;
    }
    if (is_static(obj)) return;
    u32 before = header(obj)->rc.fetch_sub(1, std::memory_order_acq_rel);
    if (before == 1) send(obj, "dealloc");
    else if (before == 0) LOG_WARN("over-release of %s 0x%llx", class_name(isa(obj)).c_str(), (unsigned long long)obj);
}

id autorelease(id obj) {
    if (!obj) return obj;
    if (is_static(obj) && isa(obj) != g_malloc_block) return obj;
    pool().push_back(obj);
    return obj;
}

u64 retain_count(id obj) { return is_static(obj) ? ~0ull >> 1 : header(obj)->rc.load(); }

HostBag* host_bag(id obj, bool create) {
    if (is_static(obj)) return nullptr;
    Header* h = header(obj);
    if (!h->host && create) h->host = new HostBag;
    return h->host;
}

void set_host_data(id obj, std::unique_ptr<HostData> d) {
    HostBag* bag = host_bag(obj, true);
    if (!bag) fatal("set_host_data on static object");
    const HostData& ref = *d;
    std::erase_if(*bag, [&](const std::unique_ptr<HostData>& e) { return typeid(*e) == typeid(ref); });
    bag->push_back(std::move(d));
}

u64 pool_push() { return pool().size() + 1; }
void pool_pop(u64 token) {
    auto& p = pool();
    if (!token) return;
    while (p.size() >= token) {
        id obj = p.back();
        p.pop_back();
        release(obj);
    }
}

namespace {

// Final teardown for objects we allocated (NSObject -dealloc).
void destroy_object(id obj) {
    if (is_static(obj)) return;
    std::vector<std::pair<id, u64>> assoc;
    {
        std::lock_guard lock(g_side_mutex);
        auto w = g_weak.find(obj);
        if (w != g_weak.end()) {
            for (GuestAddr loc : w->second)
                if (*gptr<u64>(loc) == obj) *gptr<u64>(loc) = 0;
            g_weak.erase(w);
        }
        auto a = g_assoc.find(obj);
        if (a != g_assoc.end()) {
            for (auto& [k, v] : a->second) assoc.push_back(v);
            g_assoc.erase(a);
        }
    }
    for (auto& [v, policy] : assoc)
        if (policy & 3) release(v);
    Header* h = header(obj);
    delete h->host;
    h->magic = 0;
    std::free(h);
}

void ensure_initialized(ClassInfo* ci) {
    if (ci->meta) ci = ci->partner;
    if (ci->init_state.load(std::memory_order_acquire) == 2) return;
    static std::recursive_mutex init_mutex;
    std::lock_guard lock(init_mutex);
    if (ci->init_state == 2 || (ci->init_state == 1 && ci->init_thread == GetCurrentThreadId())) return;
    ci->init_state = 1;
    ci->init_thread = GetCurrentThreadId();
    if (ci->super) ensure_initialized(ci->super);
    if (lookup_imp(ci->partner->cls, sel("initialize"))) send(ci->cls, "initialize");
    ci->init_state.store(2, std::memory_order_release);
}

[[noreturn]] void unrecognized_fatal(cpu::Thread& t, id self, SEL s) { (void)t; (void)self; (void)s; fatal("unreachable"); }

std::mutex g_unrec_mutex;
std::unordered_set<std::string> g_unrec_seen;

void unrecognized(cpu::Thread& t, id self, SEL s) {
    Class c = isa(self);
    ClassInfo* ci = info(c);
    std::string key = std::string(ci && ci->meta ? "+[" : "-[") + (ci ? ci->name : "?") + " " + sel_name(s) + "]";
    bool first;
    {
        std::lock_guard lock(g_unrec_mutex);
        first = g_unrec_seen.insert(key).second;
    }
    if (first) LOG_WARN("unrecognized selector %s (from %s)", key.c_str(), cpu::symbolize(t.x(30)).c_str());
    t.set_x(0, 0);
    t.set_x(1, 0);
    for (int i = 0; i < 4; i++) t.set_v(i, {0, 0});
}

void dispatch(cpu::Thread& t, id self, SEL s, Class start) {
    ClassInfo* ci = info(start);
    if (!ci) {
        // A message to a freed or corrupt object. iOS would crash; answer like a message to nil
        // instead, so a stray message (often UI) does not end the player's game.
        LOG_ERROR("message %s to object 0x%llx with invalid isa 0x%llx; treated as nil\n%s", sel_name(s),
                  (unsigned long long)self, (unsigned long long)start, t.backtrace().c_str());
        t.set_x(0, 0);
        t.set_x(1, 0);
        for (int i = 0; i < 4; i++) t.set_v(i, {0, 0});
        return;
    }
    if (ci->meta) ensure_initialized(ci);
    GuestAddr imp = lookup_imp(start, s);
    if (!imp) {
        unrecognized(t, self, s);
        return;
    }
    t.set_x(0, self);
    t.jump(imp);
}

// objc_msgSend: nil receivers return zero in every return register.
void msg_send(cpu::Thread& t) {
    id self = t.x(0);
    if (!self) {
        t.set_x(1, 0);
        for (int i = 0; i < 4; i++) t.set_v(i, {0, 0});
        return;
    }
    dispatch(t, self, t.x(1), isa(self));
}

void msg_send_super2(cpu::Thread& t) {
    GuestAddr sup = t.x(0);
    id self = gptr<u64>(sup)[0];
    Class cur = gptr<u64>(sup)[1];
    ClassInfo* ci = info(cur);
    if (!ci || !ci->super) fatal("objc_msgSendSuper2 with bad class");
    dispatch(t, self, t.x(1), ci->super->cls);
}

}  // namespace

GuestAddr msgsend_stub() { return g_msgsend; }

u64 send_sel(id self, SEL s, std::initializer_list<u64> args) {
    std::vector<u64> all{self, s};
    all.insert(all.end(), args);
    cpu::Thread& t = cpu::current();
    t.call_raw(g_msgsend, [&](cpu::Thread& th) {
        for (size_t i = 0; i < all.size() && i < 8; i++) th.set_x((int)i, all[i]);
    });
    return t.last_x0();
}

u64 send(id self, std::string_view selector, std::initializer_list<u64> args) { return send_sel(self, sel(selector), args); }

u64 send_fp(id self, std::string_view selector, std::initializer_list<u64> args, std::initializer_list<double> fargs) {
    std::vector<u64> all{self, sel(selector)};
    all.insert(all.end(), args);
    std::vector<double> fl(fargs);
    cpu::Thread& t = cpu::current();
    t.call_raw(g_msgsend, [&](cpu::Thread& th) {
        for (size_t i = 0; i < all.size() && i < 8; i++) th.set_x((int)i, all[i]);
        for (size_t i = 0; i < 8; i++) th.set_d((int)i, i < fl.size() ? fl[i] : 0.0);
    });
    return t.last_x0();
}

std::string describe(id obj) {
    if (!obj) return "(null)";
    if (g_is_nsstring && g_is_nsstring(obj)) return g_nsstring_utf8(obj);
    if (g_nsstring_utf8 && responds_to(obj, sel("description"))) {
        id d = send(obj, "description");
        if (d && g_is_nsstring && g_is_nsstring(d)) return g_nsstring_utf8(d);
    }
    char buf[96];
    snprintf(buf, sizeof buf, "<%s: 0x%llx>", class_name(isa(obj)).c_str(), (unsigned long long)obj);
    return buf;
}

// ---------------------------------------------------------------------------
// blocks
namespace {
constexpr s32 BLOCK_REFCOUNT_MASK = 0xfffe, BLOCK_NEEDS_FREE = 1 << 24, BLOCK_HAS_COPY_DISPOSE = 1 << 25,
              BLOCK_IS_GLOBAL = 1 << 28;
struct BlockLayout {
    u64 isa;
    std::atomic<s32> flags;
    s32 reserved;
    u64 invoke;
    u64 descriptor;
};
struct BlockDesc {
    u64 reserved, size, copy, dispose;
};
struct Byref {
    u64 isa;
    u64 forwarding;
    std::atomic<s32> flags;
    u32 size;
    u64 keep, destroy;
};

GuestAddr byref_copy(GuestAddr src_addr) {
    auto* src = gptr<Byref>(src_addr);
    auto* fwd = gptr<Byref>(src->forwarding);
    if ((fwd->flags & BLOCK_REFCOUNT_MASK) == 0) {
        auto* copy = static_cast<Byref*>(std::malloc(src->size));
        copy->isa = 0;
        copy->flags.store(src->flags.load() | BLOCK_NEEDS_FREE | 4);
        copy->forwarding = gaddr(copy);
        src->forwarding = gaddr(copy);
        copy->size = src->size;
        if (src->flags & BLOCK_HAS_COPY_DISPOSE) {
            copy->keep = src->keep;
            copy->destroy = src->destroy;
            // Extended layout info (BLOCK_BYREF_LAYOUT_EXTENDED) sits before the payload.
            size_t hdr = sizeof(Byref) + ((src->flags & (1 << 28)) ? 8 : 0);
            std::memcpy(reinterpret_cast<u8*>(copy) + sizeof(Byref), reinterpret_cast<u8*>(src) + sizeof(Byref),
                        hdr - sizeof(Byref));
            cpu::current().call(src->keep, {gaddr(copy), src_addr});
        } else {
            std::memcpy(reinterpret_cast<u8*>(copy) + 24, reinterpret_cast<u8*>(src) + 24, src->size - 24);
        }
    } else if (fwd->flags & BLOCK_NEEDS_FREE) {
        fwd->flags.fetch_add(2);
    }
    return src->forwarding;
}

void byref_release(GuestAddr addr) {
    auto* b = gptr<Byref>(gptr<Byref>(addr)->forwarding);
    if (!(b->flags & BLOCK_NEEDS_FREE)) return;
    s32 old = b->flags.fetch_sub(2);
    if ((old & BLOCK_REFCOUNT_MASK) == 2) {
        if (b->flags & BLOCK_HAS_COPY_DISPOSE) cpu::current().call(b->destroy, {gaddr(b)});
        std::free(b);
    }
}
}  // namespace

GuestAddr block_copy(GuestAddr addr) {
    if (!addr) return 0;
    auto* b = gptr<BlockLayout>(addr);
    s32 flags = b->flags.load();
    if (flags & BLOCK_NEEDS_FREE) {
        b->flags.fetch_add(2);
        return addr;
    }
    if (flags & BLOCK_IS_GLOBAL) return addr;
    auto* d = gptr<BlockDesc>(b->descriptor);
    auto* nb = static_cast<BlockLayout*>(std::malloc(d->size));
    std::memcpy(static_cast<void*>(nb), b, d->size);
    nb->isa = g_malloc_block;
    nb->flags.store((flags & ~(BLOCK_REFCOUNT_MASK | 1)) | BLOCK_NEEDS_FREE | 2);
    if (flags & BLOCK_HAS_COPY_DISPOSE) cpu::current().call(d->copy, {gaddr(nb), addr});
    return gaddr(nb);
}

void block_release(GuestAddr addr) {
    if (!addr) return;
    auto* b = gptr<BlockLayout>(addr);
    if (!(b->flags & BLOCK_NEEDS_FREE)) return;
    s32 old = b->flags.fetch_sub(2);
    if ((old & BLOCK_REFCOUNT_MASK) == 2) {
        if (b->flags & BLOCK_HAS_COPY_DISPOSE) cpu::current().call(gptr<BlockDesc>(b->descriptor)->dispose, {addr});
        std::free(b);
    }
}

u64 call_block(GuestAddr block, std::initializer_list<u64> args) {
    std::vector<u64> all{block};
    all.insert(all.end(), args);
    cpu::Thread& t = cpu::current();
    GuestAddr invoke = gptr<BlockLayout>(block)->invoke;
    t.call_raw(invoke, [&](cpu::Thread& th) {
        for (size_t i = 0; i < all.size() && i < 8; i++) th.set_x((int)i, all[i]);
    });
    return t.last_x0();
}

// ---------------------------------------------------------------------------
// setup
void init_runtime(const macho::Image& img) {
    g_image = &img;
    g_empty_cache = gaddr(hle::alloc_static(16));
    g_msgsend = cpu::make_stub("objc_msgSend", msg_send);

    // Canonical selectors come from the image so guest selrefs stay valid.
    if (auto* s = img.section("__DATA", "__objc_selrefs")) {
        std::lock_guard lock(g_sel_mutex);
        for (u64 i = 0; i < s->size / 8; i++) {
            GuestAddr name = gptr<u64>(s->addr)[i];
            g_sels.emplace(gptr<char>(name), name);
        }
    }
    if (auto* s = img.section("__TEXT", "__objc_methname")) {
        std::lock_guard lock(g_sel_mutex);
        for (u64 p = s->addr; p < s->addr + s->size;) {
            const char* n = gptr<char>(p);
            size_t len = std::strlen(n);
            if (len) g_sels.emplace(n, p);
            p += len + 1;
        }
    }

    // Root class and block classes exist before anything binds to them.
    host_class("NSObject", "");
    g_stack_block = host_class("__NSStackBlock__", "NSBlock");
    g_global_block = host_class("__NSGlobalBlock__", "NSBlock");
    g_malloc_block = host_class("__NSMallocBlock__", "NSBlock");

    // Unknown external classes resolve to placeholder host classes.
    hle::add_resolver([](const std::string& sym) -> GuestAddr {
        static const std::string kCls = "_OBJC_CLASS_$_", kMeta = "_OBJC_METACLASS_$_";
        if (sym.rfind(kCls, 0) == 0) return host_class(sym.substr(kCls.size()));
        if (sym.rfind(kMeta, 0) == 0) return isa(host_class(sym.substr(kMeta.size())));
        return 0;
    });
}

void realize_image_classes(const macho::Image& img) {
    if (auto* s = img.section("__DATA", "__objc_classlist"))
        for (u64 i = 0; i < s->size / 8; i++) realize_guest(gptr<u64>(s->addr)[i]);
    if (auto* s = img.section("__DATA", "__objc_nlclslist"))
        for (u64 i = 0; i < s->size / 8; i++) realize_guest(gptr<u64>(s->addr)[i]);
    // Categories: name, cls, instanceMethods, classMethods, protocols, instanceProperties
    if (auto* s = img.section("__DATA", "__objc_catlist")) {
        for (u64 i = 0; i < s->size / 8; i++) {
            GuestAddr cat = gptr<u64>(s->addr)[i];
            u64* c = gptr<u64>(cat);
            ClassInfo* ci = info(c[1]);
            if (!ci) ci = realize_guest(c[1]);
            std::unique_lock lock(g_lock);
            add_method_list(ci, c[2]);
            add_method_list(ci->partner, c[3]);
            add_protocol_list(ci, c[4]);
            flush_caches_locked();
        }
    }
    LOG_INFO("objc: %zu classes, %zu selectors", g_by_name.size(), g_sels.size());
}

// Development aid: logs every selector the image's code sends (its __objc_selrefs) that no
// class, host or guest, implements. Those are calls a code path might make that would end as
// "unrecognized selector".
void audit_selectors(const macho::Image& img) {
    std::unordered_set<SEL> implemented;
    {
        std::shared_lock lock(g_lock);
        for (auto& [cls, ci] : g_classes)
            for (auto& [sel, imp] : ci->methods) implemented.insert(sel);
    }
    std::set<std::string> missing;
    if (auto* s = img.section("__DATA", "__objc_selrefs"))
        for (u64 i = 0; i < s->size / 8; i++) {
            SEL sel = gptr<u64>(s->addr)[i];
            if (!implemented.count(sel)) missing.insert(sel_name(sel));
        }
    LOG_INFO("selector audit: %zu selectors sent by the game are implemented nowhere:", missing.size());
    for (auto& m : missing) LOG_INFO("  %s", m.c_str());
}

void run_load_methods() {
    if (!g_image) return;
    if (auto* s = g_image->section("__DATA", "__objc_nlclslist")) {
        for (u64 i = 0; i < s->size / 8; i++) {
            ClassInfo* ci = info(gptr<u64>(s->addr)[i]);
            auto it = ci->partner->methods.find(sel("load"));
            if (it == ci->partner->methods.end()) continue;
            LOG_DEBUG("+[%s load]", ci->name.c_str());
            cpu::current().call(it->second, {ci->cls, sel("load")});
        }
    }
}

// Exposed for objc/functions.cpp
void* runtime_internal_destroy = reinterpret_cast<void*>(&destroy_object);
Class block_class_malloc() { return g_malloc_block; }
Class block_class_stack() { return g_stack_block; }
std::mutex& side_mutex() { return g_side_mutex; }
std::unordered_map<id, std::vector<GuestAddr>>& weak_table() { return g_weak; }
std::unordered_map<id, std::unordered_map<u64, std::pair<id, u64>>>& assoc_table() { return g_assoc; }
std::unordered_map<id, std::recursive_mutex*>& sync_table() { return g_sync; }
void msg_send_handler(cpu::Thread& t) { msg_send(t); }
void msg_send_super2_handler(cpu::Thread& t) { msg_send_super2(t); }
GuestAddr block_copy_byref(GuestAddr a) { return byref_copy(a); }
void block_release_byref(GuestAddr a) { byref_release(a); }
void destroy(id obj) { destroy_object(obj); }
std::vector<GuestAddr> class_protocols(Class c) {
    std::vector<GuestAddr> out;
    for (ClassInfo* ci = info(c); ci; ci = ci->super) {
        out.insert(out.end(), ci->protocols.begin(), ci->protocols.end());
        if (ci->meta) break;
    }
    return out;
}
GuestAddr class_ro(Class c) {
    ClassInfo* ci = info(c);
    return ci ? ci->ro : 0;
}
void add_guest_method(Class c, SEL s, GuestAddr imp) {
    ClassInfo* ci = info(c);
    if (!ci) return;
    std::unique_lock lock(g_lock);
    ci->methods[s] = imp;
    flush_caches_locked();
}

}  // namespace objc
