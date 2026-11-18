**WinARM.apk: Android apps on Windows on Arm.** WinARM.apk runs Android apps on Windows 11 on Arm as ordinary Windows programs: each app gets its own window, Start menu entry and notifications. It is an unofficial port of [Mac.apk](https://github.com/kksimp/Mac.apk), based on [israel2176/win.apk](https://github.com/israel2176/win.apk) (which also has an x64 build).

> Versions are dated: 26.1118.1 is the second release of 2026-11-18.

> Preview build, not code-signed yet: SmartScreen may warn. Click **More info**, then **Run anyway**.

## Download
- **`Win.apk-Setup-arm64.exe`**, the installer (per user, no administrator rights), or **`Win.apk-windows-arm64.zip`**, portable: unzip it and run `Win.apk.exe`.

## What's new
- **The Remove button removes the app.** In the Remove dialog it only ticked and unticked "Also remove accounts and data". Tab now reaches every control in Win.apk's dialogs.
- **Two apps with the same file name stay two apps.** A second `app-release.apk` from another project no longer replaces the first.
- **An older version asks first.** Opening an older file of an installed app asks before replacing it, and **Keep installed version** is the default.
- **Closing the window no longer stops a download.** Store and Win.apk update downloads carry on; a finished Store app installs, and a finished update waits for your next **Update**.
- **Closing Win.apk no longer crashes** or takes seconds to exit.
- **Uninstalling cleans up.** File types and the apps' desktop shortcuts no longer point at a removed program, and a reinstall brings the apps' shortcuts back.
- **Clear library tells the truth.** It names what it could not remove, and says that permissions are reset.
- **Running apps show correctly.** The badge, the running count, the tile menu and the Stop row in App details agree and update by themselves, also for apps started from a shortcut.

## Highlights
- **Native on Windows on Arm.** Apps' `arm64-v8a` code runs as it is on the CPU, with no emulation or translation. The Store picks arm64 builds for you.
- **GPU rendering.** OpenGL apps and games draw straight to the window on the GPU. On by default on a Direct3D 11 GPU; turn it off under Settings > Apps > **Enable GPU rendering**, for all apps or for one.
- **A maximized window is the phone turned on its side**, at a density that draws it pixel for pixel.

## Known limitations
- Apps with only `x86_64` or only 32-bit ARM (`armeabi-v7a`) code do not run.
- A library that uses the x18 register is refused (two libGDX games of 92 tested apps).
- No in-app purchases, Play Integrity or Vulkan; OpenSL ES sound is missing; sensors report a phone lying still.
- A minimized app is paused, as a phone app sent to the home screen is; an app that quits when paused (glmark2) ends its run.
- The dim behind some dialogs is darker than on Android ([#10](https://github.com/airisu-dev/WinARM.apk/issues/10)).

Found a problem? Use **Report a problem** in WinARM.apk, or [open an issue](https://github.com/airisu-dev/WinARM.apk/issues/new/choose).
