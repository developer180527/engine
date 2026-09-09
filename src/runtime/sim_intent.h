#pragma once
// ── sim_intent — what a controller ASKED FOR, before anything derived from it ─
//
// Stage 5 of the command architecture, and the layer stage 1 deliberately named
// without building so that `MoveContribution` would not be mistaken for it.
//
// ── WHY A SECOND RECORD ─────────────────────────────────────────────────────
// sim_command.h records what systems DECIDED this tick. That is enough for
// REPLAY — same initial state plus the same commands gives the same result,
// because nothing is recomputed. It is NOT enough for ROLLBACK:
//
//     rollback restores a CORRECTED state and runs the logic forward again.
//
// If the AI computed `MoveContribution{(0.7, 0, 0.7)}` from world state that
// the correction just changed, replaying that contribution reproduces movement
// that is now wrong. It has to be RE-DERIVED, which means re-running the logic,
// which means having the logic's INPUTS. That is this file.
//
// So the two records answer different questions and both are kept:
//
//     Intent      what was ASKED    re-derivable    rollback, prediction
//     SimCommand  what was DECIDED  replayable      replay, divergence reports
//
// ── WHY IT IS SAMPLED PER TICK, WHICH IS THE OTHER HALF ─────────────────────
// A first-person controller latches the mouse in `onFrame`, at render rate, and
// then feeds the resulting yaw into its movement direction and its raycasts —
// which are simulation. `onFrame` runs AFTER the fixed steps, so `onUpdate`
// sees one frame's worth of accumulated look at 1 frame/tick and two at 2
// frames/tick: the same physical mouse motion produces a different simulation.
//
// Stage 4 moved the camera's AIM out of hashed state (`CameraLook`), which
// stopped the render-rate write reaching the world hash — and said plainly that
// it did not make look deterministic, because the *simulation* still read a
// frame-rate-accumulated value. This is what closes that: intent is sampled
// ONCE PER TICK, inside the fixed step, so a controller reading it is
// tick-driven by construction. Presentation may still latch at render rate; the
// two are now different reads of the same device, which is the presentation
// split applied to input.
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace simintent {

// One controller's intent for one tick. POD, padding-free, wire-able — the same
// discipline as SimCommand and InputSnapshot, and for the same three reasons:
// it is hashed for divergence checks, written to a replay file, and eventually
// put on a wire, and an indeterminate byte in any of those is a false
// divergence at best.
struct Intent {
    // EntityId::value, not a raw flecs id — an intent stream outlives the
    // process exactly as a command stream does. Who this intent is FOR: the
    // player's pawn, or an AI agent's.
    uint64_t entity = 0;

    // ── The axes, in the GAME's terms and not the device's ─────────────────
    // Recording device state (HID usages, button bits) would bake the current
    // key bindings into every recorded stream: edit input.json and yesterday's
    // recording means something else. It would also be unusable by AI, which
    // has no device. So intent is what the action map already produces.
    float moveX = 0.0f, moveY = 0.0f;    // -1..1, the "Move" axis2

    // The tick's look delta in raw device counts. Sensitivity stays with the
    // controller: it is a gameplay tuning value, and baking it in here would
    // make a recorded stream unreplayable against a different settings menu.
    float lookDx = 0.0f, lookDy = 0.0f;

    // ── Actions, as bitsets ────────────────────────────────────────────────
    // Bit N is the action declared Nth on the Sampler (see below). An index
    // rather than a name because this is a wire POD; the Sampler's
    // declarationHash travels with the stream so a mismatch is DETECTED rather
    // than silently reinterpreted as a different action.
    uint32_t held = 0, pressed = 0, released = 0;

    uint16_t source = 0;    // simcmd::Source — player, AI, a replaying client
    uint16_t _pad   = 0;    // explicit: no indeterminate bytes
    uint32_t seq    = 0;    // submission index within the tick, the last tie-break
    uint32_t _pad2  = 0;
};
static_assert(sizeof(Intent) == 48, "Intent is a wire/replay POD");
static_assert(alignof(Intent) == 8, "Intent alignment is part of the layout");
static_assert(offsetof(Intent, entity) == 0, "layout is the replay format");
static_assert(offsetof(Intent, moveX)  == 8, "layout is the replay format");
static_assert(offsetof(Intent, held)   == 24, "layout is the replay format");

// ── The tick's intents ──────────────────────────────────────────────────────
// Deliberately the same shape as simcmd::Buffer, including the submission
// window: an intent submitted at render rate would land in whichever tick's
// buffer happened to be open, which is the defect this whole layer exists to
// remove. Refusing it here rather than documenting it is the lesson stage 1.5
// paid for once already.
class Buffer {
public:
    bool submit(const Intent& in);

    // Canonical order: (entity, source, seq). No `kind` axis — an intent is one
    // per (entity, source) per tick by construction, and two from one source
    // for one entity is a caller bug rather than something to compose.
    void sortForExecution();

    const std::vector<Intent>& intents() const { return m_intents; }
    // Ends the tick, leaving submission CLOSED — see simcmd::Buffer::clear.
    void clear() { m_intents.clear(); m_seq = 0; m_refused = 0; m_open = false; }
    size_t size() const { return m_intents.size(); }

    void setSubmissionOpen(bool open) { m_open = open; }
    bool submissionOpen() const { return m_open; }
    uint32_t refusedOutOfPhase() const { return m_refused; }

    // The intent for one entity from one source, or null. What a controller
    // calls instead of reading the device.
    const Intent* find(uint64_t entity, uint16_t source = 0) const;

    // Order-sensitive fold of per-intent FNV-1a values, length-prefixed — the
    // same construction simcmd::Buffer::digest uses, and for the same use: a
    // replay checks it was told the same things in the same sequence.
    uint64_t digest() const;

private:
    std::vector<Intent> m_intents;
    uint32_t            m_seq     = 0;
    uint32_t            m_refused = 0;
    bool                m_open    = true;
};

// ── What the game calls an action ───────────────────────────────────────────
// The list is DECLARED, in order, and the order is the bit index. Declared
// rather than discovered from input.json because input.json's actions live in a
// context STACK — the same name can exist at two levels, and contexts push and
// pop during play, so "the Nth action in the config" is not a stable identity.
// A list the game states is.
//
// declarationHash travels with a recorded stream. A stream replayed against a
// different list is then a detected mismatch instead of a controller acting on
// the wrong bits — which would look like a physics or logic bug and be
// debugged as one.
class ActionSet {
public:
    static constexpr int kMaxActions = 32;   // one uint32_t bitset

    // Returns the bit index, or -1 when full. Declaring the same name twice
    // returns the existing index rather than burning a bit.
    int declare(const std::string& name);
    int indexOf(const std::string& name) const;
    size_t size() const { return m_names.size(); }
    const std::vector<std::string>& names() const { return m_names; }
    uint64_t declarationHash() const;
    void clear() { m_names.clear(); }

private:
    std::vector<std::string> m_names;
};

}  // namespace simintent
