// Objective-C runtime compatible with Apple's modern arm64 ABI.
//
// Guest classes are read from the Mach-O metadata. Host classes (our Foundation/UIKit
// implementations) get real class_t structures in guest memory so guest code can
// subclass them; their methods are HLE stubs in the same method tables.
//
// Objects we allocate carry a 16-byte header *before* the object pointer holding the
// retain count and optional host-side data.
#pragma once
#include "common.h"
#include "cpu.h"
#include "hle.h"
#include <memory>
#include <string>
#include <vector>

namespace objc {

using id = GuestAddr;
using SEL = GuestAddr;
using Class = GuestAddr;

// Per-object host storage for host classes (NSString contents, NSArray elements, ...).
struct HostData {
    virtual ~HostData() = default;
};

// --- setup ---
void init_runtime(const macho::Image& img);  // selectors, classes, categories (before binds)
void run_load_methods();                      // +load (before C++ static initializers)
void install_runtime_functions();             // objc_* / sel_* / Block_* HLE exports

// --- selectors ---
SEL sel(std::string_view name);
const char* sel_name(SEL s);

// --- classes ---
Class isa(id obj);
Class class_named(std::string_view name);   // 0 if unknown
std::string class_name(Class cls);
Class superclass(Class cls);
bool is_metaclass(Class cls);
bool is_kind_of(id obj, Class cls);
bool class_is_subclass(Class cls, Class parent);
u32 instance_size(Class cls);
// Returns the host class `name`, creating it (subclass of `super`) if needed.
Class host_class(std::string_view name, std::string_view super = "NSObject");

// Adds a raw method implementation. Handler sees x0 = self, x1 = _cmd, args after.
void add_method(Class cls, std::string_view selector, cpu::Handler h, bool class_method = false);
// Marshalled variants: F is a captureless lambda taking (id self, SEL cmd, args...).
template <typename F>
void method(Class cls, std::string_view selector, F f) {
    add_method(cls, selector, hle::wrap(+f), false);
}
template <typename F>
void class_method(Class cls, std::string_view selector, F f) {
    add_method(cls, selector, hle::wrap(+f), true);
}
// Replaces a guest method's implementation (used to neutralize SDKs we don't support).
void override_method(std::string_view class_name, std::string_view selector, cpu::Handler h, bool class_method = false);

// Makes every class method defined by guest class `name` a no-op returning 0 (analytics SDKs etc.).
void stub_out_class_methods(std::string_view name);

// Looks up the IMP a message would reach (0 if unrecognized).
GuestAddr lookup_imp(Class cls, SEL sel);
bool responds_to(id obj, SEL sel);

// --- objects ---
id alloc(Class cls);
id retain(id obj);
void release(id obj);
id autorelease(id obj);
u64 retain_count(id obj);
bool is_static(id obj);  // objects without our header (image constants, blocks, classes)
// Host data records attached to obj: one per type, so every class in a hierarchy
// (e.g. UIView and UILabel) can keep its own state. nullptr for static objects.
using HostBag = std::vector<std::unique_ptr<HostData>>;
HostBag* host_bag(id obj, bool create);
// Replaces any existing record of the same dynamic type.
void set_host_data(id obj, std::unique_ptr<HostData> d);
template <typename T>
T* get(id obj) {
    HostBag* bag = host_bag(obj, false);
    if (!bag) return nullptr;
    for (auto& p : *bag)
        if (T* t = dynamic_cast<T*>(p.get())) return t;
    return nullptr;
}
// Returns T data, creating a default one if absent.
template <typename T>
T& ensure(id obj) {
    if (T* t = get<T>(obj)) return *t;
    HostBag* bag = host_bag(obj, true);
    if (!bag) fatal("host data on a static object");
    bag->push_back(std::make_unique<T>());
    return static_cast<T&>(*bag->back());
}

// --- messaging from host code ---
GuestAddr msgsend_stub();
u64 send(id self, std::string_view selector, std::initializer_list<u64> args = {});
u64 send_sel(id self, SEL s, std::initializer_list<u64> args = {});
// Also passes floating-point arguments in d0.. (e.g. CGRect/CGPoint/CGFloat parameters).
u64 send_fp(id self, std::string_view selector, std::initializer_list<u64> args, std::initializer_list<double> fargs);

// Autorelease pool helpers for host-driven loops.
u64 pool_push();
void pool_pop(u64 token);

// --- blocks ---
// Invokes a guest block with integer arguments.
u64 call_block(GuestAddr block, std::initializer_list<u64> args = {});
GuestAddr block_copy(GuestAddr block);
void block_release(GuestAddr block);

// Describes an object for logging / %@ (calls -description for non-strings).
std::string describe(id obj);

}  // namespace objc
