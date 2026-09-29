// The Android window behind the game's screen. Command-line runs (adb shell) have no window and
// render off screen; the app build hands over its ANativeWindow before the game starts.
#include "uikit/uikit.h"
#include "audio/video.h"
#include "gles/gl.h"
#include "port/android/android_app.h"
#include <pthread.h>
#include <atomic>
#include <thread>

namespace android {

namespace {
std::atomic<ANativeActivity*> g_activity{nullptr};
pthread_key_t g_env_key;
pthread_once_t g_env_once = PTHREAD_ONCE_INIT;
}  // namespace

ANativeActivity* activity() { return g_activity.load(); }
void set_activity(ANativeActivity* a) { g_activity = a; }

JNIEnv* env() {
    ANativeActivity* a = activity();
    if (!a) return nullptr;
    // Threads attached here detach when they exit (the VM requires it).
    pthread_once(&g_env_once, [] { pthread_key_create(&g_env_key, [](void*) { activity()->vm->DetachCurrentThread(); }); });
    JNIEnv* e = nullptr;
    if (a->vm->GetEnv(reinterpret_cast<void**>(&e), JNI_VERSION_1_6) == JNI_OK) return e;
    if (a->vm->AttachCurrentThread(&e, nullptr) != JNI_OK) return nullptr;
    pthread_setspecific(g_env_key, e);
    return e;
}

}  // namespace android

namespace uikit {

namespace {
std::atomic<void*> g_window{nullptr};
}  // namespace

void set_native_window(void* window) { g_window = window; }

void create_window() {}
void* main_window() { return g_window.load(); }

void show_fps(unsigned fps) {
    static unsigned n = 0;
    if (++n % 5 == 0) LOG_INFO("%u FPS", fps);
}

// Boot screen: the startup movie plays while the engine loads, before the game draws anything.
void create_boot_window(void* window, const std::wstring&) {
    if (!window) return;  // command-line runs: nothing to show
    std::thread([] {
        logging::set_thread_name("boot screen");
        gles::present_until_first_frame([](int w, int h) {
            const u8* rgba = nullptr;
            int fw = 0, fh = 0;
            u64 serial = 0;
            if (video::current_frame(rgba, fw, fh, serial)) {
                gles::draw_rgba_fit(rgba, fw, fh, serial, w, h);
                video::release_frame();
            } else {
                gles::fill_rect(0, 0, w, h, h, 0, 0, 0);
            }
        });
    }).detach();
}
void resize_boot_window(int, int) {}
void end_boot_window() {}

}  // namespace uikit
