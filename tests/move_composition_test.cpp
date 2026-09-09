// ── move_composition_test — many systems asking one character to move ───────
//
// Stage 2 of the command architecture. `JoltPlugin::charMove` STORED its
// argument (`st.desiredHoriz = ...`), so the last caller in a tick silently
// erased every earlier one: input plus knockback, or AI plus a scripted nudge,
// produced whichever result plugin registration order happened to select.
//
// What this pins, in order of how much depends on it:
//
//   1. THE RESULT DOES NOT DEPEND ON THE ORDER CONTRIBUTIONS ARRIVE IN. That
//      is the property last-writer-wins lacked and the reason this subsystem
//      exists. Checked by 200 randomised shuffles, not by two hand-picked
//      orders — the same instrument sim_command_test uses on the ordering rule.
//   2. The composition rule itself: Additive sums, Override discards lower
//      priority and keeps higher, Exclusive discards everything.
//   3. Ties between EQUAL-priority contributions are resolved the same way
//      every time AND counted, so a genuine content conflict is reportable
//      rather than silently decided.
//
// composeMoves is a pure function — no world, no physics, no runtime — which is
// why the rule can be tested at all without standing up a simulation.
#include <cstdio>
#include <algorithm>
#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "runtime/move_compose.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

using simcmd::Cmd;
using simcmd::Source;
using simcmd::SimCommand;
using simcmd::ResolvedMove;
using simcmd::composeMoves;
using Mode = simcmd::move::Mode;

// Contributions carry their own seq here rather than going through
// Buffer::submit, because these tests are about the FOLD, and half of them need
// to control the tie-break input directly.
static SimCommand ct(uint64_t e, Source s, float hx, float hz, float vert,
                     Mode m, uint32_t seq = 0) {
    SimCommand c = simcmd::move::contribution(e, s, hx, hz, vert, m);
    c.seq = seq;
    return c;
}

static bool near(float a, float b) { return std::fabs(a - b) < 1e-5f; }

