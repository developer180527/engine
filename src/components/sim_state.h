#pragma once
// ── SimState / SimExempt — "is this component simulation state?" ─────────────
//
// A component's TYPE carries exactly ONE of these, and the answer to
// "is this simulation state" becomes a machine-readable property of the engine
// rather than something you reconstruct by reading systems.
//
//     w.component<Transform>().add<SimState>();      // hashed by the gate
//     w.component<Camera>().add<SimExempt>();        // presentation, ignored
//
// ── WHY THIS EXISTS ─────────────────────────────────────────────────────────
// A simulation that is not reproducible cannot have BEHAVIOURAL regression
// tests — only structural ones. The determinism gate
// (tests/determinism_gate_test.cpp) hashes classified component state after
// every tick and compares runs; `runtime/sim_hash.h` is the mechanism. Replay,
// "reproduce that bug", a soak lane that can bisect a divergence, and
// deterministic lockstep or rollback if a project ever wants one are all
// consumers of the same property.
//
// ── OPT-IN, AND WHY THAT IS SAFE ────────────────────────────────────────────
// An unclassified component would be INVISIBLE to the gate, which is the worst
// failure mode an instrument can have — it would report "reproducible" while
// silently not looking. `simhash::auditCoverage()` closes that: it fails on any
// data component present in the world carrying neither tag. So the default is
// not "silently uncovered", it is "the gate tells you to classify it".
//
// ── THE RULE THIS ENCODES ───────────────────────────────────────────────────
//   ** Authoritative gameplay state lives in ECS components. **
//
// It is cheap now and unenforceable later. What it buys is that the state a
// test, a replay or a rollback must capture is reachable through one mechanism
// instead of scattered across a physics backend, a Lua VM and kit members.
//
// It is NOT machine-enforced for state OUTSIDE the ECS: nothing here can see a
// counter living in a `lua_State` or a velocity living in `JPH::PhysicsSystem`.
// Those are a known limitation of the gate, listed in its header, and closing
// them needs a per-subsystem deterministic-state interface that does not exist.
// Do not read a green gate as "the simulation is bit-identical".

// Simulation state: hashed by the gate. Requires a hasher at declaration.
struct SimState {};

// Not simulation state: skipped by the gate. Requires a written REASON at
// declaration — an exemption without one is indistinguishable from an
// oversight, and the whole point of classifying is that the exclusions are
// reviewable facts rather than gaps.
struct SimExempt {};
