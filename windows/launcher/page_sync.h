// page_sync.h -- what a page that is built again on its own (App details, when its app starts or quits)
// keeps and decides, and the set of running apps the library's tiles show. No Windows call, so
// test/launcher_state_test.cpp drives it.
//
// A page's controls are its targets: the rows that are not plain text, in order. hot (the pointer) and
// focus (Tab) are indexes into them, so a row that appears or goes shifts every control after it;
// remap_target() finds where a control went.

#pragma once

#include <cstddef>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace winapk::launcher {

// A control, as the page's rows name it: its section, kind and title.
struct TargetKey {
    int section = 0;
    int kind = 0;
    std::wstring title;
    bool operator==(const TargetKey& o) const { return section == o.section && kind == o.kind && title == o.title; }
};

// The index in `after` of the control that was `before[index]`: the same section, kind and title (the
// n-th such one, when a title repeats). When it is gone, the index it had, held within `after`; -1 when
// `after` has no control. A negative `index` (no control, the back arrow, the scroll bar) stays.
inline int remap_target(const std::vector<TargetKey>& before, int index, const std::vector<TargetKey>& after) {
    if (index < 0) return index;
    if (index >= static_cast<int>(before.size())) return after.empty() ? -1 : static_cast<int>(after.size()) - 1;
    int nth = 0;
    for (int i = 0; i < index; ++i) nth += before[static_cast<size_t>(i)] == before[static_cast<size_t>(index)];
    for (size_t i = 0; i < after.size(); ++i) {
        if (after[i] == before[static_cast<size_t>(index)] && nth-- == 0) return static_cast<int>(i);
    }
    if (after.empty()) return -1;
    return index < static_cast<int>(after.size()) ? index : static_cast<int>(after.size()) - 1;
}

// Whether App details is built again now: what it shows no longer matches the app, and nothing modal
// is up (a menu or a dialog that has its own copy of the row it was opened for). When blocked, the next
// check finds it still differs and builds it then.
inline bool details_rebuild_due(bool shown_running, bool running, bool blocked) {
    return shown_running != running && !blocked;
}

// The library's running badge and "N running": an app is running when the library started it (`started`, known
// at once from the run events) or when it runs in a process of its own (a shortcut), which only a check of
// the app's mutex finds. That check is kept in `own`, refreshed on a timer, so painting opens no mutex.
using NameSet = std::set<std::wstring>;

// `own` becomes `now`; whether that changed it (the library repaints only then).
inline bool replace_if_changed(NameSet& own, NameSet now) {
    if (own == now) return false;
    own = std::move(now);
    return true;
}

// How many different apps run, an app that is both counted once; `skip` names the ones not to count.
template <class Skip>
size_t running_count(const std::vector<std::wstring>& started, const NameSet& own, Skip skip) {
    NameSet all(own.begin(), own.end());
    all.insert(started.begin(), started.end());
    size_t n = 0;
    for (const std::wstring& name : all) n += !skip(name);
    return n;
}

}  // namespace winapk::launcher
