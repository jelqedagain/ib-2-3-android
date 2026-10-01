// UIApplication, UIScreen, UIDevice, UIResponder, UIView, UIWindow, UIViewController, CALayer,
// CAEAGLLayer, UITouch, UIEvent. The window behind the screen is platform code (window_win32.cpp).
#include "uikit/uikit.h"
#include "settings.h"
#include "libc/vfs.h"
#include "macho.h"
#include "foundation/foundation.h"
#include "foundation/runloop.h"
#include "gles/gl.h"
#include "objc/internal.h"
#include <atomic>
#include <map>
#include <cmath>

namespace libc {
extern std::string g_device_model;
extern u64 g_device_memory;
}  // namespace libc

namespace uikit {

bool g_test_mode = false;
DeviceProfile g_device = {"iPhone7,1", "iPhone", 0, 736, 414, 3.0, 2.608};

namespace {
using namespace ns;
using objc::Class;
using objc::id;
using objc::SEL;

// ---------------------------------------------------------------------------
struct ViewData : objc::HostData {
    CGRect frame{};
    CGRect bounds{};
    id superview = 0;
    std::vector<id> subviews;
    id layer = 0;
    id controller = 0;  // view controller whose root view this is
    double scale = 1.0;
    double alpha = 1.0;
    bool hidden = false, interaction = true, multitouch = false;
    s64 tag = 0;
    u64 autoresizing = 0;
    bool autoresizes_subviews = true;
    id background = 0;
};
struct LayerData : objc::HostData {
    id view = 0;
    double contents_scale = 0;  // 0 = follow the view
    id drawable_properties = 0;
    CGRect frame{};
};
struct ControllerData : objc::HostData {
    id view = 0;
    id presented = 0, presenting = 0;
    id title = 0;
};
struct TouchData : objc::HostData {
    id view = 0, window = 0;  // retained, like UIKit: the touch keeps its view even if it is removed
    CGPoint loc{}, prev{};
    s64 phase = 0;
    u64 taps = 1;
    double timestamp = 0;
    ~TouchData() override {
        objc::release(view);
        objc::release(window);
    }
};
struct EventData : objc::HostData {
    std::vector<id> touches;
    double timestamp = 0;
};

Class g_UIView, g_UIWindow, g_UIViewController, g_CALayer, g_CAEAGLLayer, g_UITouch, g_UIEvent, g_UIApplication, g_UIScreen,
    g_UIDevice, g_UIResponder;
id g_app = 0, g_delegate = 0, g_key_window = 0, g_screen = 0, g_device_obj = 0, g_screen_mode = 0;
std::vector<id> g_windows;
std::atomic<u64> g_frames{0};

ViewData& vd(id v) { return objc::ensure<ViewData>(v); }
LayerData& ld(id l) { return objc::ensure<LayerData>(l); }
ControllerData& cd(id c) { return objc::ensure<ControllerData>(c); }

CGRect screen_bounds() { return {{0, 0}, {g_device.width_pt, g_device.height_pt}}; }

// Window-coordinate origin of a view (sum of frame origins minus bounds origins up the chain).
CGPoint window_origin(id v) {
    CGPoint p{0, 0};
    while (v) {
        auto& d = vd(v);
        p.x += d.frame.origin.x - d.bounds.origin.x;
        p.y += d.frame.origin.y - d.bounds.origin.y;
        if (objc::is_kind_of(v, g_UIWindow)) break;
        v = d.superview;
    }
    return p;
}

void set_frame(id v, CGRect f);

// UIViewAutoresizing: 1 flexible left margin, 2 width, 4 right margin, 8 top margin, 16 height, 32 bottom margin.
void autoresize_subviews(id v, CGSize old_size, CGSize new_size) {
    double dw = new_size.width - old_size.width, dh = new_size.height - old_size.height;
    if (dw == 0 && dh == 0) return;
    for (id sub : std::vector<id>(vd(v).subviews)) {
        auto& sd = vd(sub);
        u64 m = sd.autoresizing;
        if (!m) continue;
        CGRect f = sd.frame;
        auto distribute = [](double delta, bool a, bool b, bool c, double& pa, double& pb) {
            int n = (int)a + (int)b + (int)c;
            if (!n) return;
            double share = delta / n;
            if (a) pa += share;
            if (b) pb += share;
        };
        double dx = 0, dwid = 0, dy = 0, dhei = 0;
        distribute(dw, m & 1, m & 2, m & 4, dx, dwid);
        distribute(dh, m & 8, m & 16, m & 32, dy, dhei);
        f.origin.x += dx;
        f.size.width += dwid;
        f.origin.y += dy;
        f.size.height += dhei;
        set_frame(sub, f);
    }
}

void set_frame(id v, CGRect f) {
    auto& d = vd(v);
    CGSize old = d.bounds.size;
    d.frame = f;
    d.bounds.size = f.size;
    if (d.layer) ld(d.layer).frame = f;
    if (d.autoresizes_subviews) autoresize_subviews(v, old, f.size);
}

id view_window(id v) {
    while (v && !objc::is_kind_of(v, g_UIWindow)) v = vd(v).superview;
    return v;
}

void layout_tree(id v) {
    objc::send(v, "layoutSubviews");
    for (id s : std::vector<id>(vd(v).subviews)) layout_tree(s);
}

void schedule_layout(id v) {
    id w = view_window(v);
    if (!w) return;
    objc::retain(w);
    post_to_main([w] {
        layout_tree(w);
        objc::release(w);
    });
}

void add_subview(id parent, id child, s64 index = -1) {
    if (!child || child == parent) return;
    objc::retain(child);
    if (id old = vd(child).superview) {
        auto& subs = vd(old).subviews;
        std::erase(subs, child);
        objc::release(child);
    }
    auto& subs = vd(parent).subviews;
    if (index < 0 || (size_t)index > subs.size()) subs.push_back(child);
    else subs.insert(subs.begin() + index, child);
    vd(child).superview = parent;
    objc::send(child, "didMoveToSuperview");
    if (view_window(child)) {
        objc::send(child, "didMoveToWindow");
        schedule_layout(child);
    }
}

id hit_test(id v, CGPoint p /* in v's superview coordinates */) {
    auto& d = vd(v);
    if (d.hidden || !d.interaction || d.alpha < 0.01) return 0;
    CGPoint local{p.x - d.frame.origin.x + d.bounds.origin.x, p.y - d.frame.origin.y + d.bounds.origin.y};
    if (!objc::is_kind_of(v, g_UIWindow) &&
        (local.x < d.bounds.origin.x || local.y < d.bounds.origin.y || local.x >= d.bounds.origin.x + d.bounds.size.width ||
         local.y >= d.bounds.origin.y + d.bounds.size.height))
        return 0;
    for (auto it = d.subviews.rbegin(); it != d.subviews.rend(); ++it)
        if (id h = hit_test(*it, local)) return h;
    return v;
}

id make_touch_set(id touch) { return objc::send(objc::class_named("NSSet"), "setWithObject:", {touch}); }

// ---------------------------------------------------------------------------
// Touches: each "finger" (mouse = 0, key bindings / scripts use others) maps to one UITouch.
std::map<int, id> g_touches;
std::atomic<int> g_touch_count{0};  // g_touches.size(), for other threads
id g_event = 0;

// phase: 0 began, 1 moved, 2 ended
void deliver_touch(int finger, s64 phase, CGPoint p) {
    if (!g_key_window) return;
    auto found = g_touches.find(finger);
    if (phase != 0 && found == g_touches.end()) return;
    const char* sel = phase == 0 ? "touchesBegan:withEvent:" : phase == 1 ? "touchesMoved:withEvent:" : "touchesEnded:withEvent:";
    u64 pool = objc::pool_push();
    if (phase == 0) {
        if (found != g_touches.end()) deliver_touch(finger, 2, p);  // a finger cannot go down twice
        id touch = objc::alloc(g_UITouch);
        auto& t = objc::ensure<TouchData>(touch);
        t.window = objc::retain(g_key_window);
        t.view = objc::retain(hit_test(g_key_window, p));
        t.loc = t.prev = p;
        g_touches[finger] = touch;
        g_touch_count = (int)g_touches.size();
    }
    id touch = g_touches[finger];
    auto& t = objc::ensure<TouchData>(touch);
    t.prev = t.loc;
    t.loc = p;
    t.phase = phase == 0 ? 0 : phase == 1 ? 1 : 3;  // Began, Moved, Ended
    t.timestamp = (double)GetTickCount64() / 1000.0;
    if (!g_event) g_event = objc::alloc(g_UIEvent);
    auto& e = objc::ensure<EventData>(g_event);
    e.touches.clear();
    for (auto& [f, other] : g_touches) e.touches.push_back(other);
    e.timestamp = t.timestamp;
    if (t.view) objc::send(t.view, sel, {make_touch_set(touch), g_event});
    if (phase == 2) {
        g_touches.erase(finger);
        g_touch_count = (int)g_touches.size();
        objc::release(touch);
    }
    objc::pool_pop(pool);
}

id view_controller_for(id v) { return vd(v).controller; }

void dump_views(id v, int depth) {
    auto& d = vd(v);
    LOG_INFO("%*s%s 0x%llx frame=(%.0f,%.0f %.0fx%.0f) bounds=(%.0fx%.0f)%s", depth * 2, "", objc::class_name(objc::isa(v)).c_str(),
             (unsigned long long)v, d.frame.origin.x, d.frame.origin.y, d.frame.size.width, d.frame.size.height, d.bounds.size.width,
             d.bounds.size.height, d.controller ? (" vc=" + objc::class_name(objc::isa(d.controller))).c_str() : "");
    for (id s : d.subviews) dump_views(s, depth + 1);
}

}  // namespace

void collect_labels(std::vector<std::pair<id, CGRect>>& out) {
    if (!g_key_window) return;
    Class label_cls = objc::class_named("UILabel");
    std::vector<std::pair<id, CGPoint>> stack{{g_key_window, CGPoint{0, 0}}};
    while (!stack.empty()) {
        auto [v, parent_origin] = stack.back();
        stack.pop_back();
        auto& d = vd(v);
        if (d.hidden || d.alpha < 0.01) continue;
        CGPoint origin = objc::is_kind_of(v, g_UIWindow)
                             ? CGPoint{0, 0}
                             : CGPoint{parent_origin.x + d.frame.origin.x, parent_origin.y + d.frame.origin.y};
        if (label_cls && objc::is_kind_of(v, label_cls)) out.push_back({v, CGRect{origin, d.frame.size}});
        CGPoint child_origin{origin.x - d.bounds.origin.x, origin.y - d.bounds.origin.y};
        for (auto it = d.subviews.rbegin(); it != d.subviews.rend(); ++it) stack.push_back({*it, child_origin});
    }
}

void touch_down(int finger, CGPoint p) { deliver_touch(finger, 0, p); }
void touch_move(int finger, CGPoint p) { deliver_touch(finger, 1, p); }
void touch_up(int finger, CGPoint p) { deliver_touch(finger, 2, p); }
bool touch_active(int finger) { return g_touches.count(finger) != 0; }
int touches_down() { return g_touch_count; }

std::atomic<bool> g_app_active{true};
bool app_active() { return g_app_active; }

void app_set_active(bool active) {
    if (g_app_active.exchange(active) == active) return;
    LOG_INFO("app %s", active ? "returns to the foreground" : "goes to the background");
    u64 pool = objc::pool_push();
    id center = objc::send(objc::class_named("NSNotificationCenter"), "defaultCenter");
    auto step = [&](const char* delegate_sel, const char* notification) {
        if (g_delegate && objc::responds_to(g_delegate, objc::sel(delegate_sel))) objc::send(g_delegate, delegate_sel, {g_app});
        objc::send(center, "postNotificationName:object:", {str(notification), g_app});
    };
    if (active) {
        step("applicationWillEnterForeground:", "UIApplicationWillEnterForegroundNotification");
        step("applicationDidBecomeActive:", "UIApplicationDidBecomeActiveNotification");
    } else {
        step("applicationWillResignActive:", "UIApplicationWillResignActiveNotification");
        step("applicationDidEnterBackground:", "UIApplicationDidEnterBackgroundNotification");
    }
    objc::pool_pop(pool);
}

void app_will_terminate() {
    if (g_delegate && objc::responds_to(g_delegate, objc::sel("applicationWillTerminate:")))
        objc::send(g_delegate, "applicationWillTerminate:", {g_app});
}
std::function<void(int, bool)> g_key_handler;

CGRect layer_bounds(id layer) {
    id v = ld(layer).view;
    if (v) return vd(v).bounds;
    return {{0, 0}, ld(layer).frame.size};
}
double layer_scale(id layer) {
    auto& l = ld(layer);
    if (l.contents_scale > 0) return l.contents_scale;
    return l.view ? vd(l.view).scale : 1.0;
}
u64 frames_presented() { return g_frames.load(); }

void on_frame_presented() {
    if (++g_frames == 1) {
        LOG_INFO("first frame presented!");
        end_boot_window();
    }
    if (g_frames % 600 == 0) LOG_INFO("%llu frames presented", (unsigned long long)g_frames.load());
    if (settings::get().show_fps) {  // called on the render thread: post, never send
        static u64 window_start = GetTickCount64(), window_frames = 0;
        window_frames++;
        u64 now = GetTickCount64();
        if (now - window_start >= 1000) {
            show_fps((unsigned)(window_frames * 1000 / (now - window_start)));
            window_start = now;
            window_frames = 0;
        }
    }
}

void install() {
    using objc::class_method;
    using objc::method;
    libc::g_device_model = g_device.machine;
    libc::g_device_memory = 1ull << 30;

    g_UIResponder = objc::host_class("UIResponder");
    g_UIView = objc::host_class("UIView", "UIResponder");
    g_UIWindow = objc::host_class("UIWindow", "UIView");
    g_UIViewController = objc::host_class("UIViewController", "UIResponder");
    g_UIApplication = objc::host_class("UIApplication", "UIResponder");
    g_CALayer = objc::host_class("CALayer");
    g_CAEAGLLayer = objc::host_class("CAEAGLLayer", "CALayer");
    g_UITouch = objc::host_class("UITouch");
    g_UIEvent = objc::host_class("UIEvent");
    g_UIScreen = objc::host_class("UIScreen");
    g_UIDevice = objc::host_class("UIDevice");
    objc::host_class("UIScreenMode");
    objc::host_class("NSUUID");

    // ---------------- UIResponder ----------------
    Class R = g_UIResponder;
    method(R, "nextResponder", [](id, SEL) -> id { return 0; });
    method(R, "becomeFirstResponder", [](id, SEL) { return true; });
    method(R, "resignFirstResponder", [](id, SEL) { return true; });
    method(R, "canBecomeFirstResponder", [](id, SEL) { return false; });
    method(R, "isFirstResponder", [](id, SEL) { return false; });
    for (const char* s : {"touchesBegan:withEvent:", "touchesMoved:withEvent:", "touchesEnded:withEvent:", "touchesCancelled:withEvent:"}) {
        objc::add_method(R, s, [](cpu::Thread& t) {
            // Up the responder chain; like UIKit, stop at an object that is not a responder (the
            // app delegate here is a plain NSObject).
            id next = objc::send(t.x(0), "nextResponder");
            if (next && objc::responds_to(next, t.x(1))) objc::send_sel(next, t.x(1), {t.x(2), t.x(3)});
        });
    }

    // ---------------- UIView ----------------
    Class V = g_UIView;
    class_method(V, "layerClass", [](Class, SEL) { return g_CALayer; });
    auto view_init = [](id self, CGRect f) -> id {
        auto& d = vd(self);
        Class lc = objc::send(objc::isa(self), "layerClass");
        d.layer = objc::send(objc::send(lc, "alloc"), "init");
        ld(d.layer).view = self;
        set_frame(self, f);
        return self;
    };
    static decltype(view_init) s_view_init = view_init;
    objc::add_method(V, "initWithFrame:", [](cpu::Thread& t) {
        LOG_DEBUG("initWithFrame: %s d0-3=(%f %f %f %f) x2=0x%llx", objc::class_name(objc::isa(t.x(0))).c_str(), t.d(0), t.d(1), t.d(2),
                 t.d(3), (unsigned long long)t.x(2));
        static std::atomic<int> label_traces{0};
        if (logging::enabled(logging::Level::Debug) && objc::class_name(objc::isa(t.x(0))) == "UILabel" && label_traces++ < 2)
            LOG_DEBUG("UILabel created from:\n%s", t.backtrace().c_str());
        hle::Args a(t);
        id self = a.get<id>();
        a.get<SEL>();
        t.set_x(0, s_view_init(self, a.get<CGRect>()));
    });
    method(V, "init", [](id self, SEL) { return objc::send_fp(self, "initWithFrame:", {}, {0, 0, 0, 0}); });
    method(V, "initWithCoder:", [](id self, SEL, id) { return s_view_init(self, {}); });
    method(V, "frame", [](id self, SEL) { return vd(self).frame; });
    method(V, "setFrame:", [](id self, SEL, CGRect f) {
        set_frame(self, f);
        schedule_layout(self);
    });
    method(V, "bounds", [](id self, SEL) { return vd(self).bounds; });
    method(V, "setBounds:", [](id self, SEL, CGRect b) {
        auto& d = vd(self);
        d.bounds = b;
        d.frame.size = b.size;
    });
    method(V, "center", [](id self, SEL) {
        auto& f = vd(self).frame;
        return CGPoint{f.origin.x + f.size.width / 2, f.origin.y + f.size.height / 2};
    });
    method(V, "setCenter:", [](id self, SEL, CGPoint c) {
        auto& f = vd(self).frame;
        f.origin = {c.x - f.size.width / 2, c.y - f.size.height / 2};
    });
    method(V, "layer", [](id self, SEL) { return vd(self).layer; });
    method(V, "addSubview:", [](id self, SEL, id v) { add_subview(self, v); });
    method(V, "insertSubview:atIndex:", [](id self, SEL, id v, s64 i) { add_subview(self, v, i); });
    method(V, "insertSubview:aboveSubview:", [](id self, SEL, id v, id other) {
        auto& subs = vd(self).subviews;
        auto it = std::find(subs.begin(), subs.end(), other);
        add_subview(self, v, it == subs.end() ? -1 : (s64)(it - subs.begin()) + 1);
    });
    method(V, "insertSubview:belowSubview:", [](id self, SEL, id v, id other) {
        auto& subs = vd(self).subviews;
        auto it = std::find(subs.begin(), subs.end(), other);
        add_subview(self, v, it == subs.end() ? 0 : (s64)(it - subs.begin()));
    });
    method(V, "removeFromSuperview", [](id self, SEL) {
        id parent = vd(self).superview;
        if (!parent) return;
        std::erase(vd(parent).subviews, self);
        vd(self).superview = 0;
        objc::release(self);
    });
    method(V, "bringSubviewToFront:", [](id self, SEL, id v) {
        auto& subs = vd(self).subviews;
        if (std::erase(subs, v)) subs.push_back(v);
    });
    method(V, "sendSubviewToBack:", [](id self, SEL, id v) {
        auto& subs = vd(self).subviews;
        if (std::erase(subs, v)) subs.insert(subs.begin(), v);
    });
    method(V, "subviews", [](id self, SEL) { return array(vd(self).subviews); });
    method(V, "superview", [](id self, SEL) { return vd(self).superview; });
    method(V, "window", [](id self, SEL) { return view_window(self); });
    method(V, "nextResponder", [](id self, SEL) -> id {
        if (id c = vd(self).controller) return c;
        return vd(self).superview;
    });
    method(V, "isHidden", [](id self, SEL) { return vd(self).hidden; });
    method(V, "setHidden:", [](id self, SEL, bool h) { vd(self).hidden = h; });
    method(V, "alpha", [](id self, SEL) { return vd(self).alpha; });
    method(V, "setAlpha:", [](id self, SEL, double a) { vd(self).alpha = a; });
    method(V, "isUserInteractionEnabled", [](id self, SEL) { return vd(self).interaction; });
    method(V, "setUserInteractionEnabled:", [](id self, SEL, bool b) { vd(self).interaction = b; });
    method(V, "isMultipleTouchEnabled", [](id self, SEL) { return vd(self).multitouch; });
    method(V, "setMultipleTouchEnabled:", [](id self, SEL, bool b) { vd(self).multitouch = b; });
    method(V, "setExclusiveTouch:", [](id, SEL, bool) {});
    method(V, "contentScaleFactor", [](id self, SEL) { return vd(self).scale; });
    method(V, "setContentScaleFactor:", [](id self, SEL, double s) {
        vd(self).scale = s;
        LOG_INFO("%s contentScaleFactor = %.3f", objc::class_name(objc::isa(self)).c_str(), s);
    });
    method(V, "tag", [](id self, SEL) -> s64 { return vd(self).tag; });
    method(V, "setTag:", [](id self, SEL, s64 t) { vd(self).tag = t; });
    method(V, "viewWithTag:", [](id self, SEL, s64 tag) -> id {
        std::vector<id> stack{self};
        while (!stack.empty()) {
            id v = stack.back();
            stack.pop_back();
            if (vd(v).tag == tag) return v;
            for (id s : vd(v).subviews) stack.push_back(s);
        }
        return 0;
    });
    method(V, "backgroundColor", [](id self, SEL) { return vd(self).background; });
    method(V, "setBackgroundColor:", [](id self, SEL, id c) { vd(self).background = objc::retain(c); });
    for (const char* s : {"setOpaque:", "setClipsToBounds:", "setClearsContextBeforeDrawing:"})
        method(V, s, [](id, SEL, bool) {});
    method(V, "setAutoresizingMask:", [](id self, SEL, u64 m) { vd(self).autoresizing = m; });
    method(V, "autoresizingMask", [](id self, SEL) -> u64 { return vd(self).autoresizing; });
    method(V, "setAutoresizesSubviews:", [](id self, SEL, bool b) { vd(self).autoresizes_subviews = b; });
    method(V, "setContentMode:", [](id, SEL, u64) {});
    method(V, "setTransform:", [](id, SEL, const double*) {});
    for (const char* s : {"setNeedsDisplay", "layoutIfNeeded", "sizeToFit", "didMoveToWindow", "didMoveToSuperview", "layoutSubviews",
                          "removeAllGestureRecognizers"})
        method(V, s, [](id, SEL) {});
    method(V, "setNeedsLayout", [](id self, SEL) { schedule_layout(self); });
    method(V, "addGestureRecognizer:", [](id, SEL, id) {});
    method(V, "convertPoint:toView:", [](id self, SEL, CGPoint p, id other) {
        CGPoint a = window_origin(self), b = other ? window_origin(other) : CGPoint{0, 0};
        return CGPoint{p.x + a.x - b.x, p.y + a.y - b.y};
    });
    method(V, "convertPoint:fromView:", [](id self, SEL, CGPoint p, id other) {
        CGPoint a = window_origin(self), b = other ? window_origin(other) : CGPoint{0, 0};
        return CGPoint{p.x + b.x - a.x, p.y + b.y - a.y};
    });
    method(V, "convertRect:toView:", [](id self, SEL, CGRect r, id other) {
        CGPoint a = window_origin(self), b = other ? window_origin(other) : CGPoint{0, 0};
        r.origin.x += a.x - b.x;
        r.origin.y += a.y - b.y;
        return r;
    });
    // Animations run instantly.
    class_method(V, "beginAnimations:context:", [](Class, SEL, id, u64) {});
    class_method(V, "commitAnimations", [](Class, SEL) {});
    class_method(V, "setAnimationDuration:", [](Class, SEL, double) {});
    class_method(V, "setAnimationDelegate:", [](Class, SEL, id) {});
    class_method(V, "setAnimationDidStopSelector:", [](Class, SEL, SEL) {});
    class_method(V, "setAnimationsEnabled:", [](Class, SEL, bool) {});
    class_method(V, "animateWithDuration:animations:", [](Class, SEL, double, GuestAddr b) { objc::call_block(b); });
    class_method(V, "animateWithDuration:animations:completion:", [](Class, SEL, double, GuestAddr b, GuestAddr c) {
        objc::call_block(b);
        if (c) objc::call_block(c, {1});
    });
    class_method(V, "animateWithDuration:delay:options:animations:completion:", [](Class, SEL, double, double, u64, GuestAddr b, GuestAddr c) {
        objc::call_block(b);
        if (c) objc::call_block(c, {1});
    });

    // ---------------- CALayer / CAEAGLLayer ----------------
    Class L = g_CALayer;
    method(L, "bounds", [](id self, SEL) { return layer_bounds(self); });
    method(L, "frame", [](id self, SEL) {
        id v = ld(self).view;
        return v ? vd(v).frame : ld(self).frame;
    });
    method(L, "setFrame:", [](id self, SEL, CGRect f) { ld(self).frame = f; });
    method(L, "setBounds:", [](id self, SEL, CGRect f) { ld(self).frame.size = f.size; });
    method(L, "contentsScale", [](id self, SEL) { return layer_scale(self); });
    method(L, "setContentsScale:", [](id self, SEL, double s) { ld(self).contents_scale = s; });
    method(L, "delegate", [](id self, SEL) { return ld(self).view; });
    for (const char* s : {"setOpaque:", "setMasksToBounds:", "setHidden:"}) method(L, s, [](id, SEL, bool) {});
    for (const char* s : {"setNeedsDisplay", "removeAllAnimations", "removeFromSuperlayer"}) method(L, s, [](id, SEL) {});
    method(L, "addSublayer:", [](id, SEL, id) {});
    method(L, "setBackgroundColor:", [](id, SEL, u64) {});
    method(L, "setCornerRadius:", [](id, SEL, double) {});
    method(L, "setBorderWidth:", [](id, SEL, double) {});
    method(L, "setBorderColor:", [](id, SEL, u64) {});
    method(L, "setAffineTransform:", [](id, SEL, const double*) {});
    method(g_CAEAGLLayer, "setDrawableProperties:", [](id self, SEL, id p) { ld(self).drawable_properties = objc::retain(p); });
    method(g_CAEAGLLayer, "drawableProperties", [](id self, SEL) { return ld(self).drawable_properties; });

    // ---------------- UIWindow ----------------
    Class W = g_UIWindow;
    method(W, "makeKeyAndVisible", [](id self, SEL) {
        g_key_window = self;
        if (std::find(g_windows.begin(), g_windows.end(), self) == g_windows.end()) g_windows.push_back(objc::retain(self));
        LOG_INFO("window made key and visible");
        objc::retain(self);
        post_to_main([self] {
            LOG_INFO("view hierarchy:");
            dump_views(self, 1);
            objc::release(self);
        });
        if (id vc = objc::send(self, "rootViewController")) {
            objc::send(vc, "viewWillAppear:", {0});
            objc::send(vc, "viewDidAppear:", {0});
        }
        schedule_layout(self);
    });
    method(W, "makeKeyWindow", [](id self, SEL) { g_key_window = self; });
    method(W, "isKeyWindow", [](id self, SEL) { return g_key_window == self; });
    method(W, "setWindowLevel:", [](id, SEL, double) {});
    method(W, "screen", [](id, SEL) { return objc::send(g_UIScreen, "mainScreen"); });
    method(W, "setScreen:", [](id, SEL, id) {});
    method(W, "rootViewController", [](id self, SEL) { return cd(self).presented; });  // reuse slot
    method(W, "setRootViewController:", [](id self, SEL, id vc) {
        cd(self).presented = objc::retain(vc);
        id v = objc::send(vc, "view");
        if (v) {
            set_frame(v, vd(self).bounds);
            add_subview(self, v);
        }
    });
    method(W, "nextResponder", [](id, SEL) { return g_app; });

    // ---------------- UIViewController ----------------
    Class VC = g_UIViewController;
    method(VC, "init", [](id self, SEL) { return self; });
    method(VC, "initWithNibName:bundle:", [](id self, SEL, id, id) { return self; });
    method(VC, "view", [](id self, SEL) {
        auto& c = cd(self);
        if (!c.view) {
            objc::send(self, "loadView");
            if (!c.view) {
                id v = objc::send(objc::send(g_UIView, "alloc"), "init");
                set_frame(v, screen_bounds());
                c.view = v;
                vd(v).controller = self;
            }
            objc::send(self, "viewDidLoad");
        }
        return c.view;
    });
    method(VC, "setView:", [](id self, SEL, id v) {
        auto& c = cd(self);
        if (c.view == v) return;
        objc::retain(v);
        objc::release(c.view);
        c.view = v;
        if (v) vd(v).controller = self;
    });
    method(VC, "isViewLoaded", [](id self, SEL) { return cd(self).view != 0; });
    method(VC, "loadView", [](id self, SEL) {
        id v = objc::send(objc::send(g_UIView, "alloc"), "init");
        set_frame(v, screen_bounds());
        objc::send(self, "setView:", {v});
        objc::release(v);
    });
    for (const char* s : {"viewDidLoad", "viewWillLayoutSubviews", "viewDidLayoutSubviews", "didReceiveMemoryWarning",
                          "setNeedsStatusBarAppearanceUpdate", "removeFromParentViewController", "didMoveToParentViewController:"})
        method(VC, s, [](id, SEL) {});
    for (const char* s : {"viewWillAppear:", "viewDidAppear:", "viewWillDisappear:", "viewDidDisappear:", "setWantsFullScreenLayout:",
                          "setEditing:"})
        method(VC, s, [](id, SEL, bool) {});
    method(VC, "setEdgesForExtendedLayout:", [](id, SEL, u64) {});
    method(VC, "setExtendedLayoutIncludesOpaqueBars:", [](id, SEL, bool) {});
    method(VC, "setAutomaticallyAdjustsScrollViewInsets:", [](id, SEL, bool) {});
    method(VC, "shouldAutorotate", [](id, SEL) { return true; });
    method(VC, "shouldAutorotateToInterfaceOrientation:", [](id, SEL, s64 o) { return o == 3 || o == 4; });
    method(VC, "supportedInterfaceOrientations", [](id, SEL) -> u64 { return (1 << 3) | (1 << 4); });
    method(VC, "interfaceOrientation", [](id, SEL) -> s64 { return 3; });
    method(VC, "prefersStatusBarHidden", [](id, SEL) { return true; });
    method(VC, "nextResponder", [](id self, SEL) -> id {
        id v = cd(self).view;
        return v ? vd(v).superview : 0;
    });
    method(VC, "title", [](id self, SEL) { return cd(self).title; });
    method(VC, "setTitle:", [](id self, SEL, id t) { cd(self).title = objc::retain(t); });
    method(VC, "presentedViewController", [](id self, SEL) { return cd(self).presented; });
    method(VC, "presentingViewController", [](id self, SEL) { return cd(self).presenting; });
    method(VC, "parentViewController", [](id, SEL) -> id { return 0; });
    method(VC, "navigationController", [](id, SEL) -> id { return 0; });
    method(VC, "childViewControllers", [](id, SEL) { return array({}); });
    method(VC, "addChildViewController:", [](id, SEL, id) {});
    method(VC, "setModalPresentationStyle:", [](id, SEL, s64) {});
    method(VC, "setModalTransitionStyle:", [](id, SEL, s64) {});
    auto present = [](id self, id vc, GuestAddr completion) {
        LOG_INFO("presentViewController %s", objc::class_name(objc::isa(vc)).c_str());
        cd(self).presented = objc::retain(vc);
        cd(vc).presenting = self;
        id v = objc::send(vc, "view");
        if (g_key_window && v) {
            set_frame(v, screen_bounds());
            add_subview(g_key_window, v);
        }
        objc::send(vc, "viewWillAppear:", {0});
        objc::send(vc, "viewDidAppear:", {0});
        if (completion) objc::call_block(completion);
    };
    static decltype(present) s_present = present;
    method(VC, "presentViewController:animated:completion:", [](id self, SEL, id vc, bool, GuestAddr c) { s_present(self, vc, c); });
    method(VC, "presentModalViewController:animated:", [](id self, SEL, id vc, bool) { s_present(self, vc, 0); });
    auto dismiss = [](id self, GuestAddr completion) {
        id vc = cd(self).presented ? cd(self).presented : self;
        id presenter = cd(vc).presenting ? cd(vc).presenting : self;
        if (id v = cd(vc).view) objc::send(v, "removeFromSuperview");
        cd(presenter).presented = 0;
        cd(vc).presenting = 0;
        if (completion) objc::call_block(completion);
    };
    static decltype(dismiss) s_dismiss = dismiss;
    method(VC, "dismissViewControllerAnimated:completion:", [](id self, SEL, bool, GuestAddr c) { s_dismiss(self, c); });
    method(VC, "dismissModalViewControllerAnimated:", [](id self, SEL, bool) { s_dismiss(self, 0); });

    // ---------------- UITouch / UIEvent ----------------
    Class T = g_UITouch;
    method(T, "locationInView:", [](id self, SEL, id v) {
        auto& t = objc::ensure<TouchData>(self);
        CGPoint o = v ? window_origin(v) : CGPoint{0, 0};
        return CGPoint{t.loc.x - o.x, t.loc.y - o.y};
    });
    method(T, "previousLocationInView:", [](id self, SEL, id v) {
        auto& t = objc::ensure<TouchData>(self);
        CGPoint o = v ? window_origin(v) : CGPoint{0, 0};
        return CGPoint{t.prev.x - o.x, t.prev.y - o.y};
    });
    method(T, "phase", [](id self, SEL) -> s64 { return objc::ensure<TouchData>(self).phase; });
    method(T, "tapCount", [](id self, SEL) -> u64 { return objc::ensure<TouchData>(self).taps; });
    method(T, "timestamp", [](id self, SEL) { return objc::ensure<TouchData>(self).timestamp; });
    method(T, "view", [](id self, SEL) { return objc::ensure<TouchData>(self).view; });
    method(T, "window", [](id self, SEL) { return objc::ensure<TouchData>(self).window; });
    method(T, "majorRadius", [](id, SEL) { return 20.0; });
    Class E = g_UIEvent;
    method(E, "allTouches", [](id self, SEL) { return objc::send(objc::class_named("NSSet"), "setWithArray:", {array(objc::ensure<EventData>(self).touches)}); });
    method(E, "touchesForView:", [](id self, SEL, id) { return objc::send(self, "allTouches"); });
    method(E, "touchesForWindow:", [](id self, SEL, id) { return objc::send(self, "allTouches"); });
    method(E, "timestamp", [](id self, SEL) { return objc::ensure<EventData>(self).timestamp; });
    method(E, "type", [](id, SEL) -> s64 { return 0; });

    // ---------------- UIScreen ----------------
    Class S = g_UIScreen;
    class_method(S, "mainScreen", [](Class c, SEL) {
        if (!g_screen) g_screen = objc::alloc(c);
        return g_screen;
    });
    class_method(S, "screens", [](Class c, SEL) { return array({(id)objc::send(c, "mainScreen")}); });
    method(S, "bounds", [](id, SEL) { return screen_bounds(); });
    method(S, "applicationFrame", [](id, SEL) { return screen_bounds(); });
    method(S, "scale", [](id, SEL) { return g_device.scale; });
    method(S, "nativeScale", [](id, SEL) { return g_device.native_scale; });
    method(S, "nativeBounds", [](id, SEL) {
        return CGRect{{0, 0}, {std::round(g_device.height_pt * g_device.native_scale), std::round(g_device.width_pt * g_device.native_scale)}};
    });
    method(S, "currentMode", [](id, SEL) {
        if (!g_screen_mode) g_screen_mode = objc::alloc(objc::class_named("UIScreenMode"));
        return g_screen_mode;
    });
    method(S, "preferredMode", [](id self, SEL) { return objc::send(self, "currentMode"); });
    method(S, "availableModes", [](id self, SEL) { return array({(id)objc::send(self, "currentMode")}); });
    method(S, "brightness", [](id, SEL) { return 1.0; });
    method(S, "setBrightness:", [](id, SEL, double) {});
    method(S, "mirroredScreen", [](id, SEL) -> id { return 0; });
    method(objc::class_named("UIScreenMode"), "size", [](id, SEL) {
        return CGSize{std::round(g_device.height_pt * g_device.native_scale), std::round(g_device.width_pt * g_device.native_scale)};
    });
    method(objc::class_named("UIScreenMode"), "pixelAspectRatio", [](id, SEL) { return 1.0; });

    // ---------------- UIDevice ----------------
    Class D = g_UIDevice;
    class_method(D, "currentDevice", [](Class c, SEL) {
        if (!g_device_obj) g_device_obj = objc::alloc(c);
        return g_device_obj;
    });
    method(D, "model", [](id, SEL) { return str(g_device.model); });
    method(D, "localizedModel", [](id, SEL) { return str(g_device.model); });
    method(D, "name", [](id, SEL) { return str("Infinity Blade PC"); });
    method(D, "systemName", [](id, SEL) { return str("iPhone OS"); });
    method(D, "systemVersion", [](id, SEL) { return str("8.2"); });
    method(D, "userInterfaceIdiom", [](id, SEL) -> s64 { return g_device.idiom; });
    method(D, "orientation", [](id, SEL) -> s64 { return 3; });  // LandscapeLeft (home button right)
    method(D, "isMultitaskingSupported", [](id, SEL) { return true; });
    method(D, "uniqueIdentifier", [](id, SEL) { return str("ib3pc0000000000000000000000000000000000"); });
    method(D, "identifierForVendor", [](id, SEL) {
        id u = objc::alloc(objc::class_named("NSUUID"));
        return objc::autorelease(u);
    });
    method(D, "batteryLevel", [](id, SEL) { return 1.0f; });
    method(D, "batteryState", [](id, SEL) -> s64 { return 3; });
    method(D, "setBatteryMonitoringEnabled:", [](id, SEL, bool) {});
    method(D, "isBatteryMonitoringEnabled", [](id, SEL) { return false; });
    method(D, "beginGeneratingDeviceOrientationNotifications", [](id, SEL) {});
    method(D, "endGeneratingDeviceOrientationNotifications", [](id, SEL) {});
    method(D, "isGeneratingDeviceOrientationNotifications", [](id, SEL) { return false; });
    method(D, "setProximityMonitoringEnabled:", [](id, SEL, bool) {});
    method(D, "playInputClick", [](id, SEL) {});
    Class U = objc::class_named("NSUUID");
    class_method(U, "UUID", [](Class c, SEL) { return objc::autorelease(objc::alloc(c)); });
    method(U, "init", [](id self, SEL) { return self; });
    method(U, "initWithUUIDString:", [](id self, SEL, id) { return self; });
    method(U, "UUIDString", [](id, SEL) { return str("1B3E0000-0000-4000-8000-00000000C0DE"); });
    method(U, "getUUIDBytes:", [](id, SEL, u8* out) {
        static const u8 b[16] = {0x1b, 0x3e, 0, 0, 0, 0, 0x40, 0, 0x80, 0, 0, 0, 0, 0, 0xc0, 0xde};
        std::memcpy(out, b, 16);
    });

    // ---------------- UIApplication ----------------
    Class A = g_UIApplication;
    class_method(A, "sharedApplication", [](Class, SEL) { return g_app; });
    method(A, "delegate", [](id, SEL) { return g_delegate; });
    method(A, "setDelegate:", [](id, SEL, id d) { g_delegate = d; });
    method(A, "keyWindow", [](id, SEL) { return g_key_window; });
    method(A, "windows", [](id, SEL) { return array(g_windows); });
    method(A, "nextResponder", [](id, SEL) { return g_delegate; });
    method(A, "applicationState", [](id, SEL) -> s64 { return g_app_active ? 0 : 2; });  // active / background
    method(A, "statusBarOrientation", [](id, SEL) -> s64 { return 3; });
    method(A, "setStatusBarOrientation:", [](id, SEL, s64) {});
    method(A, "setStatusBarOrientation:animated:", [](id, SEL, s64, bool) {});
    method(A, "statusBarFrame", [](id, SEL) { return CGRect{}; });
    method(A, "isStatusBarHidden", [](id, SEL) { return true; });
    method(A, "setStatusBarHidden:", [](id, SEL, bool) {});
    method(A, "setStatusBarHidden:animated:", [](id, SEL, bool, bool) {});
    method(A, "setStatusBarHidden:withAnimation:", [](id, SEL, bool, s64) {});
    method(A, "setStatusBarStyle:", [](id, SEL, s64) {});
    method(A, "setIdleTimerDisabled:", [](id, SEL, bool) {});
    method(A, "isIdleTimerDisabled", [](id, SEL) { return true; });
    method(A, "setNetworkActivityIndicatorVisible:", [](id, SEL, bool) {});
    method(A, "openURL:", [](id, SEL, id url) {
        LOG_INFO("openURL: %s (ignored)", objc::describe(url).c_str());
        return false;
    });
    method(A, "canOpenURL:", [](id, SEL, id) { return false; });
    for (const char* s : {"registerForRemoteNotificationTypes:", "registerUserNotificationSettings:", "scheduleLocalNotification:",
                          "presentLocalNotificationNow:", "cancelLocalNotification:", "endBackgroundTask:"})
        method(A, s, [](id, SEL, u64) {});
    for (const char* s : {"registerForRemoteNotifications", "unregisterForRemoteNotifications", "cancelAllLocalNotifications",
                          "beginIgnoringInteractionEvents", "endIgnoringInteractionEvents", "beginReceivingRemoteControlEvents",
                          "endReceivingRemoteControlEvents"})
        method(A, s, [](id, SEL) {});
    method(A, "isRegisteredForRemoteNotifications", [](id, SEL) { return false; });
    method(A, "setApplicationSupportsShakeToEdit:", [](id, SEL, bool) {});
    method(A, "enabledRemoteNotificationTypes", [](id, SEL) -> u64 { return 0; });
    method(A, "scheduledLocalNotifications", [](id, SEL) { return array({}); });
    method(A, "applicationIconBadgeNumber", [](id, SEL) -> s64 { return 0; });
    method(A, "setApplicationIconBadgeNumber:", [](id, SEL, s64) {});
    method(A, "isIgnoringInteractionEvents", [](id, SEL) { return false; });
    method(A, "beginBackgroundTaskWithExpirationHandler:", [](id, SEL, GuestAddr) -> u64 { return 1; });
    method(A, "beginBackgroundTaskWithName:expirationHandler:", [](id, SEL, id, GuestAddr) -> u64 { return 1; });
    method(A, "backgroundTimeRemaining", [](id, SEL) { return 600.0; });
    method(A, "sendAction:to:from:forEvent:", [](id, SEL, SEL a, id to, id from, id) {
        if (to) objc::send_sel(to, a, {from});
        return to != 0;
    });
    static u64 s_bg_invalid = 0;
    hle::data("_UIBackgroundTaskInvalid", gaddr(&s_bg_invalid));
    static double s_window_level = 0;
    hle::data("_UIWindowLevelNormal", gaddr(&s_window_level));
    static double s_edge_zero[4] = {0, 0, 0, 0};
    hle::data("_UIEdgeInsetsZero", gaddr(s_edge_zero));

    // ---------------- UIApplicationMain ----------------
    hle::raw("_UIApplicationMain", [](cpu::Thread& t) {
        id principal = t.x(2), delegate_name = t.x(3);
        LOG_INFO("UIApplicationMain(principal=%s, delegate=%s)", principal ? utf8(principal).c_str() : "nil",
                 delegate_name ? utf8(delegate_name).c_str() : "nil");
        Class app_cls = principal ? objc::class_named(utf8(principal)) : g_UIApplication;
        g_app = objc::send(objc::send(app_cls ? app_cls : g_UIApplication, "alloc"), "init");
        if (delegate_name) {
            Class dc = objc::class_named(utf8(delegate_name));
            if (!dc) fatal("app delegate class %s not found", utf8(delegate_name).c_str());
            g_delegate = objc::send(objc::send(dc, "alloc"), "init");
        }
        create_window();
        gles::init((HWND)main_window());
        create_boot_window(main_window(), vfs::to_host_w((std::string(vfs::kBundlePath) + "/Startup.png").c_str()));

        // IB3's older UI code swaps screen width/height unless bIPhonePortraitMode is set, a fix-up for
        // iOS 7 where [UIScreen bounds] was always portrait. We report iOS 8 landscape bounds, so the
        // swap must not happen (movie overlays and subtitles would otherwise be laid out portrait).
        if (GuestAddr flag = cpu::image()->find("_bIPhonePortraitMode")) *gptr<u32>(flag) = 1;
        else LOG_WARN("bIPhonePortraitMode not found; movie overlays may be misplaced");

        u64 pool = objc::pool_push();
        id options = dict({});
        if (objc::responds_to(g_delegate, objc::sel("application:willFinishLaunchingWithOptions:")))
            objc::send(g_delegate, "application:willFinishLaunchingWithOptions:", {g_app, options});
        if (objc::responds_to(g_delegate, objc::sel("application:didFinishLaunchingWithOptions:")))
            objc::send(g_delegate, "application:didFinishLaunchingWithOptions:", {g_app, options});
        else if (objc::responds_to(g_delegate, objc::sel("applicationDidFinishLaunching:")))
            objc::send(g_delegate, "applicationDidFinishLaunching:", {g_app});
        id center = objc::send(objc::class_named("NSNotificationCenter"), "defaultCenter");
        objc::send(center, "postNotificationName:object:", {str("UIApplicationDidFinishLaunchingNotification"), g_app});
        if (objc::responds_to(g_delegate, objc::sel("applicationDidBecomeActive:")))
            objc::send(g_delegate, "applicationDidBecomeActive:", {g_app});
        objc::send(center, "postNotificationName:object:", {str("UIApplicationDidBecomeActiveNotification"), g_app});
        objc::pool_pop(pool);
        LOG_INFO("application launched; entering main run loop");

        for (;;) RunLoop::main().run_once(0.25);
    });
}

}  // namespace uikit
