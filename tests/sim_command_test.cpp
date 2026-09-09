// ── sim_command_test — the tick command record, and what it is for ──────────
//
// Stage 1 of the command architecture. The engine's control model was "N
// systems mutate world state and the last one wins"; a headless, replayable,
// network-capable simulation needs
//
//     per-tick commands -> one simulation -> authoritative state -> presentation
//
// and this pins the first arrow. What it must guarantee, in order of how much
// the rest depends on it:
//
//   1. The execution order is a function of the COMMANDS, not of the order they
//      happened to be submitted in. That is the property last-writer-wins
//      lacked, and it is why two systems acting on one entity no longer depend
//      on plugin registration order for their outcome.
//   2. The record is a padding-free POD, so it can be hashed, memcmp'd and
//      eventually written to a replay file or a wire without an indeterminate
//      byte producing a false divergence.
//   3. A command that cannot name its target is refused rather than recorded.
//
// The replay-equivalence test (record -> restore -> replay -> identical per-tick
// world hashes) lives in tests/sim_replay_test.cpp and needs a world snapshot;
// this file covers the record itself.
#include <cstdio>
#include <algorithm>
#include <cstring>
#include <string>
#include <random>
#include <vector>

#include "runtime/sim_command.h"

static int g_failures = 0;
#define CHECK(cond, ...) do {                                          \
    if (!(cond)) { std::printf("  FAIL  " __VA_ARGS__);                \
                   std::printf("\n"); ++g_failures; }                  \
    else { std::printf("  ok    " __VA_ARGS__); std::printf("\n"); }   \
} while (0)

using simcmd::Cmd;
using simcmd::Source;
using simcmd::SimCommand;

static SimCommand mk(uint64_t entity, Cmd k, Source s, float v = 0.0f) {
    SimCommand c;
    c.entity = entity; c.kind = k; c.source = s; c.a[0] = v;
    return c;
}

