# Infinity Blade III PC Port

> **Bring your own IPA.** This download does not contain the game. To play, you need your own copy of **Infinity Blade III for iOS as an `.ipa` file (version 1.4.4)**. The launcher installs the game from it the first time you run it.

**Infinity Blade III on Windows PC.** This port runs the original iOS release of Infinity Blade III on 64-bit Windows, with keyboard controls, a settings launcher, and higher resolutions.

Infinity Blade III was pulled from the App Store in 2018 and can no longer be bought or downloaded. This project exists so the game can still be played.

### [⬇ Download the latest version (Windows 10/11, 64-bit)](https://github.com/jelqscape/Infinity-Blade-III-PC-Port/releases/latest/download/InfinityBladeIII-PCPort-win64.zip)

![The launcher](docs/launcher.png)

## Download and play

1. [Download `InfinityBladeIII-PCPort-win64.zip`](https://github.com/jelqscape/Infinity-Blade-III-PC-Port/releases/latest/download/InfinityBladeIII-PCPort-win64.zip). All versions are on the [Releases](../../releases) page.
2. Extract it into its own folder. The installed game needs about 3.5 GB of disk space.
3. Double-click **Infinity Blade III.exe**. This opens the launcher.
4. The first time, click **Install game...** and choose your Infinity Blade III `.ipa` (version 1.4.4, 64-bit). An `.ipa` placed in the same folder is found automatically.
5. Click **Play**.

After that, just run **Infinity Blade III.exe** and click **Play**. The first start takes about 30–60 seconds while the logos and the loading animation play.

## Controls

The mouse works as your finger: click to tap, drag to swipe. Swipes attack, parry and move through menus, just like on iOS.

The keyboard layout follows the Infinity Blade II PC port:

| Key | Action |
| --- | --- |
| A / D | Dodge left / right |
| S | Block (hold) |
| F | Stab |
| Q | Super move |
| E | Magic |
| 1 / 2 / 3 | Magic slots |
| Left Alt | Clash (mash) |
| Tab | Boss info |
| Left Shift | Fast-forward a cutscene (hold) |
| Enter | Accept the on-screen prompt (OK, Yes, Continue…) |
| Escape | Back out of a menu; in the game itself, asks to quit |
| P | Menu |
| Space | Pause |
| F11 or Alt+Enter | Toggle fullscreen |

Game keys work exactly when the matching on-screen control would. For example, the dodge keys only work in a fight, and the menu key doesn't work during movies. A key can't do anything a touch couldn't at that moment.

Every key except fullscreen can be changed in the launcher (**Key bindings...**).

## Settings

The launcher changes:

- **Display:** windowed or fullscreen, window size, render resolution (720p up to 4K; 1080p is the original iPhone 6 Plus resolution), 30 FPS (original) or 60 FPS (experimental), and an FPS counter in the title bar.
- **Graphics:** anti-aliasing (off, FXAA, MSAA 4x), texture filtering, dynamic shadows, high-resolution shadows, light shafts, bloom and depth of field. These are the game's own renderer options, so they behave as they did on iOS devices.
- **Audio:** separate music and effects volume.

Settings are stored in `settings.ini` next to the executable.

## Saves

Progress is saved in `userdata\Documents\SAVE` next to the executable. Click **Open save folder** in the launcher to get there. The files are the game's own iOS save files.

## System requirements

- Windows 10 or 11, 64-bit
- A DirectX 11 graphics card
- A 4-core CPU or better (the game runs through a JIT compiler)
- About 3.5 GB of free disk space

## Known limitations

- Online features are unavailable: Game Center, Facebook, cloud saves, Clash Mobs and the in-game store.
- Loading takes longer than on an iPhone.
- 60 FPS mode is experimental.
- Controller support is not implemented yet.
- On Windows "N" editions without the Media Feature Pack, the movies are skipped.
- This is a new project. Not every part of the game has been played through on it yet.

## Reporting problems

If the game stops with an error, it saves the details to `ib3rt.log` next to the executable. Please attach that file to your [issue](../../issues), and describe what you were doing.

## How it works

This port is not an iPhone emulator. It runs the game's ARM64 program and implements the parts of iOS the game uses, directly on Windows:

- **CPU:** the ARM64 code is translated to x86-64 on the fly by [dynarmic](https://github.com/azahar-emu/dynarmic).
- **Loader and libraries:** a Mach-O loader binds the game's imports to Windows implementations of the C library, pthreads, the Objective-C runtime, Foundation, UIKit, and GameKit/StoreKit (stubbed, offline).
- **Graphics:** OpenGL ES 2 calls go to [ANGLE](https://chromium.googlesource.com/angle/angle), which draws with Direct3D 11. PVRTC textures are decoded in software.
- **Audio and video:** audio goes out through WASAPI, with an emulation of Apple's 3D mixer. Music is decoded with minimp3, and movies with Windows Media Foundation.
- **Input:** mouse clicks and drags become touches. Keys are sent to Unreal Engine 3's input system under the same input names IB3's touch buttons use, so the game's own bindings run the real actions.
- **Game-specific fixes:** a small set of engine hooks, found by reverse engineering, applies launcher settings to the engine config, keeps the startup movie from playing twice, and powers the Enter/Escape menu actions.

## Building from source

Requirements:

- [LLVM-MinGW](https://github.com/mstorsjo/llvm-mingw) (x86_64, UCRT)
- CMake 3.20 or newer
- Ninja
- Git Bash (or another bash) and curl

Steps:

```sh
git clone --recursive https://github.com/jelqscape/Infinity-Blade-III-PC-Port.git
cd Infinity-Blade-III-PC-Port
scripts/fetch-deps.sh      # dynarmic submodule and Boost headers
./build.sh                 # builds build/ib3rt.exe
```

`build.sh` uses a portable toolchain from `tools/` (`tools/llvm-mingw`, `tools/cmake`, `tools/ninja.exe`) if one is there, otherwise the tools on your `PATH`.

`scripts/package.sh` builds the release zip in `dist/` (the version number lives in `src/app.rc`).

For development, `./test.sh <seconds> [options]` runs the game hidden, with its own save folder, and writes `build/testrun/ib3rt.log`. Useful options:

- `-v`: verbose logging
- `-shot N`: save a screenshot every N frames
- `-profile`: per-thread sampling profiler
- `-script file`: timed key presses, taps, swipes and screenshots (see `src/game/script.cpp`)

## Credits and legal

- Infinity Blade III is © Chair Entertainment Group / Epic Games. This project is not affiliated with or endorsed by Chair or Epic Games. Apart from the game's icon, used as the program icon, it does not distribute any of their files. The launcher banner is made at run time from your own installed copy of the game.
- Keyboard layout inspired by the community [Infinity Blade II PC port](https://archive.org/details/infinity-blade-ii-pc).
- Third-party components and their licenses are listed in [THIRD_PARTY_NOTICES.md](THIRD_PARTY_NOTICES.md).
- The source code of this port is released under the [MIT License](LICENSE).
