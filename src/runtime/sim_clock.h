#pragma once
// ── sim_clock — where each fixed step's input window ends (WO-043) ────────────
//
// A fixed step folds the input events staged up to its boundary into its
// InputSnapshot (InputManager::beginTick). That boundary used to be
// hid::nowNs(), read INSIDE the step: which events a tick saw depended on
// when the CPU happened to run it (audit DET-01). And in a catch-up frame,
// where the accumulator runs several steps back to back, the first step's
// "now" swallowed every event staged that frame and the rest saw none.
//
// Now the frame reads the clock once, as the time input was pumped, and each
// step's boundary is derived from it and the accumulator: after a step's
// decrement, what remains in the accumulator is exactly the real time not yet
// simulated, so the step ends `leftover` before the pump. Successive steps end
// kSimDt apart, the last one `leftover` short of the pump, and the step itself
// reads no clock: it is a function of what the frame hands it.
#include <algorithm>
#include <cstdint>

namespace simclock {

// The end of a step's input window, in the input clock's nanoseconds.
//   pumpNs      when this frame's input was collected (InputManager::lastPumpNs)
//   leftoverSec the accumulator AFTER this step's decrement: time not yet simulated
//   prevEndNs   the previous step's boundary; windows never run backwards (a
//               frame dt measured on a different clock than the input's can
//               otherwise nudge a boundary before its predecessor)
inline uint64_t inputTickEndNs(uint64_t pumpNs, double leftoverSec, uint64_t prevEndNs) {
    const double   leftNs = std::max(0.0, leftoverSec) * 1e9;
    const uint64_t left   = leftNs >= (double)pumpNs ? pumpNs : (uint64_t)leftNs;
    return std::max(pumpNs - left, prevEndNs);
}

}  // namespace simclock
