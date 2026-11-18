// install_versions.h -- the version of an app file or of an installed app, and whether installing the file
// would be a downgrade.
//
// Both come as the lines of a package record (the runtime's --package-to output, library\<name>.package):
// package, versionCode, versionName. The text is read from an APK or from a file on disk, so it is parsed
// strictly: a value that is not exactly what the writer makes is "not known", and an app whose version is
// not known on either side is never asked about (the question needs both). No lock and no Windows call, so
// that test/launcher_state_test.cpp can drive it.

#pragma once

#include <string>
#include <vector>

namespace winapk::launcher {

struct AppVersion {
    bool known = false;
    long long code = 0;  // the 64-bit versionCode: versionCodeMajor << 32 | versionCode
    std::string name;    // versionName, for the question; may be empty
};

// A decimal number of 1 to 19 digits that fits a long long; no sign, space or other character. With
// `recorded`, a leading '-' is read as the 32-bit versionCode of a record from before the library kept the
// 64-bit one (Manifest's int, which prints a code of 2^31 or more as negative), which Android orders as
// unsigned.
inline bool parse_version_code(const std::string& text, long long* out, bool recorded = false) {
    size_t i = 0;
    const bool negative = recorded && !text.empty() && text[0] == '-';
    if (negative) i = 1;
    if (i >= text.size() || text.size() - i > 19) return false;
    unsigned long long v = 0;
    for (; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') return false;
        const unsigned digit = static_cast<unsigned>(text[i] - '0');
        if (v > (0x7fffffffffffffffull - digit) / 10) return false;
        v = v * 10 + digit;
    }
    if (negative) {
        if (v == 0 || v > 0x80000000ull) return false;
        v = 0x100000000ull - v;
    }
    *out = static_cast<long long>(v);
    return true;
}

// A version name as one short line: control characters dropped, at most 64 bytes without cutting a UTF-8
// sequence, no space at either end.
inline std::string clean_version_name(const std::string& text) {
    std::string s;
    for (const char c : text) {
        if (static_cast<unsigned char>(c) >= 0x20 && c != 0x7f) s += c;
    }
    if (s.size() > 64) {
        size_t cut = 64;
        while (cut > 0 && (static_cast<unsigned char>(s[cut]) & 0xC0) == 0x80) --cut;
        s.resize(cut);
    }
    const size_t first = s.find_first_not_of(' ');
    if (first == std::string::npos) return {};
    return s.substr(first, s.find_last_not_of(' ') - first + 1);
}

// The version a package record's lines say: known only when the second line is a version code.
// `recorded`: the lines are library\<name>.package's, which an older install wrote with a 32-bit code.
inline AppVersion version_from_lines(const std::vector<std::string>& lines, bool recorded) {
    AppVersion v;
    if (lines.size() < 2 || !parse_version_code(lines[1], &v.code, recorded)) return v;
    v.known = true;
    if (lines.size() > 2) v.name = clean_version_name(lines[2]);
    return v;
}

// Installing `file` over `installed` goes back a version: both are known and the file's is lower. The
// same version, a newer one, and an unknown one on either side are not.
inline bool is_downgrade(const AppVersion& file, const AppVersion& installed) {
    return file.known && installed.known && file.code < installed.code;
}

// "1.2.3 (45)", or "45" when the app has no version name.
inline std::string describe_version(const AppVersion& v) {
    if (!v.known) return v.name;
    const std::string code = std::to_string(v.code);
    return v.name.empty() ? code : v.name + " (" + code + ")";
}

}  // namespace winapk::launcher
