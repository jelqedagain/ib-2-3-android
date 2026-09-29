// The Android app (NativeActivity, no Java). The game runs on its own thread with a large stack
// (guest code and HLE calls nest deeply), started once the window exists; this thread, Android's
// "app glue" thread, forwards window changes and touches to it.
//
// Game files live in the app's external files folder:
//   /sdcard/Android/data/<package>/files/game/Payload/SwordGame.app
// and saves and logs go next to them (userdata/, ib3rt.log).
#include "common.h"
#include "audio/mixer.h"
#include "foundation/foundation.h"
#include "game/game.h"
#include "gles/gl.h"
#include "settings.h"
#include "uikit/uikit.h"
#include <android/log.h>
#include <android/native_window.h>
#include <android/window.h>
#include <android_native_app_glue.h>
#include <jni.h>
#include <pthread.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <string>

int ib3_main(int argc, char** argv);
// The NDK's app glue, built with its ANativeActivity_onCreate renamed (see CMakeLists.txt).
extern "C" void glue_ANativeActivity_onCreate(ANativeActivity* activity, void* saved_state, size_t saved_state_size);

namespace uikit {
void set_native_window(void* window);
}

namespace {

android_app* g_app = nullptr;
bool g_started = false;
constexpr int kFirstFinger = 100;  // finger ids for real touches (0-99 are the runtime's own)

void* game_thread(void*) {
    static char arg0[] = "ib3";
    char* argv[] = {arg0, nullptr};
    int rc = ib3_main(1, argv);
    __android_log_print(ANDROID_LOG_INFO, "ib3", "game exited (%d)", rc);
    _exit(rc);
}

void start_game() {
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 16 << 20);
    pthread_t thread;
    pthread_create(&thread, &attr, game_thread, nullptr);
    pthread_attr_destroy(&attr);
    pthread_detach(thread);
}

// Window pixels -> game points; the game's image is letterboxed into the window (see blit_to_window).
CGPoint to_points(float x, float y) {
    double ww = g_app->window ? ANativeWindow_getWidth(g_app->window) : 1;
    double wh = g_app->window ? ANativeWindow_getHeight(g_app->window) : 1;
    double aspect = uikit::g_device.width_pt / uikit::g_device.height_pt;
    double ow = ww, oh = ww / aspect;
    if (oh > wh) oh = wh, ow = wh * aspect;
    double ox = (ww - ow) / 2, oy = (wh - oh) / 2;
    double nx = std::clamp((x - ox) / ow, 0.0, 0.999), ny = std::clamp((y - oy) / oh, 0.0, 0.999);
    return {nx * uikit::g_device.width_pt, ny * uikit::g_device.height_pt};
}

void send_touch(int phase, int finger, CGPoint p) {
    ns::post_to_main([phase, finger, p] {
        if (phase == 0) uikit::touch_down(finger, p);
        else if (phase == 1) uikit::touch_move(finger, p);
        else uikit::touch_up(finger, p);
    });
}

