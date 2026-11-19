# WinARM.apk: notes for Claude

Runs Android apps on Windows 11 on Arm as normal Windows programs: Java on
HotSpot, the app's arm64-v8a `.so` files loaded by our own ELF loader over a
Bionic shim and run natively, graphics on ANGLE (D3D11). ARM64 only; there is no
x64 build and no translation layer. A soft fork of Mac.apk (`runtime/` is
upstream's) carrying a Windows port under `windows/`, derived from israel2176's
Win.apk. How the ARM64 host works is in the header comments of
`windows/src/arm64/arm64_native.cpp` and `windows/src/abi_thunk_arm64.cpp`.
Read this file instead of re-mapping the repository; open `windows/README.md`
(build, tests, env vars, the list of `runtime/` changes) or `windows/PORTING.md`
(design decisions, findings; grep it, don't read it whole) only when a task
needs them. Documents must match the code: a change to a flag, a file name or a
setting is also a change to the document that names it.

## Layout

| Path | What |
|---|---|
| `windows/src/`, `windows/include/winapk/` | ELF loader, linker, ELF TLS, ABI thunks, guest JNI. `include/winapk/arch.h` (ARM64 only: it errors on any other target), `src/arm64/arm64_native.cpp` + `src/abi_thunk_arm64.cpp` (arm64 guest code run natively), `include/winapk/arm64_x18.h` (the x18 scan), `include/winapk/arm64_x18_rewrite.h` (the virtual x18 a library that writes x18 is loaded with). `src/bionic/` + `mini_bionic.cpp`: the Bionic libc shim |
| `windows/host/` | The runtime host: `runtime_main.cpp` (`winapk-runtime.exe`: args `--apk --data --jdk --lib --install-only`, `--gms <cmd>` runs `com.macapk.gms.GmsCli` with no app, `--system` the system broker, `--verify-apk <file>` an app file's package and whether Google signed it: `windows/java/com/winapk/tools/ApkCheck.java`), `win32_window.cpp` (window, input; `pointer_input.h` the input decisions that need no window), `native_*.cpp` (the Java natives, one file per class: audio on WASAPI, video and MediaCodec in `native_media_video.cpp` + `video_player.cpp` + `video_decode.cpp` and `native_media_codec.cpp` + `media_encode.cpp` + `mp4_writer.cpp`, WebView on WebView2 in `native_webview.cpp` + `webview2_host.cpp`, notifications as Windows toasts, the Google sign-in window, camera, location, Bluetooth, radios, gamepad, clipboard, picker), `binder_win.cpp` (cross-process Binder over named pipes), `gl/`, `user_folders.cpp` (shared storage junctions), `cds.cpp` + `cds_util.cpp` (class-data sharing, the framework archive `<data>\cds\`) |
| `windows/launcher/launcher_main.cpp` | `Win.apk.exe`, the library app (one file, GDI+ custom-drawn). See below. Its state machines are headers beside it with no lock and no Windows call, so `test/launcher_state_test.cpp` drives them: `app_runs.h` (an app's run), `installs.h` (an install), `install_names.h` (the library name a file installs under), `install_versions.h` (an older file than the installed app), `shortcut_rules.h` (what uninstall removes, what a start re-creates), `clear_library.h` (what Clear library says), `updates.h` (the update check), `page_sync.h` (a page built again on its own), `prompt_ids.h` (what a click or key on a dialog means), `theme_choice.h` (what the Theme setting means) |
| `windows/java/` | Windows-only Java (`com/winapk/tools/AppMeta.java` writes `icon.png` + `label.txt` at install; `java_base_patch/`: the `java.base` overlay that keeps Android paths (`/data/user/0/<pkg>`) and opens the host file they name; `winapk-framework.classlist`) |
| `windows/scripts/` | `build_java.sh`, `stage_runtime.sh` (runnable `bin/` + `lib/`), `bake_jre.sh` (the shipped `jre/`), `merge_classlists.py`, `fetch_angle.sh`, `fetch_webview2.sh`, `build_framework_res.sh` |
| `windows/test/` | Unit tests (`*_test.cpp`), guest libs (`guest/`), `compat/` (app sweep: `apps.txt`, `sweep.py`; `smoke.txt` the ten apps every PR boots) |
| `windows/package/` | Installer (`winapk.iss`, Inno Setup), `README.txt`, `RELEASE_NOTES.md` (short: the release page's text), `THIRD_PARTY_NOTICES.txt` |
| `windows/CMakeLists.txt` | All Windows targets. Pulls `runtime/src/native_methods.cpp` (GL bridges) and `native_sqlite.cpp` from upstream |
| `runtime/java/` | Upstream's Android framework in Java (never edited; `windows/runtime/` replaces or adds files) + `com/macapk/loader/` (Boot, ActivityHost, package manager...). Names say macapk: that is upstream's, keep them |
| `runtime/src/`, `runtime/translator/` | Upstream native sources; Windows uses only the two files above |
| `scripts/build_framework_res.sh` | The platform resource APK (called by `windows/scripts/`) |
| `.github/workflows/` | `win-ci.yml` (PRs and by hand: what changed, then one ARM64 build on `windows-11-arm` with ccache, feeding unit tests, runtime tests, a 10-app smoke test and packaging; the full app sweep only by hand), `win-release.yml` (by hand only: builds, smoke-tests, publishes `Win.apk-Setup-arm64.exe` and a zip as a pre-release) |

Data at run time: `%LOCALAPPDATA%\Win.apk\`: `library\` (APKs + `.label`),
`apps\<name>\`, `android\` (the Android filesystem: the app sees
`/data/user/0/<pkg>` and `/storage/emulated/0`, which the host maps here:
`windows/src/bionic/paths.cpp` for native code, `jdk.internal.util.WinApkPaths`
for java.io/java.nio), `logs\run-<name>.log` (the last run; `.1.log` the one
before; `last-run.log` the newest), `logs\install-<name>.log`,
`logs\crash-<name>-<time>.dmp`, `store\` (the Store's F-Droid catalog and icons),
`installed_apps.json`, `appops\` (signed permission grants), `gms\` (the Google
account and tokens), `cds\`, `cache\`, `run\<package>.pid` (each running app's
run lock), `updates\`. Settings are in `HKCU\Software\Win.apk` (`Theme`,
`PhoneSize`, `Landscape`, `DesktopShortcut`, `ShortcutVersion`,
`UserFolders`, `GpuPresent`, `AutoUpdate`, `LastUpdateCheck`, `GoogleServices`, `WindowWidth`,
`WindowHeight`, `WindowMaximized`); `launcher_main.cpp` has their defaults. Per
app, beside its APK: `.label`, `.package`, `.orientation` (a per-app orientation
override), `.gpu` (the app's own "GPU rendering", passed as `WINAPK_GL_PRESENT`),
`.window` (the window size once the user resizes it). Runtime env
(the full list is in `windows/README.md`): `WINAPK_READY_EVENT` (launch
handshake), `WINAPK_START_WINDOW`, `WINAPK_PHONE_SIZE=WxH` (dp),
`WINAPK_RESIZABLE=0`, `WINAPK_HEADLESS=1`, `WINAPK_CDS=0`,
`WINAPK_EMBED_PARENT=<hwnd>`.

## The launcher (`windows/launcher/launcher_main.cpp`)

Sections in order: paths, settings (registry), Windows integration (file types,
shortcuts), library, running/installing, the store (WinHTTP, F-Droid's
index-v1.json), software updates (GitHub releases; the asset is
`Win.apk-Setup-arm64.exe`, `setup_asset_rank`), theme and drawing helpers,
state, layout, painting, dialogs, actions, pages (`Row`/`Section`/`Page`:
Settings, App details, Store details, the Google services setup
`show_gms_setup`), install window, log window, launch splash, drag and drop,
`wnd_proc`, the installed-apps registry, the install helper (`install` and
`uninstall` verbs), the uninstaller's `--uninstall-cleanup` (file types, app desktop shortcuts),
permissions (`namespace appops`: the signed app-ops store
and the questions the runtime asks through; `kGroupRows` must match the
runtime's permission groups), single instance (`forward_to_running`,
`maybe_quit`), `wWinMain`.

- One process serves the library, the install window and the splashes. A second
  `Win.apk.exe` hands its files (or "show the library") to the first in a
  `WM_COPYDATA` and exits (mutex `Local\WinApk-launcher`). The hidden
  process quits when nothing is left (`maybe_quit`).
- Launching: `launch()` shows the starting window at once (`show_splash`);
  `run_thread` passes it as `WINAPK_START_WINDOW` and waits on
  `WINAPK_READY_EVENT` or the process. A run's state is `g_runs`
  (`app_runs.h`), an install's `g_installs` (`installs.h`); change them only
  through their helpers (`begin_run`, `finish_run`, `begin_install`,
  `finish_install`...).
- A file's library name comes from its file name (`install_name`), but its
  package decides what that name means: `install()` without a name reads the
  package on a worker (`winapk-runtime --package-to`), and `install_resolved`
  names it by `install_names.h`: an app of the same package is updated, any
  other file gets its package as its name. A file or holder whose package
  cannot be read is never an update. The same read gives the file's
  versionCode (64-bit) and name (`install_versions.h`): an update to a lower
  versionCode than the installed one asks first (`decide_install`, one
  question at a time, Keep installed is the default), and the install helper
  refuses it; a version unknown on either side asks nothing.
- One home per action: the tile's menu is Open, Stop, App details, Remove;
  everything else per app is on the App details page. Don't add a second way
  to an action. Report a problem is `report_problem` (a prefilled issue URL;
  nothing is uploaded).
- The library watches its apps: `kRunningTimer` (1.5 s) runs only while the library or App details is on screen
  (visible, not minimized), for an app started in a process of its own (a shortcut), which sends the library nothing.
  In the library it refreshes `g_own_running`, so the tile's badge and the "N running" count (`tile_running`,
  `running_count`) agree with the menu and open no mutex while painting; in App details `details_sync` builds the
  page again when the app starts or quits. A launcher run's events do both at once. Is-it-running is
  `app_is_running`; an app in a process of its own is not the launcher's work (`any_runs`, `has_work` ignore it).
- Menus (`PopupMenu`) and dialogs (`Prompt`: `run_dialog`, and `run_prompt` for
  an app's questions) are the launcher's own, not the system's.
- Points from screen coordinates (WM_NCHITTEST's) go through `client_point`.
- The version is `launcher.rc`'s VS_VERSION_INFO: 0.1.0 unless CMake's
  `WINAPK_VERSION` writes `winapk_version.h`. The GitHub repository is
  `WINAPK_REPO` (default `airisu-dev/WinARM.apk`).
- Settings > Google account asks `winapk-runtime --gms accounts|signin-window|signout`
  (`run_gms`); exit codes 0 done, 1 error, 2 cancelled.
- The launcher is English only. A string with values in it is
  `fmt(L"... {0} ...", {args})`. Draw text with `draw_text` and images with
  `draw_image`, never directly.
- Icons are glyphs from Segoe Fluent Icons / MDL2 Assets (`kGlyph*`).

## Build and check

- Toolchain: clang only. CI builds on `windows-11-arm` with MSYS2 CLANGARM64 and
  Temurin 21 aarch64 (other aarch64 JDKs make `jint` a `long`). The installer is
  `winapk.iss` (`Win.apk-Setup-arm64.exe`).
- Full build and tests: `windows/README.md`, "Build and run".

## Conventions

- Code goes in `windows/`. Never edit `runtime/` (upstream's). To change one of
  its Java files, copy it to the same path under `windows/runtime/`, edit the
  copy and add a line to `windows/runtime/UPSTREAM.tsv` (upstream's blob, or
  `new`). `windows/scripts/check_overlay.sh` must pass; list what the copy
  changes in `windows/README.md`, "Changes to the upstream tree".
- Match the surrounding style: C++17, 120 columns, comments say why.
- Don't add app-specific hacks; fix the framework, shim or host.
- PRs go to `main`; CI must be green. Releases only via the manual
  `win-release.yml` run.
