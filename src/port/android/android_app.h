// The running Android app, for code that needs Java (dialogs). Command-line runs have none.
#pragma once
#include <android/native_activity.h>
#include <jni.h>

namespace android {

ANativeActivity* activity();  // nullptr in command-line runs
void set_activity(ANativeActivity* a);
JNIEnv* env();  // for the calling thread (attached to the VM on first use)

// Registers GameActivity's native methods (dialogs.cpp); called from ANativeActivity_onCreate.
void register_dialog_natives(JNIEnv* env, jclass activity_class);

}  // namespace android