// Hides the status and navigation bars; a swipe from an edge shows them for a moment (UI thread).
void hide_system_bars(ANativeActivity* a) {
    JNIEnv* env = a->env;
    jclass activity_cls = env->GetObjectClass(a->clazz);
    jobject window = env->CallObjectMethod(a->clazz, env->GetMethodID(activity_cls, "getWindow", "()Landroid/view/Window;"));
    jclass window_cls = env->GetObjectClass(window);
    env->CallObjectMethod(window, env->GetMethodID(window_cls, "getDecorView", "()Landroid/view/View;"));
    jobject controller = env->CallObjectMethod(window, env->GetMethodID(window_cls, "getInsetsController", "()Landroid/view/WindowInsetsController;"));
    if (controller && !env->ExceptionCheck()) {
        jclass types = env->FindClass("android/view/WindowInsets$Type");
        jint bars = env->CallStaticIntMethod(types, env->GetStaticMethodID(types, "systemBars", "()I"));
        jclass controller_cls = env->GetObjectClass(controller);
        env->CallVoidMethod(controller, env->GetMethodID(controller_cls, "hide", "(I)V"), bars);
        env->CallVoidMethod(controller, env->GetMethodID(controller_cls, "setSystemBarsBehavior", "(I)V"),
                            2 /* BEHAVIOR_SHOW_TRANSIENT_BARS_BY_SWIPE */);
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
}

void (*g_glue_focus_changed)(ANativeActivity*, int) = nullptr;

void on_focus_changed(ANativeActivity* a, int focused) {
    if (focused) hide_system_bars(a);  // Android shows the bars again after dialogs and app switches
    g_glue_focus_changed(a, focused);
}

// Leaving the app: pause the game like iOS does, and keep it quiet.
void set_active(bool active) {
    if (!g_started) return;
    const auto& st = settings::get();
    audio::set_volumes(active ? st.music_volume / 100.0f : 0, active ? st.effects_volume / 100.0f : 0);
    ns::post_to_main([active] { uikit::app_set_active(active); });
}

int32_t on_input(android_app*, AInputEvent* e) {
    if (!g_started) return 0;
    if (AInputEvent_getType(e) == AINPUT_EVENT_TYPE_KEY) {
        if (AKeyEvent_getKeyCode(e) != AKEYCODE_BACK) return 0;  // volume keys etc. keep working
        // Back (button or edge swipe) works like Escape on PC: closes menus or opens the pause menu.
        int32_t action = AKeyEvent_getAction(e);
        if (action == AKEY_EVENT_ACTION_DOWN && AKeyEvent_getRepeatCount(e) == 0) game::press_action("Back", true);
        else if (action == AKEY_EVENT_ACTION_UP) game::press_action("Back", false);
        return 1;
    }
    if (AInputEvent_getType(e) != AINPUT_EVENT_TYPE_MOTION) return 0;
    int32_t action = AMotionEvent_getAction(e);
    size_t index = (action & AMOTION_EVENT_ACTION_POINTER_INDEX_MASK) >> AMOTION_EVENT_ACTION_POINTER_INDEX_SHIFT;
    auto finger = [e](size_t i) { return kFirstFinger + AMotionEvent_getPointerId(e, i); };
    auto point = [e](size_t i) { return to_points(AMotionEvent_getX(e, i), AMotionEvent_getY(e, i)); };
    switch (action & AMOTION_EVENT_ACTION_MASK) {
    case AMOTION_EVENT_ACTION_DOWN:
    case AMOTION_EVENT_ACTION_POINTER_DOWN:
        send_touch(0, finger(index), point(index));
        break;
    case AMOTION_EVENT_ACTION_MOVE:
        for (size_t i = 0; i < AMotionEvent_getPointerCount(e); i++) send_touch(1, finger(i), point(i));
        break;
    case AMOTION_EVENT_ACTION_UP:
    case AMOTION_EVENT_ACTION_POINTER_UP:
        send_touch(2, finger(index), point(index));
        break;
    case AMOTION_EVENT_ACTION_CANCEL:
        for (size_t i = 0; i < AMotionEvent_getPointerCount(e); i++) send_touch(2, finger(i), point(i));
        break;
    default:
        return 0;
    }
    return 1;
}

void on_cmd(android_app* app, int32_t cmd) {
    switch (cmd) {
    case APP_CMD_INIT_WINDOW:
        if (!app->window) break;
        uikit::set_native_window(app->window);
        if (!g_started) {
            g_started = true;
            start_game();
        } else {
            gles::set_window(app->window);  // back from the background
        }
        break;
    case APP_CMD_TERM_WINDOW:
        // The window goes away when the app leaves the screen; the game keeps its state.
        uikit::set_native_window(nullptr);
        if (g_started) gles::set_window(nullptr);
        break;
    case APP_CMD_RESUME:
        set_active(true);
        break;
    case APP_CMD_PAUSE:
        set_active(false);
        break;
    }
}

}  // namespace

extern "C" JNIEXPORT void ANativeActivity_onCreate(ANativeActivity* activity, void* saved_state, size_t saved_state_size) {
    glue_ANativeActivity_onCreate(activity, saved_state, saved_state_size);
    g_glue_focus_changed = activity->callbacks->onWindowFocusChanged;
    activity->callbacks->onWindowFocusChanged = on_focus_changed;
    hide_system_bars(activity);
}

void android_main(android_app* app) {
    g_app = app;
    app->onAppCmd = on_cmd;
    app->onInputEvent = on_input;
    ANativeActivity_setWindowFlags(app->activity, AWINDOW_FLAG_KEEP_SCREEN_ON | AWINDOW_FLAG_FULLSCREEN, 0);
    if (const char* dir = app->activity->externalDataPath) {
        mkdir(dir, 0770);
        if (chdir(dir) != 0) __android_log_print(ANDROID_LOG_ERROR, "ib3", "cannot use %s", dir);
    }
    for (;;) {
        int events = 0;
        android_poll_source* source = nullptr;
        if (ALooper_pollOnce(-1, nullptr, &events, reinterpret_cast<void**>(&source)) >= 0 && source)
            source->process(app, source);
        if (app->destroyRequested) _exit(0);
    }
}
