// iOS alerts on Android: shown as Android dialogs by GameActivity.showAlert; the calling thread
// waits for the answer, as with the PC's task dialogs. Command-line runs answer them as dismissed.
#include "win/dialogs.h"
#include "libc/format.h"
#include "port/android/android_app.h"
#include <condition_variable>
#include <map>
#include <mutex>

namespace win {

namespace {

struct Answer {
    bool done = false;
    int button = -1;
    std::wstring text;
};
std::mutex g_mutex;
std::condition_variable g_cv;
std::map<int, Answer> g_answers;
int g_next_id = 1;

jstring to_java(JNIEnv* env, const std::wstring& s) {
    std::u16string u;  // wchar_t is UTF-32 here; Java strings are UTF-16
    for (wchar_t c : s) {
        char32_t cp = (char32_t)c;
        if (cp >= 0x10000) {
            cp -= 0x10000;
            u += (char16_t)(0xD800 + (cp >> 10));
            u += (char16_t)(0xDC00 + (cp & 0x3FF));
        } else {
            u += (char16_t)cp;
        }
    }
    return env->NewString(reinterpret_cast<const jchar*>(u.data()), (jsize)u.size());
}

std::wstring from_java(JNIEnv* env, jstring s) {
    std::wstring out;
    if (!s) return out;
    const jchar* p = env->GetStringChars(s, nullptr);
    jsize n = env->GetStringLength(s);
    for (jsize i = 0; i < n; i++) {
        char32_t c = p[i];
        if (c >= 0xD800 && c < 0xDC00 && i + 1 < n) c = 0x10000 + ((c - 0xD800) << 10) + (p[++i] - 0xDC00);
        out += (wchar_t)c;
    }
    env->ReleaseStringChars(s, p);
    return out;
}

void JNICALL alert_result(JNIEnv* env, jclass, jint id, jint button, jstring text) {
    std::wstring t = from_java(env, text);
    {
        std::lock_guard lock(g_mutex);
        Answer& a = g_answers[id];
        a.done = true;
        a.button = button;
        a.text = std::move(t);
    }
    g_cv.notify_all();
}

// Shows an alert and waits for it; false if there is no app to show it in.
bool show(const std::wstring& title, const std::wstring& message, const std::vector<std::wstring>& buttons, int cancel_index,
          int style, std::wstring& text, int& button) {
    ANativeActivity* a = android::activity();
    JNIEnv* env = android::env();
    if (!a || !env) return false;
    jclass cls = env->GetObjectClass(a->clazz);
    jmethodID show_alert =
        env->GetMethodID(cls, "showAlert", "(ILjava/lang/String;Ljava/lang/String;[Ljava/lang/String;IILjava/lang/String;)V");
    if (!show_alert) {
        env->ExceptionClear();
        return false;
    }
    jobjectArray jbuttons = env->NewObjectArray((jsize)buttons.size(), env->FindClass("java/lang/String"), nullptr);
    for (size_t i = 0; i < buttons.size(); i++) {
        jstring b = to_java(env, buttons[i]);
        env->SetObjectArrayElement(jbuttons, (jsize)i, b);
        env->DeleteLocalRef(b);
    }
    int id;
    {
        std::lock_guard lock(g_mutex);
        id = g_next_id++;
    }
    jstring jtitle = to_java(env, title), jmessage = to_java(env, message), jtext = to_java(env, text);
    env->CallVoidMethod(a->clazz, show_alert, (jint)id, jtitle, jmessage, jbuttons, (jint)cancel_index, (jint)style, jtext);
    env->DeleteLocalRef(jtitle);
    env->DeleteLocalRef(jmessage);
    env->DeleteLocalRef(jtext);
    env->DeleteLocalRef(jbuttons);
    env->DeleteLocalRef(cls);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return false;
    }
    std::unique_lock lock(g_mutex);
    g_cv.wait(lock, [&] { return g_answers[id].done; });
    button = g_answers[id].button;
    text = g_answers[id].text;
    g_answers.erase(id);
    return true;
}

}  // namespace

int choose(HWND, const std::wstring& title, const std::wstring& message, const std::vector<std::wstring>& buttons,
           int cancel_index) {
    std::wstring text;
    int button = -1;
    if (show(title, message, buttons, cancel_index, 0, text, button)) return button;
    int pick = cancel_index >= 0 ? cancel_index : (int)buttons.size() - 1;
    LOG_INFO("alert \"%s\": %s -> %s", libc::wide_to_utf8(title).c_str(), libc::wide_to_utf8(message).c_str(),
             pick >= 0 && pick < (int)buttons.size() ? libc::wide_to_utf8(buttons[pick]).c_str() : "(none)");
    return pick;
}

bool prompt_text(HWND, const std::wstring& title, const std::wstring& message, std::wstring& text, bool password,
                 const std::wstring& ok_label, const std::wstring& cancel_label) {
    int button = -1;
    std::wstring typed = text;
    if (!show(title, message, {cancel_label, ok_label}, 0, password ? 1 : 2, typed, button)) {
        LOG_INFO("text prompt \"%s\": cancelled", libc::wide_to_utf8(title).c_str());
        return false;
    }
    if (button != 1) return false;
    text = typed;
    return true;
}

}  // namespace win

namespace android {

void register_dialog_natives(JNIEnv* env, jclass activity_class) {
    static const JNINativeMethod methods[] = {
        {"nativeAlertResult", "(IILjava/lang/String;)V", reinterpret_cast<void*>(win::alert_result)},
    };
    if (env->RegisterNatives(activity_class, methods, 1) != JNI_OK) env->ExceptionClear();
}

}  // namespace android