int main() {
    std::printf("move_composition_test — composing one tick's movement\n");

    // ── 1. Additive sums ───────────────────────────────────────────────────
    {
        std::printf("\n-- 1. additive --\n");
        std::vector<SimCommand> cs = {
            ct(10, Source::Gameplay, 1.0f, 0.0f, 0.0f, Mode::Additive, 0),
            ct(10, Source::AI,       0.0f, 2.0f, 0.5f, Mode::Additive, 1),
        };
        std::vector<ResolvedMove> out;
        composeMoves(cs, out);
        CHECK(out.size() == 1, "one entity contributed to, one resolved move");
        CHECK(near(out[0].horizX, 1.0f) && near(out[0].horizZ, 2.0f)
              && near(out[0].vertical, 0.5f),
              "contributions from two sources SUM — neither erases the other, "
              "which is what charMove's assignment did");
        CHECK(out[0].exclusiveConflicts == 0 && out[0].overrideConflicts == 0,
              "and no conflict is reported: different modes are not a conflict");
    }

    // ── 2. Override discards LOWER priority and keeps HIGHER ───────────────
    // The asymmetry is the rule. An Override is not "the answer"; it is a floor
    // that everything beneath it stops mattering — knockback overrides player
    // input, but a cutscene still gets to steer through a knockback.
    {
        std::printf("\n-- 2. override --\n");
        std::vector<SimCommand> cs = {
            ct(10, Source::Gameplay, 5.0f, 0.0f, 0.0f, Mode::Additive, 0),
            ct(10, Source::Ability,  1.0f, 0.0f, 0.0f, Mode::Override, 1),
            ct(10, Source::Cutscene, 0.0f, 3.0f, 0.0f, Mode::Additive, 2),
        };
        std::vector<ResolvedMove> out;
        composeMoves(cs, out);
        CHECK(near(out[0].horizX, 1.0f),
              "the Gameplay additive BELOW the Ability override is discarded");
        CHECK(near(out[0].horizZ, 3.0f),
              "the Cutscene additive ABOVE it still applies");
    }

    // ── 3. Exclusive discards EVERYTHING, including higher priority ────────
    // Root motion is the consumer: a clip authoring the character's movement
    // must not have a stray input contribution added on top, or the animation
    // desyncs from the movement it is driving. So Exclusive is stronger than
    // Override — it is not a higher floor, it is the absence of a sum.
    {
        std::printf("\n-- 3. exclusive --\n");
        std::vector<SimCommand> cs = {
            ct(10, Source::Animation, 2.0f, 0.0f, 0.0f, Mode::Exclusive, 0),
            ct(10, Source::Cutscene,  9.0f, 9.0f, 9.0f, Mode::Additive,  1),
            ct(10, Source::Cutscene,  7.0f, 0.0f, 0.0f, Mode::Override,  2),
        };
        std::vector<ResolvedMove> out;
        composeMoves(cs, out);
        CHECK(near(out[0].horizX, 2.0f) && near(out[0].horizZ, 0.0f)
              && near(out[0].vertical, 0.0f),
              "the Exclusive contribution is the ONLY survivor, even against a "
              "HIGHER-priority additive and override");
    }

    // ── 4. Equal-priority ties: same answer every time, and COUNTED ────────
    // Two systems at one priority fighting over one character is a content bug.
    // It has to resolve deterministically — otherwise the ordering dependence
    // is back — but resolving it silently would hide the bug, so it is both.
    {
        std::printf("\n-- 4. ties --\n");
        std::vector<SimCommand> cs = {
            ct(10, Source::Animation, 1.0f, 0.0f, 0.0f, Mode::Exclusive, 7),
            ct(10, Source::Animation, 4.0f, 0.0f, 0.0f, Mode::Exclusive, 3),
        };
        std::vector<ResolvedMove> out;
        composeMoves(cs, out);
        CHECK(near(out[0].horizX, 4.0f),
              "equal source: the LOWER seq wins — first claim, not last write");
        CHECK(out[0].exclusiveConflicts == 1,
              "and the loser is COUNTED, so the content bug is reportable");

        std::vector<SimCommand> ov = {
            ct(11, Source::Ability, 1.0f, 0.0f, 0.0f, Mode::Override, 7),
            ct(11, Source::Ability, 4.0f, 0.0f, 0.0f, Mode::Override, 3),
        };
        composeMoves(ov, out);
        CHECK(near(out[0].horizX, 4.0f) && out[0].overrideConflicts == 1,
              "the same rule and the same counting for Override");

        // Higher priority is NOT a conflict — it is the rule working.
        std::vector<SimCommand> ok = {
            ct(12, Source::Gameplay, 1.0f, 0.0f, 0.0f, Mode::Override, 0),
            ct(12, Source::Cutscene, 4.0f, 0.0f, 0.0f, Mode::Override, 1),
        };
        composeMoves(ok, out);
        CHECK(near(out[0].horizX, 4.0f),
              "unequal source: priority decides, seq is never consulted");
    }

    // ── 5. THE PROPERTY: the result is independent of arrival order ────────
    // The one that matters. Every rule above is order-INDEPENDENT by
    // construction — the fold reads `source` and `seq` off the commands and
    // never off their position — and this is what proves the construction held.
    {
        std::printf("\n-- 5. randomised arrival orders --\n");
        // Every entity that survives to a sum carries at least TWO surviving
        // additives, deliberately. An earlier version of this fixture gave each
        // entity at most one, which made the whole section blind: summing and
        // overwriting agree on a single value, so mutating the fold back to
        // last-writer-wins left all of section 5 green. A shuffle test over
        // contributions that cannot disagree proves nothing.
        std::vector<SimCommand> base = {
            ct(10, Source::Gameplay,  1.5f, 0.0f, 0.0f, Mode::Additive,  0),
            ct(10, Source::AI,        0.0f, 0.5f, 0.0f, Mode::Additive,  1),
            ct(10, Source::Ability,   2.0f, 0.0f, 1.0f, Mode::Override,  2),
            ct(10, Source::Cutscene,  0.0f, 0.25f,0.0f, Mode::Additive,  3),
            ct(10, Source::Cutscene,  0.75f,0.0f, 0.5f, Mode::Additive,  7),
            ct(11, Source::Animation, 3.0f, 0.0f, 0.0f, Mode::Exclusive, 4),
            ct(11, Source::Gameplay,  9.0f, 9.0f, 9.0f, Mode::Additive,  5),
            ct(12, Source::AI,        0.5f, 0.5f, 0.0f, Mode::Additive,  6),
            ct(12, Source::Gameplay, -0.25f,1.0f, 0.0f, Mode::Additive,  8),
        };

        std::vector<ResolvedMove> expected;
        composeMoves(base, expected);
        CHECK(expected.size() == 3, "three entities resolved");
        CHECK(expected[0].entity == 10 && expected[1].entity == 11
              && expected[2].entity == 12,
              "output is ordered by entity — a canonical sequence of physics "
              "calls, whatever order the commands arrived in");

        auto sameAs = [&](const std::vector<ResolvedMove>& got) {
            if (got.size() != expected.size()) return false;
            for (size_t i = 0; i < got.size(); ++i) {
                if (got[i].entity != expected[i].entity) return false;
                if (!near(got[i].horizX,   expected[i].horizX))   return false;
                if (!near(got[i].horizZ,   expected[i].horizZ))   return false;
                if (!near(got[i].vertical, expected[i].vertical)) return false;
            }
            return true;
        };

        std::mt19937 rng(12345);   // fixed seed: a failure is reproducible
        int mismatches = 0;
        std::vector<SimCommand> shuffled = base;
        std::vector<ResolvedMove> out;
        for (int t = 0; t < 200; ++t) {
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            composeMoves(shuffled, out);
            if (!sameAs(out)) ++mismatches;
        }
        CHECK(mismatches == 0,
              "200 shuffled arrival orders of 9 contributions all compose to "
              "ONE result (%d mismatches)", mismatches);
    }

    // ── 6. A mode this build has no rule for ───────────────────────────────
    // A stream recorded by a newer engine. Treated as Additive — the one mode
    // that cannot silently erase another contribution — and counted, so it is
    // visible rather than guessed at.
    {
        std::printf("\n-- 6. unknown modes --\n");
        SimCommand c = simcmd::move::contribution(10, Source::Gameplay,
                                                  1.0f, 0.0f, 0.0f);
        c.u[simcmd::move::kSlotMode] = 999;
        CHECK(!simcmd::move::modeKnown(c), "the mode is recognised as unknown");
        std::vector<SimCommand> cs = {
            c, ct(10, Source::AI, 2.0f, 0.0f, 0.0f, Mode::Additive, 1) };
        std::vector<ResolvedMove> out;
        composeMoves(cs, out);
        CHECK(near(out[0].horizX, 3.0f),
              "it composes as Additive rather than being dropped or obeyed");
        CHECK(out[0].unknownModes == 1, "and is counted");
    }

    // ── 7. Nothing to compose ──────────────────────────────────────────────
    {
        std::printf("\n-- 7. the empty and irrelevant cases --\n");
        std::vector<ResolvedMove> out;
        composeMoves({}, out);
        CHECK(out.empty(), "an empty tick resolves to no moves");

        SimCommand jump{};
        jump.entity = 10; jump.kind = Cmd::Jump;
        composeMoves({ jump }, out);
        CHECK(out.empty(),
              "a tick of non-movement commands resolves to no moves — an "
              "entity that was never asked to move must not be driven to zero");
    }

    if (g_failures) {
        std::printf("\nmove_composition_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nmove_composition_test: ALL PASS\n");
    return 0;
}
