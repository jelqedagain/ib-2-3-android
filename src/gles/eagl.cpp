// EAGLContext / EAGLSharegroup / CAEAGLLayer on EGL (ANGLE, Direct3D 11).
#include "foundation/foundation.h"
#include "game/game.h"
#include "gles/gl.h"
#include "objc/internal.h"
#include "uikit/uikit.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <mutex>
#ifdef __ANDROID__
#include <sys/system_properties.h>
#include <unistd.h>
#include <cstdio>
#include <time.h>
#endif

namespace gles {

void load_gl_functions();
void blit_to_window(u32 rb, int w, int h, int dst_w, int dst_h);
void save_screenshot(u32 rb, int w, int h, const char* path);
void log_frame_stats(u64 frame);
void framelog_frame(u64 frame);
bool draw_movie(int dst_w, int dst_h);
int g_screenshot_every = 0;
void (*g_overlay)(int, int) = nullptr;
std::mutex g_shot_mutex;
std::string g_shot_request;
void request_screenshot(const std::string& name) {
    std::lock_guard lock(g_shot_mutex);
    g_shot_request = name;
}

namespace {

#ifdef __ANDROID__
// Memory for the perf line. Android's own reports (dumpsys meminfo, its exit records) leave out most of
// what the Adreno driver holds for textures, so that comes from the driver's per-process counters.
std::string memory_summary() {
    auto read_u64 = [](const std::string& path, u64& v) {
        FILE* f = std::fopen(path.c_str(), "r");
        if (!f) return false;
        unsigned long long x = 0;
        bool ok = std::fscanf(f, "%llu", &x) == 1;
        std::fclose(f);
        v = x;
        return ok;
    };
    char buf[256];
    u64 pages = 0, resident = 0;
    if (FILE* f = std::fopen("/proc/self/statm", "r")) {
        if (std::fscanf(f, "%llu %llu", (unsigned long long*)&pages, (unsigned long long*)&resident) != 2) resident = 0;
        std::fclose(f);
    }
    int n = snprintf(buf, sizeof buf, "RAM %llu MB", (unsigned long long)(resident * sysconf(_SC_PAGESIZE) >> 20));
    std::string kgsl = "/sys/class/kgsl/kgsl/proc/" + std::to_string(getpid());
    u64 gpu = 0, gpu_peak = 0, tex = 0;
    if (read_u64(kgsl + "/kernel", gpu)) {
        read_u64(kgsl + "/kernel_max", gpu_peak);
        read_u64(kgsl + "/memtype/texture", tex);
        n += snprintf(buf + n, sizeof buf - n, ", GPU %llu MB (textures %llu MB, peak %llu MB)", (unsigned long long)(gpu >> 20),
                      (unsigned long long)(tex >> 20), (unsigned long long)(gpu_peak >> 20));
    }
    Etc2Stats etc = etc2_stats();
    if (etc.textures)
        snprintf(buf + n, sizeof buf - n,
                 ", %llu textures as ETC2 (%llu MB instead of %llu MB; %llu converted in %llu ms, %llu from cache in %llu ms)",
                 (unsigned long long)etc.textures, (unsigned long long)(etc.bytes >> 20),
                 (unsigned long long)(etc.rgba_bytes >> 20), (unsigned long long)etc.converted,
                 (unsigned long long)(etc.converted_us / 1000), (unsigned long long)etc.cached,
                 (unsigned long long)(etc.cached_us / 1000));
    return buf;
}
#endif

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
                 EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE = 0x3208, EGL_OPENGL_ES_API = 0x30A0, EGL_PBUFFER_BIT = 0x1;

struct Egl {
    HMODULE lib = nullptr;
    void*(__stdcall* GetProcAddress)(const char*) = nullptr;
    EGLDisplay(__stdcall* GetPlatformDisplayEXT)(u32, void*, const EGLint*) = nullptr;
    EGLDisplay(__stdcall* GetDisplay)(void*) = nullptr;
    EGLSurface(__stdcall* CreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint*) = nullptr;
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
    EGLBoolean(__stdcall* DestroySurface)(EGLDisplay, EGLSurface) = nullptr;
    EGLBoolean(__stdcall* DestroyContext)(EGLDisplay, EGLContext) = nullptr;
    EGLContext(__stdcall* GetCurrentContext)() = nullptr;
    EGLSurface(__stdcall* GetCurrentSurface)(EGLint) = nullptr;
    EGLDisplay dpy = nullptr;
    EGLConfig cfg = nullptr;
    EGLSurface surf = nullptr;
    EGLContext root = nullptr;
    std::mutex mutex;
    // The window surface can be replaced (set_window); presenting holds this lock.
    std::mutex surface_mutex;
    u64 surface_generation = 0;
    HWND window = nullptr;  // what the window surface was made for
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
thread_local u64 t_surface_generation = 0;

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
    if (g.surf) {
        g.QuerySurface(g.dpy, g.surf, EGL_WIDTH, &ew);
        g.QuerySurface(g.dpy, g.surf, EGL_HEIGHT, &eh);
    }
    w = ew;
    h = eh;
}

void init(HWND hwnd) {
#ifdef _WIN32
    g.lib = LoadLibraryW(L"libEGL.dll");
    if (!g.lib) fatal("could not load libEGL.dll (ANGLE)");
#else
    g.lib = LoadLibraryW(L"libEGL.so");  // the device's own OpenGL ES driver
    if (!g.lib) fatal("could not load libEGL.so");
#endif
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
    EGLFN(GetDisplay, "eglGetDisplay");
    EGLFN(CreatePbufferSurface, "eglCreatePbufferSurface");
    EGLFN(DestroySurface, "eglDestroySurface");
    EGLFN(DestroyContext, "eglDestroyContext");
#undef EGLFN
    g.GetPlatformDisplayEXT = reinterpret_cast<decltype(g.GetPlatformDisplayEXT)>(g.GetProcAddress("eglGetPlatformDisplayEXT"));
    load_gl_functions();

#ifdef _WIN32
    const EGLint dpy_attrs[] = {EGL_PLATFORM_ANGLE_TYPE_ANGLE, EGL_PLATFORM_ANGLE_TYPE_D3D11_ANGLE, EGL_NONE};
    g.dpy = g.GetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, nullptr, dpy_attrs);
#else
    g.dpy = g.GetDisplay(nullptr);  // EGL_DEFAULT_DISPLAY
#endif
    EGLint major = 0, minor = 0;
    if (!g.dpy || !g.Initialize(g.dpy, &major, &minor)) fatal("eglInitialize failed (0x%x)", g.GetError());
    g.BindAPI(EGL_OPENGL_ES_API);
    const EGLint cfg_attrs[] = {EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
                                EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8, EGL_SURFACE_TYPE, hwnd ? EGL_WINDOW_BIT : EGL_PBUFFER_BIT,
                                EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT, EGL_NONE};
    EGLint n = 0;
    if (!g.ChooseConfig(g.dpy, cfg_attrs, &g.cfg, 1, &n) || n < 1) fatal("eglChooseConfig failed (0x%x)", g.GetError());
    g.window = hwnd;
    if (hwnd) {
        g.surf = g.CreateWindowSurface(g.dpy, g.cfg, hwnd, nullptr);
    } else {  // no window (Android command-line runs): render off screen
        const EGLint pb_attrs[] = {EGL_WIDTH, 1280, EGL_HEIGHT, 720, EGL_NONE};
        g.surf = g.CreatePbufferSurface(g.dpy, g.cfg, pb_attrs);
    }
    if (!g.surf) fatal("eglCreate%sSurface failed (0x%x)", hwnd ? "Window" : "Pbuffer", g.GetError());
#ifdef _WIN32
    LOG_INFO("EGL %d.%d on ANGLE/D3D11 initialized", major, minor);
#else
    LOG_INFO("EGL %d.%d initialized (%s)", major, minor, hwnd ? "window" : "off-screen 1280x720");
#endif
}

void set_window(HWND hwnd) {
    std::lock_guard lock(g.surface_mutex);  // waits for a frame being presented
    // Presenting holds the window current only while it holds this lock (see presentRenderbuffer).
    if (g.surf) g.DestroySurface(g.dpy, g.surf);
    g.window = hwnd;
    g.surf = hwnd ? g.CreateWindowSurface(g.dpy, g.cfg, hwnd, nullptr) : nullptr;
    if (hwnd && !g.surf) LOG_ERROR("eglCreateWindowSurface failed (0x%x)", g.GetError());
    g.surface_generation++;
    LOG_INFO("window surface %s", hwnd ? "attached" : "detached");
}

void present_until_first_frame(const std::function<void(int w, int h)>& draw) {
    const EGLint attrs[] = {EGL_CONTEXT_MAJOR_VERSION, 3, EGL_CONTEXT_MINOR_VERSION, 0, EGL_NONE};
    EGLContext ctx = g.CreateContext(g.dpy, g.cfg, nullptr, attrs);  // not shared with the game's
    if (!ctx) return;
    bool drew = false;
    while (!g_presented) {
        {
            std::lock_guard lock(g.surface_mutex);  // takes turns with presentRenderbuffer
            if (g_presented) break;
            if (g.surf && g.MakeCurrent(g.dpy, g.surf, g.surf, ctx)) {
                int w, h;
                surface_size(w, h);
                draw(w, h);
                g.SwapBuffers(g.dpy, g.surf);
                g.MakeCurrent(g.dpy, nullptr, nullptr, nullptr);  // leave the surface free for the game
                drew = true;
            }
        }
        Sleep(16);
    }
    g.DestroyContext(g.dpy, ctx);
#ifdef __ANDROID__
    // Give the game a window surface of its own, as after coming back from the background. On Mali
    // GPUs the game's textures were garbled until the app left the screen and returned while the
    // surface it drew on was the one this second context had also drawn on.
    std::lock_guard lock(g.surface_mutex);
    if (drew && g.surf && g.window) {
        g.DestroySurface(g.dpy, g.surf);
        g.surf = g.CreateWindowSurface(g.dpy, g.cfg, g.window, nullptr);
        if (!g.surf) LOG_ERROR("eglCreateWindowSurface failed (0x%x)", g.GetError());
        g.surface_generation++;
        LOG_INFO("window surface renewed for the game");
    }
#endif
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
        LOG_DEBUG("setCurrentContext:0x%llx (was 0x%llx, owner %lu)", (unsigned long long)ctx, (unsigned long long)t_current,
                  ctx ? (unsigned long)objc::ensure<ContextData>(ctx).owner.load() : 0ul);
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
        static RS rs = reinterpret_cast<RS>(gl_proc("glRenderbufferStorage"));
        rs(target, 0x8058 /*GL_RGBA8*/, w, h);
        LOG_INFO("drawable renderbuffer %d: %dx%d (scale %.2f)", rb, w, h, scale);
        return true;
    });
    method(C, "presentRenderbuffer:", [](id self, SEL, u32) {
        auto& d = objc::ensure<ContextData>(self);
        std::lock_guard surface_lock(g.surface_mutex);
        if (!g.surf) {  // no window right now (the Android app is in the background)
            if (t_surface_bound) g.MakeCurrent(g.dpy, nullptr, nullptr, d.ctx);
            t_surface_bound = false;
            return true;
        }
        if (!t_surface_bound || t_surface_generation != g.surface_generation) {
            t_surface_generation = g.surface_generation;
            if (!g.MakeCurrent(g.dpy, g.surf, g.surf, d.ctx)) {
                LOG_ERROR("eglMakeCurrent(window) failed (0x%x)", g.GetError());
                return false;
            }
            static u64 interval_set_for = ~0ull;  // the swap interval belongs to the surface
            if (interval_set_for != g.surface_generation) {
                interval_set_for = g.surface_generation;
                g.SwapInterval(g.dpy, 0);  // the game paces itself (30 fps cap); vsync would stack another wait
            }
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
        framelog_frame(n);
        if (!draw_movie(sw, sh)) blit_to_window(rb, d.rb_w, d.rb_h, sw, sh);
        if (g_overlay) g_overlay(sw, sh);
#ifdef __ANDROID__
        if (unsigned fps = uikit::shown_fps()) {  // settings: Show FPS
            static std::vector<u8> panel;
            static int pw = 0, ph = 0;
            static unsigned drawn = 0;
            if (fps != drawn) {
                uikit::render_text_panel({std::to_string(fps) + " FPS"}, sh * 0.03f, panel, pw, ph);
                drawn = fps;
            }
            if (!panel.empty()) draw_rgba_rect(panel.data(), pw, ph, 0xF9500000ull + fps, sh / 40, sh / 40, pw, ph, sh);
        }
#endif
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
        {   // One line every 10 s that shows at a glance how the game is running (for bug reports).
            using clock = std::chrono::steady_clock;
            static clock::time_point last = clock::now(), window = last;
            static int count = 0, slow = 0;
            static double worst = 0;
#ifdef __ANDROID__
            auto cpu_now = [] { timespec ts; clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &ts); return ts.tv_sec + ts.tv_nsec * 1e-9; };
            static double cpu_window = cpu_now();
#endif
            clock::time_point now = clock::now();
            double ms = std::chrono::duration<double, std::milli>(now - last).count();
            last = now;
            count++;
            worst = std::max(worst, ms);
            if (ms > 25) slow++;
            double span = std::chrono::duration<double>(now - window).count();
            if (span >= 10) {
#ifdef __ANDROID__
                double cpu = cpu_now();
                LOG_INFO("perf: %.1f fps, worst frame %.0f ms, %d frames over 25 ms, %.0f%% of one CPU core, %s", count / span,
                         worst, slow, (cpu - cpu_window) / span * 100, memory_summary().c_str());
                cpu_window = cpu;
#else
                LOG_INFO("perf: %.1f fps, worst frame %.0f ms, %d frames over 25 ms", count / span, worst, slow);
#endif
                window = now;
                count = slow = 0;
                worst = 0;
            }
        }
        g.SwapBuffers(g.dpy, g.surf);
#ifdef __ANDROID__
        // Let go of the window until the next frame. Android destroys it whenever the app leaves the
        // screen, and the game stops presenting then: a window left current on its thread was still
        // attached when that happened, and on some drivers (Mali) the game's graphics came back
        // black or with garbled textures.
        // Adreno GPUs never showed the problem, and re-attaching the window every frame costs them
        // frame time on some drivers: release only elsewhere. debug.ibport.keepsurface 1/0 forces it.
        static const bool release_surface = [] {
            char v[PROP_VALUE_MAX] = "";
            if (__system_property_get("debug.ibport.keepsurface", v) > 0 && (v[0] == '1' || v[0] == '0')) return v[0] == '0';
            if (game::is_ib2()) return true;  // IB2: as in 1.3.2, on every GPU
            return !gpu_is_adreno();
        }();
        if (release_surface) {
            g.MakeCurrent(g.dpy, nullptr, nullptr, d.ctx);
            t_surface_bound = false;
        }
#endif
        g_presented = true;
        uikit::on_frame_presented();
        return true;
    });
    (void)SG;
}

}  // namespace gles
