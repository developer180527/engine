#pragma once
// ── sim_hash — a canonical digest of ECS-observable simulation state ─────────
//
// The mechanism behind tests/determinism_gate_test.cpp. Hash the classified
// component state of a world after every tick, run the same inputs twice, and
// compare: the first differing tick localises a divergence to a tick, a
// component type and an entity.
//
// ── WHY ─────────────────────────────────────────────────────────────────────
// A simulation that is not reproducible cannot have BEHAVIOURAL regression
// tests, only structural ones — today the sim is covered by "does the query
// follow the world" and by nothing that asserts "these inputs produce this
// outcome". docs/plans/automated-testing-soak-fuzz-plan.md 6.3 specifies this
// lane and notes that "right now nothing in the engine would tell you it is
// broken". Replay, bug reproduction, a soak lane that can bisect, and
// deterministic lockstep or rollback if a project ever wants one are all
// consumers of the same property.
//
// ── WHAT THIS COVERS, AND WHAT IT DOES NOT ──────────────────────────────────
// **ECS-observable determinism.** It hashes classified ECS components. It does
// NOT hash JPH::PhysicsSystem, lua_State, AnimatorSystem::m_contexts, or kit
// members. A matching hash means "nothing that reaches a component diverged" —
// NOT "the simulation is bit-identical".
//
// Most external divergence does surface (physics divergence moves Transform; a
// Lua divergence that changes gameplay writes a component), but state that
// never reaches a component — a Lua-internal counter, an ozz sampling cursor —
// is invisible here. That is a real false-pass window, stated up front rather
// than discovered later. Closing it needs a per-subsystem deterministic-state
// interface that does not exist.
//
// ── WHY NOT THE OBVIOUS ALTERNATIVES ────────────────────────────────────────
//   * ecs_ptr_to_json / ecs_world_to_json: TEXT. A float printed and re-read
//     masks a 1-ULP divergence, which is exactly the class this exists to find.
//     A hash that reports "deterministic" while the sim diverges is worse than
//     no hash at all.
//   * memcmp of component bytes: Name, ScriptComponent, Animator and
//     CollisionEvents hold heap pointers, and padding is indeterminate. Their
//     bytes differ every run for reasons that are not divergence.
//   * SceneSerializer: queries <Name, Transform>, skips Spinner, drops tags,
//     drops all pairs including (EventStale,T), drops SerdeTransient, and
//     remaps entity ids. Every one of those is right for scene authoring and
//     fatal here.
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include <flecs.h>

#include <engine/game_module.h>          // engine_abi::mix / kFnvBasis
#include "components/sim_state.h"

namespace simhash {

// ── The digest ──────────────────────────────────────────────────────────────
// FNV-1a, reusing engine_abi's constants rather than adding a fourth copy of
// the algorithm to this tree.
struct Digest {
    uint64_t h = engine_abi::kFnvBasis;
    void u64(uint64_t v) { h = engine_abi::mix(h, v); }
    void u32(uint32_t v) { u64(v); }
    void i32(int32_t  v) { u64(static_cast<uint64_t>(static_cast<uint32_t>(v))); }
    void boolean(bool v) { u64(v ? 1u : 0u); }
    void bytes(const void* p, size_t n);

    // ── SCALAR SEMANTICS, chosen to CATCH rather than hide ──────────────────
    //   finite      exact bit pattern
    //   +0.0/-0.0   DISTINCT — a sign difference is a real path difference
    //   +inf/-inf   distinct from each other and from finite
    //   NaN         canonicalised: payloads are libm-arbitrary, so two runs
    //               that both produced "not a number" agree here
    // Unit-tested independently in tests/determinism_gate_test.cpp section 1 —
    // a hashing primitive nobody tests is the softest part of any gate.
    void f32(float  v);
    void f64(double v);

