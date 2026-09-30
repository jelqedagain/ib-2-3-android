// The running Android app, for code that needs Java (dialogs). Command-line runs have none.
#pragma once
#include <android/input.h>
#include <android/native_activity.h>
#include <jni.h>

namespace android {

ANativeActivity* activity();  // nullptr in command-line runs
void set_activity(ANativeActivity* a);
JNIEnv* env();  // for the calling thread (attached to the VM on first use)

// Registers GameActivity's native methods (dialogs.cpp); called from ANativeActivity_onCreate.
void register_dialog_natives(JNIEnv* env, jclass activity_class);

// Takes an input event if it comes from a game controller (pad.cpp); called by the app's input handler.
bool pad_event(const AInputEvent* e);

}  // namespace android
