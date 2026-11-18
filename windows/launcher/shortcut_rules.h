// shortcut_rules.h -- the decisions behind what the uninstaller removes and what a start re-creates.
//
// No Windows call here, so that test/launcher_state_test.cpp can drive them; launcher_main.cpp reads the
// registry and the shortcut files and passes in what it found.

#pragma once

#include <cwctype>
#include <string>

namespace winapk::launcher {

constexpr wchar_t kProgId[] = L"WinApk.Package";

// A file type is ours to release only while it still names our ProgID: another program that took the type
// since owns it, and removing its value would unassociate that program's files.
inline bool owns_association(const std::wstring& current_progid) { return current_progid == kProgId; }

inline std::wstring fold_path(std::wstring p) {
    for (wchar_t& c : p) c = c == L'/' ? L'\\' : static_cast<wchar_t>(towlower(c));
    return p;
}

// A shortcut the library made for an app: it starts this launcher with --run. The launcher's own shortcut
// (no arguments) is the installer's to remove, and any other program's shortcut is never ours.
inline bool is_own_app_shortcut(const std::wstring& target, const std::wstring& args,
                                const std::wstring& launcher_exe) {
    return !target.empty() && fold_path(target) == fold_path(launcher_exe) && args.compare(0, 5, L"--run") == 0 &&
           (args.size() == 5 || args[5] == L' ');
}

// Whether a start must write the apps' shortcuts again: the stored version is behind, or the Start menu folder
// is gone (an uninstall removed it and kept the library; the version setting survives that).
inline bool shortcuts_need_refresh(unsigned stored_version, unsigned current_version, bool menu_folder_exists) {
    return stored_version < current_version || !menu_folder_exists;
}

// Whether an app gets a desktop shortcut in that refresh. With the folder present the user's own choice
// (the shortcut exists or was deleted) stands; with it gone every shortcut was removed together, so the
// Desktop shortcut setting decides.
inline bool refresh_wants_desktop(bool menu_folder_exists, bool desktop_shortcut_exists, bool setting_on) {
    return menu_folder_exists ? desktop_shortcut_exists : setting_on;
}

}  // namespace winapk::launcher
