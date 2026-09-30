# Changelog

Two Android apps, IB2 and IB3, are built from one codebase. Each has its own version history; a version number is skipped for an app when that app did not change. Tags are `ib2-v<version>` and `ib3-v<version>`.

# IB2

## 1.4 beta (2026-09-30)
- Beta: back up your saves (launcher, Back up saves) before trying it.
- New launcher look, and a Settings page (writes settings.ini): frame rate limit 30 / 60 / 120 (120 is experimental), an FPS counter drawn in the corner, sharper shadows, music and effects volume, controller on/off and speeds.
- Edit save page: change gold, level, XP, stat points, the four stats and bloodline before pressing Play. The game puts the numbers into the save as it loads it and saves them itself; the page shows the current values and keeps every number within what the game can take (level up to 50, XP within the level). A higher level also gives the 2 stat points per level a level-up gives. The game's checks that undo edited numbers are skipped for saves that were edited.
- Settings: Screen can be full screen or 16:9 with black bars (for players who find the full-screen HUD stretched).

## 1.3.5 (2026-09-30)
- Textures use about a quarter of the memory. Android GPUs can't read the game's PVRTC textures, and unpacked to full size they took 700-900 MB, with GPU memory peaks of 1.2 GB, enough for Android to close the game for low memory on 6-8 GB phones. They are now re-encoded as ETC2, which Android GPUs read directly: about 220 MB of textures, GPU memory peak about 330 MB.
- The log has a `perf:` line every 10 seconds (frames per second, worst frame, CPU use, RAM and GPU memory) so a bug report shows how the game ran.
- Share logs leaves out the game's file-open lines ("OutPath"), which filled most of the log.

## 1.3.2 (2026-09-30)
- Full screen on phones longer than 16:9 (for example Galaxy S25/S26 and Pixel phones). The app serves the game patched copies of its HUD layout script and menu backdrops, so the HUD fills the screen, touch zones line up, and the menu backdrops cover the wider view. If the patch does not apply, it falls back to 16:9 with black bars.
- Share logs includes how the app last closed (Android exit reasons and crash text); Java crashes are logged.
- A watchdog writes where every game thread is to the log when the app stops drawing while on screen.

## 1.3.0 (2026-09-29)
- Game controller support (built-in handheld controls and Bluetooth/USB pads).
- Fixed the black screen after the final scene of the game: the game asked for a missing image by nil name, which now returns no image, as on iOS.
- Store-only memory barriers are made full ones, fixing a render-thread crash on some newer phones (seen on a Galaxy S25 FE).

## 1.2.4 (2026-09-29)
- App menu with Play, Back up saves, Restore saves and Share logs (for bug reports).
- The game gets a fresh window surface after the boot screen, fixing wrong textures on some Mali GPUs.

## 1.2.3 (2026-09-29)
- The window surface is released between frames, fixing corrupted pictures after switching apps on Mali GPUs.
- The game's Game Center, account, iAd and iCloud requests now always get an offline answer.

## 1.2.2 (2026-09-29)
- Menus and fight buttons were zoomed in and cut off on phones longer than 16:9. The app now keeps a 16:9 screen there, with black bars at the sides (replaced in 1.3.2).

## 1.2.1 (2026-09-29)
- First IB2 build, with sharp character shadows on GPUs other than Apple's.

# IB3

## 1.4 beta (2026-09-30)
- Beta: back up your saves (launcher, Back up saves) before trying it.
- New launcher look, and a Settings page (writes settings.ini): frame rate limit 30 / 60 / 120 (120 is experimental), an FPS counter drawn in the corner, sharper shadows, music and effects volume, controller on/off and speeds.
- Edit save page: change gold, level, XP, stat points, the four stats and awakening before pressing Play. The game puts the numbers into the save as it loads it and saves them itself; the page shows the current values and keeps every number within what the game can take (level up to 50, XP within the level). A higher level also gives the 2 stat points per level a level-up gives. The game's checks that undo edited numbers are skipped for saves that were edited.
- Edit save also sets chips and gem bag upgrades (up to 3), and can give every item or all perks (the developers' all-perks cheat also makes the character level 50 with every stat at 100).
- Settings: resolution 720p / 1080p / 1440p, anti-aliasing, shadows, light shafts, bloom, depth of field and texture filtering. The 30 and 120 limits now hold (the game raised its own limit to 62 after reading its settings).

## 1.3.4 (2026-09-30)
- Fixed the game being closed by Android for low memory, seen right after the tutorial on a Galaxy S21 FE. Android GPUs can't read the game's PVRTC textures, and unpacked to full size they took over 1.2 GB at the beach (about 2 GB at the peak, while the tutorial hands over to the beach). They are now re-encoded as ETC2, which Android GPUs read directly: about 235 MB of textures at the beach, GPU memory peak about 390 MB.
- The `perf:` log line also shows memory: RAM, GPU memory (textures and peak) and the texture re-encoding totals.

## 1.3.3 (2026-09-30)
- Aimed at frame-rate drops below 60 on some Adreno phones (a Red Magic 11 Pro dropped to about 54 in outdoor scenes and the hideout). The cause is not confirmed; the changes are:
  - The window surface is released between frames only on GPUs other than Adreno (the fix was for Mali phones; on Adreno it may only cost frame time).
  - The game tells Android it draws at 60 FPS, so 120 and 144 Hz screens run at a multiple of it instead of showing uneven frame times.
  - Shaders with constant initializers are no longer rewritten, so the game's shaders reach the GPU exactly as it ships them (only its FXAA pass and one vertex shader were affected).
- The log has a `perf:` line every 10 seconds (frames per second, worst frame, CPU use) so a bug report shows how the game ran.
- Share logs leaves out the game's file-open lines ("OutPath"), which filled most of the log.

## 1.3.1 (2026-09-30)
- Game controller support (built-in handheld controls and Bluetooth/USB pads).
- Share logs includes how the app last closed (Android exit reasons and crash text); Java crashes are logged.

## 1.2.4 (2026-09-29)
- App menu with Play, Back up saves, Restore saves and Share logs (for bug reports).
- The game gets a fresh window surface after the boot screen, fixing wrong textures on some Mali GPUs.

## 1.2.3 (2026-09-29)
- The window surface is released between frames, fixing corrupted pictures after switching apps on Mali GPUs.

## 1.2.1 (2026-09-29)
- Very large movies are converted at half size.

## 1.2.0 (2026-09-29)
- Movies, full-screen 60 FPS, the game's alerts as Android dialogs, and installing the game from an `.ipa`.
