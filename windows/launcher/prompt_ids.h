// prompt_ids.h -- what a click or a key on the launcher's dialog (Prompt) means.
//
// A dialog's targets are its buttons (the caller's ids: IDOK, IDCANCEL, an app question's 100...), its
// switches and check boxes, and an optional link. A switch must never share an id with a button (a switch
// at 1 would be IDOK: a click on Remove would toggle the box), so switches have an id range of their own.
// No Windows call, so that test/launcher_state_test.cpp can drive it.

#pragma once

#include <cstddef>

namespace winapk::launcher {

constexpr int kPromptSwitchBase = 0x6000;  // switch i is kPromptSwitchBase + i; every button id is below it
constexpr int kPromptLinkId = 0x7000;      // past any switch's

inline int prompt_switch_id(std::size_t index) { return kPromptSwitchBase + static_cast<int>(index); }

enum class PromptAct { kNone, kToggle, kLink, kPress };

struct PromptStep {
    PromptAct act = PromptAct::kNone;
    std::size_t index = 0;  // kToggle: which switch
    int id = 0;             // kPress: which button
};

// What activating target `id` does in a dialog with `switches` switches.
inline PromptStep prompt_step(int id, std::size_t switches) {
    if (id <= 0) return {};
    if (id == kPromptLinkId) return {PromptAct::kLink, 0, id};
    if (id >= kPromptSwitchBase) {
        const std::size_t i = static_cast<std::size_t>(id - kPromptSwitchBase);
        return i < switches ? PromptStep{PromptAct::kToggle, i, id} : PromptStep{};
    }
    return {PromptAct::kPress, 0, id};
}

// The target Enter activates: the focused button or link, else the default button. Enter on a focused
// check box or switch takes the default button, as in a Windows dialog.
inline int prompt_enter_target(int focus, int default_id) {
    if (focus <= 0) return default_id;
    const bool check_box = focus >= kPromptSwitchBase && focus != kPromptLinkId;
    return check_box ? default_id : focus;
}

}  // namespace winapk::launcher
