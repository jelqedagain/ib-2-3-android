# ib-2-3-android

An experimental iOS emulator for Android: a compatibility layer that runs ARM64 iOS apps built on Unreal Engine 3. It is a hobby and research project.

**This repository contains no game files.** It is only the source code of the emulator: a Mach-O loader, and implementations of the parts of iOS the apps call (C library, Objective-C runtime, Foundation, UIKit) on top of Android's OpenGL ES, AAudio and MediaCodec. To run a game you need your own copy of the app as an `.ipa` file; nothing here provides one.

## How it works

Phones and iPhones both use ARM64, so on Android the app's code runs directly on the CPU (on Windows it is translated by a JIT). Everything the app expects from iOS is emulated by this project:

- **Loader and libraries:** a Mach-O loader, plus the C library, pthreads, the Objective-C runtime, Foundation and UIKit.
- **Graphics:** OpenGL ES calls go to the device's driver.
- **Audio and video:** AAudio and MediaCodec.
- **Input:** touch, keyboard and game controllers.

## Versions

<details>
<summary><b>Click for IB2</b></summary>

**1.3.2** (2026-09-30)

- Full screen on phones longer than 16:9 (for example Galaxy S25/S26 and Pixel phones). The app serves the game patched copies of its HUD layout script and menu backdrops, so the HUD fills the screen, touch zones line up, and the menu backdrops cover the wider view. If the patch does not apply, it falls back to 16:9 with black bars.
- Share logs includes how the app last closed (Android exit reasons and crash text); Java crashes are logged.
- A watchdog writes where every game thread is to the log when the app stops drawing while on screen.

**1.3.0** (2026-09-29)

- Game controller support (built-in handheld controls and Bluetooth/USB pads).
- Fixed the black screen after the final scene of the game: the game asked for a missing image by nil name, which now returns no image, as on iOS.
- Store-only memory barriers are made full ones, fixing a render-thread crash on some newer phones (seen on a Galaxy S25 FE).

**1.2.4** (2026-09-29)

- App menu with Play, Back up saves, Restore saves and Share logs (for bug reports).
- The game gets a fresh window surface after the boot screen, fixing wrong textures on some Mali GPUs.

**1.2.3** (2026-09-29)

- The window surface is released between frames, fixing corrupted pictures after switching apps on Mali GPUs.
- The game's Game Center, account, iAd and iCloud requests now always get an offline answer.

**1.2.2** (2026-09-29)

- Menus and fight buttons were zoomed in and cut off on phones longer than 16:9. The app now keeps a 16:9 screen there, with black bars at the sides (replaced in 1.3.2).

**1.2.1** (2026-09-29)

- First IB2 build, with sharp character shadows on GPUs other than Apple's.

</details>

<details>
<summary><b>Click for IB3</b></summary>

**1.3.3** (2026-09-30)

- Aimed at frame-rate drops below 60 on some Adreno phones (a Red Magic 11 Pro dropped to about 54 in outdoor scenes and the hideout). The cause is not confirmed; the changes are:
  - The window surface is released between frames only on GPUs other than Adreno (the fix was for Mali phones; on Adreno it may only cost frame time).
  - The game tells Android it draws at 60 FPS, so 120 and 144 Hz screens run at a multiple of it instead of showing uneven frame times.
  - Shaders with constant initializers are no longer rewritten, so the game's shaders reach the GPU exactly as it ships them (only its FXAA pass and one vertex shader were affected).
- The log has a `perf:` line every 10 seconds (frames per second, worst frame, CPU use) so a bug report shows how the game ran.
- Share logs leaves out the game's file-open lines ("OutPath"), which filled most of the log.

**1.3.1** (2026-09-30)

- Game controller support (built-in handheld controls and Bluetooth/USB pads).
- Share logs includes how the app last closed (Android exit reasons and crash text); Java crashes are logged.

**1.2.4** (2026-09-29)

- App menu with Play, Back up saves, Restore saves and Share logs (for bug reports).
- The game gets a fresh window surface after the boot screen, fixing wrong textures on some Mali GPUs.

**1.2.3** (2026-09-29)

- The window surface is released between frames, fixing corrupted pictures after switching apps on Mali GPUs.

**1.2.1** (2026-09-29)

- Very large movies are converted at half size.

**1.2.0** (2026-09-29)

- Movies, full-screen 60 FPS, the game's alerts as Android dialogs, and installing the game from an `.ipa`.

</details>

## Building

The scripts run in Git Bash on Windows and need the Android NDK, SDK build-tools, CMake, Ninja, a JDK and Python 3 in `tools/`.

```sh
scripts/build-apk.sh
```

## Notes

- Not affiliated with or endorsed by any game developer or publisher. All trademarks belong to their owners.
- Third-party components and their licenses: [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- Released under the [MIT License](LICENSE).
