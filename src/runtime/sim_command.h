#pragma once
// ── sim_command — what the simulation was TOLD to do at tick N ───────────────
//
// A canonical, deterministic record of the tick's inputs, separate from the
// world state those inputs produce.
//
// ── WHY THIS EXISTS ─────────────────────────────────────────────────────────
// The engine's control model was "N systems mutate world state and the last one
// wins". Six places wrote Transform with no arbitration, decided by plugin
// registration order nobody chose — the same defect class as BUG-0053 (clocks
// at render rate) and BUG-0054 (collision events by thread race).
//
// Permission-checking those writes would protect the symptom. The shape a
// headless, replayable, network-capable simulation needs is:
//
//     per-tick commands -> one simulation -> authoritative state -> presentation
//
// This header is the first arrow. It turns replay, rollback, prediction,
// server resimulation and headless reproduction from four separate problems
// into one: capture the commands, and the state follows.
//
// ── THE INVARIANT, AND WHY IT IS ALREADY MEASURABLE ─────────────────────────
//     same initial state + same commands  =>  same resulting state
//
// That is exactly what tests/determinism_gate_test.cpp measures, and it has
// been green on all ten tier/comparison pairs since 2026-09-08. The
// verification engine for this architecture was built before the architecture:
// simhash::hashWorld already produces per-tick state hashes whose stability is
// established, so a replay test is a comparison, not a new mechanism.
//
// ── PADDING-FREE ON PURPOSE ─────────────────────────────────────────────────
// Modelled on InputSnapshot (runtime/input/input_manager.h), which carries an
// explicit `_pad` and the note "snapshots are memcmp'd/hashed/wired — NO
// indeterminate padding bytes allowed". A command stream is written to a replay
// file, hashed for divergence checks, and eventually put on a wire; an
// indeterminate byte in any of those is a false divergence at best.
#include <cstddef>
#include <cstdint>
#include <vector>

namespace simcmd {

// ── What was asked for ──────────────────────────────────────────────────────
// Kinds are APPEND-ONLY and their numeric values are part of the replay format.
// Renumbering silently invalidates every recorded stream.
enum class Cmd : uint16_t {
    MoveContribution = 0,   // horizontal + vertical locomotion, COMPOSED (stage 2)
    Jump             = 1,
    Impulse          = 2,   // already composes additively in Jolt
    SetVelocity      = 3,
    Teleport         = 4,   // atomic pose change: body + ECS + PrevTransform
    SetKinematicTarget = 5, // the pose to REACH by the end of the next step
    SetBodyType      = 6,   // Dynamic <-> Kinematic, at runtime
    Count
};

// ── Who asked ───────────────────────────────────────────────────────────────
// The axis a Transform-permission model could not express: a network
// correction, a cutscene, AI, player input and a gameplay ability are all
// "gameplay", with different priority and lifetime. Ordering is priority
// ordering, low to high, and stage 2's composition rules key off it.
enum class Source : uint16_t {
    Gameplay   = 0,   // scripts, kits, ordinary game code
    AI         = 1,
    Animation  = 2,   // root motion
    Ability    = 3,   // dashes, knockback
    Cutscene   = 4,
    Correction = 5,   // network reconciliation — outranks everything
    Count
};

// ── One command ─────────────────────────────────────────────────────────────
// POD, trivially copyable, no indeterminate bytes. 48 bytes.
struct SimCommand {
    Cmd      kind   = Cmd::MoveContribution;
    Source   source = Source::Gameplay;
    uint32_t seq    = 0;      // submission index within the tick — the LAST
                              // tie-break, so ordering is total and stable
                              // without depending on submission order for
                              // anything a caller can observe.

    // ── EntityId::value, NOT flecs::entity_t, and the distinction matters ───
    // Raw flecs ids are fine INSIDE one process — simhash::hashWorld already
    // sorts by them, and the gate proves they are stable run to run. But a
    // command stream outlives a process: a replay file, a rollback buffer, a
    // packet. Those need the persistent identity, which is what EntityId is
    // for (components/entity_id.h: "Generated once, serialized, never
    // changed"). Collapsing the two is the mistake sim_hash.h deliberately
    // refuses to make.
    uint64_t entity = 0;

    // Payload, interpreted by `kind`. A fixed array rather than a union so the
    // bytes are always initialised and the struct stays memcmp-able.
    float    a[8]   = {};
};
static_assert(sizeof(SimCommand) == 48, "SimCommand is a wire/replay POD");
static_assert(alignof(SimCommand) == 8, "SimCommand alignment is part of the layout");

// ── The tick's commands ─────────────────────────────────────────────────────
// Submitted during broadcastUpdate (and from outside the sim), drained and
// EXECUTED in a canonical order at the top of the fixed step.
class Buffer {
public:
    // Returns false when the entity id is 0 (unassigned) — a command that
    // cannot name its target is not recordable and would replay as a no-op
    // against a different entity.
    bool submit(const SimCommand& c);

    // Canonical order: (entity, source, kind, seq). Deterministic and
    // INDEPENDENT of submission order, which is precisely the property
    // last-writer-wins lacked — two systems submitting in either order now
    // produce the same execution sequence.
    void sortForExecution();

    const std::vector<SimCommand>& commands() const { return m_cmds; }
    void clear() { m_cmds.clear(); m_seq = 0; }
    size_t size() const { return m_cmds.size(); }

    // FNV-1a over the whole buffer, for divergence reporting and replay
    // verification. Reuses engine_abi's constants like simhash does rather
    // than adding another copy of the algorithm.
    uint64_t digest() const;

private:
    std::vector<SimCommand> m_cmds;
    uint32_t                m_seq = 0;
};

}  // namespace simcmd
