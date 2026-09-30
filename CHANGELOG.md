# Changelog

One codebase builds two Android apps that share a version number:

- **IB2 app:** Infinity Blade II (`com.ib2port.game`)
- **IB3 app:** Infinity Blade III (`com.ib3port.game`)

Each entry says which app a change is for. "Both" means the shared code, so it applies to both apps. A version number is skipped for an app when that app was not released at that version.

## 1.3.2 (2026-09-30)
Released: **IB2 app only** (the IB3 app stays at 1.3.1).

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