// The execution order, as a comparable string — entity/source/kind, with seq
// deliberately excluded so that two runs that submitted in different orders
// produce the same string when the ORDERING RULE is doing its job.
static std::string orderOf(const simcmd::Buffer& b) {
    std::string out;
    for (const SimCommand& c : b.commands())
        out += std::to_string(c.entity) + ":" + std::to_string((int)c.source)
             + ":" + std::to_string((int)c.kind) + " ";
    return out;
}

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    std::printf("sim_command_test: the tick command record\n");

    // ── 1. The wire/replay POD contract ─────────────────────────────────────
    // Same discipline as InputSnapshot, and for the same reason: these bytes
    // get hashed and will get written to a file and a socket.
    {
        std::printf("\n-- 1. the POD contract --\n");
        CHECK(sizeof(SimCommand) == 48, "SimCommand is 48 bytes (%zu)",
              sizeof(SimCommand));
        CHECK(std::is_trivially_copyable_v<SimCommand>,
              "trivially copyable — memcpy into a replay buffer is legal");

        // NO INDETERMINATE BYTES. Two default-constructed commands must be
        // byte-identical, or a digest over them is not reproducible. Built in
        // deliberately-dirty memory so a missing initialiser shows up.
        alignas(SimCommand) unsigned char bufA[sizeof(SimCommand)];
        alignas(SimCommand) unsigned char bufB[sizeof(SimCommand)];
        std::memset(bufA, 0xAA, sizeof bufA);
        std::memset(bufB, 0x55, sizeof bufB);
        auto* x = new (bufA) SimCommand{};
        auto* y = new (bufB) SimCommand{};
        CHECK(std::memcmp(x, y, sizeof(SimCommand)) == 0,
              "two default commands are byte-identical even from dirty memory "
              "— no padding leaks into a hash");
    }

    // ── 2. THE ORDERING PROPERTY, which is the point of stage 1 ─────────────
    // Submit the same four commands in two different orders. After
    // sortForExecution the execution sequence must be identical — otherwise
    // "who wins" is still decided by who ran first, which is the defect this
    // architecture replaces.
    {
        std::printf("\n-- 2. execution order is independent of submission order --\n");
        const uint64_t eA = 0x1111, eB = 0x2222;

        simcmd::Buffer one;
        one.submit(mk(eB, Cmd::Jump,             Source::Gameplay));
        one.submit(mk(eA, Cmd::MoveContribution, Source::Animation, 1.0f));
        one.submit(mk(eA, Cmd::MoveContribution, Source::Gameplay,  2.0f));
        one.submit(mk(eB, Cmd::Teleport,         Source::Correction));
        one.sortForExecution();

        simcmd::Buffer two;                       // reversed submission
        two.submit(mk(eB, Cmd::Teleport,         Source::Correction));
        two.submit(mk(eA, Cmd::MoveContribution, Source::Gameplay,  2.0f));
        two.submit(mk(eA, Cmd::MoveContribution, Source::Animation, 1.0f));
        two.submit(mk(eB, Cmd::Jump,             Source::Gameplay));
        two.sortForExecution();

        CHECK(orderOf(one) == orderOf(two),
              "two submission orders execute identically\n         [%s]",
              orderOf(one).c_str());

        // ...and the order is the DECLARED one: entity, then source priority.
        const auto& c = one.commands();
        CHECK(c.size() == 4 && c[0].entity == eA && c[1].entity == eA
              && c[2].entity == eB && c[3].entity == eB,
              "grouped by entity");
        CHECK(c[0].source == Source::Gameplay && c[1].source == Source::Animation,
              "and within an entity, by source priority (Gameplay < Animation)");
    }

    // ── 3. A stronger version of the same property, randomised ─────────────
    // Fixed seed: this is a determinism test, so it must not itself be
    // nondeterministic. 200 shuffles of one command set, all producing one order.
    {
        std::printf("\n-- 3. randomised submission orders --\n");
        std::vector<SimCommand> base;
        for (uint64_t e = 1; e <= 6; ++e)
            for (int s = 0; s < 4; ++s)
                base.push_back(mk(e * 0x100, (Cmd)(s % 3), (Source)s, (float)s));

        std::mt19937 rng(12345);
        std::string want;
        bool allSame = true;
        for (int iter = 0; iter < 200; ++iter) {
            std::vector<SimCommand> shuffled = base;
            std::shuffle(shuffled.begin(), shuffled.end(), rng);
            simcmd::Buffer b;
            for (const auto& c : shuffled) b.submit(c);
            b.sortForExecution();
            if (iter == 0) want = orderOf(b);
            else if (orderOf(b) != want) { allSame = false; break; }
        }
        CHECK(allSame, "200 shuffled submissions of 24 commands all execute in "
                       "one order");
    }

    // ── 4. An unnameable target is refused ─────────────────────────────────
    // EntityId 0 means "unassigned". Recording one would replay against a
    // different entity, or none — so it is refused at the door rather than
    // discovered at replay time.
    {
        std::printf("\n-- 4. entity 0 is refused --\n");
        simcmd::Buffer b;
        CHECK(!b.submit(mk(0, Cmd::Jump, Source::Gameplay)),
              "submit() returns false for an unassigned EntityId");
        CHECK(b.size() == 0, "and nothing is recorded (%zu)", b.size());
        CHECK(b.submit(mk(7, Cmd::Jump, Source::Gameplay)),
              "a real id is accepted");
    }

    // ── 5. The digest sees what a divergence would ─────────────────────────
    {
        std::printf("\n-- 5. the digest --\n");
        simcmd::Buffer a, b;
        a.submit(mk(5, Cmd::MoveContribution, Source::Gameplay, 1.0f));
        b.submit(mk(5, Cmd::MoveContribution, Source::Gameplay, 1.0f));
        CHECK(a.digest() == b.digest(), "identical buffers digest equally");

        simcmd::Buffer c;
        c.submit(mk(5, Cmd::MoveContribution, Source::Gameplay,
                    std::nextafterf(1.0f, 2.0f)));
        CHECK(a.digest() != c.digest(),
              "ONE ULP in a payload changes the digest — the class of "
              "divergence a text format would hide");

        simcmd::Buffer d;
        d.submit(mk(5, Cmd::MoveContribution, Source::AI, 1.0f));
        CHECK(a.digest() != d.digest(), "a different SOURCE changes it too");

        simcmd::Buffer e;
        CHECK(a.digest() != e.digest(), "and an empty buffer is distinct");
    }

    if (g_failures) {
        std::printf("\nsim_command_test: %d FAILURE(S)\n", g_failures);
        return 1;
    }
    std::printf("\nsim_command_test: ALL PASS\n");
    return 0;
}
