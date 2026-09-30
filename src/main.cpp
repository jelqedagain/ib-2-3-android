#include "common.h"
#include "cpu.h"
#include "foundation/foundation.h"
#include "foundation/runloop.h"
#include "gles/gl.h"
#include "uikit/uikit.h"
#include "game/game.h"
#include "settings.h"
#ifdef _WIN32
#include "launcher/install.h"
#include "launcher/launcher.h"
#endif
#include "uikit/labels.h"
#include "audio/mixer.h"
#include "audio/video.h"
namespace audio { void install(); }
#include "hle.h"
#include "libc/vfs.h"
#include "macho.h"
#include "modules.h"
#include "objc/internal.h"
#include <thread>
#include <vector>
#include <windows.h>

#ifdef _WIN32
// Reports host crashes with enough context to find the HLE code responsible.
static LONG WINAPI crash_filter(EXCEPTION_POINTERS* ep) {
    auto* rec = ep->ExceptionRecord;
    u64 addr = (u64)rec->ExceptionAddress;
    char module[MAX_PATH] = "?";
    HMODULE mod = nullptr;
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCSTR>(rec->ExceptionAddress), &mod))
        GetModuleFileNameA(mod, module, sizeof module);
    u64 access = rec->NumberParameters >= 2 ? rec->ExceptionInformation[1] : 0;
    std::string guest;
    if (cpu::Thread* t = cpu::current_or_null()) {
        const char* hle = t->in_hle.load();
        guest = std::string("inside HLE ") + (hle ? hle : "(none)") + "\n" + t->backtrace();
    }
    LOG_ERROR("HOST CRASH 0x%08lx at %s+0x%llx (access 0x%llx)\n%s", rec->ExceptionCode, module,
              (unsigned long long)(addr - (u64)mod), (unsigned long long)access, guest.c_str());
    std::fflush(nullptr);
    char what[160];
    const char* base = strrchr(module, '\\');
    snprintf(what, sizeof what, "crash 0x%08lx in %s", rec->ExceptionCode, base ? base + 1 : module);
    logging::show_error_dialog(what);
    return EXCEPTION_EXECUTE_HANDLER;
}
#endif

