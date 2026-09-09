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
// This header is the first arrow.
//
// ── WHAT IT GIVES, AND WHAT IT DOES NOT ─────────────────────────────────────
// An earlier version of this comment said the record turns replay, rollback,
// prediction and server resimulation into ONE problem. That is true of replay
// and false of rollback, and the difference decides what has to be recorded.
//
//   REPLAY   = same initial state + the same recorded commands => same result.
//              Recording a DERIVED command (MoveContribution{(0.7,0,0.7)}) is
//              sufficient, because nothing needs to be recomputed.
//   ROLLBACK = restore a CORRECTED state, then run the logic forward again.
//              If AI computed that (0.7,0,0.7) from world state the correction
//              just changed, replaying the recorded contribution reproduces
//              movement that is now wrong. It has to be RE-DERIVED.
//
// Which is why AAA systems record intent, not the output of logic. What this
// file records is the command layer — what systems DECIDED this tick. Rollback
// additionally needs an input layer (player/AI intent, re-derivable), which is
// named here so MoveContribution is not mistaken for it, and is not built.
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
    // RESERVED, AND NOT THE RECONCILIATION MECHANISM. A server correction does
    // not compose with local input inside one tick's stream: it REPLACES
    // authoritative state at tick N and re-simulates N+1..now from the client's
    // saved inputs. Modelling it as a high-priority command in this buffer
    // would produce something that looks like it reconciles and does not. The
    // value is held so the numbering stays stable; nothing may submit with it
    // until the input layer above exists.
    Correction = 5,
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

    // ── Payload, interpreted by `kind` ─────────────────────────────────────
    // Fixed size on purpose: the whole point of the POD is that a stream of
    // these is a flat array that can be memcmp'd, hashed and written to a file
    // with no per-kind serialiser. `Jump` paying for 32 unused bytes is the
    // price of that, and it is the right trade at this size.
    //
    // TWO VIEWS OF THE SAME 32 BYTES. The first version was float-only, which
    // would have forced every non-float payload through a float: SetBodyType's
    // enum, and any future entity id, tick number or bitmask — the last of
    // which loses exactness above 2^24 SILENTLY. Same storage, no padding,
    // still trivially copyable; `u` is simply the honest view when the value
    // being carried is not a real number.
    union {
        float    a[8] = {};      // NSDMI on the first member zero-fills BOTH
        uint32_t u[8];
    };
};
static_assert(sizeof(SimCommand) == 48, "SimCommand is a wire/replay POD");
static_assert(alignof(SimCommand) == 8, "SimCommand alignment is part of the layout");
static_assert(offsetof(SimCommand, entity) == 8, "layout is the replay format");
static_assert(offsetof(SimCommand, a) == 16, "layout is the replay format");

// ── MoveContribution's payload ──────────────────────────────────────────────
// Per-kind accessors, deferred from stage 1.5 until a payload had a consumer so
// the layout would not be invented ahead of use. This is that consumer.
//
// Callers use the factory and the readers; the slot indices are named once,
// here, so a payload layout is never spelled out at a call site. Slots are as
// APPEND-ONLY as the enums — a recorded stream is read back by index.
namespace move {

// How a contribution combines with the others aimed at the same entity.
enum class Mode : uint32_t {
    Additive  = 0,   // summed — player input, AI steering, wind
    Override  = 1,   // replaces every contribution of LOWER source priority
    Exclusive = 2,   // the only contribution that survives — root motion
    Count
};

inline constexpr int kSlotHorizX   = 0;
inline constexpr int kSlotHorizZ   = 1;
inline constexpr int kSlotVertical = 2;
inline constexpr int kSlotMode     = 3;   // read through `u`, not `a`

inline SimCommand contribution(uint64_t entity, Source source,
                               float horizX, float horizZ, float vertical,
                               Mode mode = Mode::Additive) {
    SimCommand c{};
    c.kind   = Cmd::MoveContribution;
    c.source = source;
    c.entity = entity;
    c.a[kSlotHorizX]   = horizX;
    c.a[kSlotHorizZ]   = horizZ;
    c.a[kSlotVertical] = vertical;
    c.u[kSlotMode]     = static_cast<uint32_t>(mode);
    return c;
}

inline float horizX  (const SimCommand& c) { return c.a[kSlotHorizX]; }
inline float horizZ  (const SimCommand& c) { return c.a[kSlotHorizZ]; }
inline float vertical(const SimCommand& c) { return c.a[kSlotVertical]; }
// A stream recorded by a newer build can carry a mode this one has no rule for.
// Reported rather than guessed at: composeMoves() counts it and treats it as
// Additive, which is the mode that cannot silently erase another contribution.
inline bool  modeKnown(const SimCommand& c) { return c.u[kSlotMode] < (uint32_t)Mode::Count; }
inline Mode  mode(const SimCommand& c) {
    return modeKnown(c) ? static_cast<Mode>(c.u[kSlotMode]) : Mode::Additive;
}

}  // namespace move

