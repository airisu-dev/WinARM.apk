**WinARM.apk: Android apps on Windows on Arm.** WinARM.apk runs Android apps on Windows 11 on Arm as ordinary Windows programs: each app gets its own window, Start menu entry and notifications. It is an unofficial port of [Mac.apk](https://github.com/kksimp/Mac.apk), based on [israel2176/win.apk](https://github.com/israel2176/win.apk) (which also has an x64 build).

> Versions are dated: 26.1118.0 is the first release of 2026-11-18.

> Preview build, not code-signed yet: SmartScreen may warn. Click **More info**, then **Run anyway**.

## Download
- **`Win.apk-Setup-arm64.exe`**, the installer (per user, no administrator rights), or **`Win.apk-windows-arm64.zip`**, portable: unzip it and run `Win.apk.exe`.

## What's new
- **Now built on Mac.apk's own tree (v1.0.2379)**, with the Windows port carried on top of it, so upstream's fixes come along.
- **GPU rendering.** OpenGL apps and games draw straight to the window on the GPU, with no copy back to memory: glmark2 goes from 58 to 118. On by default on a Direct3D 11 GPU; turn it off under Settings > Apps > **Enable GPU rendering**, for all apps or for one.
- **Screens on top of each other draw right.** An activity behind another one no longer shows through it, and a see-through screen shows what is under it: glmark2's benchmark no longer stays stuck on its menu, and Waze's sign-in steps draw.
- **AppCompat dialogs that take their title setting from the activity's theme open** instead of failing (Waze's sign-in sheet).

## Highlights
- **Native on Windows on Arm.** Apps' `arm64-v8a` code runs as it is on the CPU, with no emulation or translation. The Store picks arm64 builds for you.
- **A maximized window is the phone turned on its side**, at a density that draws it pixel for pixel.

## Known limitations
- Apps with only `x86_64` or only 32-bit ARM (`armeabi-v7a`) code do not run.
- A library that uses the x18 register is refused (two libGDX games of 92 tested apps).
- No in-app purchases, Play Integrity or Vulkan; OpenSL ES sound is missing; sensors report a phone lying still.
- A minimized app is paused, as a phone app sent to the home screen is; an app that quits when paused (glmark2) ends its run.
- The dim behind some dialogs is darker than on Android ([#10](https://github.com/airisu-dev/WinARM.apk/issues/10)).

Found a problem? Use **Report a problem** in WinARM.apk, or [open an issue](https://github.com/airisu-dev/WinARM.apk/issues/new/choose).
