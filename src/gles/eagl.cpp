// EAGLContext / EAGLSharegroup / CAEAGLLayer on EGL (ANGLE, Direct3D 11).
#include "foundation/foundation.h"
#include "gles/gl.h"
#include "objc/internal.h"
#include "uikit/uikit.h"
#include <atomic>
#include <mutex>

namespace gles {

void load_gl_functions();
void blit_to_window(u32 rb, int w, int h, int dst_w, int dst_h);
void save_screenshot(u32 rb, int w, int h, const char* path);
void log_frame_stats(u64 frame);
bool draw_movie(int dst_w, int dst_h);
int g_screenshot_every = 0;
std::mutex g_shot_mutex;
std::string g_shot_request;
void request_screenshot(const std::string& name) {
    std::lock_guard lock(g_shot_mutex);
    g_shot_request = name;
}

namespace {

using EGLDisplay = void*;
using EGLConfig = void*;
using EGLSurface = void*;
using EGLContext = void*;
using EGLint = s32;
using EGLBoolean = u32;

constexpr EGLint EGL_NONE = 0x3038, EGL_RED_SIZE = 0x3024, EGL_GREEN_SIZE = 0x3023, EGL_BLUE_SIZE = 0x3022,
                 EGL_ALPHA_SIZE = 0x3021, EGL_DEPTH_SIZE = 0x3025, EGL_STENCIL_SIZE = 0x3026, EGL_SURFACE_TYPE = 0x3033,
                 EGL_WINDOW_BIT = 0x4, EGL_RENDERABLE_TYPE = 0x3040, EGL_OPENGL_ES3_BIT = 0x40,
                 EGL_CONTEXT_MAJOR_VERSION = 0x3098, EGL_CONTEXT_MINOR_VERSION = 0x30FB, EGL_WIDTH = 0x3057,
                 EGL_HEIGHT = 0x3056, EGL_PLATFORM_ANGLE_ANGLE = 0x3202, EGL_PLATFORM_ANGLE_TYPE_ANGLE = 0x3203,
                 EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE = 0x3208, EGL_OPENGL_ES_API = 0x30A0;

struct Egl {
    HMODULE lib = nullptr;
    void*(__stdcall* GetProcAddress)(const char*) = nullptr;
    EGLDisplay(__stdcall* GetPlatformDisplayEXT)(u32, void*, const EGLint*) = nullptr;
    EGLBoolean(__stdcall* Initialize)(EGLDisplay, EGLint*, EGLint*) = nullptr;
    EGLBoolean(__stdcall* ChooseConfig)(EGLDisplay, const EGLint*, EGLConfig*, EGLint, EGLint*) = nullptr;
    EGLSurface(__stdcall* CreateWindowSurface)(EGLDisplay, EGLConfig, HWND, const EGLint*) = nullptr;
    EGLContext(__stdcall* CreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint*) = nullptr;
    EGLBoolean(__stdcall* MakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = nullptr;
    EGLBoolean(__stdcall* SwapBuffers)(EGLDisplay, EGLSurface) = nullptr;
    EGLBoolean(__stdcall* SwapInterval)(EGLDisplay, EGLint) = nullptr;
    EGLBoolean(__stdcall* QuerySurface)(EGLDisplay, EGLSurface, EGLint, EGLint*) = nullptr;
    EGLint(__stdcall* GetError)() = nullptr;
    EGLBoolean(__stdcall* BindAPI)(u32) = nullptr;
    EGLContext(__stdcall* GetCurrentContext)() = nullptr;
    EGLSurface(__stdcall* GetCurrentSurface)(EGLint) = nullptr;
    EGLDisplay dpy = nullptr;
    EGLConfig cfg = nullptr;
    EGLSurface surf = nullptr;
    EGLContext root = nullptr;
    std::mutex mutex;
} g;

struct ContextData : objc::HostData {
    EGLContext ctx = nullptr;
    std::atomic<DWORD> owner{0};  // host thread the EGL context is current on
    s64 api = 2;
    objc::id sharegroup = 0;
    u32 drawable_rb = 0;
    int rb_w = 0, rb_h = 0;
};

thread_local objc::id t_current = 0;
thread_local bool t_surface_bound = false;

EGLContext create_context() {
    const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    std::lock_guard lock(g.mutex);
    EGLContext c = g.CreateContext(g.dpy, g.cfg, g.root, attrs);
    if (!c) fatal("eglCreateContext failed (0x%x)", g.GetError());
    if (!g.root) g.root = c;
    return c;
}

std::atomic<bool> g_presented{false};

}  // namespace

bool window_presented() { return g_presented; }
void* current_egl_context() { return g.GetCurrentContext ? g.GetCurrentContext() : nullptr; }

bool take_screenshot_request(std::string& name) {
    std::lock_guard lock(g_shot_mutex);
    if (g_shot_request.empty()) return false;
    name.swap(g_shot_request);
    g_shot_request.clear();
    return true;
}

void surface_size(int& w, int& h) {
    EGLint ew = 0, eh = 0;
    g.QuerySurface(g.dpy, g.surf, EGL_WIDTH, &ew);
    g.QuerySurface(g.dpy, g.surf, EGL_HEIGHT, &eh);
    w = ew;
    h = eh;
}

void init(HWND hwnd) {
    g.lib = LoadLibraryW(L"libEGL.dll");
    if (!g.lib) fatal("could not load libEGL.dll (ANGLE)");
#define EGLFN(member, name) g.member = reinterpret_cast<decltype(g.member)>(::GetProcAddress(g.lib, name))
    EGLFN(GetProcAddress, "eglGetProcAddress");
    EGLFN(Initialize, "eglInitialize");
    EGLFN(ChooseConfig, "eglChooseConfig");
    EGLFN(CreateWindowSurface, "eglCreateWindowSurface");
    EGLFN(CreateContext, "eglCreateContext");
    EGLFN(MakeCurrent, "eglMakeCurrent");
    EGLFN(SwapBuffers, "eglSwapBuffers");
    EGLFN(SwapInterval, "eglSwapInterval");
    EGLFN(QuerySurface, "eglQuerySurface");
    EGLFN(GetError, "eglGetError");
    EGLFN(BindAPI, "eglBindAPI");
    EGLFN(GetCurrentContext, "eglGetCurrentContext");
    EGLFN(GetCurrentSurface, "eglGetCurrentSurface");
#undef EGLFN
    g.GetPlatformDisplayEXT = reinterpret_cast<decltype(g.GetPlatformDisplayEXT)>(g.GetProcAddress("eglGetPlatformDisplayEXT"));
    load_gl_functions();

    const EGLint dpy_attrs[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE, EGL_NONE};
    g.dpy = g.GetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, nullptr, dpy_attrs);
    EGLint major = 0, minor = 0;
    if (!g.dpy || !g.Initialize(g.dpy, &major, &minor)) fatal("eglInitialize failed (0x%x)", g.GetError());
    g.BindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attrs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_SURFACE_TYPE, EGL_WINDOW_BIT,
                                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLint n = 0;
    if (!g.ChooseConfig(g.dpy, cfg_attrs, &g.cfg, 1, &n) || n < 1) fatal("eglChooseConfig failed (0x%x)", g.GetError());
    g.surf = g.CreateWindowSurface(g.dpy, g.cfg, hwnd, nullptr);
    if (!g.surf) fatal("eglCreateWindowSurface failed (0x%x)", g.GetError());
    LOG_INFO("EGL %d.%d on ANGLE/D3D11 initialized", major, minor);
}

void install_eagl() {
    using objc::class_method;
    using objc::id;
    using objc::method;
    using objc::SEL;
    objc::Class C = objc::host_class("EAGLContext");
    objc::Class SG = objc::host_class("EAGLSharegroup");
    static id s_sharegroup = 0;

    auto init_ctx = [](id self, s64 api, id sg) -> id {
        if (api == 3) {  // Keep the game on its OpenGL ES 2 path.
            LOG_INFO("EAGLContext: refusing OpenGL ES 3 so the game uses its ES2 renderer");
            objc::release(self);
            return 0;
        }
        auto& d = objc::ensure<ContextData>(self);
        d.api = api;
        d.ctx = create_context();
        if (!s_sharegroup) s_sharegroup = objc::alloc(objc::class_named("EAGLSharegroup"));
        d.sharegroup = sg ? sg : s_sharegroup;
        LOG_INFO("EAGLContext created (API %lld)", (long long)api);
        return self;
    };
    static decltype(init_ctx) s_init = init_ctx;
    method(C, "initWithAPI:", [](id self, SEL, s64 api) { return s_init(self, api, 0); });
    method(C, "initWithAPI:sharegroup:", [](id self, SEL, s64 api, id sg) { return s_init(self, api, sg); });
    method(C, "API", [](id self, SEL) -> s64 { return objc::ensure<ContextData>(self).api; });
    method(C, "sharegroup", [](id self, SEL) { return objc::ensure<ContextData>(self).sharegroup; });
    class_method(C, "currentContext", [](objc::Class, SEL) { return t_current; });
    class_method(C, "setCurrentContext:", [](objc::Class, SEL, id ctx) {
        DWORD me = GetCurrentThreadId();
        LOG_DEBUG("setCurrentContext:0x%llx (was 0x%llx, owner %lu) RHIinit=%u Current=0x%llx Active[0]=0x%llx num=%d",
                 (unsigned long long)ctx, (unsigned long long)t_current,
                 ctx ? (unsigned long)objc::ensure<ContextData>(ctx).owner.load() : 0ul, *gptr<u32>(0x100f3f7a8),
                 (unsigned long long)*gptr<u64>(0x100f63690),
                 (unsigned long long)(*gptr<u64>(0x100f63680) ? *gptr<u64>(*gptr<u64>(0x100f63680)) : 0), *gptr<s32>(0x100f63688));
        if (t_current && t_current != ctx) {
            auto& old = objc::ensure<ContextData>(t_current);
            if (old.owner.load() == me) old.owner.store(0);
        }
        t_current = ctx;
        t_surface_bound = false;
        EGLContext c = nullptr;
        if (ctx) {
            auto& d = objc::ensure<ContextData>(ctx);
            c = d.ctx;
            DWORD owner = d.owner.load();
            if (owner && owner != me) {
                // iOS allows one context to be current on several threads; EGL does not. The main thread
                // sets up the view's context and then hands it to the game thread, so release it there.
                if (ns::is_main_thread()) {
                    LOG_WARN("EAGL context 0x%llx is current on thread %lu; main thread cannot take it", (unsigned long long)ctx, owner);
                } else {
                    objc::retain(ctx);
                    ns::post_to_main([ctx] {
                        auto& dd = objc::ensure<ContextData>(ctx);
                        if (dd.owner.load() == GetCurrentThreadId()) {
                            g.MakeCurrent(g.dpy, nullptr, nullptr, nullptr);
                            if (t_current == ctx) t_current = 0;
                            t_surface_bound = false;
                            dd.owner.store(0);
                        }
                        objc::release(ctx);
                    });
                    for (int i = 0; i < 2000 && d.owner.load() == owner; i++) Sleep(1);
                    if (d.owner.load() == owner) LOG_ERROR("timed out waiting for thread %lu to release the GL context", owner);
                }
            }
            d.owner.store(me);
        }
        if (!g.MakeCurrent(g.dpy, nullptr, nullptr, c)) LOG_ERROR("eglMakeCurrent failed (0x%x)", g.GetError());
        return true;
    });
    method(C, "renderbufferStorage:fromDrawable:", [](id self, SEL, u32 target, id layer) {
        CGRect b = uikit::layer_bounds(layer);
        double scale = uikit::layer_scale(layer);
        int w = (int)(b.size.width * scale + 0.5), h = (int)(b.size.height * scale + 0.5);
        auto& d = objc::ensure<ContextData>(self);
        s32 rb = 0;
        fn::GetIntegerv(0x8CA7, &rb);  // GL_RENDERBUFFER_BINDING
        d.drawable_rb = rb;
        d.rb_w = w;
        d.rb_h = h;
        using RS = void(__stdcall*)(u32, u32, s32, s32);
        static RS rs = reinterpret_cast<RS>(GetProcAddress(GetModuleHandleW(L"libGLESv2.dll"), "glRenderbufferStorage"));
        rs(target, 0x8058 /*GL_RGBA8*/, w, h);
        LOG_INFO("drawable renderbuffer %d: %dx%d (scale %.2f)", rb, w, h, scale);
        return true;
    });
    method(C, "presentRenderbuffer:", [](id self, SEL, u32) {
        auto& d = objc::ensure<ContextData>(self);
        if (!t_surface_bound) {
            if (!g.MakeCurrent(g.dpy, g.surf, g.surf, d.ctx)) {
                LOG_ERROR("eglMakeCurrent(window) failed (0x%x)", g.GetError());
                return false;
            }
            g.SwapInterval(g.dpy, 0);  // the game paces itself (30 fps cap); vsync would stack another wait
            t_surface_bound = true;
        }
        s32 rb = 0;
        fn::GetIntegerv(0x8CA7, &rb);
        if (!rb) rb = d.drawable_rb;
        int sw, sh;
        surface_size(sw, sh);
        static std::atomic<u64> frames{0};
        u64 n = ++frames;
        if (n <= 5 || n % 120 == 0) log_frame_stats(n);
        if (!draw_movie(sw, sh)) blit_to_window(rb, d.rb_w, d.rb_h, sw, sh);
        if (g_screenshot_every && n % g_screenshot_every == 0) {
            char path[64];
            snprintf(path, sizeof path, "shot_%05llu.png", (unsigned long long)n);
            save_screenshot(0, sw, sh, path);  // 0 = what the window shows
        }
        {
            std::lock_guard lock(g_shot_mutex);
            if (!g_shot_request.empty()) {
                save_screenshot(0, sw, sh, g_shot_request.c_str());
                g_shot_request.clear();
            }
        }
        g.SwapBuffers(g.dpy, g.surf);
        g_presented = true;
        uikit::on_frame_presented();
        return true;
    });
    (void)SG;
}

}  // namespace gles
