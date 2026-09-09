// ── move_compose — implementation ───────────────────────────────────────────
#include "runtime/move_compose.h"

#include <algorithm>
#include <cstddef>

namespace simcmd {
namespace {

// Higher source wins; equal sources fall to the lower submission index.
//
// FIRST CLAIM WINS, deliberately, rather than "last write wins" — the rule this
// whole subsystem exists to remove. It also behaves better under change: adding
// a second system at an existing priority cannot silently take over from the
// one that was already there, it collides with it and is counted.
bool beats(const SimCommand& c, const SimCommand& best) {
    if (c.source != best.source) return c.source > best.source;
    return c.seq < best.seq;
}

void addTo(ResolvedMove& r, const SimCommand& c) {
    r.horizX   += move::horizX(c);
    r.horizZ   += move::horizZ(c);
    r.vertical += move::vertical(c);
}

void setFrom(ResolvedMove& r, const SimCommand& c) {
    r.horizX   = move::horizX(c);
    r.horizZ   = move::horizZ(c);
    r.vertical = move::vertical(c);
}

}  // namespace

void composeMoves(const std::vector<SimCommand>& cmds,
                  std::vector<ResolvedMove>& out) {
    out.clear();

    // Indices rather than copies: a SimCommand is 48 bytes and this runs every
    // tick. Gathering first also means the two passes below walk only the
    // MoveContributions, not the whole tick's buffer, three times over.
    std::vector<size_t> idx;
    idx.reserve(cmds.size());
    for (size_t i = 0; i < cmds.size(); ++i)
        if (cmds[i].kind == Cmd::MoveContribution) idx.push_back(i);
    if (idx.empty()) return;

    // Group by entity. Sorted by entity ALONE — within an entity the fold reads
    // `source` and `seq` off the commands, so their relative order here does
    // not reach the result. std::stable_sort so a debugger sees runs in
    // submission order, which is what a divergence report wants to print.
    std::stable_sort(idx.begin(), idx.end(),
        [&](size_t x, size_t y) { return cmds[x].entity < cmds[y].entity; });

    size_t i = 0;
    while (i < idx.size()) {
        const uint64_t entity = cmds[idx[i]].entity;
        size_t j = i;
        while (j < idx.size() && cmds[idx[j]].entity == entity) ++j;
        // [i, j) is this entity's run.

        ResolvedMove r;
        r.entity = entity;

        // Pass 1 — find the winning Exclusive and the winning Override, and
        // count how many lost a tie they should not have been in.
        const SimCommand* excl = nullptr;
        const SimCommand* ovr  = nullptr;
        for (size_t k = i; k < j; ++k) {
            const SimCommand& c = cmds[idx[k]];
            if (!move::modeKnown(c)) ++r.unknownModes;
            switch (move::mode(c)) {
                case move::Mode::Exclusive:
                    if (excl) ++r.exclusiveConflicts;
                    if (!excl || beats(c, *excl)) excl = &c;
                    break;
                case move::Mode::Override:
                    if (ovr) ++r.overrideConflicts;
                    if (!ovr || beats(c, *ovr)) ovr = &c;
                    break;
                default: break;
            }
        }

        if (excl) {
            // Rule 1. Nothing else survives — not even a higher-priority
            // Additive, which is the whole point: a root-motion clip drives the
            // character exactly, and a stray input contribution added on top
            // would desync the animation from the movement it is authoring.
            setFrom(r, *excl);
        } else {
            // Rules 2 and 3. The Override sets the value; Additives strictly
            // above it are then summed on top, and those at or below it are
            // discarded — with no Override, `floor` admits everything.
            if (ovr) setFrom(r, *ovr);
            for (size_t k = i; k < j; ++k) {
                const SimCommand& c = cmds[idx[k]];
                if (move::mode(c) != move::Mode::Additive) continue;
                if (ovr && !(c.source > ovr->source)) continue;
                addTo(r, c);
            }
        }

        out.push_back(r);
        i = j;
    }
}

}  // namespace simcmd
