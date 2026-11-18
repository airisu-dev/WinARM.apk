// install_names.h -- which library name a file installs under.
//
// The library keys an app by a name (library\<name>.apk, apps\<name>): install_name() derives it from
// the file's name, and two unrelated files can derive the same one ("app-release.apk" from two
// GitHub projects). The package inside the file decides what the name means: the app that holds it
// is updated only by a file of the same package; any other file gets a name of its own, so no
// install replaces a different app. No lock and no Windows call, so that test/launcher_state_test.cpp
// can drive it; launcher_main.cpp supplies who holds a name.

#pragma once

#include <string>

namespace winapk::launcher {

// Who has a library name: an app in the library or an install queued from here.
struct NameHolder {
    bool taken = false;
    std::string package;  // its package; empty when not known
};

struct NameChoice {
    std::wstring name;  // empty: no free name was found
    bool update = false;  // the name is held by an app of the file's package
};

// `wanted` is the name derived from the file; `package` the file's own, empty when it could not be
// read; `holder(name)` says who has a name. A name that is free, or held by the file's package, is
// taken. A held name whose package differs or is unknown is never reused: the file goes under its
// package as a name ("com.example.app"), or when that is held too, the next one of "<name> (2)",
// "<name> (3)"... An unreadable file has no package to name it by, so it is numbered from `wanted`.
template <class Holder>
NameChoice choose_install_name(const std::wstring& wanted, const std::string& file_package, Holder&& holder) {
    constexpr int kTries = 100;
    // The package names a folder, so only Android's own shape is trusted (AppInstall.PACKAGE_NAME): anything
    // else ("..", a trailing dot, a path character) is treated as unreadable.
    auto letter = [](char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); };
    bool valid = !file_package.empty() && letter(file_package.front()) && file_package.back() != '.' &&
                 file_package.find("..") == std::string::npos;
    for (const char c : file_package) valid = valid && (letter(c) || (c >= '0' && c <= '9') || c == '.' || c == '_');
    const std::string package = valid ? file_package : std::string();
    std::wstring base = wanted;
    if (valid) base.assign(package.begin(), package.end());
    const NameHolder first = holder(wanted);
    if (!first.taken) return {wanted, false};
    if (!package.empty() && first.package == package) return {wanted, true};
    // An unreadable file never matches a holder, so it starts at "(2)" beside `wanted`.
    for (int n = package.empty() ? 2 : 1; n <= kTries; ++n) {
        const std::wstring name = n == 1 ? base : base + L" (" + std::to_wstring(n) + L")";
        const NameHolder h = holder(name);
        if (!h.taken) return {name, false};
        if (!package.empty() && h.package == package) return {name, true};
    }
    return {};
}

}  // namespace winapk::launcher
