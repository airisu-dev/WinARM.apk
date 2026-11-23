// vsync_grid.h -- the fixed time grid the display vsync ticks on, with no Windows call so that
// test/window_test.cpp checks it.

#pragma once

#include <cstdint>

namespace winapk::host {

// The tick that answers a request, given the previous tick `prev` (a grid time), the time `now`, and the period.
// Ticks stay on prev + k * period, so they never drift; one that is already late is not fired in a burst: the
// next slot at or after `now` is taken instead.
inline int64_t next_grid_tick(int64_t prev, int64_t now, int64_t period) {
    const int64_t t = prev + period;
    if (t >= now) return t;
    return t + ((now - t + period - 1) / period) * period;
}

}  // namespace winapk::host