// The game's code must live at 0x100000000. Reserve that range before anything else can land
// there; if something already has (Windows randomizes where DLLs and heaps go), run a fresh copy
// of the process, which gets a different layout.
static void claim_guest_image_range() {
    constexpr u64 kLo = 0x100000000ull, kSize = 0x40000000ull;  // 1 GB, well over the image size
    if (VirtualAlloc(gptr<void>(kLo), kSize, MEM_RESERVE, PAGE_NOACCESS)) return;
#ifndef _WIN32
    LOG_WARN("address 0x%llx is taken", (unsigned long long)kLo);  // not seen on Android: mappings go high
#else
    MEMORY_BASIC_INFORMATION mbi{};
    VirtualQuery(gptr<void>(kLo), &mbi, sizeof mbi);
    wchar_t buf[16] = {};
    int attempt = GetEnvironmentVariableW(L"IB3RT_RELAUNCH", buf, 16) ? _wtoi(buf) : 0;
    LOG_WARN("address 0x%llx is taken (allocation 0x%p, type 0x%lx); restarting (attempt %d)",
             (unsigned long long)kLo, mbi.AllocationBase, mbi.Type, attempt + 1);
    if (attempt >= 5) return;  // give up; loading the image reports the error
    SetEnvironmentVariableW(L"IB3RT_RELAUNCH", std::to_wstring(attempt + 1).c_str());
    STARTUPINFOW si{sizeof si};
    PROCESS_INFORMATION pi{};
    std::wstring cmd = GetCommandLineW();
    if (!CreateProcessW(nullptr, cmd.data(), nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) return;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    ExitProcess(code);
#endif
}

#ifdef IB3_ANDROID_APP
int ib3_main(int argc, char** argv) {  // started by the Android app (port/android/app.cpp)
#else
int main(int argc, char** argv) {
#endif
    claim_guest_image_range();
#ifdef _WIN32
    // Started on its own (double-clicked): show the launcher, which starts the game with -play.
    if (argc == 1) return launcher::run();
    if (argc == 4 && std::string(argv[1]) == "-launcher-shot") return launcher::screenshot(argv[2], argv[3]);
    if (argc == 5 && std::string(argv[1]) == "-movie-frame") {  // tool: -movie-frame <movie> <seconds> <png>
        std::vector<u8> rgba;
        int w = 0, h = 0;
        if (!video::grab_frame(launcher::widen(argv[2]), atof(argv[3]), rgba, w, h)) return 1;
        gles::write_png(argv[4], rgba.data(), w, h);
        return 0;
    }
    if (argc == 3 && std::string(argv[1]) == "-install") {  // headless install: -install <path to .ipa>
        std::wstring error;
        int last = -1;
        bool ok = launcher::install_from_ipa(launcher::widen(argv[2]), [&](double f) {
            if ((int)(f * 20) != last) LOG_INFO("installing: %d%%", (last = (int)(f * 20)) * 5);
        }, error);
        if (!ok) LOG_ERROR("install failed: %s", launcher::narrow(error).c_str());
        else LOG_INFO("installed Infinity Blade III %s", launcher::installed_version().c_str());
        return ok ? 0 : 1;
    }
    SetUnhandledExceptionFilter(crash_filter);
#endif
    std::string app = "game/Payload/SwordGame.app";
    std::string home = "userdata";
    bool keep_console = false, play = false, audit = false, script_trace = false;
    std::string script;
    for (int i = 1; i < argc; i++) {
        std::string a = argv[i];
        if (a == "-v") logging::min_level = logging::Level::Debug;
        else if (a == "-vv") logging::min_level = logging::Level::Trace;
        else if (a == "-home" && i + 1 < argc) home = argv[++i];
        else if (a == "-console") keep_console = true;
        else if (a == "-play") play = true;
        else if (a == "-audit-selectors") audit = true;
        else if (a == "-scripttrace") script_trace = true;
        else if (a == "-script" && i + 1 < argc) script = argv[++i];
        else if (a == "-test") {
            uikit::g_test_mode = true;
            if (audio::g_wav_path.empty()) audio::g_wav_path = "audio_test.wav";
        }
        else if (a == "-wav" && i + 1 < argc) audio::g_wav_path = argv[++i];
        else if (a == "-glcheck") gles::g_glcheck = true;
        else if (a == "-profile") {
            cpu::enable_profiling();
            std::thread([] {
                logging::set_thread_name("profiler");
                std::unordered_map<std::string, u64> samples;
                u64 n = 0;
                for (int tick = 1;; tick++) {
                    Sleep(2);
                    cpu::profile_sample(samples);
                    n++;
                    if (tick % 5000 == 0) {  // ~10 s
                        cpu::profile_report(10.0, samples, n);
                        samples.clear();
                        n = 0;
                    }
                }
            }).detach();
        }
        else if (a == "-shot" && i + 1 < argc) gles::g_screenshot_every = std::atoi(argv[++i]);
        else if (a == "-dump" && i + 1 < argc) {
            int secs = std::atoi(argv[++i]);
            std::thread([secs] {
                logging::set_thread_name("watchdog");
                Sleep(secs * 1000);
                cpu::dump_all_threads();
            }).detach();
        }
        else app = a;
    }
#ifdef _WIN32
    if (play) {  // installed layout: everything next to the executable
        std::string dir = launcher::narrow(settings::exe_dir());
        app = dir + "game/Payload/SwordGame.app";
        home = dir + "userdata";
        if (!launcher::game_installed()) {
            MessageBoxW(nullptr, L"The game files are not installed. Start Infinity Blade III.exe without -play to install them.",
                        L"Infinity Blade III", MB_ICONERROR);
            return 1;
        }
    }
    if (!uikit::g_test_mode) logging::g_error_dialogs = true;
    // A windowed (GUI) program has no console; logs go to ib3rt.log (and to stderr when the
    // parent redirected it, as test.sh does). -console opens one for live logs.
    if (keep_console && AllocConsole()) {
        freopen("CONOUT$", "w", stdout);
        freopen("CONOUT$", "w", stderr);
    }
#endif
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    logging::set_thread_name("main");

    static macho::Image img = macho::load(app + "/SwordGame");
    cpu::init(&img);
    vfs::set_roots(app, home);

    // The main thread's guest context must exist before anything can call guest code.
    cpu::Thread* main_thread = new cpu::Thread(8 << 20);
    ns::set_main_thread();

    objc::init_runtime(img);
    libc::install_string();
    libc::install_stdio();
    libc::install_fs();
    libc::install_time();
    libc::install_pthread();
    libc::install_system();
    libc::install_crypto();
    libc::install_thirdparty();
    objc::install_runtime_functions();
    ns::install_string();
    ns::install_collections();
    ns::install_thread();
    ns::install_system();
    ns::install_cf();
    ns::install_misc();
    gles::install_gl();
    gles::install_eagl();
    uikit::install();
    uikit::install_misc();
    uikit::install_labels();
    audio::install();

    hle::bind_image(img);
    game::strengthen_memory_barriers(img);
    game::install_keyboard(img);
    if (script_trace) game::install_script_trace(img);
    game::start_controller();
    game::install_config(img);
    game::install_widescreen();
    {
        const auto& st = settings::get();
        audio::set_volumes(st.music_volume / 100.0f, st.effects_volume / 100.0f);
    }
    objc::realize_image_classes(img);
    game::install_startup_movie_fix();
    if (script_trace) game::install_script_trace_objc();
    if (audit) {
        objc::audit_selectors(img);
        return 0;
    }
    ns::install_thread_late();

    // Analytics / ad SDKs only matter online; keep them from starting threads and crash hooks.
    for (const char* sdk : {"Flurry", "Apsalar", "FBAppEvents", "FBInsights", "FlurryPLCrashReporter"})
        objc::stub_out_class_methods(sdk);

#ifdef __ANDROID__
    // A game that stops drawing while it is on screen is stuck (or loading very slowly): log where
    // every game thread is, so Share logs carries it. Players cannot run adb for -dump.
    std::thread([] {
        logging::set_thread_name("watchdog");
        constexpr u64 kStuckMs = 20000, kFirstFrameMs = 60000;
        u64 last_frames = 0, since = GetTickCount64();
        bool reported = false;
        for (;;) {
            Sleep(1000);
            u64 now = GetTickCount64(), frames = uikit::frames_presented();
            if (frames != last_frames || !uikit::app_active()) {
                if (reported && frames != last_frames)
                    LOG_INFO("watchdog: the game draws again after %.0f s", (now - since) / 1000.0);
                last_frames = frames;
                since = now;
                reported = false;
                continue;
            }
            if (!reported && now - since >= (frames ? kStuckMs : kFirstFrameMs)) {
                reported = true;
                LOG_WARN("watchdog: no new frame for %.0f s while on screen (%llu frames so far); where the game is:",
                         (now - since) / 1000.0, (unsigned long long)frames);
                cpu::dump_all_threads();
            }
        }
    }).detach();
#endif

    if (!script.empty()) game::run_script(script);

    objc::run_load_methods();
    if (auto* init = img.section("__DATA", "__mod_init_func")) {
        size_t n = init->size / 8;
        LOG_INFO("running %zu static initializers", n);
        for (size_t i = 0; i < n; i++) {
            GuestAddr fn = gptr<u64>(init->addr)[i];
            LOG_DEBUG("init[%zu] 0x%llx %s", i, (unsigned long long)fn, cpu::symbolize(fn).c_str());
            main_thread->call(fn);
        }
        LOG_INFO("static initializers done");
    }

    // int main(int argc, char** argv, char** envp, char** apple)
    auto* args = static_cast<u64*>(hle::alloc_static(sizeof(u64) * 6));
    args[0] = gaddr(hle::static_cstr(std::string(vfs::kBundlePath) + "/SwordGame"));
    args[1] = 0;
    args[2] = 0;  // envp
    args[3] = gaddr(hle::static_cstr("executable_path=SwordGame"));
    args[4] = 0;
    LOG_INFO("calling main at 0x%llx", (unsigned long long)img.entry);
    u64 rc = main_thread->call(img.entry, {1, gaddr(&args[0]), gaddr(&args[2]), gaddr(&args[3])});
    LOG_INFO("main returned %d", (int)rc);
    return (int)rc;
}
