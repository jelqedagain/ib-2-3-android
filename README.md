# ib-2-3-android

An experimental iOS emulator for Android: a compatibility layer that runs ARM64 iOS apps built on Unreal Engine 3. It is a hobby and research project.

**This repository contains no game files.** It is only the source code of the emulator: a Mach-O loader, and implementations of the parts of iOS the apps call (C library, Objective-C runtime, Foundation, UIKit) on top of Android's OpenGL ES, AAudio and MediaCodec. To run a game you need your own copy of the app as an `.ipa` file; nothing here provides one.

## How it works

Phones and iPhones both use ARM64, so on Android the app's code runs directly on the CPU (on Windows it is translated by a JIT). Everything the app expects from iOS is emulated by this project:

- **Loader and libraries:** a Mach-O loader, plus the C library, pthreads, the Objective-C runtime, Foundation and UIKit.
- **Graphics:** OpenGL ES calls go to the device's driver.
- **Audio and video:** AAudio and MediaCodec.
- **Input:** touch, keyboard and game controllers.

## Building

The scripts run in Git Bash on Windows and need the Android NDK, SDK build-tools, CMake, Ninja, a JDK and Python 3 in `tools/`.

```sh
scripts/build-apk.sh
```

## Notes

- Not affiliated with or endorsed by any game developer or publisher. All trademarks belong to their owners.
- Third-party components and their licenses: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- Released under the [MIT License](LICENSE).
