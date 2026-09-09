#pragma once
// ── move_compose — many systems asking one character to move ────────────────
//
// Stage 2 of the command architecture. Stage 1 recorded what the tick was told
// to do; this decides what the tick DOES with it, for the one place the engine
// genuinely had last-writer-wins.
//
// ── WHY ONLY LOCOMOTION ─────────────────────────────────────────────────────
// Checked rather than assumed, and it is narrower than it looks: impulses on
// rigid bodies ALREADY compose — Jolt sums them, so two systems pushing a crate
// both push it. The defect is confined to character locomotion, where
// JoltPlugin::charMove STORED its argument (`st.desiredHoriz = ...`) and the
// last caller in the tick silently erased every earlier one. Two systems
// steering one character — input plus knockback, AI plus a scripted nudge —
// produced whichever result plugin registration order happened to select.
//
// ── WHY THIS IS A PURE FUNCTION ─────────────────────────────────────────────
// No world, no physics, no runtime. Composition is the part with a rule worth
// arguing about, so it is the part that must be testable without standing up a
// simulation — tests/move_composition_test.cpp needs neither.
//
// ── THE RULE, STATED BEFORE ROOT MOTION EXISTS ──────────────────────────────
// That ordering is deliberate. The previous plan blocked root motion on an
// arbitration rule it had not written; a rule invented to fit its first
// consumer fits only that consumer.
//
// Per entity, over its MoveContribution commands:
//
//   1. If any contribution is Exclusive, it is the ONLY one that survives.
//      Winner: highest `source`, ties broken by lowest `seq`. Root motion is
//      Exclusive for horizontal while a root-motion clip plays.
//   2. Otherwise, the highest-priority Override (same tie-break) sets the
//      value, and Additive contributions from sources STRICTLY ABOVE it are
//      then summed on top. Additive contributions at or below it are discarded
//      — that is what "overrides" means. Knockback is Override for its
//      duration.
//   3. With no Override, every Additive contribution is summed. Input and AI
//      are Additive.
//
// ── ORDER INDEPENDENCE IS STRUCTURAL HERE, NOT MERELY TESTED ────────────────
// This reads `source` and `seq` off the commands and never depends on the order
// they appear in the input, so it does not require a sorted buffer and cannot
// be broken by an unsorted one. `seq` is consulted ONLY to break a tie between
// two contributions that are identical in mode and priority — a genuine
// conflict, which is also counted and reported rather than silently resolved.
#include <cstdint>
#include <vector>

#include "runtime/sim_command.h"

namespace simcmd {

// One entity's composed movement for one tick.
struct ResolvedMove {
    uint64_t entity   = 0;
    float    horizX   = 0.0f;
    float    horizZ   = 0.0f;
    float    vertical = 0.0f;

    // Genuine conflicts: contributions that lost to an equal-priority rival and
    // were therefore decided by submission index. Non-zero means two systems
    // are fighting over one character at the same priority, which is a content
    // bug the caller should surface — not something to resolve silently.
    uint16_t exclusiveConflicts = 0;
    uint16_t overrideConflicts  = 0;
    // Contributions whose mode this build has no rule for — a stream recorded
    // by a newer engine. Treated as Additive; see move::mode().
    uint16_t unknownModes       = 0;
};

// Folds every MoveContribution in `cmds` into one ResolvedMove per entity.
//
// `out` is REPLACED, and is ordered by ascending entity id — a canonical order
// that does not depend on the input's, so the sequence of physics calls it
// drives is the same however the commands arrived. Entities with no
// MoveContribution do not appear; commands of other kinds are ignored.
void composeMoves(const std::vector<SimCommand>& cmds,
                  std::vector<ResolvedMove>& out);

}  // namespace simcmd
