# Changelog

One codebase builds two Android apps that share a version number:

- **IB2 app:** Infinity Blade II (`com.ib2port.game`)
- **IB3 app:** Infinity Blade III (`com.ib3port.game`)

Each entry says which app a change is for. "Both" means the shared code, so it applies to both apps. A version number is skipped for an app when that app was not released at that version.

## 1.3.3 (2026-09-30)
Released: **IB3 app only** (Infinity Blade II behaves exactly as in 1.3.2: the frame-rate changes below are switched off for it; release tag `ib3-v1.3.3`; from now on each app's releases have their own tag series, `ib3-v…` and `ib2-v…`).

- **IB3:** Aimed at frame-rate drops below 60 on some Adreno phones (a Red Magic 11 Pro dropped to about 54 in outdoor scenes and the hideout). The cause is not confirmed; the changes are:
  - The window surface is released between frames only on GPUs other than Adreno (the fix was for Mali phones; on Adreno it may only cost frame time).
  - The game tells Android it draws at 60 FPS, so 120 and 144 Hz screens run at a multiple of it instead of showing uneven frame times.
  - Shaders with constant initializers are no longer rewritten, so Infinity Blade III's shaders reach the GPU exactly as the game ships them (only its FXAA pass and one vertex shader were affected).
- **Both:** The log has a `perf:` line every 10 seconds (frames per second, worst frame, CPU use) so a bug report shows how the game ran.
- **Both:** Share logs leaves out the game's file-open lines ("OutPath"), which filled most of the log.

## 1.3.2 (2026-09-30)
Released: **IB2 app** (the IB3 app on archive.org stays at 1.3.1; an IB3 build of the same code, with no IB3-specific changes, is attached to the GitHub release).

- **IB2:** Full screen on phones longer than 16:9 (for example Galaxy S25/S26 and Pixel phones). The app serves the game patched copies of its HUD layout script and menu backdrops, so the HUD fills the screen, touch zones line up, and the menu backdrops cover the wider view. If the patch does not apply, it falls back to 16:9 with black bars. This replaces the 1.2.2 behaviour.
- **Both:** A watchdog writes where every game thread is to the log when the app stops drawing while on screen.

## 1.3.1 (2026-09-30)
Released: **IB3 app only**.

- **Both (code), IB3 (released):** Share logs also includes how the app last closed (Android exit reasons and crash text), and Java crashes are logged.

## 1.3.0 (2026-09-29)
Released: **both apps**.

- **Both:** Game controller support (built-in handheld controls and Bluetooth/USB pads).
- **IB2:** Fixed the black screen after Isa's scene at the end of the game: the game asked for a missing image by nil name, which now returns no image, as on iOS.
- **IB2:** Store-only memory barriers are made full ones, fixing a render-thread crash on some newer phones (seen on a Galaxy S25 FE).

## 1.2.4 (2026-09-29)
Released: **both apps**.

- **Both:** App menu with Play, Back up saves, Restore saves and Share logs (for bug reports).
- **Both:** The game gets a fresh window surface after the boot screen, fixing wrong textures on some Mali GPUs.

## 1.2.3 (2026-09-29)
Released: **both apps**.

- **Both:** The window surface is released between frames, fixing corrupted pictures after switching apps on Mali GPUs.
- **IB2:** The game's Game Center, account, iAd and iCloud requests now always get an offline answer.

## 1.2.2 (2026-09-29)
Released: **IB2 app** (the IB3 app was rebuilt at this version with no changes for it).

- **IB2:** Menus and fight buttons were zoomed in and cut off on phones longer than 16:9. The app now keeps a 16:9 screen there, with black bars at the sides (replaced in 1.3.2).

## 1.2.1 (2026-09-29)
Released: **IB3 app** update, and the **first release of the IB2 app**.

- **IB2:** First release, built from the same code as the IB3 app, with sharp character shadows on non-Apple GPUs.
- **IB3:** Very large movies are converted at half size.

## 1.2.0 (2026-09-29)
**IB3 app** (the IB2 app did not exist yet).

- Movies, full-screen 60 FPS, the game's alerts as Android dialogs, and installing the game from an `.ipa`.
