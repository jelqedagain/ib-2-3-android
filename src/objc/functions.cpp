// libobjc exports, the blocks runtime, and NSObject's root methods.
#include "objc/internal.h"
#include "libc/format.h"

namespace objc {

namespace {

// protocol_t: isa, name, protocols, instanceMethods, classMethods, optionalInstanceMethods,
//             optionalClassMethods, instanceProperties, size, flags
struct GuestProtocol {
    u64 isa, name, protocols, instance_methods, class_methods, opt_instance_methods, opt_class_methods, properties;
};

bool protocol_conforms(GuestAddr p, GuestAddr other) {
    if (!p || !other) return false;
    if (p == other) return true;
    auto* gp = gptr<GuestProtocol>(p);
    auto* go = gptr<GuestProtocol>(other);
    if (std::strcmp(gptr<char>(gp->name), gptr<char>(go->name)) == 0) return true;
    if (gp->protocols) {
        u64 n = *gptr<u64>(gp->protocols);
        for (u64 i = 0; i < n; i++)
            if (protocol_conforms(gptr<u64>(gp->protocols + 8)[i], other)) return true;
    }
    return false;
}

bool class_conforms(Class c, GuestAddr proto) {
    for (GuestAddr p : class_protocols(c))
        if (protocol_conforms(p, proto)) return true;
    return false;
}

void set_ivar(id self, s64 offset, id value, bool atomic, int copy) {
    if (copy == 1) value = send(value, "copy");
    else if (copy == 2) value = send(value, "mutableCopy");
    else value = retain(value);
    static std::mutex prop_mutex;
    id old;
    {
        std::unique_lock<std::mutex> lock(prop_mutex, std::defer_lock);
        if (atomic) lock.lock();
        old = *gptr<u64>(self + offset);
        *gptr<u64>(self + offset) = value;
    }
    release(old);
}

std::recursive_mutex* sync_mutex(id obj) {
    std::lock_guard lock(side_mutex());
    auto& m = sync_table()[obj];
    if (!m) m = new std::recursive_mutex;
    return m;
}

}  // namespace

void install_runtime_functions() {
    using hle::fn;
    hle::data("__objc_empty_cache", gaddr(hle::alloc_static(16)));
    hle::data("__objc_empty_vtable", gaddr(hle::alloc_static(16)));
    hle::data("_OBJC_EHTYPE_id", gaddr(hle::alloc_static(32)));
    hle::data("_OBJC_EHTYPE_$_NSException", gaddr(hle::alloc_static(32)));
    hle::raw("_objc_msgSend", msg_send_handler);
    hle::raw("_objc_msgSendSuper2", msg_send_super2_handler);

    fn("_objc_retain", [](id o) { return retain(o); });
    fn("_objc_release", [](id o) { release(o); });
    fn("_objc_autorelease", [](id o) { return autorelease(o); });
    fn("_objc_retainAutorelease", [](id o) { return autorelease(retain(o)); });
    fn("_objc_autoreleaseReturnValue", [](id o) { return autorelease(o); });
    fn("_objc_retainAutoreleaseReturnValue", [](id o) { return autorelease(retain(o)); });
    fn("_objc_retainAutoreleasedReturnValue", [](id o) { return retain(o); });
    fn("_objc_retainBlock", [](id b) { return block_copy(b); });
    fn("_objc_storeStrong", [](u64* loc, id v) {
        id old = *loc;
        *loc = retain(v);
        release(old);
    });
    fn("_objc_autoreleasePoolPush", []() { return pool_push(); });
    fn("_objc_autoreleasePoolPop", [](u64 t) { pool_pop(t); });

    // weak references
    fn("_objc_storeWeak", [](GuestAddr loc, id v) {
        std::lock_guard lock(side_mutex());
        auto& wt = weak_table();
        id old = *gptr<u64>(loc);
        if (old) {
            auto it = wt.find(old);
            if (it != wt.end()) std::erase(it->second, loc);
        }
        if (v && !is_static(v)) wt[v].push_back(loc);
        *gptr<u64>(loc) = v;
        return v;
    });
    fn("_objc_loadWeakRetained", [](GuestAddr loc) {
        std::lock_guard lock(side_mutex());
        return retain(*gptr<u64>(loc));
    });
    fn("_objc_destroyWeak", [](GuestAddr loc) {
        std::lock_guard lock(side_mutex());
        id old = *gptr<u64>(loc);
        if (old) {
            auto it = weak_table().find(old);
            if (it != weak_table().end()) std::erase(it->second, loc);
        }
        *gptr<u64>(loc) = 0;
    });

    // associated objects (policy: 0 assign, 1/0x301 retain, 3/0x303 copy)
    fn("_objc_setAssociatedObject", [](id obj, u64 key, id value, u64 policy) {
        if (policy & 3) value = (policy & 2) ? send(value, "copy") : retain(value);
        id old = 0;
        u64 old_policy = 0;
        {
            std::lock_guard lock(side_mutex());
            auto& m = assoc_table()[obj];
            auto it = m.find(key);
            if (it != m.end()) std::tie(old, old_policy) = it->second;
            if (value) m[key] = {value, policy};
            else m.erase(key);
        }
        if (old && (old_policy & 3)) release(old);
    });
    fn("_objc_getAssociatedObject", [](id obj, u64 key) -> id {
        std::lock_guard lock(side_mutex());
        auto a = assoc_table().find(obj);
        if (a == assoc_table().end()) return 0;
        auto it = a->second.find(key);
        return it == a->second.end() ? 0 : it->second.first;
    });

    // properties
    fn("_objc_getProperty", [](id self, SEL, s64 offset, bool atomic) -> id {
        id v = *gptr<u64>(self + offset);
        return atomic ? autorelease(retain(v)) : v;
    });
    fn("_objc_setProperty", [](id self, SEL, s64 offset, id v, bool atomic, s8 copy) { set_ivar(self, offset, v, atomic, copy); });
    fn("_objc_setProperty_atomic", [](id self, SEL, id v, s64 off) { set_ivar(self, off, v, true, 0); });
    fn("_objc_setProperty_nonatomic", [](id self, SEL, id v, s64 off) { set_ivar(self, off, v, false, 0); });
    fn("_objc_setProperty_atomic_copy", [](id self, SEL, id v, s64 off) { set_ivar(self, off, v, true, 1); });
    fn("_objc_setProperty_nonatomic_copy", [](id self, SEL, id v, s64 off) { set_ivar(self, off, v, false, 1); });
    fn("_objc_copyStruct", [](void* dst, const void* src, s64 size, bool, bool) { std::memmove(dst, src, size); });

    // synchronization
    fn("_objc_sync_enter", [](id obj) {
        if (obj) sync_mutex(obj)->lock();
        return 0;
    });
    fn("_objc_sync_exit", [](id obj) {
        if (obj) sync_mutex(obj)->unlock();
        return 0;
    });

    // exceptions: we cannot unwind through JIT frames, so a throw is fatal (with details).
    fn("_objc_exception_throw", [](cpu::Thread& t, id exc) {
        std::string name = describe(send(exc, "name")), reason = describe(send(exc, "reason"));
        fatal("Objective-C exception %s: %s\n%s", name.c_str(), reason.c_str(), t.backtrace().c_str());
    });
    fn("_objc_exception_rethrow", [](cpu::Thread& t) { fatal("objc_exception_rethrow\n%s", t.backtrace().c_str()); });
    fn("_objc_begin_catch", [](u64 e) { return e; });
    fn("_objc_end_catch", []() {});
    fn("_objc_terminate", [](cpu::Thread& t) { fatal("objc_terminate\n%s", t.backtrace().c_str()); });
    fn("_objc_enumerationMutation", [](cpu::Thread& t, id obj) {
        LOG_WARN("collection 0x%llx mutated during enumeration\n%s", (unsigned long long)obj, t.backtrace().c_str());
    });

    // introspection
    fn("_objc_getClass", [](const char* name) { return class_named(name); });
    fn("_class_getSuperclass", [](Class c) { return superclass(c); });
    fn("_sel_getName", [](SEL s) { return sel_name(s); });
    fn("_sel_registerName", [](const char* n) { return sel(n); });
    fn("_class_addMethod", [](Class c, SEL s, GuestAddr imp, const char*) {
        add_guest_method(c, s, imp);
        return true;
    });
    fn("_class_copyPropertyList", [](Class c, u32* out_count) -> u64* {
        GuestAddr ro = class_ro(c);
        GuestAddr list = ro ? gptr<u64>(ro)[8] : 0;  // baseProperties
        u32 n = list ? *gptr<u32>(list + 4) : 0;
        u32 entsize = list ? *gptr<u32>(list) : 16;
        if (out_count) *out_count = n;
        if (!n) return nullptr;
        auto* arr = static_cast<u64*>(std::calloc(n + 1, 8));
        for (u32 i = 0; i < n; i++) arr[i] = list + 8 + (u64)i * entsize;
        return arr;
    });
    fn("_property_getName", [](GuestAddr p) { return gptr<u64>(p)[0]; });
    fn("_property_getAttributes", [](GuestAddr p) { return gptr<u64>(p)[1]; });
    fn("_protocol_conformsToProtocol", [](GuestAddr p, GuestAddr o) { return protocol_conforms(p, o); });
    fn("_protocol_copyProtocolList", [](GuestAddr p, u32* out_count) -> u64* {
        auto* gp = gptr<GuestProtocol>(p);
        u64 n = gp->protocols ? *gptr<u64>(gp->protocols) : 0;
        if (out_count) *out_count = (u32)n;
        if (!n) return nullptr;
        auto* arr = static_cast<u64*>(std::calloc(n + 1, 8));
        std::memcpy(arr, gptr<void>(gp->protocols + 8), n * 8);
        return arr;
    });
    fn("_protocol_copyMethodDescriptionList", [](GuestAddr p, bool required, bool instance, u32* out_count) -> u64* {
        auto* gp = gptr<GuestProtocol>(p);
        GuestAddr list = required ? (instance ? gp->instance_methods : gp->class_methods)
                                  : (instance ? gp->opt_instance_methods : gp->opt_class_methods);
        u32 n = list ? *gptr<u32>(list + 4) : 0;
        u32 entsize = list ? (*gptr<u32>(list) & ~3u) : 24;
        if (out_count) *out_count = n;
        if (!n) return nullptr;
        auto* arr = static_cast<u64*>(std::calloc(n + 1, 16));
        for (u32 i = 0; i < n; i++) {
            u64* m = gptr<u64>(list + 8 + (u64)i * entsize);
            arr[i * 2] = sel(gptr<char>(m[0]));
            arr[i * 2 + 1] = m[1];
        }
        return arr;
    });

    // blocks runtime
    fn("__Block_copy", [](GuestAddr b) { return block_copy(b); });
    fn("__Block_release", [](GuestAddr b) { block_release(b); });
    fn("__Block_object_assign", [](u64* dst, GuestAddr obj, int flags) {
        switch (flags & 0x9f) {
        case 3: *dst = retain(obj); break;                  // BLOCK_FIELD_IS_OBJECT
        case 7: *dst = block_copy(obj); break;              // BLOCK_FIELD_IS_BLOCK
        case 8: case 24: *dst = block_copy_byref(obj); break;  // BYREF (| WEAK)
        default: *dst = obj; break;                         // BLOCK_BYREF_CALLER variants
        }
    });
    fn("__Block_object_dispose", [](GuestAddr obj, int flags) {
        switch (flags & 0x9f) {
        case 3: release(obj); break;
        case 7: block_release(obj); break;
        case 8: case 24: block_release_byref(obj); break;
        default: break;
        }
    });
    hle::data("__NSConcreteStackBlock", block_class_stack());
    hle::data("__NSConcreteGlobalBlock", class_named("__NSGlobalBlock__"));
    hle::data("__NSConcreteMallocBlock", block_class_malloc());

    // ---- NSObject ----
    Class o = class_named("NSObject");
    class_method(o, "alloc", [](Class c, SEL) { return alloc(c); });
    class_method(o, "allocWithZone:", [](Class c, SEL, u64) { return alloc(c); });
    class_method(o, "new", [](Class c, SEL) { return send(alloc(c), "init"); });
    class_method(o, "initialize", [](Class, SEL) {});
    class_method(o, "load", [](Class, SEL) {});
    class_method(o, "class", [](Class c, SEL) { return c; });
    class_method(o, "superclass", [](Class c, SEL) { return superclass(c); });
    class_method(o, "isSubclassOfClass:", [](Class c, SEL, Class p) { return class_is_subclass(c, p); });
    class_method(o, "instancesRespondToSelector:", [](Class c, SEL, SEL s) { return lookup_imp(c, s) != 0; });
    class_method(o, "instanceMethodForSelector:", [](Class c, SEL, SEL s) { return lookup_imp(c, s); });
    class_method(o, "conformsToProtocol:", [](Class c, SEL, GuestAddr p) { return class_conforms(c, p); });
    class_method(o, "description", [](Class c, SEL) { return send(class_named("NSString"), "stringWithUTF8String:", {gaddr(hle::static_cstr(class_name(c)))}); });

    method(o, "init", [](id self, SEL) { return self; });
    method(o, "dealloc", [](id self, SEL) { destroy(self); });
    method(o, "retain", [](id self, SEL) { return retain(self); });
    method(o, "release", [](id self, SEL) { release(self); });
    method(o, "autorelease", [](id self, SEL) { return autorelease(self); });
    method(o, "retainCount", [](id self, SEL) { return retain_count(self); });
    method(o, "class", [](id self, SEL) { return is_metaclass(isa(self)) ? self : isa(self); });
    method(o, "superclass", [](id self, SEL) { return superclass(isa(self)); });
    method(o, "self", [](id self, SEL) { return self; });
    method(o, "zone", [](id, SEL) -> u64 { return 0; });
    method(o, "isProxy", [](id, SEL) { return false; });
    method(o, "hash", [](id self, SEL) -> u64 { return self >> 4; });
    method(o, "isEqual:", [](id self, SEL, id other) { return self == other; });
    method(o, "isKindOfClass:", [](id self, SEL, Class c) {
        Class k = is_metaclass(isa(self)) ? isa(self) : isa(self);
        return class_is_subclass(k, c);
    });
    method(o, "isMemberOfClass:", [](id self, SEL, Class c) { return isa(self) == c; });
    method(o, "respondsToSelector:", [](id self, SEL, SEL s) { return responds_to(self, s); });
    method(o, "conformsToProtocol:", [](id self, SEL, GuestAddr p) { return class_conforms(isa(self), p); });
    method(o, "methodForSelector:", [](id self, SEL, SEL s) { return lookup_imp(isa(self), s); });
    method(o, "copy", [](id self, SEL) { return send(self, "copyWithZone:", {0}); });
    method(o, "mutableCopy", [](id self, SEL) { return send(self, "mutableCopyWithZone:", {0}); });
    method(o, "performSelector:", [](id self, SEL, SEL s) { return send_sel(self, s); });
    method(o, "performSelector:withObject:", [](id self, SEL, SEL s, id a) { return send_sel(self, s, {a}); });
    method(o, "performSelector:withObject:withObject:", [](id self, SEL, SEL s, id a, id b) { return send_sel(self, s, {a, b}); });
    method(o, "forwardingTargetForSelector:", [](id, SEL, SEL) -> id { return 0; });
    method(o, "description", [](id self, SEL) {
        char buf[96];
        snprintf(buf, sizeof buf, "<%s: 0x%llx>", class_name(isa(self)).c_str(), (unsigned long long)self);
        return send(class_named("NSString"), "stringWithUTF8String:", {gaddr(hle::static_cstr(buf))});
    });
    method(o, "debugDescription", [](id self, SEL) { return send(self, "description"); });

    // Blocks as objects.
    Class nsblock = class_named("NSBlock");
    method(nsblock, "copy", [](id self, SEL) { return block_copy(self); });
    method(nsblock, "copyWithZone:", [](id self, SEL, u64) { return block_copy(self); });
    method(nsblock, "retain", [](id self, SEL) { return retain(self); });
    method(nsblock, "release", [](id self, SEL) { release(self); });
    method(nsblock, "autorelease", [](id self, SEL) { return self; });
    method(nsblock, "invoke", [](id self, SEL) { call_block(self); });
}

}  // namespace objc