    // CONTENT, length-prefixed. Never the pointer — std::string's bytes differ
    // every run. The length prefix is what stops "ab"+"c" colliding with
    // "a"+"bc" when two fields are hashed in sequence.
    void str(std::string_view s);
};

// ── Classification ──────────────────────────────────────────────────────────
enum class Classification { SimState, SimExempt };

// Takes the OWNING ENTITY as well as the component. Components holding entity
// references or handles need context to canonicalise, and adding the parameter
// later would mean touching every hasher ever written.
using HashFn = void (*)(flecs::entity, const void* comp, Digest&);

// Per-world singleton — component ids are world-local, so the registry must be
// too. Mirrors EventRegistry (components/event_component.h), which is the
// established pattern in this tree for exactly this problem.
struct SimStateRegistry {
    struct Entry {
        flecs::entity_t comp = 0;
        uint32_t        canonicalIndex = 0;  // ORDERING. Never the name.
        std::string     debugName;           // diagnostics only
        Classification  cls = Classification::SimExempt;
        HashFn          hash = nullptr;      // required iff SimState
        const char*     reason = nullptr;    // required iff SimExempt
    };
    std::vector<Entry> entries;              // in canonicalIndex order
};

// ── Declaration ─────────────────────────────────────────────────────────────
// declare<T> and exempt<T> are MUTUALLY EXCLUSIVE and each type may be
// registered once. The registry refuses duplicate registration, a SimState
// without a hasher, and a SimExempt without a reason — an exemption with no
// written reason is indistinguishable from an oversight, and the point of
// classifying is that the exclusions are reviewable facts rather than gaps.
//
// canonicalIndex is assigned by declaration order and is what orders the hash.
// It is deliberately NOT the component path: renaming a component for
// readability must not silently change the determinism protocol.
//
// THE COST, stated because it is real: the protocol is now the ORDER OF THE
// registerClassification() CALLS. Reordering those lines changes every hash in
// the tree — silently, since nothing about a reorder looks like a protocol
// change. Neither scheme (nor a path-sorted one) is stable across binaries, and
// this gate only ever compares hashes produced by ONE binary, so the exposure
// is bounded. If a hash ever has to survive across builds — a golden value
// checked into the repo, a cross-machine lane — this is the first thing that
// has to become explicit rather than positional.
bool declareId(flecs::world&, flecs::entity comp, HashFn);
bool exemptId (flecs::world&, flecs::entity comp, const char* reason);

template <typename T>
inline bool declare(flecs::world& w, HashFn fn) {
    return declareId(w, w.template component<T>(), fn);
}
template <typename T>
inline bool exempt(flecs::world& w, const char* reason) {
    return exemptId(w, w.template component<T>(), reason);
}

// ── Hashing ─────────────────────────────────────────────────────────────────
// Shaped so FIELD-level diagnostics can be added beneath these without
// restructuring: per-type and per-entity live in separate vectors, and a
// per-field tier slots in below them.
struct HashReport {
    uint64_t total = 0;
    std::vector<std::pair<std::string, uint64_t>> perType;    // debugName -> digest
    std::vector<std::pair<uint64_t,    uint64_t>> perEntity;  // raw id  -> digest
};

// The canonical digest of every SimState component in `w`.
//
// ORDER, and each level is chosen to catch rather than hide:
//   1. Entities by RAW flecs::entity_t (id + generation), with the raw id mixed
//      in. Deliberately not by EntityId::value — those are different concepts
//      and must not collapse: flecs::entity_t observes CREATION ORDER, EntityId
//      is persistent gameplay identity. Sorting by the raw id means a change in
//      entity creation order shows up as a divergence, which is the point.
//   2. Components by canonicalIndex, via ecs_get_id, skipping absent. NOT
//      e.each(flecs::id) — that is table column order, which depends on
//      component registration order, which depends on kit load order.
//   3. Pairs: the sorted set of canonical indices for which (EventStale, T) is
//      present. One-tick event lifetime IS simulation state; SceneSerializer
//      drops it and this must not.
uint64_t hashWorld(flecs::world& w, HashReport* detail = nullptr);

// ── Coverage ────────────────────────────────────────────────────────────────
// An unclassified component is INVISIBLE to hashWorld, which is the worst
// failure mode an instrument can have — it would report "reproducible" while
// silently not looking. This inverts the default: write a new gameplay
// component and the gate tells you to classify it.
//
// Audits component ids actually PRESENT ON ENTITIES, not every registered flecs
// type — the latter sweeps in SerdeTransient, event metadata and primitives as
// though they were gameplay state.
//
// Call it at the START and the END of a run: a type instantiated mid-run (a
// script spawning something) is absent at the start and present at the end. A
// type declared but never instantiated contributes nothing to the hash and
// cannot cause divergence, so its absence here is correct rather than a gap.
//
// KNOWN GAP: flecs stores a singleton on its own component-type entity, and
// this skips component-type entities (they are declarations, not simulation
// data). Singleton components are therefore neither hashed nor audited.
bool auditCoverage(flecs::world& w, std::vector<std::string>& unclassified);

// True if `id` is a data component this engine owns — not a pair, not a flecs
// builtin, not a zero-size tag. The scope test matches reflected_serde.h's
// existing convention (a path beginning "flecs" is theirs, not ours); the id
// range test does NOT work, because flecs' own modules and meta constants are
// ordinary entities well above EcsFirstUserEntityId.
bool isAuditableComponent(flecs::world& w, flecs::id id);

} // namespace simhash
