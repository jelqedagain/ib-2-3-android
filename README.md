# Infinity Blade III for Android

> **Bring your own IPA.** This app does not contain the game. To play, you need your own copy of **Infinity Blade III for iOS as an `.ipa` file (version 1.4.4)**. The app installs the game from it the first time you open it.

**Infinity Blade III on Android phones and handhelds.** This port runs the original iOS release of Infinity Blade III on Android, full screen at 60 FPS, with the same touch controls as on iPhone.

Infinity Blade III was pulled from the App Store in 2018 and can no longer be bought or downloaded. This project exists so the game can still be played.

### [⬇ Download the APK (Android 11+, 64-bit)](https://archive.org/details/infinity-blade-3-android)

The same code also builds an **Infinity Blade II** app (version 1.3.5 `.ipa`, for example the Community Patch v2.5): [download it here](https://archive.org/details/infinity-blade-2-android). Both apps can be installed side by side.

![Infinity Blade III running on a Galaxy S25](docs/android-gameplay.jpg)

## Install and play

1. [Download the APK](https://archive.org/details/infinity-blade-3-android) on your phone and open it. The first time, Android asks you to allow installing apps from your browser or file manager. The download page also has the changelog and the source code of each version.
2. Put your Infinity Blade III `.ipa` (version 1.4.4) on the phone, for example in Downloads.
3. Open **Infinity Blade III**, tap **Choose .ipa file** and pick it. Installing takes under a minute. You can delete the `.ipa` afterwards.

After that, the app starts straight into the game.

![The install screen](docs/android-install.png)

## Controls

Touch works exactly as on an iPhone: tap, and swipe to attack, parry and move through menus.

The **Back** button or gesture backs out of a menu. In the game itself it opens the pause menu. Leaving the app pauses the game, and it picks up where you left off when you come back.

A Bluetooth or USB keyboard works too, with the layout of the Infinity Blade II PC port:

| Key | Action |
| --- | --- |
| A / D | Left / right fight button: dodge (block left / right with heavy weapons) |
| S | Center fight button: block, hold it (dodge down with dual blades) |
| F | Stab |
| Q | Super move |
| E | Magic (special attack in boss battles) |
| R | Final strike |
| 1 / 2 / 3 | Magic slots |
| Left Alt | Clash (mash) |
| Tab | Boss info |
| Left Shift | Fast-forward a cutscene (hold) |
| Enter | Accept the on-screen prompt (OK, Yes, Continue…) |
| Escape | Back out of a menu |
| P | Menu |
| Space | Pause |

Keys only work when the matching on-screen control would. For example, the dodge keys only work in a fight.

Game controllers are not supported yet. That is planned for the next release.

## Saves and settings

Everything the app keeps is in its folder on the phone's storage, `Android/data/com.ib3port.game/files`:

- `game/`: the installed game (about 3 GB)
- `userdata/Documents/SAVE/`: your progress. These are the game's own iOS save files.
- `settings.ini`: settings (see below)
- `ib3rt.log`: the log, for bug reports

**Uninstalling the app deletes this folder, including your saves.** To keep them, copy `userdata` to a computer first (over USB). Updating the app keeps everything.

There is no settings screen yet. You can create or edit `settings.ini` in that folder (for example from a computer over USB), then restart the app:

| Section | Setting | Values |
| --- | --- | --- |
| `[Display]` | `MaxFPS` | `60` (default), or `30` to save battery |
| `[Display]` | `RenderResolution` | The height the game renders at: `720`, `1080` (default) or `1440` |
| `[Graphics]` | `AntiAliasing` | `0` off, `1` FXAA (default), `2` MSAA 4x |
| `[Graphics]` | `DynamicShadows`, `HighResShadows`, `LightShafts`, `Bloom`, `DepthOfField` | `1` on, `0` off |
| `[Graphics]` | `Anisotropy` | Texture filtering: `1`, `2`, `4` (default), `8` or `16` |
| `[Audio]` | `MusicVolume`, `EffectsVolume` | `0` to `100` |
| `[Controls]` | An action and a key, e.g. `Stab=F` | Keyboard bindings, as Unreal key names |

## Requirements

- Android 11 or newer
- A 64-bit (ARM64) phone or handheld with OpenGL ES 3
- About 3 GB of free space for the game, plus room for the `.ipa` while installing

Tested on a Samsung Galaxy S25 and an AYN Odin 2.

## Known limitations

- Online features are unavailable: Game Center, Facebook, cloud saves, Clash Mobs and the in-game store.
- No controller support and no settings screen yet.
- This is a new project. Not every part of the game has been played through on it yet.

## Reporting problems

If something goes wrong, please open an [issue](../../issues), describe what you were doing, and attach `ib3rt.log` from `Android/data/com.ib3port.game/files`. If your file manager can't open that folder, copy the file to a computer over USB.

## How it works

This port is not an iPhone emulator. Phones have the same kind of processor as iPhones (ARM64), so the game's own program runs directly on the CPU, and the parts of iOS the game uses are implemented on Android:

- **Loader and libraries:** a Mach-O loader places the game's code where it expects to be, and binds its imports to implementations of the C library, pthreads, the Objective-C runtime, Foundation, UIKit, and GameKit/StoreKit (stubbed, offline). Calls into these go through small trampolines that save the game's registers.
- **Graphics:** the game's OpenGL ES 2 calls go to the device's own graphics driver. PVRTC textures (an iPhone-only format) are decoded in software.
- **Audio and video:** sound goes out through AAudio, with an emulation of Apple's 3D mixer. Music is decoded with minimp3, and movies with Android's hardware video decoder (MediaCodec).
- **Input:** touches become iOS touches. Keys are sent to Unreal Engine 3's input system under the same input names IB3's touch buttons use, so the game's own bindings run the real actions.
- **App:** the game runs in a NativeActivity. A small Java launcher installs the game from the `.ipa` and shows the game's alerts as Android dialogs.
- **Game-specific fixes:** a few engine hooks, found by reverse engineering, apply the settings to the engine config, keep the startup movie from playing twice, and power the Back and keyboard menu actions.

## Building from source

The build scripts run in Git Bash on Windows. They need, in `tools/android/`:

- The [Android NDK](https://developer.android.com/ndk/downloads) r30 (`android-ndk-r30`)
- The Android SDK build-tools (`build-tools`, with aapt2, d8, zipalign and apksigner)
- The Android 35 platform (`android-35/android.jar`)

Plus CMake 3.20 or newer (`tools/cmake`), Ninja (`tools/ninja.exe`), a JDK and Python 3.

```sh
git clone https://github.com/jelqedagain/ib-2-3-android.git
cd ib-2-3-android
scripts/build-apk.sh       # dist/InfinityBladeIII-Android.apk
./build-android.sh         # build-android/ib3android: a command-line build for testing over adb
```

The first run of `scripts/build-apk.sh` creates a signing key in `tools/android/`.

## Windows version

The same project also runs Infinity Blade III on Windows PCs, with keyboard and controller support and a settings launcher. The Windows version is not published here at the moment.

## Credits and legal

- Infinity Blade III is © Chair Entertainment Group / Epic Games. This project is not affiliated with or endorsed by Chair or Epic Games. Apart from the game's icon, used as the app icon, it does not distribute any of their files.
- Keyboard layout inspired by the community [Infinity Blade II PC port](https://archive.org/details/infinity-blade-ii-pc).
- Third-party components and their licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- The source code of this port is released under the [MIT License](LICENSE).
