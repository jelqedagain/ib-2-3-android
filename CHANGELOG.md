# Changelog

## 1.3.2 (2026-09-30)
- Full screen on phones longer than 16:9 for the Infinity Blade II app: the HUD layout limit is raised in a patched copy served to the app, and the menu backdrops are widened to match. Falls back to 16:9 if the patch does not apply.
- A watchdog writes where every game thread is to the log if the app stops drawing while on screen.

## 1.3.1 (2026-09-30)
- Share logs includes how the app last closed (exit reasons and crash text); Java crashes are logged.

## 1.3.0 (2026-09-29)
- Game controller support.
- Fixed the black screen after the ending scene in Infinity Blade II (a nil image name now returns no image, as on iOS).
- Memory barriers in Infinity Blade II are made full ones, fixing a render-thread crash on some phones.

## 1.2.4 (2026-09-29)
- App menu: Play, Back up saves, Restore saves, Share logs.
- A fresh window surface after the boot screen (fixes wrong textures on some Mali GPUs).

## 1.2.3 (2026-09-29)
- Window surface released between frames (fixes corruption after resuming on Mali GPUs).
- Offline answers for the game's Game Center, account and iCloud requests.

## 1.2.2 (2026-09-29)
- Infinity Blade II keeps a 16:9 screen on longer phones (replaced in 1.3.2).

## 1.2.1 (2026-09-29)
- Very large movies are converted at half size.

## 1.2.0 (2026-09-29)
- Movies, full-screen 60 FPS, Android dialogs, install from an `.ipa`.