// ── The tick's commands ─────────────────────────────────────────────────────
// Submitted during broadcastUpdate ONLY, then ordered canonically and executed
// within the same fixed step.
//
// ── WHY SUBMISSION HAS A PHASE, AND WHY IT IS ENFORCED ──────────────────────
// The buffer is reachable from anywhere holding the runtime — including
// onFrame, an editor panel and a render-rate kit callback. A command submitted
// at RENDER rate lands in whichever tick's buffer happens to be open, so the
// set of commands a tick receives becomes frame-rate-dependent: two frames per
// tick and one frame per tick produce different simulations from identical
// content. That is BUG-0053's defect class exactly (gameplay clocks advancing
// at render rate), one layer up, inside the subsystem built to remove it.
//
// So it is refused rather than documented. A guard costs a branch now and is
// unremovable once kits are written against the loose behaviour.
class Buffer {
public:
    // Returns false when the entity id is 0 (unassigned) — a command that
    // cannot name its target is not recordable and would replay as a no-op
    // against a different entity — or when submission is CLOSED (see above).
    bool submit(const SimCommand& c);

    // The window. The runtime opens it around broadcastUpdate and closes it for
    // the rest of the frame. Default-OPEN so a standalone Buffer — a test, a
    // replay tool, a server with no frame loop — needs no ceremony; a runtime
    // closes it explicitly when a session starts.
    void setSubmissionOpen(bool open) { m_open = open; }
    bool submissionOpen() const { return m_open; }
    // Commands refused for arriving outside the window, since the last clear().
    // A count rather than a hard failure: the first job is to NAME the caller.
    uint32_t refusedOutOfPhase() const { return m_refused; }

    // Canonical order: (entity, source, kind, seq). Deterministic and
    // INDEPENDENT of submission order, which is precisely the property
    // last-writer-wins lacked — two systems submitting in either order now
    // produce the same execution sequence.
    void sortForExecution();

    const std::vector<SimCommand>& commands() const { return m_cmds; }
    // Ends the tick. Leaves submission CLOSED: the next tick's window is opened
    // by the runtime, so the gap between ticks refuses rather than accumulates.
    void clear() { m_cmds.clear(); m_seq = 0; m_refused = 0; m_open = false; }
    size_t size() const { return m_cmds.size(); }

    // An order-sensitive fold of per-command FNV-1a values, length-prefixed —
    // not one FNV pass over the buffer's bytes, which is what this said before.
    // Order-sensitive is the property wanted: it is used to check that a replay
    // was told the same things in the same sequence.
    uint64_t digest() const;

private:
    std::vector<SimCommand> m_cmds;
    uint32_t                m_seq     = 0;
    uint32_t                m_refused = 0;
    bool                    m_open    = true;
};

}  // namespace simcmd
