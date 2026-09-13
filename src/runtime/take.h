#pragma once
// ── take — a recorded performance, replayable to the tick ────────────────────
//
// Replay stage R1. A take is everything needed to run a simulation again and
// get the same world, tick for tick:
//
//   start          the scene snapshot the session began from — the exact string
//                  Snapshot Play already loads into a fresh world. Replay loads
//                  the SAME string, so the two runs start from byte-identical
//                  input, and even an EntityId minted at random when the snapshot
//                  was taken cannot make them differ.
//   ticks[i]       per fixed step:
//     intents        what the DEVICE SAMPLER produced — the only input that is
//                    not derived. Everything else (kit intents, AI, commands,
//                    physics) is re-derived by running the same logic again.
//     commandDigest  simcmd::Buffer::digest() of what the tick was TOLD to do
//     worldHash      simhash::hashWorld() of what it produced
//
// On replay the two digests LOCALISE a divergence: the command digest differing
// first means the logic decided something different; only the world hash
// differing means the same decisions produced different state.
//
// ── WHAT R1 IS NOT ──────────────────────────────────────────────────────────
// Replay FROM THE START only. Rollback — restoring a mid-session state and
// re-running forward — needs a world snapshot/restore that does not exist yet
// (R3). The two need different things, and sim_intent.h says why.
//
// ── THE RULES A REPLAYABLE SESSION DEPENDS ON, stated rather than discovered ─
//   * Plugins and kits RESET their session state in onSimulationStart. A
//     plugin keeping, say, an accumulated yaw in a member across sessions
//     starts the replay where the recording ended. tests/sim_replay_test.cpp's
//     driver shows the pattern.
//   * Simulation code reads INTENTS, not the device. During replay the sampler
//     is off; a kit calling engineActionDown from onUpdate reads the live
//     device instead of the recording, and its replay will diverge.
//   * The same BINARY. worldHash is positional in the classification order
//     (sim_hash.h's stated cost), so a take verifies only against the build
//     that recorded it. The intents replay anywhere; the verdict does not.
//
// ── THE FORMAT ──────────────────────────────────────────────────────────────
// Binary, little-endian host order (every target this engine builds for is
// little-endian), Intent records written as their raw 48-byte POD — which is
// safe only because Intent is padding-free by static_assert. A trailer carries
// the body length and an FNV-1a digest, so a truncated or corrupted take is
// DETECTED rather than replayed partially. The version check is `==`, not
// `>=`: a reader that does not know a version cannot know where its body ends,
// and guessing produces a partial take that parses — the reasoning the Add-on
// protocol uses for its frames (docs/architecture/extension-model.md §6).
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "runtime/sim_intent.h"

namespace take {

struct Tick {
    uint64_t                        tick = 0;       // m_simFrame, from 1
    std::vector<simintent::Intent>  intents;        // the sampler's output
    uint64_t                        commandDigest = 0;
    uint64_t                        worldHash = 0;
};

struct Take {
    static constexpr uint32_t kVersion = 1;
    uint64_t          actionHash = 0;       // ActionSet::declarationHash at record
    uint64_t          localController = 0;  // EntityId whose device was sampled
    float             simDt = 0.0f;         // the fixed step it was recorded at
    std::string       start;                // the scene snapshot it began from
    std::vector<Tick> ticks;
};

std::vector<uint8_t> encode(const Take& take);
// False — with a reason in `error` — for anything not wholly and exactly a take
// of this version. `out` is untouched on failure.
bool decode(const uint8_t* bytes, size_t size, Take& out,
            std::string* error = nullptr);

}  // namespace take
