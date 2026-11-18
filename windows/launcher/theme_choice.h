// theme_choice.h -- which theme the stored "Theme" setting means.
//
// The setting is a DWORD in the registry that a person can edit by hand: 0 follows Windows, 1 is light,
// 2 is dark. Anything else is not a theme, so it reads as the default (0) rather than as a neighbouring
// value; drawing and the Settings label both go through here so they cannot disagree. Reading never
// writes the value back. No Windows call, so that test/launcher_state_test.cpp can drive it.

#pragma once

#include <cstdint>

namespace winapk::launcher {

enum class ThemeChoice : int { kFollowWindows = 0, kLight = 1, kDark = 2 };

inline ThemeChoice theme_choice(std::uint32_t stored) {
    return stored == 1 ? ThemeChoice::kLight : stored == 2 ? ThemeChoice::kDark : ThemeChoice::kFollowWindows;
}

}  // namespace winapk::launcher
