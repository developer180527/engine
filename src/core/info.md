---
status: as-built
tier: hardened
verified: 2026-08-04
covers:
  - src/core/
tests:
  - tests/profiler_test.cpp
  - tests/arena_test.cpp
  - tests/stress_deep_tree.cpp
  - tests/stress_swarm.cpp
---
# Core

## Purpose
Dependency-light fundamentals used by every other subsystem. Nothing here may
include renderer, ECS, or editor headers.

## Contents
- **`handle.h`** — `Handle<Tag>`: type-safe uint32_t wrapper for registry
  slots. Slot 0 is reserved as the null handle in every registry.
- **`transform.h` / `transform_utils.h`** — position/rotation(quat)/scale
  component + matrix composition helpers (bx conventions, row-major).
  `Transform::getMatrix` writes the SRT matrix directly rather than composing
  it with two `bx::mtxMul`s: same 16 floats, but the multiply form spent 128
  multiplies computing them, three quarters against the structural zeros of a
  diagonal S and a near-identity T. It is the most-called function in the
  engine, and it was the bulk of the renderer's extraction cost at 20 000
  objects. Bit-exact equivalence to the two-multiply reference is asserted over
  randomised transforms in `tests/extract_partition_test.cpp` — including
  negative, non-uniform and zero scale.
  `getMatrix`'s standing contract is unchanged and load-bearing: `m[12..14]`
  equals `position` exactly, whatever the scale or rotation, which is how the
  gizmo reads position back out.
  `localMatrixLerp` / `getWorldMatrixLerpFrom` take components the caller
  already holds; the entity-only `getWorldMatrixLerp` remains for callers that
  only have an entity. Passing them in rather than looking them up is worth
  several ms per frame at scene scale — see `src/render/issues.md` R14.
- **`math_types.h`** — small shared math types.
- **`entity_id_util.h`** — stable entity id helpers for serialization.
- **`logger.h`** — LOG_INFO/WARN/ERROR/SUCCESS → stdout + editor console.
- **`profiler.h`** — extensible instrumenting profiler (hub + channel
  registry; timer is the first channel). `ENGINE_PROFILE_SCOPE("name")`.
  GPU/ECS-free so it times boot and works in engine_core tools. Hardened:
  fixed-capacity buffers (overflow drops+warns), parent IDs, platform clock
  seam, cache-line-aligned recorders. See `docs/guides/performance.md`.
- **`mem_counters.h/.cpp`** — process-wide counting `operator new`/`delete`
  (COUNTS + forwards to malloc/free; never pools — ASan/leaks keep working).
  Gated by ENGINE_MEM_COUNT. Read per-frame deltas via runtime's MemoryChannel.
- **`frame_arena.h`** — linear bump allocator for per-frame transient data;
  reset() frees everything in O(1). Opt-in/explicit (our code uses it, it
  intercepts nothing). Lifetime: valid only within the frame; trivial
  destruction only; one arena per thread. EngineRuntime owns one (4 MB),
  reset each frameBegin; reach it via `engine.frameArena()`.

## Rules
- Keep this layer header-only and free of engine state; it should compile in
  a unit test with no GPU, no ECS, no filesystem.
