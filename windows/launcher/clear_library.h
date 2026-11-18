// clear_library.h -- what Settings > Clear library tells the user once it has tried every app.
//
// No Windows call here, so that test/launcher_state_test.cpp can drive it; launcher_main.cpp passes in the
// labels of the apps that could not be removed.

#pragma once

#include <string>
#include <vector>

namespace winapk::launcher {

struct ClearOutcome {
    std::wstring text;
    bool ok;  // every app was removed
};

// Success only when nothing was left behind; otherwise the leftovers are named (one) or counted (several), and
// stay in the library for another try. Short: the InfoBar cuts what does not fit on one line.
inline ClearOutcome clear_outcome(const std::vector<std::wstring>& left) {
    if (left.empty()) return {L"The library was cleared.", true};
    if (left.size() == 1) return {left[0] + L" could not be removed. Close it and try again.", false};
    return {std::to_wstring(left.size()) + L" apps could not be removed. Close them and try again.", false};
}

}  // namespace winapk::launcher
